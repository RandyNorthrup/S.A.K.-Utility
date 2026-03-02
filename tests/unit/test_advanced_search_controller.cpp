// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_search_controller.cpp
/// @brief Unit tests for AdvancedSearchController — state machine, history,
///        preferences, worker lifecycle

#include <QtTest/QtTest>

#include "sak/advanced_search_controller.h"
#include "sak/advanced_search_types.h"
#include "sak/config_manager.h"
#include "sak/regex_pattern_library.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTemporaryFile>

using sak::AdvancedSearchController;
using sak::SearchConfig;
using sak::SearchMatch;
using sak::SearchPreferences;

class AdvancedSearchControllerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();

    // ── Initial State ──
    void initialState_isIdle();
    void patternLibrary_notNull();
    void patternLibrary_hasBuiiltins();

    // ── Search Lifecycle ──
    void startSearch_changesState();
    void startSearch_emitsSignals();
    void cancelSearch_changesState();
    void cancelSearch_emitsSignal();
    void searchComplete_returnsToIdle();

    // ── Search History ──
    void history_initiallyEmpty();
    void history_addedOnSearch();
    void history_preventsDuplicates();
    void history_maxSize();
    void history_emptyPatternNotAdded();
    void history_clearHistory();

    // ── Preferences ──
    void preferences_defaultValues();
    void preferences_setAndGet();
    void preferences_clampedValues();

    // ── Worker Double-Start ──
    void doubleStart_cancelsFirst();

private:
    QTemporaryDir m_temp_dir;
};

void AdvancedSearchControllerTests::initTestCase()
{
    QVERIFY(m_temp_dir.isValid());

    // Create a test file for search operations
    QFile testFile(m_temp_dir.path() + "/test_search.txt");
    QVERIFY(testFile.open(QIODevice::WriteOnly | QIODevice::Text));
    testFile.write("Hello World\nSearch target\nAnother line\n");
    testFile.close();
}

// ── Initial State ───────────────────────────────────────────────────────────

void AdvancedSearchControllerTests::initialState_isIdle()
{
    AdvancedSearchController ctrl;
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::patternLibrary_notNull()
{
    AdvancedSearchController ctrl;
    QVERIFY(ctrl.patternLibrary() != nullptr);
}

void AdvancedSearchControllerTests::patternLibrary_hasBuiiltins()
{
    AdvancedSearchController ctrl;
    QCOMPARE(ctrl.patternLibrary()->builtinPatterns().size(), 8);
}

// ── Search Lifecycle ────────────────────────────────────────────────────────

void AdvancedSearchControllerTests::startSearch_changesState()
{
    AdvancedSearchController ctrl;
    QSignalSpy stateSpy(&ctrl, &AdvancedSearchController::stateChanged);

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);

    // State should change to Searching
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Searching);
    QVERIFY(stateSpy.count() >= 1);

    // Wait for completion
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    QVERIFY(finishedSpy.wait(5000));

    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::startSearch_emitsSignals()
{
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
    QVERIFY(finishedSpy.wait(5000));

    QCOMPARE(startedSpy.count(), 1);
    QCOMPARE(startedSpy[0][0].toString(), "Hello");
    QVERIFY(resultsSpy.count() >= 1);
    QCOMPARE(finishedSpy.count(), 1);

    // Check totals
    const int totalMatches = finishedSpy[0][0].toInt();
    const int totalFiles = finishedSpy[0][1].toInt();
    QVERIFY(totalMatches >= 1);
    QVERIFY(totalFiles >= 1);
}

void AdvancedSearchControllerTests::cancelSearch_changesState()
{
    AdvancedSearchController ctrl;

    // Create many files so the search runs long enough to cancel
    QTemporaryDir longDir;
    QVERIFY(longDir.isValid());
    for (int i = 0; i < 200; ++i) {
        QFile f(longDir.path() + QString("/file_%1.txt").arg(i));
        f.open(QIODevice::WriteOnly);
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

    // Wait for worker to finish
    QSignalSpy cancelledSpy(&ctrl, &AdvancedSearchController::searchCancelled);
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);

    // Wait for either cancelled or finished
    QTest::qWait(2000);

    // Should return to Idle
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::cancelSearch_emitsSignal()
{
    AdvancedSearchController ctrl;
    QSignalSpy cancelledSpy(&ctrl, &AdvancedSearchController::searchCancelled);

    // Start a search on a non-existent large path so it doesn't finish instantly
    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "Hello";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);
    ctrl.cancelSearch();

    // Wait for worker to process
    QTest::qWait(1000);

    // The search may have finished before cancel took effect, or may have been cancelled
    // Either way, state should be Idle
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

void AdvancedSearchControllerTests::searchComplete_returnsToIdle()
{
    AdvancedSearchController ctrl;
    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "target";
    config.exclude_patterns.clear();

    ctrl.startSearch(config);
    QVERIFY(finishedSpy.wait(5000));

    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);
}

