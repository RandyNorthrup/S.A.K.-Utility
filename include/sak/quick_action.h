// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>
#include <QIcon>
#include <QDateTime>
#include <memory>
#include <expected>
#include <atomic>

namespace sak {

/**
 * @brief Base class for all quick actions
 *
 * Provides a common interface for one-click operations that technicians
 * frequently perform. Each action supports:
 * - Pre-scan to detect what will be affected (files, size, count)
 * - Execution with progress tracking
 * - Result reporting
 * - Applicability checking
 *
 * Thread-Safety: scan() and execute() may be called from worker threads.
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
        QuickBackup,
        Maintenance,
        Troubleshooting,
        EmergencyRecovery
    };
    Q_ENUM(ActionCategory)

    /**
     * @brief Current status of the action
     */
    enum class ActionStatus {
        Idle,          // Not started
        Scanning,      // Pre-scanning to determine scope
        Ready,         // Scan complete, ready to execute
        Running,       // Currently executing
        Success,       // Completed successfully
        Failed,        // Failed with error
        Cancelled      // Cancelled by user
    };
    Q_ENUM(ActionStatus)

    /**
     * @brief Scan result from pre-execution scan
     */
    struct ScanResult {
        bool applicable{false};        // Is this action applicable?
        QString summary;               // e.g., "Frees: 2.3 GB"
        QString details;               // Additional details
        qint64 bytes_affected{0};      // Total bytes to process
        qint64 files_count{0};         // Number of files
        qint64 estimated_duration_ms{0}; // Estimated time
        QString warning;               // Optional warning message
    };

    /**
     * @brief Execution result
     */
    struct ExecutionResult {
        bool success{false};
        QString message;               // Success or error message
        qint64 bytes_processed{0};     // Actual bytes processed
        qint64 files_processed{0};     // Actual files processed
        qint64 duration_ms{0};         // Actual duration
        QString output_path;           // Path to backup/report (if applicable)
        QString log;                   // Detailed operation log
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
     * @brief Get current status
     * @return Current action status
     */
    ActionStatus status() const { return m_status; }

    /**
     * @brief Get last scan result
     * @return Most recent scan result
     */
    const ScanResult& lastScanResult() const { return m_scan_result; }

    /**
     * @brief Get last execution result
     * @return Most recent execution result
     */
    const ExecutionResult& lastExecutionResult() const { return m_execution_result; }

    /**
     * @brief Update status from controller
     * @param status New status
     */
    void updateStatus(ActionStatus status) { setStatus(status); }

    /**
     * @brief Apply execution result from controller
     * @param result Execution result
     * @param status Final status
     */
    void applyExecutionResult(const ExecutionResult& result, ActionStatus status) {
        setExecutionResult(result);
        setStatus(status);
    }

Q_SIGNALS:
    /**
     * @brief Emitted when status changes
     * @param status New status
     */
    void statusChanged(ActionStatus status);

    /**
     * @brief Emitted during scan progress
     * @param message Progress message
     */
    void scanProgress(const QString& message);

    /**
     * @brief Emitted when scan completes
     * @param result Scan result
     */
    void scanComplete(const ScanResult& result);

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
     * @brief Perform pre-execution scan
     *
     * Scans the system to determine what files/data will be affected.
     * This is called when the panel loads to show size estimates.
     *
     * Thread-Safety: May be called from worker thread
     */
    virtual void scan() = 0;

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
     * @brief Set scan result
     * @param result Scan result to store
     */
    void setScanResult(const ScanResult& result);

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
     * @brief Build a box-drawing formatted log string
     * @param title Title for the box header (e.g., "BROWSER CACHE CLEARING - RESULTS")
     * @param content_lines Lines of content to display inside the box
     * @param duration_ms Optional duration to append in footer (-1 to omit)
     * @return Formatted string with ╔═╗║╚═╝ box-drawing characters
     */
    static QString formatLogBox(const QString& title, const QStringList& content_lines,
        qint64 duration_ms = -1);

    /**
     * @brief Sanitize a filesystem path for use as a backup subdirectory name
     * @param path Source filesystem path
     * @return Path with ':', '\\', '/' replaced by '_'
     */
    static QString sanitizePathForBackup(const QString& path);

private:
    ActionStatus m_status{ActionStatus::Idle};
    ScanResult m_scan_result;
    ExecutionResult m_execution_result;
    std::atomic<bool> m_cancelled{false};
};

} // namespace sak
