// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_search_controller.cpp
/// @brief Unit tests for AdvancedSearchController -- state machine, history,
///        preferences, worker lifecycle

#include "sak/advanced_search_controller.h"
#include "sak/advanced_search_types.h"
#include "sak/config_manager.h"
#include "sak/regex_pattern_library.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#ifdef _WIN32
#include <windows.h>
#endif

using sak::AdvancedSearchController;
using sak::SearchConfig;
using sak::SearchMatch;
using sak::SearchPreferences;

class AdvancedSearchControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // -- Initial State --
    void initialState_isIdle();
    void patternLibrary_notNull();
    void patternLibrary_hasBuiiltins();

    // -- Search Lifecycle --
    void startSearch_changesState();
    void startSearch_emitsSignals();
    void cancelSearch_changesState();
    void cancelSearch_emitsSignal();
    void searchComplete_returnsToIdle();

    // -- Search History --
    void history_initiallyEmpty();
    void history_addedOnSearch();
    void history_preventsDuplicates();
    void history_maxSize();
    void history_emptyPatternNotAdded();
    void history_clearHistory();

    // -- Preferences --
    void preferences_defaultValues();
    void preferences_setAndGet();
    void preferences_clampedValues();

    // -- Worker Double-Start --
    void doubleStart_cancelsFirst();

    // -- Codex review 5: completeness must travel with the counts --
    void searchFinished_carriesCompleteness();

private:
    QTemporaryDir m_temp_dir;
};

void AdvancedSearchControllerTests::initTestCase() {
    QVERIFY(m_temp_dir.isValid());

    // Create a test file for search operations
    QFile testFile(m_temp_dir.path() + "/test_search.txt");
    QVERIFY(testFile.open(QIODevice::WriteOnly | QIODevice::Text));
    testFile.write("Hello World\nSearch target\nAnother line\n");
    testFile.close();
}

// -- Initial State -----------------------------------------------------------

void AdvancedSearchControllerTests::initialState_isIdle() {
    AdvancedSearchController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::patternLibrary_notNull() {
    AdvancedSearchController ctrl;
    QVERIFY(ctrl.patternLibrary() != nullptr);
}

void AdvancedSearchControllerTests::patternLibrary_hasBuiiltins() {
    AdvancedSearchController ctrl;
    QCOMPARE(ctrl.patternLibrary()->builtinPatterns().size(), 8);
}

// -- Search Lifecycle --------------------------------------------------------

void AdvancedSearchControllerTests::startSearch_changesState() {
    AdvancedSearchController ctrl;
    QSignalSpy stateSpy(&ctrl, &AdvancedSearchController::stateChanged);

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);

    // State should change to Searching
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Searching);
    // Exactly one transition so far (Idle -> Searching); the state above proves the search has not
    // yet finished, so `>= 1` can be tightened to the exact count.
    QCOMPARE(stateSpy.count(), 1);

    // Wait for completion by polling the count, not with spy.wait(). The worker emits
    // finished() from its own thread and the controller re-emits searchFinished on the main
    // thread through a queued connection, so whether the emission lands before or after this
    // line depends on when the event loop next runs. wait() only reports emissions that
    // arrive after it is entered, so it fails on the ordering it does not happen to like;
    // polling the absolute count is correct for both orderings.
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);

    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::startSearch_emitsSignals() {
    AdvancedSearchController ctrl;
    QSignalSpy startedSpy(&ctrl, &AdvancedSearchController::searchStarted);
    QSignalSpy resultsSpy(&ctrl, &AdvancedSearchController::resultsReceived);
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);

    // Wait for completion
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);

    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy[0][0].toString(), "Hello");
    QVERIFY(resultsSpy.count() >= 1);
    QCOMPARE(finishedSpy.count(), 1);

    // Check totals
    const int totalMatches = finishedSpy[0][0].toInt();
    const int totalFiles = finishedSpy[0][1].toInt();
    // The fixture is "Hello World\nSearch target\nAnother line\n" and the pattern is "Hello", which
    // matches exactly one line in the one searched file.
    QCOMPARE(totalMatches, 1);
    QCOMPARE(totalFiles, 1);
}

