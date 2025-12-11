#pragma once

#include "sak/worker_base.h"
#include <QString>
#include <QStringList>
#include <QMap>
#include <QDateTime>
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
        bool was_executed{false};  ///< Track if operation was completed
    };

    /**
     * @brief Undo log entry for rollback capability
     */
    struct UndoEntry {
        std::filesystem::path original_source;
        std::filesystem::path current_location;
        QDateTime timestamp;
        bool can_undo{true};
    };

    /**
     * @brief Configuration for organization operation
     */
    struct Config {
        QString target_directory;                      ///< Directory to organize
        QMap<QString, QStringList> category_mapping;   ///< Category -> extensions
        bool preview_mode{false};                      ///< Dry run without moving
        bool create_subdirectories{true};              ///< Create category folders
        QString collision_strategy{"rename"};          ///< rename/skip/overwrite
        bool enable_undo_log{true};                    ///< Track operations for undo
    };

    /**
     * @brief Construct organizer worker
     * @param config Organization configuration
     * @param parent Parent QObject
     */
    explicit OrganizerWorker(const Config& config, QObject* parent = nullptr);

    /**
     * @brief Get undo history
     * @return Vector of undo entries
     */
    [[nodiscard]] const std::vector<UndoEntry>& getUndoHistory() const { return m_undo_history; }

    /**
     * @brief Check if undo is available
     * @return True if operations can be undone
     */
    [[nodiscard]] bool canUndo() const { return !m_undo_history.empty(); }

    /**
     * @brief Undo last organization operation
     * @return Success or error code
     */
    auto undoLastOperation() -> std::expected<void, sak::error_code>;

    /**
     * @brief Undo all operations in this session
     * @return Success or error code
     */
    auto undoAllOperations() -> std::expected<void, sak::error_code>;

    /**
     * @brief Save undo log to file for future recovery
     * @param file_path Path to save undo log
     * @return Success or error code
     */
    auto saveUndoLog(const QString& file_path) -> std::expected<void, sak::error_code>;

    /**
     * @brief Load undo log from file
     * @param file_path Path to undo log file
     * @return Success or error code
     */
    auto loadUndoLog(const QString& file_path) -> std::expected<void, sak::error_code>;

Q_SIGNALS:
    /**
     * @brief Emitted when file processing progresses
     * @param current_file Current file index
     * @param total_files Total number of files
     * @param current_file_path Path of current file
     */
    void file_progress(int current_file, int total_files, const QString& current_file_path);

    /**
     * @brief Emitted in preview mode with summary of planned operations
     * @param summary Text summary of planned operations
     * @param operation_count Number of operations planned
     */
    void preview_results(const QString& summary, int operation_count);

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
    auto scan_directory() -> std::expected<std::vector<std::filesystem::path>, sak::error_code>;

    /**
     * @brief Categorize file by extension
     * @param file_path File to categorize
     * @return Category name or empty if no match
     */
    auto categorize_file(const std::filesystem::path& file_path) -> QString;

    /**
     * @brief Plan move operation for a file
     * @param file_path File to move
     * @param category Target category
     * @return Move operation details
     */
    auto plan_move(const std::filesystem::path& file_path, const QString& category) -> MoveOperation;

    /**
     * @brief Execute a single file move
     * @param operation Move operation to execute
     * @return Success or error code
     */
    auto execute_move(const MoveOperation& operation) -> std::expected<void, sak::error_code>;

    /**
     * @brief Handle file collision based on strategy
     * @param operation Move operation with collision
     * @return Modified destination path
     */
    auto handle_collision(const MoveOperation& operation) -> std::filesystem::path;

    /**
     * @brief Generate preview summary text
     * @return Summary string
     */
    auto generate_preview_summary() -> QString;

    /**
     * @brief Log operation for undo capability
     * @param operation Move operation to log
     */
    void log_for_undo(const MoveOperation& operation);

    /**
     * @brief Verify file can be restored to original location
     * @param entry Undo entry to verify
     * @return True if file can be restored
     */
    bool can_restore(const UndoEntry& entry);

    Config m_config;
    std::vector<MoveOperation> m_planned_operations;
    std::vector<UndoEntry> m_undo_history;
};
