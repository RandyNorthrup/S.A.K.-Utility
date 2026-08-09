// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file organizer_worker.cpp
/// @brief Implements the background worker thread for file organization operations

#include "sak/organizer_worker.h"

#include "sak/app_organizer_helpers.h"
#include "sak/input_validator.h"
#include "sak/logger.h"

#include <QFile>
#include <QtGlobal>
#include <QVector>

#include <algorithm>

namespace {
constexpr qsizetype kInitialFileReserveCount = 256;

// The three policies handleCollision() actually implements. Anything else is a malformed
// configuration: coercing it into the "rename" default would silently apply a policy the caller
// never asked for (an "overwite" typo would quietly rename instead of overwriting).
bool isKnownCollisionStrategy(const QString& strategy) {
    return strategy == QLatin1String("rename") || strategy == QLatin1String("skip") ||
           strategy == QLatin1String("overwrite");
}

// Fail closed on a configuration that cannot be applied as asked, BEFORE anything is scanned or
// moved. Each of these was previously coerced into a working default: an unknown collision
// strategy became "rename"; a category name became a path component that std::filesystem's
// operator/ resolves OUTSIDE the target (an absolute name REPLACES the target, ".." walks out of
// it, an empty one collapses the destination onto the source and renames the file in place); an
// empty mapping ran to an empty "success"; and a negative preview cap silently meant "unlimited".
// The AI organize/preview ops screen a model-supplied mapping the same way (app_organizer_helpers
// .h) -- this is the independent worker-side layer that covers every other caller too.
std::expected<void, sak::error_code> validateOrganizerConfig(
    const OrganizerWorker::Config& config) {
    if (config.category_mapping.isEmpty()) {
        sak::logError("Organizer refused: the category mapping is empty");
        return std::unexpected(sak::error_code::invalid_configuration);
    }
    for (auto it = config.category_mapping.begin(); it != config.category_mapping.end(); ++it) {
        if (it.key().isEmpty() || !sak::isSafeCategoryName(it.key())) {
            sak::logError("Organizer refused: category '{}' is not a valid subfolder name",
                          it.key().toStdString());
            return std::unexpected(sak::error_code::invalid_configuration);
        }
    }
    if (!isKnownCollisionStrategy(config.collision_strategy)) {
        sak::logError("Organizer refused: unknown collision strategy '{}'",
                      config.collision_strategy.toStdString());
        return std::unexpected(sak::error_code::invalid_configuration);
    }
    if (config.max_preview_files < 0) {
        sak::logError("Organizer refused: negative preview cap {}", config.max_preview_files);
        return std::unexpected(sak::error_code::invalid_configuration);
    }
    return {};
}

// Decide whether the target directory can serve THIS run. A preview is a pure dry run that writes
// NOTHING, so it must not require write access -- otherwise a read-only inspection fails on
// directories the user can fully read (notably folders carrying FILE_ATTRIBUTE_READONLY, which
// Windows sets on many ordinary user folders and does not actually enforce for directories). An
// apply still requires write permission.
std::expected<void, sak::error_code> validateTargetDirectory(
    const OrganizerWorker::Config& config) {
    sak::path_validation_config dir_cfg;
    dir_cfg.must_exist = true;
    dir_cfg.must_be_directory = true;
    dir_cfg.check_read_permission = true;
    dir_cfg.check_write_permission = !config.preview_mode;
    auto dir_result = sak::input_validator::validatePath(
        std::filesystem::path(config.target_directory.toStdString()), dir_cfg);
    if (!dir_result) {
        sak::logError("Target directory validation failed: {}", dir_result.error_message);
        return std::unexpected(sak::error_code::invalid_path);
    }
    return {};
}
}  // namespace

// An empty target_directory is not asserted here: execute() validates the path (must_exist +
// must_be_directory) and fails the run with invalid_path, which is the contract.
OrganizerWorker::OrganizerWorker(const Config& config, QObject* parent)
    : WorkerBase(parent), m_config(config) {}

auto OrganizerWorker::execute() -> std::expected<void, sak::error_code> {
    sak::logInfo("Starting directory organization: {}", m_config.target_directory.toStdString());
    m_moved_count = 0;
    m_plan_truncated = false;

    // Refuse a malformed configuration before touching the filesystem (see
    // validateOrganizerConfig): every field it screens used to fall back to a working default.
    if (const auto config_ok = validateOrganizerConfig(m_config); !config_ok) {
        return std::unexpected(config_ok.error());
    }

    // An apply needs a writable target; a preview deliberately does not.
    if (const auto dir_ok = validateTargetDirectory(m_config); !dir_ok) {
        return std::unexpected(dir_ok.error());
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
    files.reserve(kInitialFileReserveCount);

    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(target_path, ec)) {
        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        if (entry.is_regular_file()) {
            files.push_back(entry.path());
            // Preview mode bounds peak memory: a model-reachable dry run on a pathological
            // directory (hundreds of thousands of immediate files) must not spike RSS building the
            // file list plus one MoveOperation per file. Stop collecting at the cap and mark the
            // plan truncated so the reported count is an honest lower bound. An apply
            // (preview_mode=false, so max_preview_files unused) stays uncapped -- it must move
            // every matching file.
            if (m_config.preview_mode && m_config.max_preview_files > 0 &&
                files.size() >= static_cast<size_t>(m_config.max_preview_files)) {
                m_plan_truncated = true;
                sak::logInfo("Preview scan capped at {} files", m_config.max_preview_files);
                break;
            }
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

        // Handle collision. Re-check existence at EXECUTE time, not just the plan-time
        // would_overwrite flag: a file may have appeared at the destination between
        // planning and this move, and a plain std::filesystem::rename silently REPLACES
        // an existing destination (on Windows, MOVEFILE_REPLACE_EXISTING). Re-checking
        // here closes that TOCTOU so a "rename"/"skip" run can never clobber a file.
        std::filesystem::path final_dest = operation.destination;
        std::error_code exists_ec;
        if (std::filesystem::exists(operation.destination, exists_ec)) {
            final_dest = handleCollision(operation);
        }

        // Move file. A "skip" collision resolves final_dest back to the source, which
        // is a rename-to-self (no relocation) -- do not count it as moved.
        std::filesystem::rename(operation.source, final_dest);
        if (final_dest != operation.source) {
            ++m_moved_count;
        }
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

    const int collisions = static_cast<int>(
        std::count_if(m_planned_operations.begin(), m_planned_operations.end(), [](const auto& op) {
            return op.would_overwrite;
        }));

    if (collisions > 0) {
        summary += QString("\nWarning: %1 file(s) would have collisions\n").arg(collisions);
    }

    return summary;
}
