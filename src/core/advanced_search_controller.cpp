// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_controller.cpp
/// @brief Implements orchestration of search operations, history, and preferences

#include "sak/advanced_search_controller.h"

#include "sak/advanced_search_worker.h"
#include "sak/config_manager.h"
#include "sak/logger.h"

#include <QtGlobal>

#include <algorithm>

namespace sak {

// -- Construction / Destruction ----------------------------------------------

AdvancedSearchController::AdvancedSearchController(QObject* parent)
    : QObject(parent), m_pattern_library(std::make_unique<RegexPatternLibrary>(nullptr)) {
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
    Q_ASSERT(m_worker);
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
                                        1024 * 1024;
    }

    m_worker = std::make_unique<AdvancedSearchWorker>(effectiveConfig);

    // Connect worker signals
    connect(m_worker.get(), &WorkerBase::started, this, &AdvancedSearchController::onWorkerStarted);
    connect(
        m_worker.get(), &WorkerBase::finished, this, &AdvancedSearchController::onWorkerFinished);
    connect(m_worker.get(), &WorkerBase::failed, this, &AdvancedSearchController::onWorkerFailed);
    connect(
        m_worker.get(), &WorkerBase::cancelled, this, &AdvancedSearchController::onWorkerCancelled);
    connect(
        m_worker.get(), &WorkerBase::progress, this, &AdvancedSearchController::onWorkerProgress);
    connect(m_worker.get(),
            &AdvancedSearchWorker::resultsReady,
            this,
            &AdvancedSearchController::onResultsReady);
    connect(m_worker.get(),
            &AdvancedSearchWorker::fileSearched,
            this,
            &AdvancedSearchController::onFileSearched);

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
    Q_ASSERT(m_worker);
    if (m_worker) {
        if (m_worker->isRunning()) {
            m_worker->requestStop();
            if (!m_worker->wait(5000)) {
                logError("AdvancedSearchController: worker did not stop within 5s");
            }
        }
        m_worker.reset();
    }
}

// -- Worker Signal Handlers --------------------------------------------------

void AdvancedSearchController::onWorkerStarted() {
    Q_EMIT statusMessage(tr("Search started..."), 0);
}

void AdvancedSearchController::onWorkerFinished() {
    Q_EMIT searchFinished(m_total_matches, m_total_files);
    Q_EMIT statusMessage(
        tr("Search complete: %1 matches in %2 files").arg(m_total_matches).arg(m_total_files),
        5000);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerFailed(int errorCode, const QString& errorMessage) {
    logError("AdvancedSearchController: search failed ({}) -- {}",
             errorCode,
             errorMessage.toStdString());
    Q_EMIT searchFailed(errorMessage);
    Q_EMIT statusMessage(tr("Search failed: %1").arg(errorMessage), 5000);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerCancelled() {
    Q_EMIT searchCancelled();
    Q_EMIT statusMessage(
        tr("Search cancelled: %1 matches found before cancellation").arg(m_total_matches), 5000);
    setState(State::Idle);
}

void AdvancedSearchController::onWorkerProgress(int current, int total, const QString& message) {
    Q_EMIT progressUpdate(current, total);
    if (!message.isEmpty()) {
        Q_EMIT statusMessage(message, 0);
    }
}

void AdvancedSearchController::onResultsReady(QVector<sak::SearchMatch> matches) {
    m_total_matches += matches.size();
    Q_EMIT resultsReceived(std::move(matches));
}

void AdvancedSearchController::onFileSearched(const QString& filePath, int matchCount) {
    m_total_files++;
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
    savePreferences();
}

SearchPreferences AdvancedSearchController::preferences() const {
    return m_preferences;
}

void AdvancedSearchController::loadPreferences() {
    auto& cfg = ConfigManager::instance();

    m_preferences.max_results = std::max(0, cfg.getValue("advsearch/max_results", 0).toInt());
    m_preferences.max_preview_file_size_mb =
        std::clamp(cfg.getValue("advsearch/max_preview_file_size_mb", 10).toInt(), 1, 500);
    m_preferences.max_search_file_size_mb =
        std::clamp(cfg.getValue("advsearch/max_search_file_size_mb", 50).toInt(), 1, 1000);
    m_preferences.max_cache_size =
        std::clamp(cfg.getValue("advsearch/cache_size", 50).toInt(), 1, 1000);
    m_preferences.context_lines =
        std::clamp(cfg.getValue("advsearch/context_lines", 2).toInt(), 0, 10);

    // Load search history
    const QStringList history =
        cfg.getValue("advsearch/search_history", QStringList()).toStringList();
    m_search_history = history;
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
