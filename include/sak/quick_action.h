// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QIcon>
#include <QObject>
#include <QString>

#include <atomic>
#include <expected>
#include <memory>

namespace sak {

/// How long a run of a quick action may take, end to end, unless the action declares its own
/// bound (QuickAction::executionTimeoutMs). The assistant's registry thunk waits exactly this
/// long, so one number bounds a run instead of an inner and an outer clock that disagree.
inline constexpr int kDefaultQuickActionTimeoutMs = 30 * 60 * 1000;

/**
 * @brief Base class for all quick actions
 *
 * Provides a common interface for one-click operations that technicians
 * frequently perform. Each action supports execution with progress tracking
 * and result reporting.
 *
 * Thread-Safety: execute() may be called from worker threads.
 * Signals are emitted on the thread where the action was created.
 */
class QuickAction : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Action category for grouping in UI
     */
    enum class ActionCategory {
        SystemOptimization,
        Maintenance,
        Troubleshooting,
        EmergencyRecovery
    };
    Q_ENUM(ActionCategory)

    /**
     * @brief Current status of the action
     */
    enum class ActionStatus {
        Idle,      // Not started
        Running,   // Currently executing
        Success,   // Completed successfully
        Failed,    // Failed with error
        Cancelled  // Cancelled by user
    };
    Q_ENUM(ActionStatus)