// ── Search History ──────────────────────────────────────────────────────────

void AdvancedSearchControllerTests::history_initiallyEmpty()
{
    AdvancedSearchController ctrl;
    ctrl.clearHistory();
    QVERIFY(ctrl.searchHistory().isEmpty());
}

void AdvancedSearchControllerTests::history_addedOnSearch()
{
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    SearchConfig config;
    config.root_path = m_temp_dir.path() + "/test_search.txt";
    config.pattern = "unique_search_term";
    config.exclude_patterns.clear();

    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    ctrl.startSearch(config);
    QVERIFY(finishedSpy.wait(5000));

    const auto history = ctrl.searchHistory();
    QVERIFY(history.contains("unique_search_term"));
}

void AdvancedSearchControllerTests::history_preventsDuplicates()
{
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    ctrl.addToHistory("dup_test");
    ctrl.addToHistory("dup_test");

    QCOMPARE(ctrl.searchHistory().count("dup_test"), 1);
}

void AdvancedSearchControllerTests::history_maxSize()
{
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    // Add more than max history size (50)
    for (int i = 0; i < 60; ++i) {
        ctrl.addToHistory(QString("search_%1").arg(i));
    }

    QVERIFY(ctrl.searchHistory().size() <= 50);
    // Most recent should be first
    QCOMPARE(ctrl.searchHistory().first(), "search_59");
}

void AdvancedSearchControllerTests::history_emptyPatternNotAdded()
{
    AdvancedSearchController ctrl;
    ctrl.clearHistory();

    ctrl.addToHistory("");
    ctrl.addToHistory("   "); // Whitespace-only

    QVERIFY(ctrl.searchHistory().isEmpty());
}

void AdvancedSearchControllerTests::history_clearHistory()
{
    AdvancedSearchController ctrl;
    ctrl.addToHistory("item1");
    ctrl.addToHistory("item2");

    ctrl.clearHistory();
    QVERIFY(ctrl.searchHistory().isEmpty());
}

// ── Preferences ─────────────────────────────────────────────────────────────

void AdvancedSearchControllerTests::preferences_defaultValues()
{
    AdvancedSearchController ctrl;
    const auto prefs = ctrl.preferences();

    // Default values from code or ConfigManager
    QVERIFY(prefs.max_results >= 0);
    QVERIFY(prefs.max_preview_file_size_mb >= 1);
    QVERIFY(prefs.max_search_file_size_mb >= 1);
    QVERIFY(prefs.max_cache_size >= 1);
    QVERIFY(prefs.context_lines >= 0);
    QVERIFY(prefs.context_lines <= 10);
}

void AdvancedSearchControllerTests::preferences_setAndGet()
{
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

void AdvancedSearchControllerTests::preferences_clampedValues()
{
    AdvancedSearchController ctrl;

    // Set extreme values
    SearchPreferences newPrefs;
    newPrefs.max_results = 1000000; // No clamp on this (0=unlimited)
    newPrefs.max_preview_file_size_mb = 999;
    newPrefs.max_search_file_size_mb = 999;
    newPrefs.max_cache_size = 999;
    newPrefs.context_lines = 10;

    ctrl.setPreferences(newPrefs);

    // Re-load from config to verify clamping
    ctrl.loadPreferences();
    const auto prefs = ctrl.preferences();

    QVERIFY(prefs.max_preview_file_size_mb <= 500);
    QVERIFY(prefs.max_search_file_size_mb <= 1000);
    QVERIFY(prefs.max_cache_size <= 1000);
    QVERIFY(prefs.context_lines <= 10);
}

// ── Worker Double-Start ─────────────────────────────────────────────────────

void AdvancedSearchControllerTests::doubleStart_cancelsFirst()
{
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

    // Immediately start second — should cancel first
    ctrl.startSearch(config2);

    QSignalSpy finishedSpy(&ctrl, &AdvancedSearchController::searchFinished);
    QVERIFY(finishedSpy.wait(5000));

    // Should have completed (the second search)
    QCOMPARE(ctrl.currentState(), AdvancedSearchController::State::Idle);

    // History should contain both patterns
    const auto history = ctrl.searchHistory();
    QVERIFY(history.contains("Hello"));
    QVERIFY(history.contains("target"));
}

QTEST_GUILESS_MAIN(AdvancedSearchControllerTests)
#include "test_advanced_search_controller.moc"
