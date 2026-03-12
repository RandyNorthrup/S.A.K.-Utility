// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_controller.h
/// @brief Orchestrates search operations, manages state, and persists settings
///
/// Acts as the bridge between the AdvancedSearchPanel UI and the
/// AdvancedSearchWorker background thread. Manages search history,
/// user preferences, and the regex pattern library.

#pragma once

#include "sak/advanced_search_types.h"
#include "sak/regex_pattern_library.h"

#include <QObject>
#include <QStringList>

#include <memory>
#include <type_traits>

namespace sak {

class AdvancedSearchWorker;

/// @brief Controller orchestrating advanced search operations
///
/// Manages the lifecycle of search workers, maintains search history,
/// persists preferences, and owns the regex pattern library.
class AdvancedSearchController : public QObject {
    Q_OBJECT

public:
    /// @brief Controller state machine
    enum class State {
        Idle,
        Searching,
        Cancelled
    };

    explicit AdvancedSearchController(QObject* parent = nullptr);
    ~AdvancedSearchController() override;

    // Disable copy/move
    AdvancedSearchController(const AdvancedSearchController&) = delete;
    AdvancedSearchController& operator=(const AdvancedSearchController&) = delete;
    AdvancedSearchController(AdvancedSearchController&&) = delete;
    AdvancedSearchController& operator=(AdvancedSearchController&&) = delete;

    /// @brief Start a new search with the given configuration
    void startSearch(const SearchConfig& config);

    /// @brief Cancel the current search
    void cancelSearch();

    /// @brief Get current controller state
    [[nodiscard]] State currentState() const;

    // -- Search History --

    /// @brief Add a pattern to search history
    void addToHistory(const QString& pattern);

    /// @brief Clear all search history
    void clearHistory();

    /// @brief Get the search history list
    [[nodiscard]] QStringList searchHistory() const;

    // -- Preferences --

    /// @brief Set search preferences
    void setPreferences(const SearchPreferences& prefs);

    /// @brief Get current search preferences
    [[nodiscard]] SearchPreferences preferences() const;

    /// @brief Load preferences from persistent storage
    void loadPreferences();

    /// @brief Save preferences to persistent storage
    void savePreferences();

    // -- Regex Pattern Library --

    /// @brief Access the regex pattern library
    [[nodiscard]] RegexPatternLibrary* patternLibrary() const;

Q_SIGNALS:
    /// @brief Emitted when controller state changes
    void stateChanged(sak::AdvancedSearchController::State newState);

    /// @brief Emitted when search starts
    void searchStarted(const QString& pattern);

    /// @brief Emitted with batch search results
    void resultsReceived(QVector<sak::SearchMatch> matches);

    /// @brief Emitted per-file as search progresses
    void fileSearched(const QString& filePath, int matchCount);

    /// @brief Emitted when search completes successfully
    void searchFinished(int totalMatches, int totalFiles);

    /// @brief Emitted when search fails
    void searchFailed(const QString& error);

    /// @brief Emitted when search is cancelled
    void searchCancelled();

    /// @brief Status message for the UI status bar
    void statusMessage(const QString& message, int timeout);

    /// @brief Progress update for the UI progress bar
    void progressUpdate(int current, int maximum);

private Q_SLOTS:
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int errorCode, const QString& errorMessage);
    void onWorkerCancelled();
    void onWorkerProgress(int current, int total, const QString& message);
    void onResultsReady(QVector<sak::SearchMatch> matches);
    void onFileSearched(const QString& filePath, int matchCount);

private:
    /// @brief Transition to a new state
    void setState(State newState);

    /// @brief Clean up the current worker
    void cleanupWorker();

    State m_state = State::Idle;
    std::unique_ptr<AdvancedSearchWorker> m_worker;
    std::unique_ptr<RegexPatternLibrary> m_pattern_library;

    QStringList m_search_history;
    SearchPreferences m_preferences;

    int m_total_matches = 0;
    int m_total_files = 0;

    static constexpr int kMaxHistorySize = 50;
};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// AdvancedSearchController must inherit QObject.
static_assert(std::is_base_of_v<QObject, AdvancedSearchController>,
              "AdvancedSearchController must inherit QObject.");

/// AdvancedSearchController must not be copyable.
static_assert(!std::is_copy_constructible_v<AdvancedSearchController>,
              "AdvancedSearchController must not be copy-constructible.");

/// AdvancedSearchController must not be movable.
static_assert(!std::is_move_constructible_v<AdvancedSearchController>,
              "AdvancedSearchController must not be move-constructible.");

/// AdvancedSearchController must not be copyable.
static_assert(!std::is_copy_constructible_v<AdvancedSearchController>,
              "AdvancedSearchController must not be copy-constructible.");

}  // namespace sak
