#pragma once

#include "gui/undo_manager.h"
#include <QString>
#include <QStringList>
#include <vector>

namespace sak {

/**
 * @brief Undo command for file organization
 */
class OrganizeFilesCommand : public UndoCommand {
public:
    struct FileMove {
        QString source_path;
        QString dest_path;
    };

    OrganizeFilesCommand(std::vector<FileMove> moves, const QString& pattern);

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    std::vector<FileMove> m_moves;
    QString m_pattern;
    bool m_executed{false};
};

/**
 * @brief Undo command for backup creation
 */
class BackupCommand : public UndoCommand {
public:
    BackupCommand(const QString& source_dir, const QString& backup_path);

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    QString m_source_dir;
    QString m_backup_path;
    bool m_backup_exists{false};
};

/**
 * @brief Undo command for duplicate file operations
 */
class DuplicateActionCommand : public UndoCommand {
public:
    enum class Action {
        Delete,
        Move,
        MarkOnly
    };

    DuplicateActionCommand(Action action, const QStringList& files, 
                          const QString& target_dir = QString());

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    Action m_action;
    QStringList m_files;
    QString m_target_dir;
    QStringList m_original_locations; // For move undo
    bool m_executed{false};
};

/**
 * @brief Undo command for file deletion with trash support
 */
class DeleteFilesCommand : public UndoCommand {
public:
    DeleteFilesCommand(const QStringList& files, bool use_trash = true);

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    QStringList m_files;
    bool m_use_trash;
    QString m_trash_location; // Where files were moved
    bool m_executed{false};
};

/**
 * @brief Undo command for permission changes
 */
class PermissionChangeCommand : public UndoCommand {
public:
    struct PermissionState {
        QString file_path;
        quint32 old_permissions;
        quint32 new_permissions;
    };

    PermissionChangeCommand(std::vector<PermissionState> changes);

    void redo() override;
    void undo() override;

private:
    std::vector<PermissionState> m_changes;
    bool m_executed{false};
};

/**
 * @brief Undo command for file rename
 */
class RenameFileCommand : public UndoCommand {
public:
    RenameFileCommand(const QString& old_path, const QString& new_path);

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    QString m_old_path;
    QString m_new_path;
    bool m_executed{false};
};

/**
 * @brief Undo command for batch file renames
 */
class BatchRenameCommand : public UndoCommand {
public:
    struct RenameOperation {
        QString old_path;
        QString new_path;
    };

    BatchRenameCommand(std::vector<RenameOperation> operations);

    void redo() override;
    void undo() override;
    [[nodiscard]] bool canUndo() const override;

private:
    std::vector<RenameOperation> m_operations;
    size_t m_executed_count{0};
};

} // namespace sak
