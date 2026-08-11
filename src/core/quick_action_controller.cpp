// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action_controller.h"

#include "sak/app_action_guards.h"
#include "sak/app_paths.h"
#include "sak/elevation_broker.h"
#include "sak/elevation_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/quick_action_result_io.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTextStream>

namespace sak {

namespace {
constexpr int kThreadShutdownWaitMs = 10'000;
}

namespace {
constexpr int kElevationRequestProgress = 5;
constexpr auto kBackupBitLockerKeysAction = "Backup BitLocker Keys";
constexpr auto kDefaultBackupLocation = "C:/SAK_Backups";
constexpr auto kBackupLocationPayloadKey = "backup_location";
constexpr auto kMessageResultKey = "message";
constexpr auto kLogResultKey = "log";
constexpr auto kStatusResultKey = "status";

QJsonObject buildElevatedActionPayload(const QString& action_name,
                                       const QString& configuredBackupLocation) {
    QJsonObject payload;
    if (action_name != QString::fromLatin1(kBackupBitLockerKeysAction)) {
        return payload;
    }

    const QString backup_location = configuredBackupLocation.isEmpty()
                                        ? QString::fromLatin1(kDefaultBackupLocation)
                                        : configuredBackupLocation;
    payload[QString::fromLatin1(kBackupLocationPayloadKey)] = backup_location;
    return payload;
}

QuickAction::ExecutionResult makeElevationFailureResult(sak::error_code error) {
    QuickAction::ExecutionResult result;
    result.success = false;

    if (error == sak::error_code::elevation_denied) {
        result.message = QStringLiteral("Administrator privileges required but not granted");
        result.log = QStringLiteral("User cancelled the UAC prompt");
        return result;
    }

    result.message = QStringLiteral("Elevated helper error: %1")
                         .arg(QString::fromStdString(std::string(sak::to_string(error))));
    result.log = result.message;
    return result;
}

QuickAction::ExecutionResult makeElevatedActionResult(
    const std::expected<ElevatedTaskResult, sak::error_code>& broker_result,
    QuickAction::ActionStatus& status) {
    if (!broker_result) {
        return makeElevationFailureResult(broker_result.error());
    }

    QuickAction::ExecutionResult result;
    result.success = broker_result->success;
    result.message = broker_result->data[QString::fromLatin1(kMessageResultKey)].toString();
    result.log = broker_result->data[QString::fromLatin1(kLogResultKey)].toString();

    // Decode the status defensively: never cast an out-of-range integer to the enum, and
    // never surface a status that contradicts the authoritative success flag. A malformed
    // helper response therefore cannot yield an invalid enum value or a success/failure
    // mismatch.
    int status_int = broker_result->data[QString::fromLatin1(kStatusResultKey)].toInt(
        static_cast<int>(QuickAction::ActionStatus::Failed));
    if (status_int < static_cast<int>(QuickAction::ActionStatus::Idle) ||
        status_int > static_cast<int>(QuickAction::ActionStatus::Cancelled)) {
        status_int = static_cast<int>(QuickAction::ActionStatus::Failed);
    }
    status = static_cast<QuickAction::ActionStatus>(status_int);
    if (result.success) {
        status = QuickAction::ActionStatus::Success;
    } else if (status == QuickAction::ActionStatus::Success) {
        status = QuickAction::ActionStatus::Failed;
    }

    if (!broker_result->success && result.message.isEmpty()) {
        result.message = broker_result->error_message;
    }
    return result;
}
}  // namespace

QuickActionController::QuickActionController(QObject* parent) : QObject(parent) {
    // Setup log file path
    const QString log_dir = sak::app_paths::logsDirectory();
    if (!sak::app_paths::ensureDirectory(log_dir)) {
        sak::logWarning("Failed to create quick actions log directory: {}", log_dir.toStdString());
    }
    m_log_file_path = log_dir + "/quick_actions.log";
}

QuickActionController::~QuickActionController() {
    // Cancel any running operations
    if (m_current_scan_action != nullptr) {
        m_current_scan_action->cancel();
    }
    if (m_current_execution_action != nullptr) {
        m_current_execution_action->cancel();
    }
    // Reach any in-flight ELEVATED task too: cancelling the action only sets its flag,
    // which the helper process never sees. cancelCurrentTask() is safe to call while
    // executeTask() blocks on the worker, and lets that blocking call return so the join
    // below does not wait out the helper's full inactivity timeout.
    if (m_broker != nullptr) {
        m_broker->cancelCurrentTask();
    }

    // Join before freeing. The owned QuickActions (m_actions) are destroyed right
    // after this dtor body, so a thread still running scan()/execute() would use
    // freed state. Block until the thread actually stops, then delete it directly
    // (deleteLater would never fire -- the event loop does not run during teardown).
    auto cleanupThread = [](QThread*& thread) {
        if (!thread) {
            return;
        }
        thread->quit();
        if (!thread->wait(kThreadShutdownWaitMs)) {
            sak::logError("QuickAction thread slow to stop; blocking until joined");
            // SAK-ALLOW-BLOCKING: `delete thread` follows and the owned QuickActions are freed
            // right after this dtor body, so a thread still in scan()/execute() would use freed
            // state. The bounded wait above already gave up once; the escalation is logged.
            thread->wait();
        }
        delete thread;
        thread = nullptr;
    };

    cleanupThread(m_scan_thread);
    cleanupThread(m_execution_thread);
}

void QuickActionController::setLoggingEnabled(bool enabled) {
    m_logging_enabled = enabled;
}

void QuickActionController::setBackupLocation(const QString& backup_location) {
    const QString candidate = backup_location.trimmed();
    if (candidate.isEmpty()) {
        // Explicit reset: buildElevatedActionPayload then uses the built-in default.
        m_backup_location.clear();
        return;
    }

    // Fail closed: this string is handed to the ELEVATED helper as the BitLocker key backup
    // destination. A relative path (resolved against a mutable CWD), a UNC/device literal or
    // a path reached through a symlink/junction is REFUSED here rather than stored and then
    // written to with administrator rights. The setter cannot report failure, so refusing
    // (and saying so on logMessage) is the only fail-closed answer.
    if (!QDir::isAbsolutePath(candidate) || isNetworkOrDevicePath(candidate) ||
        pathReparseUnsafe(candidate)) {
        sak::logWarning("Rejected quick-action backup location: {}", candidate.toStdString());
        Q_EMIT logMessage(
            QStringLiteral(
                "Rejected backup location (needs an absolute local path with no links): %1")
                .arg(candidate));
        return;
    }

    m_backup_location = candidate;
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

    connect(
        action_ptr, &QuickAction::scanProgress, this, [this, action_ptr](const QString& message) {
            logOperation(action_ptr, QString("Scanning: %1").arg(message));
        });

    connect(action_ptr,
            &QuickAction::executionProgress,
            this,
            [this, action_ptr](const QString& msg, int prog) {
                const QString message = QString("%1 - %2%").arg(msg).arg(prog);
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
    QuickAction* action = getAction(action_name);
    if (action == nullptr) {
        Q_EMIT logMessage(QString("Action not found: %1").arg(action_name));
        return;
    }

    // Fail closed: never scan an action that is currently executing. On the non-elevated
    // path it lives on the execution worker thread, so moving it to the scan thread from
    // here would fail and leave it wedged. Skip it, but keep the scan queue moving so a
    // scanAllActions() sweep does not stall on the running action.
    if (action == m_current_execution_action) {
        Q_EMIT logMessage(QString("Skipped scan of '%1' while it is executing").arg(action_name));
        if ((m_current_scan_action == nullptr) && !m_scan_queue.isEmpty()) {
            const QString next_action = m_scan_queue.dequeue();
            scanAction(next_action);
        }
        return;
    }

    // Check admin requirements - but allow scan to proceed
    // Scan can determine if action is applicable regardless of admin status
    // Admin check will happen again before execution
    if (action->requiresAdmin() && !hasAdminPrivileges()) {
        logOperation(action, "Note: Action requires admin privileges for execution");
    }

    // Check if already scanning
    if (m_current_scan_action != nullptr) {
        m_scan_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Scan queued: %1").arg(action_name));
        return;
    }

    startScanWorker(action);
}

void QuickActionController::executeAction(const QString& action_name, bool require_confirmation) {
    // Fail closed: this controller has no confirmation channel, so a request that still needs
    // confirmation must NOT run. Ignoring the flag (as this did) executed a destructive action
    // unconfirmed. The caller confirms with the user and re-requests with false. Checked
    // BEFORE the elevation branch so an admin action cannot slip past it either.
    if (require_confirmation) {
        Q_EMIT logMessage(
            QStringLiteral("Refused unconfirmed execution request for '%1': confirm the action "
                           "first, then request execution with require_confirmation = false")
                .arg(action_name));
        return;
    }

    QuickAction* action = getAction(action_name);
    if (action == nullptr) {
        Q_EMIT logMessage(QString("Action not found: %1").arg(action_name));
        return;
    }

    // Fail closed: an action currently on the scan worker thread cannot also be moved to
    // the execution thread -- a second moveToThread() from the wrong thread would fail and
    // wedge the action. Refuse to start execution while it is still scanning.
    if (action == m_current_scan_action) {
        Q_EMIT logMessage(
            QString("Cannot execute '%1' while it is still scanning").arg(action_name));
        return;
    }

    // Check admin requirements
    if (action->requiresAdmin() && !hasAdminPrivileges()) {
        executeElevatedAction(action, action_name);
        return;
    }

    // Check if already executing
    if (m_current_execution_action != nullptr) {
        // Queue for later
        m_action_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Action queued: %1").arg(action_name));
        return;
    }

    startExecutionWorker(action);
}

void QuickActionController::executeElevatedAction(QuickAction* action, const QString& action_name) {
    // executeAction is the only caller and returns early when getAction() found no
    // action for this name.
    Q_ASSERT(action);
    if (m_current_execution_action != nullptr) {
        m_action_queue.enqueue(action_name);
        Q_EMIT logMessage(QString("Action queued: %1").arg(action_name));
        return;
    }

    m_current_execution_action = action;
    Q_EMIT actionExecutionStarted(action);
    action->updateStatus(QuickAction::ActionStatus::Running);
    Q_EMIT actionExecutionProgress(action,
                                   QStringLiteral("Requesting administrator approval..."),
                                   kElevationRequestProgress);
    logOperation(action, QStringLiteral("Requesting administrator elevation via helper"));

    const QJsonObject payload = buildElevatedActionPayload(action_name, m_backup_location);

    // Own the broker on the controller thread (via the m_broker member that existed for
    // exactly this) so cancelCurrentAction()/the destructor can reach an in-flight task.
    // The broker is designed for executeTask() on a worker while cancelCurrentTask() runs
    // on the controller thread. Single-flight is guaranteed by the m_current_execution_action
    // guard above, so m_broker is null here; it is released in onExecutionComplete().
    m_broker = new ElevationBroker(this);
    connect(
        m_broker,
        &ElevationBroker::progressUpdated,
        this,
        [this](int percent, const QString& status) {
            if (m_current_execution_action) {
                Q_EMIT actionExecutionProgress(m_current_execution_action, status, percent);
            }
        },
        Qt::QueuedConnection);

    m_execution_thread = new QThread(this);
    auto* thread = m_execution_thread;
    auto* context = new QObject();
    context->moveToThread(thread);
    connect(thread,
            &QThread::started,
            context,
            [this, thread, context, action, action_name, payload]() {
                QuickAction::ExecutionResult result;
                QuickAction::ActionStatus status = QuickAction::ActionStatus::Failed;
                result = makeElevatedActionResult(
                    m_broker->executeTask(action_name, action_name, payload), status);

                QMetaObject::invokeMethod(
                    this,
                    [this, action, result, status]() {
                        action->applyExecutionResult(result, status);
                        onExecutionComplete();
                    },
                    Qt::QueuedConnection);
                context->deleteLater();
                thread->quit();
            });
    thread->start();
}

void QuickActionController::scanAllActions() {
    // Queue all registered actions for sequential scan
    m_scan_queue.clear();
    for (const auto& action : m_actions) {
        m_scan_queue.enqueue(action->name());
    }

    if ((m_current_scan_action == nullptr) && !m_scan_queue.isEmpty()) {
        const QString next_action = m_scan_queue.dequeue();
        scanAction(next_action);
    }
}

void QuickActionController::cancelCurrentAction() {
    if (m_current_scan_action != nullptr) {
        m_current_scan_action->cancel();
        logOperation(m_current_scan_action, "Scan cancelled by user");
    }
    if (m_current_execution_action != nullptr) {
        m_current_execution_action->cancel();
        // An elevated action runs inside the helper process; the action's own cancel flag
        // never reaches it. Signal the broker so the privileged task actually stops.
        if (m_broker != nullptr) {
            m_broker->cancelCurrentTask();
        }
        logOperation(m_current_execution_action, "Execution cancelled by user");
    }
}

void QuickActionController::onScanComplete() {
    if (m_current_scan_action == nullptr) {
        return;
    }

    QuickAction* action = m_current_scan_action;
    m_current_scan_action = nullptr;

    // Detach and join the worker thread BEFORE emitting the public completion signal. A
    // slot on actionScanComplete may re-enter and start a new scan; nulling the member
    // first means that reentrant run's fresh m_scan_thread is not torn down here, and the
    // old thread pointer cannot be overwritten before it has been joined.
    if (QThread* thread = m_scan_thread) {
        m_scan_thread = nullptr;
        thread->quit();
        // SAK-ALLOW-BLOCKING: load-bearing, not just teardown hygiene. The thread's finished
        // handler moves `action` back to the app thread, and the next queued scan calls
        // action->moveToThread() -- which is only legal once that has happened. Dropping this
        // join would move an object that still lives on a running thread. The action has
        // already emitted its completion signal, so only its return path remains.
        thread->wait();
        thread->deleteLater();
    }

    Q_EMIT actionScanComplete(action);
    logOperation(action, QString("Scan complete: %1").arg(action->lastScanResult().summary));

    // Process scan queue
    if (!m_scan_queue.isEmpty()) {
        const QString next_action = m_scan_queue.dequeue();
        scanAction(next_action);
    }
}

void QuickActionController::onExecutionComplete() {
    if (m_current_execution_action == nullptr) {
        return;
    }

    QuickAction* action = m_current_execution_action;
    m_current_execution_action = nullptr;

    // Release the elevated broker (only set on an elevated run). It is created and
    // destroyed on this controller thread, and the worker has already returned from
    // executeTask() by the time this completion handler runs.
    if (m_broker != nullptr) {
        m_broker->deleteLater();
        m_broker = nullptr;
    }

    // Detach and join the worker thread BEFORE emitting the public completion signal, for
    // the same reentrancy reason as onScanComplete(): a slot on actionExecutionComplete may
    // start a new run, and that fresh m_execution_thread must not be torn down here.
    if (QThread* thread = m_execution_thread) {
        m_execution_thread = nullptr;
        thread->quit();
        // SAK-ALLOW-BLOCKING: same load-bearing contract as onScanComplete() -- the next
        // queued action calls action->moveToThread(), which is only legal once this thread's
        // finished handler has moved the action back to the app thread.
        thread->wait();
        thread->deleteLater();
    }

    Q_EMIT actionExecutionComplete(action);

    const auto& result = action->lastExecutionResult();
    const qint64 duration_sec = result.duration_ms / kMillisecondsPerSecond;
    const QString log_msg = result.success ? QString("Execution complete: %1 (%2 bytes in %3s)")
                                                 .arg(result.message)
                                                 .arg(result.bytes_processed)
                                                 .arg(duration_sec)
                                           : QString("Execution failed: %1").arg(result.message);
    logOperation(action, log_msg);

    // Process queue
    if (!m_action_queue.isEmpty()) {
        const QString next_action = m_action_queue.dequeue();
        executeAction(next_action, false);
    }
}

void QuickActionController::startScanWorker(QuickAction* action) {
    // scanAction is the only caller and returns early on an unknown action name.
    Q_ASSERT(action);
    m_current_scan_action = action;
    action->clearCancellation();  // a stale cancel flag would abort this fresh run immediately
    Q_EMIT actionScanStarted(action);
    logOperation(action, "Scan started");

    // Create worker thread
    m_scan_thread = new QThread(this);
    action->moveToThread(m_scan_thread);

    // Connect completion (drop any prior connection so re-running an action fires onScanComplete
    // exactly once; UniqueConnection is ignored for lambda connections).
    disconnect(action, &QuickAction::scanComplete, this, nullptr);
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
    // executeAction is the only caller and returns early on an unknown action name.
    Q_ASSERT(action);
    m_current_execution_action = action;
    action->clearCancellation();  // a stale cancel flag would abort this fresh run immediately
    Q_EMIT actionExecutionStarted(action);
    logOperation(action, "Execution started");

    // Create worker thread
    m_execution_thread = new QThread(this);
    action->moveToThread(m_execution_thread);

    // Connect completion (drop any prior connection so re-running fires onExecutionComplete once)
    disconnect(action, &QuickAction::executionComplete, this, nullptr);
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
    // Private helper: every call site in this file sits behind a null check on the
    // action (or holds a just-registered pointer) and passes a literal-prefixed
    // message, so both hold by construction.
    Q_ASSERT(action);
    Q_ASSERT(!message.isEmpty());
    if (!m_logging_enabled) {
        return;
    }

    const QString timestamp = QDateTime::currentDateTime().toString(Qt::ISODate);
    const QString log_entry = QString("[%1] %2: %3").arg(timestamp, action->name(), message);

    Q_EMIT logMessage(log_entry);

    // Fail closed: if the log file has been replaced with a reparse point (junction/
    // symlink), do NOT append into a redirected target -- a possibly-elevated process must
    // not be tricked into writing elsewhere. The in-memory logMessage signal above still
    // delivered the line.
    if (QFileInfo(m_log_file_path).isSymLink()) {
        return;
    }

    // Write to file
    QFile log_file(m_log_file_path);
    if (log_file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log_file);
        stream << log_entry << "\n";
    }
}

}  // namespace sak
