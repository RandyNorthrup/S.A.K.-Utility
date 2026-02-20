#include "sak/organizer_worker.h"
#include "sak/logger.h"
#include <QVector>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDateTime>

OrganizerWorker::OrganizerWorker(const Config& config, QObject* parent)
    : WorkerBase(parent)
    , m_config(config)
{
}

auto OrganizerWorker::execute() -> std::expected<void, sak::error_code>
{
    sak::logInfo("Starting directory organization: {}", m_config.target_directory.toStdString());

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

        Q_EMIT fileProgress(static_cast<int>(i + 1), static_cast<int>(files.size()), 
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

        Q_EMIT fileProgress(static_cast<int>(i + 1), static_cast<int>(m_planned_operations.size()),
                           QString::fromStdString(operation.source.string()));
    }

    sak::logInfo("Directory organization complete");
    return {};
}

auto OrganizerWorker::scanDirectory() 
    -> std::expected<std::vector<std::filesystem::path>, sak::error_code>
{
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

    try {
        // Only scan immediate files, not subdirectories
        // Reserve capacity to reduce allocations
        files.reserve(256);  // Reasonable default, will grow if needed
        
        for (const auto& entry : std::filesystem::directory_iterator(target_path)) {
            if (checkStop()) {
                return std::unexpected(sak::error_code::operation_cancelled);
            }

            if (entry.is_regular_file()) {
                files.push_back(entry.path());
            }
        }
    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Filesystem error during scan: {}", e.what());
        return std::unexpected(sak::error_code::scan_failed);
    }

    return files;
}

