// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_worker.h
/// @brief Worker thread for directory-recursive file content searching
///
/// Extends WorkerBase to perform text, regex, metadata, archive, and binary
/// searches on a background thread with cancellation and progress reporting.

#pragma once

#include "sak/advanced_search_types.h"
#include "sak/worker_base.h"

#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QVector>

#include <expected>
#include <type_traits>

namespace sak {

/// @brief Worker thread that executes file content searches
///
/// Runs directory-recursive search on a background thread using WorkerBase.
/// Supports text content, regex, image metadata, file metadata, archive, and
/// binary/hex search modes. Results are emitted in batches for UI responsiveness.
class AdvancedSearchWorker : public WorkerBase {
    Q_OBJECT

public:
    /// @brief Construct search worker with configuration
    /// @param config Search configuration
    /// @param parent Optional parent QObject
    explicit AdvancedSearchWorker(SearchConfig config, QObject* parent = nullptr);

    ~AdvancedSearchWorker() override = default;

    // Disable copy/move (inherited from WorkerBase)
    AdvancedSearchWorker(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker& operator=(const AdvancedSearchWorker&) = delete;
    AdvancedSearchWorker(AdvancedSearchWorker&&) = delete;
    AdvancedSearchWorker& operator=(AdvancedSearchWorker&&) = delete;

Q_SIGNALS:
    /// @brief Emitted with accumulated results (batch updates for performance)
    void resultsReady(QVector<sak::SearchMatch> matches);

    /// @brief Emitted per-file as search progresses
    void fileSearched(const QString& filePath, int matchCount);

protected:
    /// @brief Main search execution -- runs on worker thread
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /// @brief Check if path should be excluded from search
    [[nodiscard]] bool isExcluded(const QString& path) const;

    /// @brief Search a single file for the pattern (dispatches by type)
    [[nodiscard]] QVector<SearchMatch> searchFile(const QString& filePath,
                                                  const QRegularExpression& regex);

    /// @brief Search file text content line-by-line with context extraction
    [[nodiscard]] QVector<SearchMatch> searchTextContent(const QString& filePath,
                                                         const QRegularExpression& regex);

    /// @brief Search image metadata (EXIF, GPS, PNG) for the pattern
    [[nodiscard]] QVector<SearchMatch> searchImageMetadata(const QString& filePath,
                                                           const QRegularExpression& regex);

    /// @brief Search file metadata (PDF, Office, audio, video) for the pattern
    [[nodiscard]] QVector<SearchMatch> searchFileMetadata(const QString& filePath,
                                                          const QRegularExpression& regex);

    /// @brief Search inside archive files (ZIP, EPUB)
    [[nodiscard]] QVector<SearchMatch> searchArchive(const QString& filePath,
                                                     const QRegularExpression& regex);

    /// @brief Search binary file for hex/text patterns
    [[nodiscard]] QVector<SearchMatch> searchBinary(const QString& filePath,
                                                    const QRegularExpression& regex);

    /// @brief Check if path is a UNC network path
    [[nodiscard]] static bool isNetworkPath(const QString& path);

    /// @brief Check network path accessibility with timeout
    [[nodiscard]] bool checkNetworkPathAccessible(const QString& path) const;

    /// @brief Compile the search regex from config
    [[nodiscard]] std::expected<QRegularExpression, QString> compileRegex() const;

    /// @brief Check if a file extension matches configured filters
    [[nodiscard]] bool matchesExtensionFilter(const QString& filePath) const;

    /// @brief Prepare regex + exclusion patterns before search
    [[nodiscard]] std::expected<QRegularExpression, sak::error_code> prepareSearchConfig();

    /// @brief Run directory-recursive search loop
    void runDirectorySearch(const QRegularExpression& regex, int& total_matches, int& total_files);

    /// @brief Process a single file during search, returns false if max results reached
    bool processSearchFile(const QString& file_path,
                           const QRegularExpression& regex,
                           QVector<SearchMatch>& batch_matches,
                           int& total_matches,
                           int& total_files);

    /// @brief Check if a file should be skipped (excluded, wrong extension, too large)
    [[nodiscard]] bool shouldSkipFile(const QString& file_path) const;

    /// @brief Build a SearchMatch with context lines around a regex hit
    [[nodiscard]] SearchMatch buildContextMatch(const QString& file_path,
                                                const QStringList& lines,
                                                int line_index,
                                                const QRegularExpressionMatch& regex_match) const;

    SearchConfig m_config;

    /// @brief Compiled exclusion patterns (built once in execute())
    QVector<QRegularExpression> m_compiled_excludes;

    /// @brief Batch size for result emission
    static constexpr int kBatchSize = 50;
};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// AdvancedSearchWorker must inherit WorkerBase.
static_assert(std::is_base_of_v<WorkerBase, AdvancedSearchWorker>,
              "AdvancedSearchWorker must inherit WorkerBase.");

/// AdvancedSearchWorker must not be copyable.
static_assert(!std::is_copy_constructible_v<AdvancedSearchWorker>,
              "AdvancedSearchWorker must not be copy-constructible.");

/// AdvancedSearchWorker must not be movable.
static_assert(!std::is_move_constructible_v<AdvancedSearchWorker>,
              "AdvancedSearchWorker must not be move-constructible.");

}  // namespace sak
