// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/worker_base.h"

#include <QMap>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <vector>

/**
 * @brief Worker thread for directory organization operations
 *
 * Organizes files by extension into categorized subdirectories.
 * Provides preview mode for dry-run testing and collision handling.
 *
 * Thread-Safety: All signals are emitted from worker thread and should
 * be connected with Qt::QueuedConnection.
 */
class OrganizerWorker : public WorkerBase {
    Q_OBJECT

public:
    /**
     * @brief File move operation result
     */
    struct MoveOperation {
        std::filesystem::path source;
        std::filesystem::path destination;
        QString category;
        bool would_overwrite{false};
    };

    /**
     * @brief Configuration for organization operation
     */
    struct Config {
        QString target_directory;                     ///< Directory to organize
        QMap<QString, QStringList> category_mapping;  ///< Category -> extensions
        bool preview_mode{false};                     ///< Dry run without moving
        bool create_subdirectories{true};             ///< Create category folders
        QString collision_strategy{"rename"};         ///< rename/skip/overwrite
    };

    /**
     * @brief Construct organizer worker
     * @param config Organization configuration
     * @param parent Parent QObject
     */
    explicit OrganizerWorker(const Config& config, QObject* parent = nullptr);

Q_SIGNALS:
    /**
     * @brief Emitted when file processing progresses
     * @param current_file Current file index
     * @param total_files Total number of files
     * @param current_file_path Path of current file
     */
    void fileProgress(int current_file, int total_files, const QString& current_file_path);

    /**
     * @brief Emitted in preview mode with summary of planned operations
     * @param summary Text summary of planned operations
     * @param operation_count Number of operations planned
     */
    void previewResults(const QString& summary, int operation_count);

protected:
    /**
     * @brief Execute organization workflow
     * @return Success or error code
     */
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    /**
     * @brief Scan directory and collect files to organize
     * @return File list or error
     */
    auto scanDirectory() -> std::expected<std::vector<std::filesystem::path>, sak::error_code>;

    /**
     * @brief Categorize file by extension
     * @param file_path File to categorize
     * @return Category name or empty if no match
     */
    auto categorizeFile(const std::filesystem::path& file_path) -> QString;

    /**
     * @brief Plan move operation for a file
     * @param file_path File to move
     * @param category Target category
     * @return Move operation details
     */
    auto planMove(const std::filesystem::path& file_path, const QString& category) -> MoveOperation;

    /**
     * @brief Execute a single file move
     * @param operation Move operation to execute
     * @return Success or error code
     */
    auto executeMove(const MoveOperation& operation) -> std::expected<void, sak::error_code>;

    /// @brief Execute all planned file move operations
    auto executePlannedMoves() -> std::expected<void, sak::error_code>;

    /**
     * @brief Handle file collision based on strategy
     * @param operation Move operation with collision
     * @return Modified destination path
     */
    auto handleCollision(const MoveOperation& operation) -> std::filesystem::path;

    /**
     * @brief Generate preview summary text
     * @return Summary string
     */
    auto generatePreviewSummary() -> QString;

    Config m_config;
    std::vector<MoveOperation> m_planned_operations;
};