    /**
     * @brief Execution result
     */
    struct ExecutionResult {
        bool success{false};
        QString message;            // Success or error message
        qint64 bytes_processed{0};  // Actual bytes processed
        qint64 files_processed{0};  // Actual files processed
        qint64 duration_ms{0};      // Actual duration
        QString output_path;        // Path to backup/report (if applicable)
        QString log;                // Detailed operation log
        /// It ran, but this app could not read what it found. Not a pass, so success stays false,
        /// and not a failure, so nothing tells the person it failed.
        bool inconclusive{false};
    };

    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QuickAction(QObject* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~QuickAction() override = default;

    // Disable copy and move
    QuickAction(const QuickAction&) = delete;
    QuickAction& operator=(const QuickAction&) = delete;
    QuickAction(QuickAction&&) = delete;
    QuickAction& operator=(QuickAction&&) = delete;

    /**
     * @brief Get action name
     * @return User-friendly action name
     */
    virtual QString name() const = 0;

    /**
     * @brief Get action description
     * @return Short description of what the action does
     */
    virtual QString description() const = 0;

    /**
     * @brief Get action category
     * @return Category for UI grouping
     */
    virtual ActionCategory category() const = 0;

    /**
     * @brief Get action icon
     * @return Icon for button display
     */
    virtual QIcon icon() const = 0;

    /**
     * @brief Check if action requires admin privileges
     * @return True if admin elevation required
     */
    virtual bool requiresAdmin() const = 0;

    /**
     * @brief Whether running the action leaves the machine as it found it
     *
     * A read-only action only reads system state and writes its own output (a report into
     * the caller's output folder). The assistant runs it without the per-action approval and
     * lease a system change needs. Defaults to false: an action has to state that it changes
     * nothing.
     */
    virtual bool isReadOnly() const { return false; }

    /**
     * @brief The one end-to-end time bound for a run of this action, in milliseconds
     *
     * The assistant's registry thunk waits this long before it stops the run, and an action
     * whose steps have their own timeouts keeps their sum inside it. Override when the work
     * legitimately runs longer (or must stop sooner) than kDefaultQuickActionTimeoutMs.
     */
    virtual int executionTimeoutMs() const { return kDefaultQuickActionTimeoutMs; }

    /**
     * @brief Get current status
     * @return Current action status
     */
    ActionStatus status() const { return m_status.load(); }

    /**
     * @brief Get last execution result
     * @return Most recent execution result
     *
     * Thread-Safety: m_execution_result is not synchronized. execute() may write it
     * from a worker thread, so this reference is only valid to read AFTER the queued
     * executionComplete() signal has been delivered (that queued-connection delivery
     * establishes the happens-before edge). Do NOT read it concurrently while an
     * execute() is in progress on a worker thread -- that is an unsynchronized race.
     */
    const ExecutionResult& lastExecutionResult() const { return m_execution_result; }

    /**
     * @brief Update status from controller
     * @param status New status
     */
    void updateStatus(ActionStatus status) { setStatus(status); }

    /// @brief Clear the cancellation flag before a fresh run (a stale flag from a prior
    ///        completed/cancelled run would otherwise make every future run cancel immediately).
    void clearCancellation() { resetCancelled(); }

    /**
     * @brief Apply execution result from controller
     * @param result Execution result
     * @param status Final status
     */
    void applyExecutionResult(const ExecutionResult& result, ActionStatus status) {
        setExecutionResult(result);
        setStatus(status);
    }

    /**
     * @brief The folder a run writes its files into (a report, screenshots, recovery keys)
     *
     * Empty for an action that writes nothing; an action that writes files refuses to run
     * without an absolute folder. Set before execute() runs and read only by that run.
     */
    void setOutputFolder(const QString& folder) { m_output_folder = folder; }
    [[nodiscard]] const QString& outputFolder() const { return m_output_folder; }

Q_SIGNALS:
    /**
     * @brief Emitted when status changes
     * @param status New status
     */
    void statusChanged(ActionStatus status);

    /**
     * @brief Emitted during execution progress
     * @param message Progress message
     * @param progress Progress value (0-100)
     */
    void executionProgress(const QString& message, int progress);

    /**
     * @brief Emitted when execution completes
     * @param result Execution result
     */
    void executionComplete(const ExecutionResult& result);

    /**
     * @brief Emitted on error
     * @param error_message Error description
     */
    void errorOccurred(const QString& error_message);

    /**
     * @brief Emitted for log output
     * @param message Log message
     */
    void logMessage(const QString& message);

public Q_SLOTS:
    /**
     * @brief Execute the action
     *
     * Performs the actual operation (cleanup, backup, etc.)
     * Should emit executionProgress regularly for UI updates.
     *
     * Thread-Safety: May be called from worker thread
     */
    virtual void execute() = 0;

    /**
     * @brief Cancel ongoing operation
     */
    virtual void cancel();

protected:
    /**
     * @brief Set current status and emit signal
     * @param status New status
     */
    void setStatus(ActionStatus status);

    /**
     * @brief Set execution result
     * @param result Execution result to store
     */
    void setExecutionResult(const ExecutionResult& result);

    /**
     * @brief Check if cancellation was requested
     * @return True if cancel() was called
     */
    bool isCancelled() const { return m_cancelled; }

    /**
     * @brief Reset cancellation flag
     */
    void resetCancelled() { m_cancelled = false; }

    // === Shared helpers to eliminate boilerplate across action subclasses ===

    /**
     * @brief Emit a cancelled result and return (pre-execution, no duration)
     * @param message Cancellation message
     */
    void emitCancelledResult(const QString& message);

    /**
     * @brief Emit a cancelled result with duration tracking
     * @param message Cancellation message
     * @param start_time When execution started (for duration calculation)
     */
    void emitCancelledResult(const QString& message, const QDateTime& start_time);

    /**
     * @brief Emit a failed result and return
     * @param message Failure message
     * @param log Detailed log message
     * @param start_time When execution started (for duration calculation)
     */
    void emitFailedResult(const QString& message, const QString& log, const QDateTime& start_time);

    /**
     * @brief Set result, status, and emit completion signal in one call
     * @param result Execution result to store and emit
     * @param status Final action status
     */
    void finishWithResult(const ExecutionResult& result, ActionStatus status);

    /**
     * @brief Format a byte count as a human-readable string
     * @param bytes Number of bytes
     * @return Formatted string (e.g., "2.34 GB", "512 KB")
     */
    static QString formatFileSize(qint64 bytes);

    /**
     * @brief Sanitize a filesystem path for use as a backup subdirectory name
     * @param path Source filesystem path
     * @return Path with ':', '\\', '/' replaced by '_'
     */
    static QString sanitizePathForBackup(const QString& path);

private:
    std::atomic<ActionStatus> m_status{ActionStatus::Idle};
    ExecutionResult m_execution_result;
    std::atomic<bool> m_cancelled{false};
    QString m_output_folder;
};

}  // namespace sak
