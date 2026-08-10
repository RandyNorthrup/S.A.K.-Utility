// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_controller.cpp
/// @brief Implements orchestration of search operations, history, and preferences

#include "sak/advanced_search_controller.h"

#include "sak/advanced_search_worker.h"
#include "sak/config_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QMetaType>
#include <QtGlobal>

#include <algorithm>
#include <limits>

namespace sak {

namespace {

constexpr int kWorkerStopTimeoutMs = kTimeoutProcessShortMs;
constexpr int kSearchStatusMessageMs = kTimerStatusDefaultMs;
constexpr int kDefaultPreviewFileSizeMb = 10;
constexpr int kDefaultSearchFileSizeMb = 50;
constexpr int kDefaultCacheSize = 50;
constexpr int kDefaultContextLines = 2;
constexpr int kMinimumPreferenceValue = 1;
constexpr int kMaximumPreviewFileSizeMb = 500;
constexpr int kMaximumSearchFileSizeMb = 1000;
constexpr int kMaximumCacheSize = 1000;
constexpr int kMaximumContextLines = 10;
constexpr int kMaxHistoryEntryLength = 2048;

}  // namespace

// -- Construction / Destruction ----------------------------------------------

AdvancedSearchController::AdvancedSearchController(QObject* parent)
    : QObject(parent), m_pattern_library(std::make_unique<RegexPatternLibrary>(nullptr)) {
    qRegisterMetaType<QVector<sak::SearchMatch>>("QVector<sak::SearchMatch>");
    loadPreferences();
    logInfo("AdvancedSearchController initialized");
}

AdvancedSearchController::~AdvancedSearchController() {
    cleanupWorker();
    savePreferences();
    logInfo("AdvancedSearchController destroyed");
}

// -- State Management --------------------------------------------------------

AdvancedSearchController::State AdvancedSearchController::currentState() const {
    return m_state;
}

void AdvancedSearchController::setState(State newState) {
    if (m_state != newState) {
        m_state = newState;
        Q_EMIT stateChanged(newState);
    }
}

// -- Search Operations -------------------------------------------------------

void AdvancedSearchController::startSearch(const SearchConfig& config) {
    if (m_state == State::Searching) {
        logWarning("AdvancedSearchController: search already in progress, cancelling first");
        cancelSearch();
    }

    cleanupWorker();

    m_total_matches = 0;
    m_total_files = 0;

    // Apply preferences to config
    SearchConfig effectiveConfig = config;
    if (m_preferences.max_results > 0 && effectiveConfig.max_results == 0) {
        effectiveConfig.max_results = m_preferences.max_results;
    }
    if (m_preferences.max_search_file_size_mb > 0) {
        effectiveConfig.max_file_size = static_cast<qint64>(m_preferences.max_search_file_size_mb) *
                                        kBytesPerMB;
    }

    m_worker = std::make_unique<AdvancedSearchWorker>(effectiveConfig);

    // Connect worker signals, tagged with the generation of this search so that
    // any queued signal from a superseded worker is dropped rather than allowed
    // to corrupt the replacement search's state.
    connectWorkerSignals(m_worker.get(), ++m_worker_generation);

    // Add to history
    addToHistory(config.pattern);

    setState(State::Searching);
    Q_EMIT searchStarted(config.pattern);

    m_worker->start();
}

void AdvancedSearchController::cancelSearch() {
    if (m_worker && m_state == State::Searching) {
        logInfo("AdvancedSearchController: cancelling search");
        m_worker->requestStop();
        setState(State::Cancelled);
    }
}

void AdvancedSearchController::cleanupWorker() {
    if (m_worker) {
        // Bump the generation so any already-queued signals from this worker are
        // ignored by their guards, then disconnect to stop further emissions.
        ++m_worker_generation;
        m_worker->disconnect(this);
        if (m_worker->isRunning()) {
            m_worker->requestStop();
            if (!m_worker->wait(kWorkerStopTimeoutMs)) {
                // Fail closed: never reset() a still-running worker -- destroying a running
                // QThread aborts the process. Block until the cooperative stop is honored. The
                // worker checks its stop flag between files, so this returns once the in-flight
                // file finishes rather than proceeding to tear down a live thread.
                logError(
                    "AdvancedSearchController: worker did not stop within 5s; "
                    "waiting for clean teardown before reset");
                // SAK-ALLOW-BLOCKING: destroying a still-running QThread aborts the whole
                // process, so reset() below MUST NOT run until the worker thread has actually
                // exited. requestStop() was already sent and the worker honors it between
                // files, so this unbounded wait resolves once the in-flight file finishes; it
                // is a mandatory teardown barrier, not an event-loop stall.
                m_worker->wait();
            }
        }
        m_worker.reset();
    }
}

void AdvancedSearchController::connectWorkerSignals(AdvancedSearchWorker* worker,
                                                    quint64 generation) {
    const auto stale = [this, generation] {
        return generation != m_worker_generation;
    };

    connect(worker, &WorkerBase::started, this, [this, stale] {
        if (stale()) {
            return;
        }
        onWorkerStarted();
    });
    connect(worker, &WorkerBase::finished, this, [this, stale] {
        if (stale()) {
            return;
        }
        onWorkerFinished();
    });
    connect(worker, &WorkerBase::failed, this, [this, stale](int code, const QString& message) {
        if (stale()) {
            return;
        }
        onWorkerFailed(code, message);
    });
    connect(worker, &WorkerBase::cancelled, this, [this, stale] {
        if (stale()) {
            return;
        }
        onWorkerCancelled();
    });
    connect(worker,
            &WorkerBase::progress,
            this,
            [this, stale](int current, int total, const QString& message) {
                if (stale()) {
                    return;
                }
                onWorkerProgress(current, total, message);
            });
    connect(worker,
            &AdvancedSearchWorker::resultsReady,
            this,
            [this, stale](QVector<sak::SearchMatch> matches) {
                if (stale()) {
                    return;
                }
                onResultsReady(std::move(matches));
            });
    connect(worker,
            &AdvancedSearchWorker::fileSearched,
            this,
            [this, stale](const QString& filePath, int matchCount) {
                if (stale()) {
                    return;
                }
                onFileSearched(filePath, matchCount);
            });
}

// -- Worker Signal Handlers --------------------------------------------------

void AdvancedSearchController::onWorkerStarted() {
    Q_EMIT statusMessage(tr("Search started..."), 0);
}

void AdvancedSearchController::onWorkerFinished() {
    if (!m_worker) {
        // Unreachable in practice: finished() is only connected to the live worker
        // and cleanupWorker disconnects before resetting it. If it ever happens we
        // have no completeness information, and "cannot tell" must never be
        // announced as complete -- fail closed and say so.
        logError("AdvancedSearchController: finished with no worker; reporting INCOMPLETE");
    }
    // The worker records every file/directory it could not fully search (and any
    // result cap it hit). The completeness travels WITH the counts so no consumer
    // can announce "complete" over a partial run; the status bar would otherwise
    // contradict the panel's own log, and it would also overwrite the worker's own
    // INCOMPLETE progress message, which lands first.
    const bool complete = m_worker && !m_worker->scanIncomplete();
    Q_EMIT searchFinished(m_total_matches, m_total_files, complete);
    if (!complete) {
        if (m_worker) {
            logWarning("AdvancedSearchController: search incomplete -- {}",
                       m_worker->incompleteReasons().join(QStringLiteral("; ")).toStdString());
        }
        Q_EMIT statusMessage(tr("Search INCOMPLETE: %1 matches in %2 files; some files could not "
                                "be searched, results may be missing matches")
                                 .arg(m_total_matches)
                                 .arg(m_total_files),
                             kSearchStatusMessageMs);
        setState(State::Idle);
        return;
    }
    Q_EMIT statusMessage(
        tr("Search complete: %1 matches in %2 files").arg(m_total_matches).arg(m_total_files),
        kSearchStatusMessageMs);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerFailed(int errorCode, const QString& errorMessage) {
    logError("AdvancedSearchController: search failed ({}) -- {}",
             errorCode,
             errorMessage.toStdString());
    Q_EMIT searchFailed(errorMessage);
    Q_EMIT statusMessage(tr("Search failed: %1").arg(errorMessage), kSearchStatusMessageMs);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerCancelled() {
    Q_EMIT searchCancelled();
    Q_EMIT statusMessage(
        tr("Search cancelled: %1 matches found before cancellation").arg(m_total_matches),
        kSearchStatusMessageMs);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerProgress(int current, int total, const QString& message) {
    Q_EMIT progressUpdate(current, total);
    if (!message.isEmpty()) {
        Q_EMIT statusMessage(message, 0);
    }
}

void AdvancedSearchController::onResultsReady(QVector<sak::SearchMatch> matches) {
    // Saturate rather than let the qsizetype batch size narrow into (or overflow) the int
    // total; a wrong-but-huge total must never be announced as an authoritative count.
    const qint64 sum = static_cast<qint64>(m_total_matches) + matches.size();
    m_total_matches = static_cast<int>(std::min<qint64>(sum, std::numeric_limits<int>::max()));
    Q_EMIT resultsReceived(std::move(matches));
}

void AdvancedSearchController::onFileSearched(const QString& filePath, int matchCount) {
    // Saturate at INT_MAX: incrementing past it would be signed-overflow UB.
    if (m_total_files < std::numeric_limits<int>::max()) {
        ++m_total_files;
    }
    Q_EMIT fileSearched(filePath, matchCount);
}

// -- Search History ----------------------------------------------------------

void AdvancedSearchController::addToHistory(const QString& pattern) {
    if (pattern.trimmed().isEmpty()) {
        return;
    }

    // Remove duplicate if exists
    m_search_history.removeAll(pattern);

    // Insert at front
    m_search_history.prepend(pattern);

    // Trim to max size
    while (m_search_history.size() > kMaxHistorySize) {
        m_search_history.removeLast();
    }
}

void AdvancedSearchController::clearHistory() {
    m_search_history.clear();
}

QStringList AdvancedSearchController::searchHistory() const {
    return m_search_history;
}

// -- Preferences -------------------------------------------------------------

void AdvancedSearchController::setPreferences(const SearchPreferences& prefs) {
    m_preferences = prefs;
    // Fail closed: a caller must not be able to install out-of-range limits. A negative
    // max_results reads downstream as "unlimited"; an over-huge file-size ceiling defeats the
    // bound entirely. Clamp to the SAME ranges loadPreferences() enforces so the in-session
    // preferences can never be looser than what a reload would produce.
    m_preferences.max_results = std::max(0, m_preferences.max_results);
    m_preferences.max_preview_file_size_mb = std::clamp(m_preferences.max_preview_file_size_mb,
                                                        kMinimumPreferenceValue,
                                                        kMaximumPreviewFileSizeMb);
    m_preferences.max_search_file_size_mb = std::clamp(m_preferences.max_search_file_size_mb,
                                                       kMinimumPreferenceValue,
                                                       kMaximumSearchFileSizeMb);
    m_preferences.max_cache_size =
        std::clamp(m_preferences.max_cache_size, kMinimumPreferenceValue, kMaximumCacheSize);
    m_preferences.context_lines = std::clamp(m_preferences.context_lines, 0, kMaximumContextLines);
    savePreferences();
}

const SearchPreferences& AdvancedSearchController::preferences() const {
    return m_preferences;
}

void AdvancedSearchController::loadPreferences() {
    auto& cfg = ConfigManager::instance();

    m_preferences.max_results = std::max(0, cfg.getValue("advsearch/max_results", 0).toInt());
    m_preferences.max_preview_file_size_mb = std::clamp(
        cfg.getValue("advsearch/max_preview_file_size_mb", kDefaultPreviewFileSizeMb).toInt(),
        kMinimumPreferenceValue,
        kMaximumPreviewFileSizeMb);
    m_preferences.max_search_file_size_mb = std::clamp(
        cfg.getValue("advsearch/max_search_file_size_mb", kDefaultSearchFileSizeMb).toInt(),
        kMinimumPreferenceValue,
        kMaximumSearchFileSizeMb);
    m_preferences.max_cache_size =
        std::clamp(cfg.getValue("advsearch/cache_size", kDefaultCacheSize).toInt(),
                   kMinimumPreferenceValue,
                   kMaximumCacheSize);
    m_preferences.context_lines =
        std::clamp(cfg.getValue("advsearch/context_lines", kDefaultContextLines).toInt(),
                   0,
                   kMaximumContextLines);

    // Load search history, enforcing the SAME invariants addToHistory() maintains. The config
    // store is attacker-writable, so it must not be able to smuggle in an unbounded list or
    // over-long entries: drop blanks and duplicates, bound each entry's length, and cap the
    // count at kMaxHistorySize.
    const QStringList history =
        cfg.getValue("advsearch/search_history", QStringList()).toStringList();
    QStringList sanitized;
    for (const QString& entry : history) {
        if (entry.trimmed().isEmpty() || entry.size() > kMaxHistoryEntryLength) {
            continue;
        }
        if (sanitized.contains(entry)) {
            continue;
        }
        sanitized.append(entry);
        if (sanitized.size() >= kMaxHistorySize) {
            break;
        }
    }
    m_search_history = sanitized;
}

void AdvancedSearchController::savePreferences() {
    auto& cfg = ConfigManager::instance();

    cfg.setValue("advsearch/max_results", m_preferences.max_results);
    cfg.setValue("advsearch/max_preview_file_size_mb", m_preferences.max_preview_file_size_mb);
    cfg.setValue("advsearch/max_search_file_size_mb", m_preferences.max_search_file_size_mb);
    cfg.setValue("advsearch/cache_size", m_preferences.max_cache_size);
    cfg.setValue("advsearch/context_lines", m_preferences.context_lines);
    cfg.setValue("advsearch/search_history", m_search_history);

    cfg.sync();
}

RegexPatternLibrary* AdvancedSearchController::patternLibrary() const {
    return m_pattern_library.get();
}

}  // namespace sak
