// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action_controller.h"

#include "sak/action_constants.h"
#include "sak/app_action_guards.h"
#include "sak/app_paths.h"
#include "sak/elevation_broker.h"
#include "sak/elevation_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/thread_custody.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <memory>
#include <ranges>
#include <utility>

namespace sak {

namespace {
constexpr int kThreadShutdownWaitMs = 10'000;
constexpr int kElevationRequestProgress = 5;
constexpr auto kBackupLocationPayloadKey = "backup_location";
constexpr auto kMessageResultKey = "message";
constexpr auto kLogResultKey = "log";
constexpr auto kInconclusiveResultKey = "inconclusive";
constexpr auto kWarningResultKey = "warning";
constexpr auto kOutputPathResultKey = "output_path";
constexpr auto kStatusResultKey = "status";

// What an elevated execution thread hands back. Shared between the thread, which writes it
// before quitting, and the controller's finished() handler, which reads it afterwards.
struct ElevatedExecutionOutcome {
    QuickAction::ExecutionResult result;
    QuickAction::ActionStatus status{QuickAction::ActionStatus::Failed};
};

// Everything a still-running action thread uses once its controller is gone. Members are
// destroyed in reverse order, so the thread object (finished by then) goes last.
struct RetainedActionRun {
    std::unique_ptr<QThread> thread;
    std::unique_ptr<QuickAction> action;
    std::unique_ptr<ElevationBroker> broker;
};

#ifdef SAK_CORE_TEST_HOOKS
std::function<std::unique_ptr<ElevationBroker>()>& brokerFactoryForTesting() {
    static std::function<std::unique_ptr<ElevationBroker>()> factory;
    return factory;
}
#endif

std::unique_ptr<ElevationBroker> makeElevationBroker() {
#ifdef SAK_CORE_TEST_HOOKS
    if (const auto& factory = brokerFactoryForTesting()) {
        return factory();
    }
#endif
    return std::make_unique<ElevationBroker>();
}

QuickAction::ExecutionResult failedResult(const QString& message) {
    QuickAction::ExecutionResult result;
    result.success = false;
    result.message = message;
    result.log = message;
    return result;
}

// The status a finished in-process run ends in: its own, unless that contradicts its result.
QuickAction::ActionStatus terminalStatus(const QuickAction::ExecutionResult& result,
                                         QuickAction::ActionStatus reported) {
    if (result.success) {
        return QuickAction::ActionStatus::Success;
    }
    return reported == QuickAction::ActionStatus::Cancelled ? QuickAction::ActionStatus::Cancelled
                                                            : QuickAction::ActionStatus::Failed;
}

QuickAction::ExecutionResult makeElevationFailureResult(sak::error_code error) {
    if (error == sak::error_code::elevation_denied) {
        QuickAction::ExecutionResult result;
        result.success = false;
        result.message = QStringLiteral("Administrator privileges required but not granted");
        result.log = QStringLiteral("User cancelled the UAC prompt");
        return result;
    }
    return failedResult(QStringLiteral("Elevated helper error: %1")
                            .arg(QString::fromStdString(std::string(sak::to_string(error)))));
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
    result.inconclusive = !result.success &&
                          broker_result->data[QString::fromLatin1(kInconclusiveResultKey)].toBool();
    result.warning = broker_result->data[QString::fromLatin1(kWarningResultKey)].toString();
    result.output_path = broker_result->data[QString::fromLatin1(kOutputPathResultKey)].toString();

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

QString elevatedTaskIdForQuickAction(const QString& action_name) {
    using namespace quick_action_elevation;
    constexpr std::array mappings{
        std::pair{kVerifySystemFilesName, kVerifySystemFilesTaskId},
        std::pair{kCheckDiskErrorsName, kCheckDiskErrorsTaskId},
        std::pair{kResetNetworkName, kResetNetworkTaskId},
        std::pair{kBackupBitLockerKeysName, kBackupBitLockerKeysTaskId},
        std::pair{kDefenderQuickScanName, kDefenderQuickScanTaskId},
        std::pair{kDefenderFullScanName, kDefenderFullScanTaskId},
        std::pair{kDefenderSignatureUpdateName, kDefenderSignatureUpdateTaskId},
    };
    const auto match = std::ranges::find_if(mappings, [&action_name](const auto& mapping) {
        return action_name == QString::fromLatin1(mapping.first);
    });
    return match == mappings.end() ? QString() : QString::fromLatin1(match->second);
}

QJsonObject buildElevatedActionPayload(const QString& task_id,
                                       const QString& output_folder,
                                       bool output_folder_warning_accepted) {
    QJsonObject payload;
    if (task_id != QString::fromLatin1(quick_action_elevation::kBackupBitLockerKeysTaskId)) {
        return payload;
    }
    const QString backup_location = output_folder.trimmed();
    if (backup_location.isEmpty()) {
        return payload;
    }
    payload[QString::fromLatin1(kBackupLocationPayloadKey)] = backup_location;
    if (output_folder_warning_accepted) {
        payload[QString::fromLatin1(quick_action_elevation::kOutputFolderWarningAcceptedKey)] =
            true;
    }
    return payload;
}

#ifdef SAK_CORE_TEST_HOOKS
void QuickActionController::setElevationBrokerFactoryForTesting(
    std::function<std::unique_ptr<ElevationBroker>()> factory) {
    brokerFactoryForTesting() = std::move(factory);
}
#endif

QuickActionController::QuickActionController(QObject* parent) : QObject(parent) {
    const QString log_dir = sak::app_paths::logsDirectory();
    if (!sak::app_paths::ensureDirectory(log_dir)) {
        sak::logWarning("Failed to create quick actions log directory: {}", log_dir.toStdString());
    }
    m_log_file_path = log_dir + "/quick_actions.log";
}

QuickActionController::~QuickActionController() {
    // Application exit: nothing queued starts, and the running action -- and an elevated task,
    // whose helper never sees the action's own flag -- is asked to stop, so the bounded join
    // below does not wait out the helper's full inactivity timeout.
    m_queue.clear();
    if (m_current_action != nullptr) {
        m_current_action->cancel();
    }
    if (m_current_elevated && m_broker) {
        m_broker->cancelCurrentTask();
    }
    retireThreadOnDestroy();
    // m_broker, unless custody took it, is destroyed with the controller; ~ElevationBroker shuts
    // the helper down.
}

void QuickActionController::retireThreadOnDestroy() {
    QThread* const running = std::exchange(m_thread, nullptr);
    if (running == nullptr) {
        return;
    }
    running->quit();
    if (running->wait(kThreadShutdownWaitMs)) {
        delete running;  // deleteLater would never fire: no event loop runs during teardown.
        return;
    }
    // Overran: never terminated and never waited on without bound -- reported, and retained
    // with everything it uses until it finishes.
    auto retained = std::make_shared<RetainedActionRun>();
    running->setParent(nullptr);
    retained->thread.reset(running);
    QuickAction* const action = m_current_action;
    const auto owned = std::ranges::find_if(m_actions, [action](const auto& candidate) {
        return candidate.get() == action;
    });
    if (owned != m_actions.end()) {
        retained->action = std::move(*owned);
        m_actions.erase(owned);
    }
    if (m_current_elevated) {
        retained->broker = std::move(m_broker);
    }
    ThreadCustody::retain(
        running,
        retained,
        QStringLiteral("QuickAction thread for '%1' still running %2 ms after its stop request")
            .arg(action != nullptr ? action->name() : QStringLiteral("unknown"))
            .arg(kThreadShutdownWaitMs));
}

QString QuickActionController::registerAction(std::unique_ptr<QuickAction> action) {
    if (!action) {
        return QString();
    }
    const QString action_name = action->name();

    // Fail closed: refuse a duplicate name instead of shadowing it. m_action_map.insert would
    // overwrite the key so getAction()/routing resolves only the newest object, while
    // getAllActions() would still list BOTH -- an ambiguous listing/routing split.
    if (m_action_map.contains(action_name)) {
        sak::logWarning("Rejected duplicate quick-action registration: {}",
                        action_name.toStdString());
        return QString();
    }

    QuickAction* action_ptr = action.get();
    m_actions.push_back(std::move(action));
    m_action_map.insert(action_name, action_ptr);

    // Progress and log lines come from the worker running the action; they are queued here
    // ahead of its completion, so they always land while that run is still the current one.
    connect(action_ptr,
            &QuickAction::executionProgress,
            this,
            [this, action_ptr](const QString& message, int percent) {
                if (m_current_action == action_ptr && m_current.has_value()) {
                    Q_EMIT runProgress(*m_current, message, percent);
                }
            });
    connect(action_ptr, &QuickAction::logMessage, this, [this, action_ptr](const QString& message) {
        if (m_current_action == action_ptr && m_current.has_value()) {
            logRun(*m_current, message);
        } else {
            writeLogLine(logLine(action_ptr->name(), message));
        }
    });

    writeLogLine(logLine(action_name, QStringLiteral("Action registered")));
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

void QuickActionController::requestRun(const QuickActionRun& run) {
    if (run.owner.trimmed().isEmpty()) {
        finishUnstarted(run,
                        QStringLiteral("Refused a quick-action request with no owner"),
                        QuickAction::ActionStatus::Failed);
        return;
    }
    if (getAction(run.action_name) == nullptr) {
        finishUnstarted(
            run,
            QStringLiteral("No quick action is registered as \"%1\"").arg(run.action_name),
            QuickAction::ActionStatus::Failed);
        return;
    }
    if (m_current.has_value()) {
        m_queue.enqueue(run);
        logRun(run, QStringLiteral("Queued behind %1").arg(m_current->action_name));
        Q_EMIT runQueued(run, m_current->action_name);
        return;
    }
    startRun(run);
}

void QuickActionController::cancelRunsOf(const QString& owner) {
    QList<QuickActionRun> dropped;
    QQueue<QuickActionRun> kept;
    for (const QuickActionRun& queued : std::as_const(m_queue)) {
        if (queued.owner == owner) {
            dropped << queued;
        } else {
            kept.enqueue(queued);
        }
    }
    m_queue = std::move(kept);
    for (const QuickActionRun& run : std::as_const(dropped)) {
        finishUnstarted(run,
                        QStringLiteral("Cancelled before it started"),
                        QuickAction::ActionStatus::Cancelled);
    }

    if (!m_current.has_value() || m_current->owner != owner) {
        return;
    }
    m_current_action->cancel();
    // An elevated action runs inside the helper process; the action's own cancel flag never
    // reaches it. Signal the broker so the privileged task actually stops.
    if (m_current_elevated && m_broker) {
        m_broker->cancelCurrentTask();
    }
    logRun(*m_current, QStringLiteral("Cancel requested"));
}

bool QuickActionController::hasRunsOf(const QString& owner) const {
    if (m_current.has_value() && m_current->owner == owner) {
        return true;
    }
    return std::ranges::any_of(m_queue,
                               [&owner](const QuickActionRun& run) { return run.owner == owner; });
}

void QuickActionController::startRun(const QuickActionRun& run) {
    QuickAction* action = getAction(run.action_name);
    Q_ASSERT(action);  // requestRun refuses an unknown name; actions are never unregistered
    m_current = run;
    m_current_action = action;
    m_current_elevated = action->requiresAdmin() && !ElevationManager::isElevated();
    action->clearCancellation();  // a stale cancel flag would abort this fresh run immediately
    // This run's own folder, applied as it starts: a run queued behind another caller's never
    // writes into the folder that caller chose.
    action->setOutputFolder(run.output_folder);
    action->setOutputFolderWarningAccepted(run.output_folder_warning_accepted);
    Q_EMIT runStarted(run);
    logRun(run, QStringLiteral("Execution started"));
    if (m_current_elevated) {
        startElevated(action);
    } else {
        startInProcess(action);
    }
}

void QuickActionController::startInProcess(QuickAction* action) {
    m_thread = new QThread(this);
    action->moveToThread(m_thread);

    // Drop any prior connection so a re-run reports its completion exactly once
    // (UniqueConnection is ignored for lambda connections).
    disconnect(action, &QuickAction::executionComplete, this, nullptr);
    connect(action,
            &QuickAction::executionComplete,
            this,
            [this, action](const QuickAction::ExecutionResult& result) {
                if (m_current_action == action) {
                    finishCurrent(result, terminalStatus(result, action->status()));
                }
            });
    connect(m_thread, &QThread::finished, action, [action]() {
        auto* app_thread = QCoreApplication::instance()->thread();
        if (action->thread() != app_thread) {
            action->moveToThread(app_thread);
        }
    });
    connect(m_thread, &QThread::started, action, &QuickAction::execute);
    m_thread->start();
}

QString QuickActionController::elevatedRunRefusal(const QString& task_id) const {
    if (task_id.isEmpty()) {
        return QStringLiteral("No elevated helper task is registered for this action");
    }
    // Fail closed: the folder goes to the ELEVATED helper as the place it writes with
    // administrator rights. A relative path (resolved against a mutable CWD), a UNC/device
    // literal or a path reached through a symlink/junction is refused rather than handed over.
    const QString folder = m_current->output_folder.trimmed();
    if (!folder.isEmpty() && (!QDir::isAbsolutePath(folder) || isNetworkOrDevicePath(folder) ||
                              pathReparseUnsafe(folder))) {
        return QStringLiteral(
                   "Refused output folder (needs an absolute local path with no "
                   "links): %1")
            .arg(folder);
    }
    return {};
}

ElevationBroker& QuickActionController::elevationBroker() {
    if (!m_broker) {
        m_broker = makeElevationBroker();
        connect(
            m_broker.get(),
            &ElevationBroker::progressUpdated,
            this,
            [this](int percent, const QString& status) {
                if (m_current.has_value() && m_current_elevated) {
                    Q_EMIT runProgress(*m_current, status, percent);
                }
            },
            Qt::QueuedConnection);
    }
    return *m_broker;
}

void QuickActionController::startElevated(QuickAction* action) {
    action->updateStatus(QuickAction::ActionStatus::Running);
    const QString task_id = elevatedTaskIdForQuickAction(action->name());
    if (const QString refusal = elevatedRunRefusal(task_id); !refusal.isEmpty()) {
        logRun(*m_current, refusal);
        const QuickAction::ExecutionResult result = failedResult(refusal);
        action->applyExecutionResult(result, QuickAction::ActionStatus::Failed);
        finishCurrent(result, QuickAction::ActionStatus::Failed);
        return;
    }
    const QJsonObject payload = buildElevatedActionPayload(
        task_id, m_current->output_folder, m_current->output_folder_warning_accepted);
    Q_EMIT runProgress(*m_current,
                       QStringLiteral("Requesting administrator approval..."),
                       kElevationRequestProgress);
    logRun(*m_current, QStringLiteral("Requesting administrator elevation via helper"));

    ElevationBroker* broker = &elevationBroker();
    m_thread = new QThread(this);
    auto* thread = m_thread;
    auto* context = new QObject();
    context->moveToThread(thread);
    // The thread touches only the broker and this shared outcome, never the controller: a
    // controller torn down mid-task can then retain the broker and thread in custody without
    // leaving the thread a dangling `this`. The outcome comes back through the thread's own
    // finished(), whose connection to this controller dies with it.
    auto outcome = std::make_shared<ElevatedExecutionOutcome>();
    const QString action_name = action->name();
    connect(thread,
            &QThread::started,
            context,
            [broker, outcome, thread, context, action_name, task_id, payload]() {
                outcome->result = makeElevatedActionResult(
                    broker->executeTask(task_id, action_name, payload), outcome->status);
                context->deleteLater();
                thread->quit();
            });
    connect(thread, &QThread::finished, this, [this, action, outcome]() {
        if (m_current_action == action) {
            action->applyExecutionResult(outcome->result, outcome->status);
            finishCurrent(outcome->result, outcome->status);
        }
    });
    thread->start();
}

void QuickActionController::joinCurrentThread() {
    QThread* thread = std::exchange(m_thread, nullptr);
    if (thread == nullptr) {
        return;
    }
    thread->quit();
    // SAK-ALLOW-BLOCKING: load-bearing. The in-process thread's finished handler moves the
    // action back to the controller's thread, and the next run calls action->moveToThread() --
    // which is only legal once that has happened. The run has already reported its completion,
    // so only its return path remains.
    thread->wait();
    thread->deleteLater();
}

void QuickActionController::finishCurrent(const QuickAction::ExecutionResult& result,
                                          QuickAction::ActionStatus status) {
    if (!m_current.has_value()) {
        return;
    }
    const QuickActionRun run = *std::exchange(m_current, std::nullopt);
    m_current_action = nullptr;
    m_current_elevated = false;
    // Joined before the public signal: a slot on runFinished may request a new run, and that
    // run's fresh thread must not be the one torn down here.
    joinCurrentThread();

    const qint64 duration_sec = result.duration_ms / kMillisecondsPerSecond;
    if (result.success) {
        logRun(run,
               QStringLiteral("Execution complete: %1 (%2 bytes in %3s)")
                   .arg(result.message)
                   .arg(result.bytes_processed)
                   .arg(duration_sec));
    } else if (result.inconclusive) {
        logRun(run, QStringLiteral("Execution finished, result unknown: %1").arg(result.message));
    } else {
        logRun(run, QStringLiteral("Execution failed: %1").arg(result.message));
    }
    // The action's own detail: per-drive results, backup paths, the CBS log path. Every panel shows
    // runLog in its Log, so this is the one place that makes it reach all of them.
    if (!result.log.trimmed().isEmpty()) {
        logRun(run, QStringLiteral("Details:\n%1").arg(result.log.trimmed()));
    }
    Q_EMIT runFinished(run, result, status);
    startNextQueued();
}

void QuickActionController::finishUnstarted(const QuickActionRun& run,
                                            const QString& message,
                                            QuickAction::ActionStatus status) {
    logRun(run, message);
    Q_EMIT runFinished(run, failedResult(message), status);
}

void QuickActionController::startNextQueued() {
    // A slot on runFinished may already have started a run; the queue then waits for that one.
    if (m_current.has_value() || m_queue.isEmpty()) {
        return;
    }
    startRun(m_queue.dequeue());
}

QString QuickActionController::logLine(const QString& action_name, const QString& message) {
    return QStringLiteral("[%1] %2: %3")
        .arg(QDateTime::currentDateTime().toString(Qt::ISODate), action_name, message);
}

void QuickActionController::logRun(const QuickActionRun& run, const QString& message) {
    // The file needs the date and time on each line. A caller's Log stamps every line with its
    // own time already, so a second stamp there read "[06:51:00] [2026-11-05T06:51:00] ...".
    Q_EMIT runLog(run, QStringLiteral("%1: %2").arg(run.action_name, message));
    writeLogLine(logLine(run.action_name, message));
}

namespace {

// Connects one run signal of @p controller to @p handler for the runs @p owner asked for, and no
// others. Queued, so a run refused inside requestRun() reaches its caller after the request
// returns; the connection ends with @p receiver. An empty handler connects nothing.
template <typename... Rest>
void connectOwnRuns(QuickActionController& controller,
                    void (QuickActionController::*signal)(const QuickActionRun&, Rest...),
                    QObject* receiver,
                    const QString& owner,
                    const std::function<void(const QuickActionRun&, Rest...)>& handler) {
    if (!handler) {
        return;
    }
    QObject::connect(
        &controller,
        signal,
        receiver,
        [owner, handler](const QuickActionRun& run, Rest... rest) {
            if (run.owner == owner) {
                handler(run, rest...);
            }
        },
        Qt::QueuedConnection);
}

}  // namespace

void connectRunsOf(QuickActionController& controller,
                   const QString& owner,
                   QObject* receiver,
                   QuickActionRunHandlers handlers) {
    connectOwnRuns(controller, &QuickActionController::runQueued, receiver, owner, handlers.queued);
    connectOwnRuns(
        controller, &QuickActionController::runStarted, receiver, owner, handlers.started);
    connectOwnRuns(
        controller, &QuickActionController::runProgress, receiver, owner, handlers.progress);
    connectOwnRuns(controller, &QuickActionController::runLog, receiver, owner, handlers.log);
    connectOwnRuns(
        controller, &QuickActionController::runFinished, receiver, owner, handlers.finished);
}

QString quickActionOutcomeText(const QuickActionRun& run,
                               const QuickAction::ExecutionResult& result,
                               QuickAction::ActionStatus status) {
    QString text;
    if (result.success) {
        text = QStringLiteral("Completed: %1").arg(run.action_name);
    } else if (status == QuickAction::ActionStatus::Cancelled) {
        text = QStringLiteral("Cancelled: %1").arg(run.action_name);
    } else if (result.inconclusive) {
        text = QStringLiteral("Result unknown: %1 - %2").arg(run.action_name, result.message);
    } else {
        text = QStringLiteral("Failed: %1 - %2").arg(run.action_name, result.message);
    }
    // Output saved even partly says where it went.
    if (!result.output_path.isEmpty()) {
        text += QStringLiteral(" - saved to %1").arg(QDir::toNativeSeparators(result.output_path));
    }
    if (!result.warning.isEmpty()) {
        text += QStringLiteral(" - %1").arg(result.warning);
    }
    return text;
}

QString quickActionQueuedText(const QuickActionRun& run, const QString& running_action) {
    return QStringLiteral("Queued: %1 - waits for %2 to finish")
        .arg(run.action_name, running_action);
}

void QuickActionController::writeLogLine(const QString& line) const {
    // Fail closed: if the log file has been replaced with a reparse point (junction/symlink), do
    // NOT append into a redirected target -- a possibly-elevated process must not be tricked into
    // writing elsewhere. The caller's runLog signal still delivered the line.
    if (QFileInfo(m_log_file_path).isSymLink()) {
        return;
    }
    QFile log_file(m_log_file_path);
    if (log_file.open(QIODevice::Append | QIODevice::Text)) {
        QTextStream stream(&log_file);
        stream << line << "\n";
    }
}

}  // namespace sak
