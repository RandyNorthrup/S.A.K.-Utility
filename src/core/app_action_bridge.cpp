// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Action-agnostic half of the app-action service: the async->sync bridge that runs
// one QuickActionController run to completion, and the generic registration of a
// controller's actions into an AppActionRegistry. Deliberately free of any concrete
// built-in action so it links (and unit-tests) without the whole actions subsystem.

#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"
#include "sak/worker_base.h"

#include <QEventLoop>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <functional>
#include <utility>

namespace sak {

namespace {

QString categoryToString(QuickAction::ActionCategory category) {
    switch (category) {
    case QuickAction::ActionCategory::SystemOptimization:
        return QStringLiteral("optimization");
    case QuickAction::ActionCategory::Maintenance:
        return QStringLiteral("maintenance");
    case QuickAction::ActionCategory::Troubleshooting:
        return QStringLiteral("troubleshooting");
    case QuickAction::ActionCategory::EmergencyRecovery:
        return QStringLiteral("recovery");
    }
    return QStringLiteral("other");
}

QString slugify(const QString& name) {
    QString slug;
    slug.reserve(name.size());
    bool last_underscore = false;
    for (const QChar ch : name.toLower()) {
        if (ch.isLetterOrNumber()) {
            slug.append(ch);
            last_underscore = false;
        } else if (!last_underscore && !slug.isEmpty()) {
            slug.append(QLatin1Char('_'));
            last_underscore = true;
        }
    }
    while (slug.endsWith(QLatin1Char('_'))) {
        slug.chop(1);
    }
    return slug;
}

AppActionResult resultFromRun(const QuickAction::ExecutionResult& result) {
    return appActionResultFromExecution({result.success,
                                         result.message,
                                         result.bytes_processed,
                                         result.files_processed,
                                         result.duration_ms,
                                         result.output_path});
}

// A fresh owner token per assistant call: the shared controller's signals are filtered by it,
// so this call never takes another caller's completion (a panel's run of the same action) as its
// own, and its Stop or timeout cancels only its own run.
QString nextAssistantRunOwner() {
    static std::atomic<quint64> next_run{0};
    return QStringLiteral("assistant#%1").arg(next_run.fetch_add(1) + 1);
}

// Ask @p controller, on its own thread, to cancel the run @p owner asked for -- which also
// reaches the elevated helper for an administrator action -- so it unwinds and reports itself.
void requestRunCancel(QuickActionController* controller, const QString& owner) {
    QMetaObject::invokeMethod(
        controller,
        [controller, owner]() { controller->cancelRunsOf(owner); },
        Qt::QueuedConnection);
}

// Poll the caller's cancel route during runQuickActionSync. The first Stop cancels this call's
// run; the wait then continues for the run's own result (it reports itself cancelled once it
// unwinds) and settles with appActionCancelledResult() only if that has not arrived within
// kAppActionStopGraceMs. The poll timer is a child of @p context, so it dies with the wait.
void watchForStop(QObject* context,
                  const AppActionCancelCheck& should_cancel,
                  const std::function<void()>& request_cancel,
                  const std::function<void(const AppActionResult&)>& settle,
                  const bool& done) {
    if (!should_cancel) {
        return;
    }
    auto* poll = new QTimer(context);
    QObject::connect(poll,
                     &QTimer::timeout,
                     context,
                     [poll, context, should_cancel, request_cancel, settle, &done]() {
                         if (done || !should_cancel()) {
                             return;
                         }
                         poll->stop();
                         request_cancel();
                         QTimer::singleShot(kAppActionStopGraceMs, context, [settle]() {
                             settle(appActionCancelledResult());
                         });
                     });
    poll->start(kAppActionCancelPollMs);
}

}  // namespace

AsyncActionInvocation::AsyncActionInvocation(int timeout_ms,
                                             AppActionCancelCheck should_cancel,
                                             int stop_grace_ms)
    : m_timeout_ms(timeout_ms)
    , m_stop_grace_ms(stop_grace_ms)
    , m_should_cancel(std::move(should_cancel)) {}

QObject* AsyncActionInvocation::context() {
    return &m_ctx;
}

void AsyncActionInvocation::finish(const AppActionResult& result) {
    if (m_done) {
        return;
    }
    m_done = true;
    m_result = result;
    m_loop.quit();
}

bool AsyncActionInvocation::isDone() const {
    return m_done;
}

void AsyncActionInvocation::setStopRequest(std::function<void()> request_stop) {
    m_request_stop = std::move(request_stop);
}

void AsyncActionInvocation::onCancelPoll() {
    if (m_done || m_stop_requested || !m_should_cancel()) {
        return;
    }
    m_stop_requested = true;
    m_cancel_poll.stop();
    if (!m_request_stop) {
        finish(appActionCancelledResult());
        return;
    }
    // Ask the operation to stop, then wait briefly for its OWN terminal signal: a cooperative op
    // reports itself cancelled (or its real result, if it finished first), which is the truth;
    // only an op that does not answer within the grace gets the generic cancelled result.
    m_request_stop();
    QTimer::singleShot(m_stop_grace_ms, &m_ctx, [this]() { finish(appActionCancelledResult()); });
}

AppActionResult AsyncActionInvocation::run(const std::function<void()>& start) {
    // Fail closed on reuse: a terminal result was already recorded, so a second run()
    // would launch a fresh operation, skip the loop (m_done is still set) and return
    // the stale prior result while the new op runs unobserved. Each operation needs its
    // own invocation -- reject rather than start() a duplicate nobody waits on.
    if (m_done) {
        return {false, QStringLiteral("AsyncActionInvocation already used"), {}};
    }
    // A Stop that arrived before the work started: never start it.
    if (m_should_cancel && m_should_cancel()) {
        finish(appActionCancelledResult());
        return m_result;
    }
    // Timer is owned by m_ctx (caller-thread affinity), so it fires on this thread
    // via the local loop even though the controller emits from a worker thread.
    QTimer::singleShot(m_timeout_ms, &m_ctx, [this]() {
        finish({false, QStringLiteral("Action timed out after %1 ms").arg(m_timeout_ms), {}});
    });
    if (m_should_cancel) {
        QObject::connect(&m_cancel_poll, &QTimer::timeout, &m_ctx, [this]() { onCancelPoll(); });
        m_cancel_poll.start(kAppActionCancelPollMs);
    }
    if (start) {
        start();
    }
    // A well-behaved async op completes later; but if start() finished (or failed)
    // synchronously, m_done is already set and we must not enter a loop that would
    // never be quit again.
    if (!m_done) {
        m_loop.exec();
    }
    m_cancel_poll.stop();
    return m_result;
}

void stopWorkerOnCancel(AsyncActionInvocation& invocation, ::WorkerBase& worker) {
    invocation.setStopRequest([&worker]() { worker.requestStop(); });
    QObject::connect(&worker, &::WorkerBase::cancelled, invocation.context(), [&invocation]() {
        invocation.finish(appActionCancelledResult());
    });
}

AppActionResult appActionResultFromExecution(const AppActionExecutionFields& fields) {
    QJsonObject data{{QStringLiteral("bytes_processed"), static_cast<double>(fields.bytes)},
                     {QStringLiteral("files_processed"), static_cast<double>(fields.files)},
                     {QStringLiteral("duration_ms"), static_cast<double>(fields.duration_ms)}};
    if (!fields.output_path.isEmpty()) {
        data.insert(QStringLiteral("output_path"), fields.output_path);
    }
    return {fields.success, fields.message, data};
}

AppActionResult runQuickActionSync(QuickActionController* controller,
                                   const QString& action_name,
                                   int timeout_ms,
                                   const AppActionCancelCheck& should_cancel,
                                   const QString& output_folder) {
    if (controller == nullptr) {
        return {false, QStringLiteral("No action controller"), {}};
    }
    if (controller->getAction(action_name) == nullptr) {
        return {false, QStringLiteral("Unknown action: %1").arg(action_name), {}};
    }
    if (should_cancel && should_cancel()) {
        return appActionCancelledResult();  // a Stop before the action started: never start it
    }

    const QString owner = nextAssistantRunOwner();
    // SAK-ALLOW-BLOCKING: nested loop on the CALLER's thread, which is the assistant's
    // worker thread, never the controller's. requestRun is marshaled with a queued
    // connection so the run cannot finish before exec() begins, and the timeout below
    // always quits the loop.
    QEventLoop loop;
    QObject ctx;  // caller-thread affinity: all lambdas run serially on this thread
    AppActionResult out{false, QStringLiteral("Action did not complete"), {}};
    bool done = false;
    const auto settle = [&done, &out, &loop](const AppActionResult& result) {
        if (done) {
            return;
        }
        done = true;
        out = result;
        loop.quit();
    };

    // The run's own result, carried by the signal: never read back from the action object,
    // which a run queued after this one may already be writing.
    QObject::connect(controller,
                     &QuickActionController::runFinished,
                     &ctx,
                     [&settle, owner](const QuickActionRun& run,
                                      const QuickAction::ExecutionResult& result,
                                      QuickAction::ActionStatus) {
                         if (run.owner == owner) {
                             settle(resultFromRun(result));
                         }
                     });
    const auto cancel_this_run = [controller, owner]() {
        requestRunCancel(controller, owner);
    };
    // On timeout, cancel the still-running (or still-queued) run before abandoning it: without
    // that a timed-out run leaves the controller busy and every later request queues behind it.
    QTimer::singleShot(timeout_ms, &ctx, [&settle, &done, cancel_this_run, timeout_ms]() {
        if (!done) {
            cancel_this_run();
            settle({false, QStringLiteral("Action timed out after %1 ms").arg(timeout_ms), {}});
        }
    });
    watchForStop(&ctx, should_cancel, cancel_this_run, settle, done);

    // Request the run on the controller's own thread. The assistant applied its own
    // human-gate/restore point before getting here; the run carries this call's output folder.
    const QuickActionRun run{action_name, owner, output_folder};
    QMetaObject::invokeMethod(
        controller, [controller, run]() { controller->requestRun(run); }, Qt::QueuedConnection);

    loop.exec();
    return out;
}

int registerQuickActionsInto(QuickActionController& controller,
                             AppActionRegistry& registry,
                             const QuickActionOutputFolder& output_folder) {
    int registered = 0;
    for (const QuickAction* action : controller.getAllActions()) {
        const QString name = action->name();
        AppActionDescriptor descriptor;
        descriptor.id = QStringLiteral("action.%1").arg(slugify(name));
        descriptor.title = name;
        descriptor.description = action->description();
        descriptor.category = categoryToString(action->category());
        // A QuickAction is mutating (which alone forces the per-action human gate and
        // lease) unless it states it changes nothing: System Inventory Report only reads
        // and writes its own report, so it runs without an approval. `destructive` stays
        // false: none of the built-in QuickActions are irreversible data-loss, and
        // QuickAction exposes no destructive flag today. If an irreversible action is ever
        // added, give QuickAction an isDestructive() and map it here so Unattended mode
        // forces a fresh restore point per invocation instead of the once-per-session offer.
        descriptor.read_only = action->isReadOnly();
        descriptor.mutating = !descriptor.read_only;
        descriptor.requires_admin = action->requiresAdmin();

        // QPointer so a controller destroyed before this stored thunk is ever invoked
        // fails closed (runQuickActionSync returns "No action controller") instead of
        // dereferencing a dangling raw pointer.
        const QPointer<QuickActionController> controller_ptr = &controller;
        // The action's own declared bound is the wait: one clock per run, so a step the action
        // is still allowed to finish is never cut off by a shorter generic ceiling here.
        const int timeout_ms = action->executionTimeoutMs();
        AppActionCancellableInvoke invoke = [controller_ptr, name, timeout_ms, output_folder](
                                                const QJsonObject&,
                                                const AppActionCancelCheck& should_cancel) {
            return runQuickActionSync(controller_ptr,
                                      name,
                                      timeout_ms,
                                      should_cancel,
                                      output_folder ? output_folder() : QString());
        };
        if (registry.registerCancellableAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    }
    return registered;
}

}  // namespace sak
