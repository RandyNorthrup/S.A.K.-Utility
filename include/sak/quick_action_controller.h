// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"

#include <QObject>
#include <QThread>
#include <QString>
#include <QQueue>
#include <memory>
#include <vector>

namespace sak {

/**
 * @brief Controls quick action execution and threading
 * 
 * Manages the lifecycle of quick actions including:
 * - Thread pool for background execution
 * - Action queue for sequential operations
 * - Admin privilege escalation
 * - Logging and error handling
 * - Progress aggregation
 * 
 * Thread-Safety: Thread-safe for action submission
 */
class QuickActionController : public QObject {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent QObject
     */
    explicit QuickActionController(QObject* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~QuickActionController() override;

    // Disable copy and move
    QuickActionController(const QuickActionController&) = delete;
    QuickActionController& operator=(const QuickActionController&) = delete;
    QuickActionController(QuickActionController&&) = delete;
    QuickActionController& operator=(QuickActionController&&) = delete;

    /**
     * @brief Register an action
     * @param action Action to register (ownership transferred)
     * @return Unique action identifier
     */
    QString registerAction(std::unique_ptr<QuickAction> action);

    /**
     * @brief Get action by name
     * @param action_name Action identifier
     * @return Pointer to action, or nullptr if not found
     */
    QuickAction* getAction(const QString& action_name) const;

    /**
     * @brief Get all registered actions
     * @return Vector of action pointers
     */
    std::vector<QuickAction*> getAllActions() const;

    /**
     * @brief Get actions by category
     * @param category Action category
     * @return Vector of actions in that category
     */
    std::vector<QuickAction*> getActionsByCategory(QuickAction::ActionCategory category) const;

    /**
     * @brief Check if admin privileges are available
     * @return True if running as administrator
     */
    static bool hasAdminPrivileges();

    /**
     * @brief Request admin elevation
     * @param reason Reason for elevation (shown to user)
     * @return True if elevation successful
     */
    static bool requestAdminElevation(const QString& reason);

Q_SIGNALS:
    /**
     * @brief Emitted when action scan starts
     * @param action Action being scanned
     */
    void actionScanStarted(QuickAction* action);

    /**
     * @brief Emitted when action scan completes
     * @param action Action that completed scan
     */
    void actionScanComplete(QuickAction* action);

    /**
     * @brief Emitted when action execution starts
     * @param action Action being executed
     */
    void actionExecutionStarted(QuickAction* action);

    /**
     * @brief Emitted during action execution
     * @param action Action being executed
     * @param message Progress message
     * @param progress Progress value (0-100)
     */
    void actionExecutionProgress(QuickAction* action, const QString& message, int progress);

    /**
     * @brief Emitted when action execution completes
     * @param action Action that completed
     */
    void actionExecutionComplete(QuickAction* action);

    /**
     * @brief Emitted on error
     * @param action Action that failed
     * @param error_message Error description
     */
    void actionError(QuickAction* action, const QString& error_message);

    /**
     * @brief Emitted when log message generated
     * @param message Log message
     */
    void logMessage(const QString& message);

public Q_SLOTS:
    /**
     * @brief Scan action asynchronously
     * @param action_name Action identifier
     */
    void scanAction(const QString& action_name);

    /**
     * @brief Execute action asynchronously
     * @param action_name Action identifier
     * @param require_confirmation If true, emit signal for confirmation first
     */
    void executeAction(const QString& action_name, bool require_confirmation = true);

    /**
     * @brief Scan all actions
     */
    void scanAllActions();

    /**
     * @brief Cancel current action
     */
    void cancelCurrentAction();

private Q_SLOTS:
    /**
     * @brief Handle scan complete from worker
     */
    void onScanComplete();

    /**
     * @brief Handle execution complete from worker
     */
    void onExecutionComplete();

    /**
     * @brief Handle worker error
     */
    void onWorkerError(const QString& error);

private:
    /**
     * @brief Start scan in worker thread
     * @param action Action to scan
     */
    void startScanWorker(QuickAction* action);

    /**
     * @brief Start execution in worker thread
     * @param action Action to execute
     */
    void startExecutionWorker(QuickAction* action);

    /**
     * @brief Log operation
     * @param action Action being logged
     * @param message Log message
     */
    void logOperation(QuickAction* action, const QString& message);

    // Registered actions (owned)
    std::vector<std::unique_ptr<QuickAction>> m_actions;

    // Action lookup
    QHash<QString, QuickAction*> m_action_map;

    // Worker threads
    QThread* m_scan_thread{nullptr};
    QThread* m_execution_thread{nullptr};

    // Current operations
    QuickAction* m_current_scan_action{nullptr};
    QuickAction* m_current_execution_action{nullptr};

    // Action queue
    QQueue<QString> m_action_queue;

    // Logging
    bool m_logging_enabled{true};
    QString m_log_file_path;
};

} // namespace sak
