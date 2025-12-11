#include "gui/undo_commands.h"
#include "sak/logger.h"
#include <QFile>
#include <QDir>
#include <QFileInfo>

#ifdef _WIN32
#include <Windows.h>
#include <shellapi.h>
#endif

namespace sak {

// ============================================================================
// Helper Functions
// ============================================================================

namespace {

/**
 * @brief Move file to trash (Windows recycle bin)
 */
bool moveToTrash(const QString& file_path)
{
#ifdef _WIN32
    // Use SHFileOperation to move to recycle bin
    std::wstring path = file_path.toStdWString() + L'\0'; // Double null-terminated
    
    SHFILEOPSTRUCTW fileOp{};
    fileOp.wFunc = FO_DELETE;
    fileOp.pFrom = path.c_str();
    fileOp.fFlags = FOF_ALLOWUNDO | FOF_NO_UI;
    
    int result = SHFileOperationW(&fileOp);
    return result == 0 && !fileOp.fAnyOperationsAborted;
#else
    // On non-Windows, move to ~/.local/share/Trash (XDG standard)
    QDir trash_dir(QDir::homePath() + "/.local/share/Trash/files");
    if (!trash_dir.exists()) {
        trash_dir.mkpath(".");
    }
    
    QFileInfo file_info(file_path);
    QString trash_path = trash_dir.absoluteFilePath(file_info.fileName());
    
    return QFile::rename(file_path, trash_path);
#endif
}

/**
 * @brief Restore file from trash
 */
bool restoreFromTrash(const QString& file_path, const QString& original_path)
{
    // This is simplified - full implementation would need trash metadata
    return QFile::rename(file_path, original_path);
}

/**
 * @brief Set file permissions (Windows)
 */
bool setFilePermissions(const QString& file_path, quint32 permissions)
{
    QFile file(file_path);
    return file.setPermissions(static_cast<QFile::Permissions>(permissions));
}

} // anonymous namespace

// ============================================================================
// OrganizeFilesCommand Implementation
// ============================================================================

OrganizeFilesCommand::OrganizeFilesCommand(std::vector<FileMove> moves, const QString& pattern)
    : UndoCommand(QString("Organize %1 files by pattern: %2").arg(moves.size()).arg(pattern))
    , m_moves(std::move(moves))
    , m_pattern(pattern)
{
}

void OrganizeFilesCommand::redo()
{
    int success_count = 0;
    
    for (const auto& move : m_moves) {
        // Create destination directory if needed
        QFileInfo dest_info(move.dest_path);
        QDir dest_dir = dest_info.dir();
        if (!dest_dir.exists()) {
            if (!dest_dir.mkpath(".")) {
                sak::log_error("Failed to create directory: {}", dest_dir.path().toStdString());
                continue;
            }
        }
        
        // Move file
        if (QFile::rename(move.source_path, move.dest_path)) {
            ++success_count;
        } else {
            sak::log_error("Failed to move: {} -> {}", 
                         move.source_path.toStdString(), 
                         move.dest_path.toStdString());
        }
    }
    
    m_executed = true;
    sak::log_info("Organized {} of {} files", success_count, m_moves.size());
}

void OrganizeFilesCommand::undo()
{
    if (!m_executed) {
        sak::log_warning("Cannot undo: organize command not yet executed");
        return;
    }
    
    int success_count = 0;
    
    for (const auto& move : m_moves) {
        // Move back to original location
        if (QFile::exists(move.dest_path)) {
            if (QFile::rename(move.dest_path, move.source_path)) {
                ++success_count;
            } else {
                sak::log_error("Failed to restore: {} -> {}", 
                             move.dest_path.toStdString(), 
                             move.source_path.toStdString());
            }
        }
    }
    
    sak::log_info("Restored {} of {} files to original locations", success_count, m_moves.size());
}

bool OrganizeFilesCommand::canUndo() const
{
    return m_executed;
}

// ============================================================================
// BackupCommand Implementation
// ============================================================================

BackupCommand::BackupCommand(const QString& source_dir, const QString& backup_path)
    : UndoCommand(QString("Backup: %1").arg(source_dir))
    , m_source_dir(source_dir)
    , m_backup_path(backup_path)
{
}

void BackupCommand::redo()
{
    // Backup creation is handled by BackupWorker
    // This command just tracks that backup was created
    m_backup_exists = QFile::exists(m_backup_path);
    sak::log_info("Backup created: {}", m_backup_path.toStdString());
}

void BackupCommand::undo()
{
    if (!m_backup_exists) {
        sak::log_warning("Cannot undo: backup file doesn't exist");
        return;
    }
    
    // Delete backup file
    if (QFile::remove(m_backup_path)) {
        sak::log_info("Removed backup: {}", m_backup_path.toStdString());
        m_backup_exists = false;
    } else {
        sak::log_error("Failed to remove backup: {}", m_backup_path.toStdString());
    }
}

bool BackupCommand::canUndo() const
{
    return m_backup_exists && QFile::exists(m_backup_path);
}

// ============================================================================
// DuplicateActionCommand Implementation
// ============================================================================

DuplicateActionCommand::DuplicateActionCommand(Action action, const QStringList& files,
                                               const QString& target_dir)
    : UndoCommand(QString("Duplicate action: %1 on %2 files")
                 .arg(action == Action::Delete ? "Delete" : 
                      action == Action::Move ? "Move" : "Mark")
                 .arg(files.size()))
    , m_action(action)
    , m_files(files)
    , m_target_dir(target_dir)
{
}

void DuplicateActionCommand::redo()
{
    int success_count = 0;
    
    switch (m_action) {
        case Action::Delete:
            for (const QString& file : m_files) {
                if (moveToTrash(file)) {
                    ++success_count;
                }
            }
            break;
            
        case Action::Move:
            m_original_locations.clear();
            for (const QString& file : m_files) {
                m_original_locations.append(file);
                QFileInfo file_info(file);
                QString dest = m_target_dir + "/" + file_info.fileName();
                if (QFile::rename(file, dest)) {
                    ++success_count;
                }
            }
            break;
            
        case Action::MarkOnly:
            // Just marking, no file operations
            success_count = m_files.size();
            break;
    }
    
    m_executed = true;
    sak::log_info("Duplicate action completed: {} of {} files", success_count, m_files.size());
}

void DuplicateActionCommand::undo()
{
    if (!m_executed) {
        sak::log_warning("Cannot undo: duplicate action not yet executed");
        return;
    }
    
    int success_count = 0;
    
    switch (m_action) {
        case Action::Delete:
            // Cannot easily restore from trash without metadata
            sak::log_warning("Cannot undo delete from trash (not implemented)");
            break;
            
        case Action::Move:
            for (int i = 0; i < m_files.size() && i < m_original_locations.size(); ++i) {
                QFileInfo file_info(m_files[i]);
                QString current_path = m_target_dir + "/" + file_info.fileName();
                if (QFile::rename(current_path, m_original_locations[i])) {
                    ++success_count;
                }
            }
            break;
            
        case Action::MarkOnly:
            // No undo needed for marking
            success_count = m_files.size();
            break;
    }
    
    sak::log_info("Undid duplicate action: {} of {} files", success_count, m_files.size());
}

bool DuplicateActionCommand::canUndo() const
{
    return m_executed && m_action != Action::Delete; // Can't undo trash deletion easily
}

// ============================================================================
// DeleteFilesCommand Implementation
// ============================================================================

DeleteFilesCommand::DeleteFilesCommand(const QStringList& files, bool use_trash)
    : UndoCommand(QString("Delete %1 files").arg(files.size()))
    , m_files(files)
    , m_use_trash(use_trash)
{
}

void DeleteFilesCommand::redo()
{
    int success_count = 0;
    
    if (m_use_trash) {
        for (const QString& file : m_files) {
            if (moveToTrash(file)) {
                ++success_count;
            }
        }
    } else {
        // Permanent deletion
        for (const QString& file : m_files) {
            if (QFile::remove(file)) {
                ++success_count;
            }
        }
    }
    
    m_executed = true;
    sak::log_info("Deleted {} of {} files (trash: {})", 
                 success_count, m_files.size(), m_use_trash);
}

void DeleteFilesCommand::undo()
{
    if (!m_executed) {
        sak::log_warning("Cannot undo: delete not yet executed");
        return;
    }
    
    if (!m_use_trash) {
        sak::log_error("Cannot undo permanent deletion");
        return;
    }
    
    // Restore from trash would require trash metadata
    sak::log_warning("Restore from trash not fully implemented");
}

bool DeleteFilesCommand::canUndo() const
{
    return m_executed && m_use_trash;
}

// ============================================================================
// PermissionChangeCommand Implementation
// ============================================================================

PermissionChangeCommand::PermissionChangeCommand(std::vector<PermissionState> changes)
    : UndoCommand(QString("Change permissions on %1 files").arg(changes.size()))
    , m_changes(std::move(changes))
{
}

void PermissionChangeCommand::redo()
{
    int success_count = 0;
    
    for (const auto& change : m_changes) {
        if (setFilePermissions(change.file_path, change.new_permissions)) {
            ++success_count;
        }
    }
    
    m_executed = true;
    sak::log_info("Changed permissions on {} of {} files", success_count, m_changes.size());
}

void PermissionChangeCommand::undo()
{
    if (!m_executed) {
        sak::log_warning("Cannot undo: permission change not yet executed");
        return;
    }
    
    int success_count = 0;
    
    for (const auto& change : m_changes) {
        if (setFilePermissions(change.file_path, change.old_permissions)) {
            ++success_count;
        }
    }
    
    sak::log_info("Restored permissions on {} of {} files", success_count, m_changes.size());
}

// ============================================================================
// RenameFileCommand Implementation
// ============================================================================

RenameFileCommand::RenameFileCommand(const QString& old_path, const QString& new_path)
    : UndoCommand(QString("Rename: %1 -> %2").arg(old_path).arg(new_path))
    , m_old_path(old_path)
    , m_new_path(new_path)
{
}

void RenameFileCommand::redo()
{
    if (QFile::rename(m_old_path, m_new_path)) {
        m_executed = true;
        sak::log_info("Renamed: {} -> {}", m_old_path.toStdString(), m_new_path.toStdString());
    } else {
        sak::log_error("Failed to rename: {} -> {}", 
                     m_old_path.toStdString(), m_new_path.toStdString());
    }
}

void RenameFileCommand::undo()
{
    if (!m_executed) {
        sak::log_warning("Cannot undo: rename not yet executed");
        return;
    }
    
    if (QFile::rename(m_new_path, m_old_path)) {
        sak::log_info("Restored name: {} -> {}", m_new_path.toStdString(), m_old_path.toStdString());
    } else {
        sak::log_error("Failed to restore name: {} -> {}", 
                     m_new_path.toStdString(), m_old_path.toStdString());
    }
}

bool RenameFileCommand::canUndo() const
{
    return m_executed && QFile::exists(m_new_path);
}

// ============================================================================
// BatchRenameCommand Implementation
// ============================================================================

BatchRenameCommand::BatchRenameCommand(std::vector<RenameOperation> operations)
    : UndoCommand(QString("Batch rename %1 files").arg(operations.size()))
    , m_operations(std::move(operations))
{
}

void BatchRenameCommand::redo()
{
    m_executed_count = 0;
    
    for (const auto& op : m_operations) {
        if (QFile::rename(op.old_path, op.new_path)) {
            ++m_executed_count;
        } else {
            sak::log_error("Failed to rename: {} -> {}", 
                         op.old_path.toStdString(), op.new_path.toStdString());
            break; // Stop on first failure to maintain consistency
        }
    }
    
    sak::log_info("Batch renamed {} of {} files", m_executed_count, m_operations.size());
}

void BatchRenameCommand::undo()
{
    if (m_executed_count == 0) {
        sak::log_warning("Cannot undo: batch rename not yet executed");
        return;
    }
    
    size_t restored_count = 0;
    
    // Undo in reverse order
    for (size_t i = m_executed_count; i > 0; --i) {
        const auto& op = m_operations[i - 1];
        if (QFile::rename(op.new_path, op.old_path)) {
            ++restored_count;
        } else {
            sak::log_error("Failed to restore name: {} -> {}", 
                         op.new_path.toStdString(), op.old_path.toStdString());
            break;
        }
    }
    
    sak::log_info("Restored {} of {} renamed files", restored_count, m_executed_count);
}

bool BatchRenameCommand::canUndo() const
{
    return m_executed_count > 0;
}

} // namespace sak

