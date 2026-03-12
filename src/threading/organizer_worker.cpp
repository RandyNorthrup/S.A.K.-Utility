// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file organizer_worker.cpp
/// @brief Implements the background worker thread for file organization operations

#include "sak/organizer_worker.h"

#include "sak/input_validator.h"
#include "sak/logger.h"

#include <QFile>
#include <QtGlobal>
#include <QVector>
#include <algorithm>

OrganizerWorker::OrganizerWorker(const Config& config, QObject* parent)
    : WorkerBase(parent), m_config(config) {
    Q_ASSERT_X(!config.target_directory.isEmpty(),
               "OrganizerWorker",
               "target_directory must not be empty");
}

auto OrganizerWorker::execute() -> std::expected<void, sak::error_code> {
    sak::logInfo("Starting directory organization: {}", m_config.target_directory.toStdString());

    // Validate target directory path
    sak::path_validation_config dir_cfg;
    dir_cfg.must_exist = true;
    dir_cfg.must_be_directory = true;
    dir_cfg.check_read_permission = true;
    dir_cfg.check_write_permission = true;
    auto dir_result = sak::input_validator::validatePath(
        std::filesystem::path(m_config.target_directory.toStdString()), dir_cfg);
    if (!dir_result) {
        sak::logError("Target directory validation failed: {}", dir_result.error_message);
        return std::unexpected(sak::error_code::invalid_path);
    }

    // Scan directory for files
    auto files_result = scanDirectory();
    if (!files_result) {
        return std::unexpected(files_result.error());
    }

    const auto& files = files_result.value();
    sak::logInfo("Found {} files to organize", files.size());

    // Plan moves for all files
    m_planned_operations.clear();
    const size_t file_count = files.size();
    m_planned_operations.reserve(file_count);

    for (size_t i = 0; i < file_count; ++i) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        const auto& file = files[i];
        auto category = categorizeFile(file);

        if (!category.isEmpty()) {
            auto operation = planMove(file, category);
            m_planned_operations.push_back(operation);
        }

        Q_EMIT fileProgress(static_cast<int>(i + 1),
                            static_cast<int>(files.size()),
                            QString::fromStdString(file.string()));
    }

    sak::logInfo("Planned {} move operations", m_planned_operations.size());

    // If preview mode, emit results and exit
    if (m_config.preview_mode) {
        QString summary = generatePreviewSummary();
        Q_EMIT previewResults(summary, static_cast<int>(m_planned_operations.size()));
        sak::logInfo("Preview mode complete");
        return {};
    }

    // Execute moves
    return executePlannedMoves();
}

auto OrganizerWorker::executePlannedMoves() -> std::expected<void, sak::error_code> {
    const size_t op_count = m_planned_operations.size();
    for (size_t i = 0; i < op_count; ++i) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        const auto& operation = m_planned_operations[i];
        auto result = executeMove(operation);
        if (!result) {
            sak::logError("Failed to move file: {}", operation.source.string());
            return result;
        }

        Q_EMIT fileProgress(static_cast<int>(i + 1),
                            static_cast<int>(m_planned_operations.size()),
                            QString::fromStdString(operation.source.string()));
    }

    sak::logInfo("Directory organization complete");
    return {};
}

auto OrganizerWorker::scanDirectory()
    -> std::expected<std::vector<std::filesystem::path>, sak::error_code> {
    std::vector<std::filesystem::path> files;
    std::filesystem::path target_path(m_config.target_directory.toStdString());

    if (!std::filesystem::exists(target_path)) {
        sak::logError("Target directory does not exist: {}", target_path.string());
        return std::unexpected(sak::error_code::file_not_found);
    }

    if (!std::filesystem::is_directory(target_path)) {
        sak::logError("Target path is not a directory: {}", target_path.string());
        return std::unexpected(sak::error_code::invalid_path);
    }

    // Only scan immediate files, not subdirectories
    // Reserve capacity to reduce allocations
    files.reserve(256);  // Reasonable default, will grow if needed

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(target_path, ec)) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        if (entry.is_regular_file()) {
            files.push_back(entry.path());
        }
    }

    if (ec) {
        sak::logError("Filesystem error during scan: {}", ec.message());
        return std::unexpected(sak::error_code::scan_failed);
    }

    return files;
}

