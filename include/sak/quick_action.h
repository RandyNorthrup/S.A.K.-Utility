// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QIcon>
#include <QDateTime>
#include <memory>
#include <expected>

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

private:
    ActionStatus m_status{ActionStatus::Idle};
    ScanResult m_scan_result;
    ExecutionResult m_execution_result;
    bool m_cancelled{false};
};

} // namespace sak