void AdvancedSearchControllerTests::cancelSearch_changesState() {
    AdvancedSearchController ctrl;

    // Create many files so the search runs long enough to cancel
    QTemporaryDir longDir;
    QVERIFY(longDir.isValid());
    for (int i = 0; i < 200; ++i) {
        QFile f(longDir.path() + QString("/file_%1.txt").arg(i));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QString("Content %1\nWith searchable text\n").arg(i).toUtf8());
        f.close();
    }

    SearchConfig config;
    config.root_path = longDir.path();
    config.pattern = "searchable";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Searching);

    ctrl.cancelSearch();
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Cancelled);

    // What this test actually asserts is that the controller returns to Idle once the worker
    // has acknowledged the stop. The state is Cancelled on entry, so polling cannot pass
    // before the worker has been heard from, and it replaces a fixed qWait(2000) that failed
    // outright if the worker needed 2001ms. Same budget, no false failure.
    //
    // It does NOT assert that searchCancelled is emitted -- see R5-G18-10. The spies that
    // used to sit here were never read, so they were asserting nothing; adding a real
    // assertion is a separate change because a fast search can legitimately complete before
    // cancelSearch() takes effect, and the honest form has to allow both outcomes.
    QTRY_COMPARE_WITH_TIMEOUT(ctrl.currentState(), AdvancedSearchController::State::Idle, 2000);
}

void AdvancedSearchControllerTests::cancelSearch_emitsSignal() {
    AdvancedSearchController ctrl;
    QSignalSpy cancelledSpy(&ctrl, &AdvancedSearchController::searchCancelled);
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);

    // Start a search on a non-existent large path so it doesn't finish instantly
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);
    ctrl.cancelSearch();

    // Wait for worker to process. The search may have finished before cancel took effect,
    // or may have been cancelled -- either way the state must reach Idle, and it is
    // Cancelled on entry so polling cannot pass before the worker has been heard from.
    QTRY_COMPARE_WITH_TIMEOUT(ctrl.currentState(), AdvancedSearchController::State::Idle, 1000);

    // cancelledSpy used to be constructed here and never read, which left a test named
    // cancelSearch_emitsSignal asserting nothing about any signal. Requiring an exact
    // searchCancelled would be wrong -- a search this small can genuinely complete before
    // the stop is observed -- but the controller must announce the outcome one way or the
    // other, and that claim does not depend on who won the race.
    QVERIFY2(cancelledSpy.count() + finishedSpy.count() >= 1,
             "controller returned to Idle without emitting searchCancelled or searchFinished");
}

void AdvancedSearchControllerTests::searchComplete_returnsToIdle() {
    AdvancedSearchController ctrl;
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "target";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);

    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

// -- Search History ----------------------------------------------------------

void AdvancedSearchControllerTests::history_initiallyEmpty() {
    AdvancedSearchController ctrl;
    ctrl.clearHistory();
    QVERIFY(ctrl.searchHistory().isEmpty());
}

void AdvancedSearchControllerTests::history_addedOnSearch() {
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "unique_search_term";
    config.exclude_patterns.clear();

    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    ctrl.startSearch(config);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);

    const auto history = ctrl.searchHistory();
    QVERIFY(history.contains("unique_search_term"));
}

void AdvancedSearchControllerTests::history_preventsDuplicates() {
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    ctrl.addToHistory("dup_test");
    ctrl.addToHistory("dup_test");

    QCOMPARE(ctrl.searchHistory().count("dup_test"), 1);
}

