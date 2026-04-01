// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action_controller.h"

#include "sak/elevation_broker.h"
#include "sak/elevation_manager.h"
#include "sak/logger.h"
#include "sak/quick_action_result_io.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

namespace sak {

QuickActionController::QuickActionController(QObject* parent) : QObject(parent) {
    // Setup log file path
    QString log_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    if (!QDir().mkpath(log_dir)) {
        sak::logWarning("Failed to create quick actions log directory: {}", log_dir.toStdString());
    }
    m_log_file_path = log_dir + "/quick_actions.log";
}

QuickActionController::~QuickActionController() {
    // Cancel any running operations
    if (m_current_scan_action) {
        m_current_scan_action->cancel();
    }
    if (m_current_execution_action) {
        m_current_execution_action->cancel();
    }

    // Gracefully shut down threads -- use deleteLater for safe cleanup
    auto cleanupThread = [](QThread*& thread) {
        if (!thread) {
            return;
        }
        if (thread->isRunning()) {
            thread->quit();
            if (!thread->wait(10'000)) {
                sak::logError("QuickAction thread did not stop within 10s");
            }
        }
        thread->deleteLater();
        thread = nullptr;
    };

    cleanupThread(m_scan_thread);
    cleanupThread(m_execution_thread);
}

void QuickActionController::setLoggingEnabled(bool enabled) {
    m_logging_enabled = enabled;
}

void QuickActionController::setBackupLocation(const QString& backup_location) {
    m_backup_location = backup_location;
}

QString QuickActionController::registerAction(std::unique_ptr<QuickAction> action) {
    Q_ASSERT(!m_actions.empty());
    Q_ASSERT(action);
    if (!action) {
        return QString();
    }

    QString action_name = action->name();
    QuickAction* action_ptr = action.get();

    // Store action
    m_actions.push_back(std::move(action));
    m_action_map.insert(action_name, action_ptr);

    // Connect signals
    connect(action_ptr, &QuickAction::statusChanged, this, [this, action_ptr]() {
        if (action_ptr->status() == QuickAction::ActionStatus::Failed) {
            Q_EMIT actionError(action_ptr, action_ptr->lastExecutionResult().message);
        }
    });

    connect(
        action_ptr, &QuickAction::scanProgress, this, [this, action_ptr](const QString& message) {
            logOperation(action_ptr, QString("Scanning: %1").arg(message));
        });

    connect(action_ptr,
            &QuickAction::executionProgress,
            this,
            [this, action_ptr](const QString& msg, int prog) {
                QString message = QString("%1 - %2%").arg(msg).arg(prog);
                Q_EMIT actionExecutionProgress(action_ptr, message, prog);
                logOperation(action_ptr, message);
            });

    connect(
        action_ptr, &QuickAction::errorOccurred, this, [this, action_ptr](const QString& error) {
            Q_EMIT actionError(action_ptr, error);
            logOperation(action_ptr, QString("ERROR: %1").arg(error));
        });

    logOperation(action_ptr, "Action registered");
    return action_name;
}

QuickAction* QuickActionController::getAction(const QString& action_name) const {
    return m_action_map.value(action_name, nullptr);
}

std::vector<QuickAction*> QuickActionController::getAllActions() const {
    std::vector<QuickAction*> result;
    result.reserve(m_actions.size());
    for (const auto& action : m_actions) {
        result.push_back(action.get());
    }
    return result;
}

std::vector<QuickAction*> QuickActionController::getActionsByCategory(
    QuickAction::ActionCategory category) const {
    std::vector<QuickAction*> result;
    for (const auto& action : m_actions) {
        if (action->category() == category) {
            result.push_back(action.get());
        }
    }
    return result;
}

bool QuickActionController::hasAdminPrivileges() {
    return ElevationManager::isElevated();
}

void QuickActionController::scanAction(const QString& action_name) {
    Q_ASSERT(!action_name.isEmpty());
    QuickAction* action = getAction(action_name);
    if (!action) {
        Q_EMIT logMessage(QString("Action not found: %1").arg(action_name));
        return;
    }

    // Check admin requirements - but allow scan to proceed
    // Scan can determine if action is applicable regardless of admin status
    // Admin check will happen again before execution
    if (action->requiresAdmin() && !hasAdminPrivileges()) {
        logOperation(action, "Note: Action requires admin privileges for execution");
    }

    // Check if already scanning
    if (m_current_scan_action) {
        m_scan_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Scan queued: %1").arg(action_name));
        return;
    }

    startScanWorker(action);
}

void QuickActionController::executeAction(const QString& action_name, bool require_confirmation) {
    Q_ASSERT(!action_name.isEmpty());
    QuickAction* action = getAction(action_name);
    if (!action) {
        Q_EMIT logMessage(QString("Action not found: %1").arg(action_name));
        return;
    }

    // Check admin requirements
    if (action->requiresAdmin() && !hasAdminPrivileges()) {
        executeElevatedAction(action, action_name);
        return;
    }

    // Check if already executing
    if (m_current_execution_action) {
        // Queue for later
        m_action_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Action queued: %1").arg(action_name));
        return;
    }

    // Confirmation handled by panel if required
    Q_UNUSED(require_confirmation)

    startExecutionWorker(action);
}

void QuickActionController::executeElevatedAction(QuickAction* action, const QString& action_name) {
    Q_ASSERT(action);
    Q_ASSERT(!action_name.isEmpty());
    if (m_current_execution_action) {
        m_action_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Action queued: %1").arg(action_name));
        return;
    }

    m_current_execution_action = action;
    Q_EMIT actionExecutionStarted(action);
    action->updateStatus(QuickAction::ActionStatus::Running);
    Q_EMIT actionExecutionProgress(action, "Requesting administrator approval...", 5);
    logOperation(action, "Requesting administrator elevation via helper");

    // Lazy-initialize the elevation broker
    if (!m_broker) {
        m_broker = new ElevationBroker(this);
        connect(m_broker,
                &ElevationBroker::progressUpdated,
                this,
                [this](int percent, const QString& status) {
                    if (m_current_execution_action) {
                        Q_EMIT actionExecutionProgress(m_current_execution_action, status, percent);
                    }
                });
    }

    // Build task payload
    QJsonObject payload;
    if (action_name == "Backup BitLocker Keys") {
        QString backup_location = m_backup_location.isEmpty() ? "C:/SAK_Backups"
                                                              : m_backup_location;
        payload["backup_location"] = backup_location;
    }

    auto broker_result = m_broker->executeTask(action_name, action_name, payload);

    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status = QuickAction::ActionStatus::Failed;

    if (broker_result) {
        result.success = broker_result->success;
        result.message = broker_result->data["message"].toString();
        result.log = broker_result->data["log"].toString();
        status = static_cast<QuickAction::ActionStatus>(broker_result->data["status"].toInt(
            static_cast<int>(QuickAction::ActionStatus::Failed)));
        if (!broker_result->success && result.message.isEmpty()) {
            result.message = broker_result->error_message;
        }
    } else {
        result.success = false;
        auto error = broker_result.error();
        if (error == sak::error_code::elevation_denied) {
            result.message = "Administrator privileges required but not granted";
            result.log = "User cancelled the UAC prompt";
        } else {
            result.message = QString("Elevated helper error: %1")
                                 .arg(QString::fromStdString(std::string(sak::to_string(error))));
            result.log = result.message;
        }
    }

    action->applyExecutionResult(result, status);
    onExecutionComplete();
}

void QuickActionController::scanAllActions() {
    // Queue all registered actions for sequential scan
    m_scan_queue.clear();
    for (const auto& action : m_actions) {
        m_scan_queue.enqueue(action->name());
    }

    if (!m_current_scan_action && !m_scan_queue.isEmpty()) {
        QString next_action = m_scan_queue.dequeue();
        scanAction(next_action);
    }
}

void QuickActionController::cancelCurrentAction() {
    if (m_current_scan_action) {
        m_current_scan_action->cancel();
        logOperation(m_current_scan_action, "Scan cancelled by user");
    }
    if (m_current_execution_action) {
        m_current_execution_action->cancel();
        logOperation(m_current_execution_action, "Execution cancelled by user");
    }
}

void QuickActionController::onScanComplete() {
    Q_ASSERT(m_scan_thread);
    Q_ASSERT(!m_scan_queue.isEmpty());
    if (!m_current_scan_action) {
        return;
    }

    QuickAction* action = m_current_scan_action;
    m_current_scan_action = nullptr;

    Q_EMIT actionScanComplete(action);
    logOperation(action, QString("Scan complete: %1").arg(action->lastScanResult().summary));

    // Cleanup thread
    if (m_scan_thread) {
        m_scan_thread->quit();
        m_scan_thread->wait();
        m_scan_thread->deleteLater();
        m_scan_thread = nullptr;
    }

    // Process scan queue
    if (!m_scan_queue.isEmpty()) {
        QString next_action = m_scan_queue.dequeue();
        scanAction(next_action);
    }
}

void QuickActionController::onExecutionComplete() {
    Q_ASSERT(m_execution_thread);
    Q_ASSERT(!m_action_queue.isEmpty());
    if (!m_current_execution_action) {
        return;
    }

    QuickAction* action = m_current_execution_action;
    m_current_execution_action = nullptr;

    Q_EMIT actionExecutionComplete(action);

    const auto& result = action->lastExecutionResult();
    qint64 duration_sec = result.duration_ms / 1000;
    QString log_msg = result.success ? QString("Execution complete: %1 (%2 bytes in %3s)")
                                           .arg(result.message)
                                           .arg(result.bytes_processed)
                                           .arg(duration_sec)
                                     : QString("Execution failed: %1").arg(result.message);
    logOperation(action, log_msg);

    // Cleanup thread
    if (m_execution_thread) {
        m_execution_thread->quit();
        m_execution_thread->wait();
        m_execution_thread->deleteLater();
        m_execution_thread = nullptr;
    }

    // Process queue
    if (!m_action_queue.isEmpty()) {
        QString next_action = m_action_queue.dequeue();
        executeAction(next_action, false);
    }
}

void QuickActionController::onWorkerError(const QString& error) {
    QuickAction* action = m_current_execution_action ? m_current_execution_action
                                                     : m_current_scan_action;
    if (action) {
        Q_EMIT actionError(action, error);
        logOperation(action, QString("ERROR: %1").arg(error));
    }
}

void QuickActionController::startScanWorker(QuickAction* action) {
    Q_ASSERT(m_scan_thread);
    Q_ASSERT(action);
    m_current_scan_action = action;
    Q_EMIT actionScanStarted(action);
    logOperation(action, "Scan started");

    // Create worker thread
    m_scan_thread = new QThread(this);
    action->moveToThread(m_scan_thread);

    // Connect completion
    connect(
        action, &QuickAction::scanComplete, this, [this](const QuickAction::ScanResult& result) {
            Q_UNUSED(result);
            onScanComplete();
        });
    connect(m_scan_thread, &QThread::finished, action, [action]() {
        auto* app_thread = QCoreApplication::instance()->thread();
        if (action->thread() != app_thread) {
            action->moveToThread(app_thread);
        }
    });
    connect(m_scan_thread, &QThread::started, action, &QuickAction::scan);

    m_scan_thread->start();
}

void QuickActionController::startExecutionWorker(QuickAction* action) {
    Q_ASSERT(m_execution_thread);
    Q_ASSERT(action);
    m_current_execution_action = action;
    Q_EMIT actionExecutionStarted(action);
    logOperation(action, "Execution started");

    // Create worker thread
    m_execution_thread = new QThread(this);
    action->moveToThread(m_execution_thread);

    // Connect completion
    connect(action,
            &QuickAction::executionComplete,
            this,
            [this](const QuickAction::ExecutionResult& result) {
                Q_UNUSED(result);
                onExecutionComplete();
            });
    connect(m_execution_thread, &QThread::finished, action, [action]() {
        auto* app_thread = QCoreApplication::instance()->thread();
        if (action->thread() != app_thread) {
            action->moveToThread(app_thread);
        }
    });
    connect(m_execution_thread, &QThread::started, action, &QuickAction::execute);

    m_execution_thread->start();
}

void QuickActionController::logOperation(QuickAction* action, const QString& message) {
    Q_ASSERT(action);
    Q_ASSERT(!message.isEmpty());
    if (!m_logging_enabled) {
        return;
    }

    QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    QString log_entry = QString("[%1] %2: %3").arg(timestamp, action->name(), message);

    Q_EMIT logMessage(log_entry);

    // Write to file
    QFile log_file(m_log_file_path);
    if (log_file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log_file);
        stream << log_entry << "\n";
    }
}

}  // namespace sak