auto OrganizerWorker::categorizeFile(const std::filesystem::path& file_path) -> QString {
    auto extension = file_path.extension().string();
    if (extension.empty()) {
        return QString();
    }

    // Remove leading dot and convert to lowercase
    if (extension[0] == '.') {
        extension = extension.substr(1);
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char c) {
        return std::tolower(c);
    });

    QString ext_lower = QString::fromStdString(extension);

    // Find matching category
    for (auto it = m_config.category_mapping.begin(); it != m_config.category_mapping.end(); ++it) {
        const auto& extensions = it.value();
        if (extensions.contains(ext_lower, Qt::CaseInsensitive)) {
            return it.key();
        }
    }

    return QString();
}

auto OrganizerWorker::planMove(const std::filesystem::path& file_path, const QString& category)
    -> MoveOperation {
    MoveOperation op;
    op.source = file_path;
    op.category = category;

    // Build destination path
    std::filesystem::path target_dir(m_config.target_directory.toStdString());
    std::filesystem::path category_dir = target_dir / category.toStdString();
    op.destination = category_dir / file_path.filename();

    // Check for collision
    op.would_overwrite = std::filesystem::exists(op.destination);

    return op;
}

auto OrganizerWorker::executeMove(const MoveOperation& operation)
    -> std::expected<void, sak::error_code> {
    try {
        // Create category directory if needed (create_directories is a no-op if it exists)
        if (m_config.create_subdirectories) {
            std::filesystem::create_directories(operation.destination.parent_path());
        }

        // Handle collision
        std::filesystem::path final_dest = operation.destination;
        if (operation.would_overwrite) {
            final_dest = handleCollision(operation);
        }

        // Move file
        std::filesystem::rename(operation.source, final_dest);
        sak::logInfo("Moved: {} -> {}", operation.source.string(), final_dest.string());

        return {};

    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Failed to move file: {}", e.what());
        return std::unexpected(sak::error_code::write_error);
    }
}

auto OrganizerWorker::handleCollision(const MoveOperation& operation) -> std::filesystem::path {
    if (m_config.collision_strategy == "skip") {
        return operation.source;  // Don't move
    }

    if (m_config.collision_strategy == "overwrite") {
        return operation.destination;  // Use original destination
    }

    // Default: rename with counter
    auto dest = operation.destination;
    auto stem = dest.stem();
    auto extension = dest.extension();
    auto parent = dest.parent_path();

    int counter = 1;
    while (std::filesystem::exists(dest)) {
        auto new_filename = stem.string() + "_" + std::to_string(counter) + extension.string();
        dest = parent / new_filename;
        ++counter;
    }

    return dest;
}

auto OrganizerWorker::generatePreviewSummary() -> QString {
    QString summary;
    summary += "Preview Results:\n\n";
    summary += QString("Total files to organize: %1\n\n").arg(m_planned_operations.size());

    QMap<QString, int> category_counts;
    for (const auto& op : m_planned_operations) {
        category_counts[op.category]++;
    }

    summary += "Files by category:\n";
    for (auto it = category_counts.begin(); it != category_counts.end(); ++it) {
        summary += QString("  %1: %2 files\n").arg(it.key()).arg(it.value());
    }

    const int collisions = static_cast<int>(std::count_if(
        m_planned_operations.begin(), m_planned_operations.end(),
        [](const auto& op) { return op.would_overwrite; }));

    if (collisions > 0) {
        summary += QString("\nWarning: %1 file(s) would have collisions\n").arg(collisions);
    }

    return summary;
}