auto OrganizerWorker::categorizeFile(const std::filesystem::path& file_path) -> QString
{
    auto extension = file_path.extension().string();
    if (extension.empty()) {
        return QString();
    }

    // Remove leading dot and convert to lowercase
    if (extension[0] == '.') {
        extension = extension.substr(1);
    }
    std::transform(extension.begin(), extension.end(), extension.begin(), 
                   [](unsigned char c) { return std::tolower(c); });

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
    -> MoveOperation
{
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
    -> std::expected<void, sak::error_code>
{
    try {
        // Create category directory if needed
        if (m_config.create_subdirectories) {
            std::filesystem::path category_dir = operation.destination.parent_path();
            if (!std::filesystem::exists(category_dir)) {
                std::filesystem::create_directories(category_dir);
                sak::logInfo("Created directory: {}", category_dir.string());
            }
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

auto OrganizerWorker::handleCollision(const MoveOperation& operation) 
    -> std::filesystem::path
{
    if (m_config.collision_strategy == "skip") {
        return operation.source; // Don't move
    }
    
    if (m_config.collision_strategy == "overwrite") {
        return operation.destination; // Use original destination
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

auto OrganizerWorker::generatePreviewSummary() -> QString
{
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

    int collisions = 0;
    for (const auto& op : m_planned_operations) {
        if (op.would_overwrite) {
            ++collisions;
        }
    }

    if (collisions > 0) {
        summary += QString("\nWarning: %1 file(s) would have collisions\n").arg(collisions);
    }

    return summary;
}

void OrganizerWorker::logForUndo(const MoveOperation& operation)
{
    if (!operation.was_executed) {
        return;
    }

    UndoEntry entry;
    entry.original_source = operation.source;
    entry.current_location = operation.destination;
    entry.timestamp = QDateTime::currentDateTime();
    entry.can_undo = std::filesystem::exists(operation.destination);

    m_undo_history.push_back(entry);
    sak::logInfo("Logged undo entry: {} -> {}", 
                 entry.original_source.string(), 
                 entry.current_location.string());
}

bool OrganizerWorker::canRestore(const UndoEntry& entry)
{
    // Check if file still exists at current location
    if (!std::filesystem::exists(entry.current_location)) {
        return false;
    }

    // Check if original parent directory exists
    auto original_parent = entry.original_source.parent_path();
    if (!std::filesystem::exists(original_parent)) {
        return false;
    }

    // Check if original location is now occupied
    if (std::filesystem::exists(entry.original_source)) {
        return false; // Would cause collision
    }

    return true;
}

auto OrganizerWorker::undoLastOperation() -> std::expected<void, sak::error_code>
{
    if (m_undo_history.empty()) {
        sak::logInfo("No operations to undo");
        return std::unexpected(sak::error_code::invalid_operation);
    }

    const auto& entry = m_undo_history.back();
    
    if (!canRestore(entry)) {
        sak::logError("Cannot undo: file state changed");
        return std::unexpected(sak::error_code::invalid_operation);
    }

    try {
        std::filesystem::rename(entry.current_location, entry.original_source);
        sak::logInfo("Undone: {} -> {}", 
                     entry.current_location.string(), 
                     entry.original_source.string());
        
        m_undo_history.pop_back();
        return {};

    } catch (const std::filesystem::filesystem_error& e) {
        sak::logError("Failed to undo operation: {}", e.what());
        return std::unexpected(sak::error_code::write_error);
    }
}

auto OrganizerWorker::undoAllOperations() -> std::expected<void, sak::error_code>
{
    if (m_undo_history.empty()) {
        sak::logInfo("No operations to undo");
        return {};
    }

    int successful_undos = 0;
    int failed_undos = 0;

    // Undo in reverse order (last operation first)
    while (!m_undo_history.empty()) {
        auto result = undoLastOperation();
        if (result) {
            ++successful_undos;
        } else {
            ++failed_undos;
            sak::logError("Failed to undo operation, stopping undo process");
            break;
        }
    }

    sak::logInfo("Undo complete: {} succeeded, {} failed", successful_undos, failed_undos);
    
    if (failed_undos > 0) {
        return std::unexpected(sak::error_code::partial_failure);
    }

    return {};
}

auto OrganizerWorker::saveUndoLog(const QString& file_path) -> std::expected<void, sak::error_code>
{
    if (m_undo_history.empty()) {
        sak::logInfo("No undo history to save");
        return {};
    }

    QJsonArray entries_array;
    for (const auto& entry : m_undo_history) {
        QJsonObject entry_obj;
        entry_obj["original_source"] = QString::fromStdString(entry.original_source.string());
        entry_obj["current_location"] = QString::fromStdString(entry.current_location.string());
        entry_obj["timestamp"] = entry.timestamp.toString(Qt::ISODate);
        entry_obj["can_undo"] = entry.can_undo;
        entries_array.append(entry_obj);
    }

    QJsonObject root;
    root["version"] = 1;
    root["timestamp"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["entries"] = entries_array;

    QJsonDocument doc(root);
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        sak::logError("Failed to open undo log file for writing: {}", file_path.toStdString());
        return std::unexpected(sak::error_code::write_error);
    }

    file.write(doc.toJson());
    file.close();

    sak::logInfo("Saved undo log: {} entries to {}", m_undo_history.size(), file_path.toStdString());
    return {};
}

auto OrganizerWorker::loadUndoLog(const QString& file_path) -> std::expected<void, sak::error_code>
{
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        sak::logError("Failed to open undo log file for reading: {}", file_path.toStdString());
        return std::unexpected(sak::error_code::file_not_found);
    }

    QByteArray data = file.readAll();
    file.close();

    QJsonDocument doc = QJsonDocument::fromJson(data);
    if (!doc.isObject()) {
        sak::logError("Invalid undo log file format");
        return std::unexpected(sak::error_code::parse_error);
    }

    QJsonObject root = doc.object();
    QJsonArray entries_array = root["entries"].toArray();

    m_undo_history.clear();
    m_undo_history.reserve(static_cast<size_t>(entries_array.size()));

    for (const auto& entry_value : entries_array) {
        QJsonObject entry_obj = entry_value.toObject();
        
        UndoEntry entry;
        entry.original_source = std::filesystem::path(entry_obj["original_source"].toString().toStdString());
        entry.current_location = std::filesystem::path(entry_obj["current_location"].toString().toStdString());
        entry.timestamp = QDateTime::fromString(entry_obj["timestamp"].toString(), Qt::ISODate);
        entry.can_undo = entry_obj["can_undo"].toBool();

        m_undo_history.push_back(entry);
    }

    sak::logInfo("Loaded undo log: {} entries from {}", m_undo_history.size(), file_path.toStdString());
    return {};
}

