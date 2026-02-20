// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/quick_action_controller.h"
#include "sak/elevation_manager.h"
#include "sak/quick_action_result_io.h"
#include "sak/logger.h"

#include <QDateTime>
#include <QFile>
#include <QTextStream>
#include <QStandardPaths>
#include <QDir>
#include <QCoreApplication>
#include <QUuid>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace sak {

QuickActionController::QuickActionController(QObject* parent)
    : QObject(parent) {
    // Setup log file path
    QString log_dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(log_dir);
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

    // Gracefully shut down threads — use deleteLater for safe cleanup
    auto cleanupThread = [](QThread*& thread) {
        if (!thread) return;
        if (thread->isRunning()) {
            thread->quit();
            if (!thread->wait(10000)) {
                sak::log_error("QuickAction thread did not stop within 10s");
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

    connect(action_ptr, &QuickAction::scanProgress, this, [this, action_ptr](const QString& message) {
        logOperation(action_ptr, QString("Scanning: %1").arg(message));
    });

    connect(action_ptr, &QuickAction::executionProgress, this, [this, action_ptr](const QString& msg, int prog) {
        QString message = QString("%1 - %2%").arg(msg).arg(prog);
        Q_EMIT actionExecutionProgress(action_ptr, message, prog);
        logOperation(action_ptr, message);
    });

    connect(action_ptr, &QuickAction::errorOccurred, this, [this, action_ptr](const QString& error) {
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

std::vector<QuickAction*> QuickActionController::getActionsByCategory(QuickAction::ActionCategory category) const {
    std::vector<QuickAction*> result;
    for (const auto& action : m_actions) {
        if (action->category() == category) {
            result.push_back(action.get());
        }
    }
    return result;
}

bool QuickActionController::hasAdminPrivileges() {
#ifdef _WIN32
    BOOL is_admin = FALSE;
    PSID admin_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&nt_authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admin_group)) {
        CheckTokenMembership(nullptr, admin_group, &is_admin);
        FreeSid(admin_group);
    }

    return is_admin == TRUE;
#else
    return geteuid() == 0;
#endif
}

bool QuickActionController::requestAdminElevation(const QString& reason) {
#ifdef _WIN32
    // Show elevation prompt with reason
    QString app_path = QCoreApplication::applicationFilePath();
    QString params = QString("--elevated --reason \"%1\"").arg(reason);

    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = reinterpret_cast<LPCWSTR>(app_path.utf16());
    sei.lpParameters = reinterpret_cast<LPCWSTR>(params.utf16());
    sei.nShow = SW_SHOWNORMAL;

    if (ShellExecuteExW(&sei)) {
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            DWORD exit_code = 0;
            GetExitCodeProcess(sei.hProcess, &exit_code);
            CloseHandle(sei.hProcess);
            return exit_code == 0;
        }
        return true;
    }
    return false;
#else
    Q_UNUSED(reason)
    return false;
#endif
}

void QuickActionController::scanAction(const QString& action_name) {
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
    QuickAction* action = getAction(action_name);
    if (!action) {
        Q_EMIT logMessage(QString("Action not found: %1").arg(action_name));
        return;
    }

    // Check admin requirements
    if (action->requiresAdmin() && !hasAdminPrivileges()) {
        // Check if already executing
        if (m_current_execution_action) {
            // Queue for later
            m_action_queue.enqueue(action_name);
            Q_EMIT logMessage(QString("Action queued: %1").arg(action_name));
            return;
        }

        m_current_execution_action = action;
        Q_EMIT actionExecutionStarted(action);
        action->updateStatus(QuickAction::ActionStatus::Running);
        Q_EMIT actionExecutionProgress(action, "Requesting administrator approval...", 5);
        logOperation(action, "Requesting administrator elevation");

        QString temp_dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
        QDir().mkpath(temp_dir);
        QString safe_name = action->name();
        safe_name.replace(' ', '_');
        QString result_file = QDir(temp_dir).filePath(
            QString("sak_quick_action_%1_%2.json")
                .arg(safe_name, QUuid::createUuid().toString(QUuid::Id128)));

        QString backup_location = m_backup_location.isEmpty() ? "C:/SAK_Backups" : m_backup_location;

        QStringList args;
        args << "--run-quick-action" << action->name();
        args << "--backup-location" << backup_location;
        args << "--result-file" << result_file;

        QString arg_string;
        for (const auto& arg : args) {
            if (!arg_string.isEmpty()) {
                arg_string += ' ';
            }
            QString escaped = arg;
            escaped.replace('"', "\\\"");
            if (escaped.contains(' ')) {
                arg_string += QString("\"%1\"").arg(escaped);
            } else {
                arg_string += escaped;
            }
        }

        const QString exe_path = QCoreApplication::applicationFilePath();
        auto elevation_result = ElevationManager::execute_elevated(
            exe_path.toStdString(),
            arg_string.toStdString(),
            true);

        QuickAction::ExecutionResult result;
        QuickAction::ActionStatus status = QuickAction::ActionStatus::Failed;

        if (!elevation_result) {
            result.success = false;
            result.message = "Administrator privileges required but not granted";
            result.log = "Elevation request failed or was cancelled";
            status = QuickAction::ActionStatus::Failed;
        } else {
            QString error_message;
            if (!readExecutionResultFile(result_file, &result, &status, &error_message)) {
                result.success = false;
                result.message = "Failed to read elevated action result";
                result.log = error_message;
                status = QuickAction::ActionStatus::Failed;
            }
        }

        QFile::remove(result_file);

        action->applyExecutionResult(result, status);
        onExecutionComplete();
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
    if (!m_current_execution_action) {
        return;
    }

    QuickAction* action = m_current_execution_action;
    m_current_execution_action = nullptr;

    Q_EMIT actionExecutionComplete(action);
    
    const auto& result = action->lastExecutionResult();
    qint64 duration_sec = result.duration_ms / 1000;
    QString log_msg = result.success 
        ? QString("Execution complete: %1 (%2 bytes in %3s)")
            .arg(result.message).arg(result.bytes_processed).arg(duration_sec)
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
    QuickAction* action = m_current_execution_action ? m_current_execution_action : m_current_scan_action;
    if (action) {
        Q_EMIT actionError(action, error);
        logOperation(action, QString("ERROR: %1").arg(error));
    }
}

void QuickActionController::startScanWorker(QuickAction* action) {
    m_current_scan_action = action;
    Q_EMIT actionScanStarted(action);
    logOperation(action, "Scan started");

    // Create worker thread
    m_scan_thread = new QThread(this);
    action->moveToThread(m_scan_thread);

    // Connect completion
    connect(action, &QuickAction::scanComplete, this, [this](const QuickAction::ScanResult& result) {
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
    m_current_execution_action = action;
    Q_EMIT actionExecutionStarted(action);
    logOperation(action, "Execution started");

    // Create worker thread
    m_execution_thread = new QThread(this);
    action->moveToThread(m_execution_thread);

    // Connect completion
    connect(action, &QuickAction::executionComplete, this, [this](const QuickAction::ExecutionResult& result) {
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

} // namespace sak