void AdvancedSearchControllerTests::history_maxSize() {
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    // Add more than max history size (50)
    for (int i = 0; i < 60; ++i) {
        ctrl.addToHistory(QString("search_%1").arg(i));
    }

    // 60 added, capped at the 50-entry maximum. `<= 50` would miss a cap that failed to evict.
    QCOMPARE(ctrl.searchHistory().size(), 50);
    // Most recent should be first
    QCOMPARE(ctrl.searchHistory().first(), "search_59");
}

void AdvancedSearchControllerTests::history_emptyPatternNotAdded() {
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    ctrl.addToHistory("");
    ctrl.addToHistory("   ");  // Whitespace-only

    QVERIFY(ctrl.searchHistory().isEmpty());
}

void AdvancedSearchControllerTests::history_clearHistory() {
    AdvancedSearchController ctrl;
    ctrl.addToHistory("item1");
    ctrl.addToHistory("item2");

    ctrl.clearHistory();
    QVERIFY(ctrl.searchHistory().isEmpty());
}

// -- Preferences -------------------------------------------------------------

void AdvancedSearchControllerTests::preferences_defaultValues() {
    // Reset the persisted keys so the controller reads the compiled-in defaults rather than
    // whatever a prior session left in the ConfigManager singleton -- without this, the two exact
    // defaults below are not deterministic in a unit run, which is why they were only loosely
    // bounded before.
    auto& cfg = sak::ConfigManager::instance();
    cfg.remove(QStringLiteral("advsearch/max_results"));
    cfg.remove(QStringLiteral("advsearch/context_lines"));

    AdvancedSearchController ctrl;
    const auto prefs = ctrl.preferences();

    QCOMPARE(prefs.max_results, 0);  // default: 0 == unlimited
    QVERIFY(prefs.max_preview_file_size_mb >= 1);
    QVERIFY(prefs.max_search_file_size_mb >= 1);
    QVERIFY(prefs.max_cache_size >= 1);
    QCOMPARE(prefs.context_lines, 2);  // kDefaultContextLines
}

void AdvancedSearchControllerTests::preferences_setAndGet() {
    AdvancedSearchController ctrl;

    SearchPreferences newPrefs;
    newPrefs.max_results = 500;
    newPrefs.max_preview_file_size_mb = 25;
    newPrefs.max_search_file_size_mb = 100;
    newPrefs.max_cache_size = 75;
    newPrefs.context_lines = 5;

    ctrl.setPreferences(newPrefs);

    const auto readBack = ctrl.preferences();
    QCOMPARE(readBack.max_results, 500);
    QCOMPARE(readBack.max_preview_file_size_mb, 25);
    QCOMPARE(readBack.max_search_file_size_mb, 100);
    QCOMPARE(readBack.max_cache_size, 75);
    QCOMPARE(readBack.context_lines, 5);
}

void AdvancedSearchControllerTests::preferences_clampedValues() {
    AdvancedSearchController ctrl;

    // Set extreme values
    SearchPreferences newPrefs;
    newPrefs.max_results = 1'000'000;  // No clamp on this (0=unlimited)
    newPrefs.max_preview_file_size_mb = 999;
    newPrefs.max_search_file_size_mb = 999;
    newPrefs.max_cache_size = 999;
    newPrefs.context_lines = 999;  // above the ceiling, so the clamp is actually exercised

    ctrl.setPreferences(newPrefs);

    // Re-load from config to verify clamping
    ctrl.loadPreferences();
    const auto prefs = ctrl.preferences();

    // 999 preview clamps to the 500 ceiling; 999 search/cache are within their 1000 ceiling so they
    // survive unchanged. `<=` would pass even if the clamp silently zeroed or mis-applied.
    QCOMPARE(prefs.max_preview_file_size_mb, 500);
    QCOMPARE(prefs.max_search_file_size_mb, 999);
    QCOMPARE(prefs.max_cache_size, 999);
    QCOMPARE(prefs.context_lines, 10);  // 999 clamps to the kMaximumContextLines ceiling
}

// -- Worker Double-Start -----------------------------------------------------

void AdvancedSearchControllerTests::doubleStart_cancelsFirst() {
    AdvancedSearchController ctrl;

    SearchConfig config1;
    config1.root_path = m_temp_dir.path() + "/test_search.txt";
    config1.pattern = "Hello";
    config1.exclude_patterns.clear();

    SearchConfig config2;
    config2.root_path = m_temp_dir.path() + "/test_search.txt";
    config2.pattern = "target";
    config2.exclude_patterns.clear();

    // Start first search
    ctrl.startSearch(config1);

    // Immediately start second -- should cancel first
    ctrl.startSearch(config2);

    // Only the second search reaches this signal: the superseded worker's queued
    // finished() is dropped by the generation guard.
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 5000);

    // Should have completed (the second search)
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);

    // History should contain both patterns
    const auto history = ctrl.searchHistory();
    QVERIFY(history.contains("Hello"));
    QVERIFY(history.contains("target"));
}

// ============================================================================
// Codex review 5: completeness must travel with the counts
// ============================================================================

void AdvancedSearchControllerTests::searchFinished_carriesCompleteness() {
    // A run that skipped files must not be announceable as complete by ANY
    // consumer -- the status bar and the panel's log pane both read this flag.

    // (a) A clean run: complete == true.
    QTemporaryDir clean_dir;
    QVERIFY(clean_dir.isValid());
    {
        QFile readable(QDir(clean_dir.path()).filePath(QStringLiteral("readable.txt")));
        QVERIFY(readable.open(QIODevice::WriteOnly));
        readable.write("Hello there");
        readable.close();
    }

    SearchPreferences prefs;
    prefs.max_results = 0;  // an on-disk cap would itself make the run incomplete

    AdvancedSearchController clean_ctrl;
    clean_ctrl.setPreferences(prefs);
    QSignalSpy cleanFinished(&clean_ctrl, &AdvancedSearchController::searchFinished);

    SearchConfig clean_config;
    clean_config.root_path = clean_dir.path();
    clean_config.pattern = QStringLiteral("Hello");
    clean_config.exclude_patterns.clear();
    clean_ctrl.startSearch(clean_config);

    QTRY_COMPARE_WITH_TIMEOUT(cleanFinished.count(), 1, 10'000);
    QCOMPARE(cleanFinished.count(), 1);
    QCOMPARE(cleanFinished[0][2].toBool(), true);

    // (b) A run holding a file the worker cannot open: complete == false.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString lockedPath = QDir(dir.path()).filePath(QStringLiteral("locked.txt"));
    {
        QFile locked(lockedPath);
        QVERIFY(locked.open(QIODevice::WriteOnly));
        locked.write("Hello secret");
        locked.close();
    }

    const std::wstring wpath = lockedPath.toStdWString();
    HANDLE handle = CreateFileW(
        wpath.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    QVERIFY(handle != INVALID_HANDLE_VALUE);

    AdvancedSearchController ctrl;
    ctrl.setPreferences(prefs);
    QSignalSpy finished(&ctrl, &AdvancedSearchController::searchFinished);

    SearchConfig config;
    config.root_path = dir.path();
    config.pattern = QStringLiteral("Hello");
    config.exclude_patterns.clear();
    ctrl.startSearch(config);

    // qWaitFor re-checks the predicate, so it returns at once when the emission already
    // landed; finished.wait() would latch the count on entry and burn the whole timeout.
    // The QTRY_* form is not usable here -- the handle has to be released before any
    // assertion can abort the function, or a failure leaves the temp dir undeletable.
    const bool emitted = QTest::qWaitFor([&finished]() { return finished.count() > 0; }, 10'000);
    CloseHandle(handle);
    QVERIFY(emitted);
    QCOMPARE(finished.count(), 1);
    QCOMPARE(finished[0][2].toBool(), false);
}

QTEST_GUILESS_MAIN(AdvancedSearchControllerTests)
#include "test_advanced_search_controller.moc"
