// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

// Action-agnostic half of the app-action service: the async->sync bridge that runs
// a QuickActionController action to completion, and the generic registration of a
// controller's actions into an AppActionRegistry. Deliberately free of any concrete
// built-in action so it links (and unit-tests) without the whole actions subsystem.

#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"

#include <QEventLoop>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

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

AppActionResult resultFromAction(const QuickAction* action) {
    const QuickAction::ExecutionResult r = action->lastExecutionResult();
    return appActionResultFromExecution(
        {r.success, r.message, r.bytes_processed, r.files_processed, r.duration_ms, r.output_path});
}

}  // namespace

AsyncActionInvocation::AsyncActionInvocation(int timeout_ms) : m_timeout_ms(timeout_ms) {}

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

AppActionResult AsyncActionInvocation::run(const std::function<void()>& start) {
    // Fail closed on reuse: a terminal result was already recorded, so a second run()
    // would launch a fresh operation, skip the loop (m_done is still set) and return
    // the stale prior result while the new op runs unobserved. Each operation needs its
    // own invocation -- reject rather than start() a duplicate nobody waits on.
    if (m_done) {
        return {false, QStringLiteral("AsyncActionInvocation already used"), {}};
    }
    // Timer is owned by m_ctx (caller-thread affinity), so it fires on this thread
    // via the local loop even though the controller emits from a worker thread.
    QTimer::singleShot(m_timeout_ms, &m_ctx, [this]() {
        finish({false, QStringLiteral("Action timed out after %1 ms").arg(m_timeout_ms), {}});
    });
    if (start) {
        start();
    }
    // A well-behaved async op completes later; but if start() finished (or failed)
    // synchronously, m_done is already set and we must not enter a loop that would
    // never be quit again.
    if (!m_done) {
        m_loop.exec();
    }
    return m_result;
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
                                   int timeout_ms) {
    if (controller == nullptr) {
        return {false, QStringLiteral("No action controller"), {}};
    }
    const QuickAction* action = controller->getAction(action_name);
    if (action == nullptr) {
        return {false, QStringLiteral("Unknown action: %1").arg(action_name), {}};
    }

    // SAK-ALLOW-BLOCKING: nested loop on the CALLER's thread, which is the assistant's
    // worker thread, never the controller's. executeAction is marshaled with a queued
    // connection so it cannot complete before exec() begins, and the timeout below
    // always quits the loop.
    QEventLoop loop;
    const QObject ctx;  // caller-thread affinity: all lambdas run serially on this thread
    AppActionResult out{false, QStringLiteral("Action did not complete"), {}};
    bool done = false;

    QObject::connect(controller,
                     &QuickActionController::actionExecutionComplete,
                     &ctx,
                     [&, action](QuickAction* finished) {
                         if (done || finished != action) {
                             return;
                         }
                         done = true;
                         out = resultFromAction(action);
                         loop.quit();
                     });
    QObject::connect(controller,
                     &QuickActionController::actionError,
                     &ctx,
                     [&, action](QuickAction* failed, const QString& error) {
                         if (done || failed != action) {
                             return;
                         }
                         done = true;
                         out = {false, error, {}};
                         loop.quit();
                     });
    QTimer::singleShot(timeout_ms, &ctx, [&, controller]() {
        if (done) {
            return;
        }
        done = true;
        out = {false, QStringLiteral("Action timed out after %1 ms").arg(timeout_ms), {}};
        // Request cancellation of the still-running action on the controller's thread
        // before we abandon it. Without this a timed-out action leaves the controller
        // marked busy, and every later action would be silently queued behind it.
        // Best-effort: a well-behaved action honors the cooperative cancel and unwinds.
        QMetaObject::invokeMethod(
            controller,
            [controller]() { controller->cancelCurrentAction(); },
            Qt::QueuedConnection);
        loop.quit();
    });

    // Run the action on the controller's own thread; false = no GUI confirmation
    // (the assistant applies its own human-gate/restore before we get here).
    QMetaObject::invokeMethod(
        controller,
        [controller, action_name]() { controller->executeAction(action_name, false); },
        Qt::QueuedConnection);

    loop.exec();
    return out;
}

int registerQuickActionsInto(QuickActionController& controller, AppActionRegistry& registry) {
    int registered = 0;
    for (const QuickAction* action : controller.getAllActions()) {
        const QString name = action->name();
        AppActionDescriptor descriptor;
        descriptor.id = QStringLiteral("action.%1").arg(slugify(name));
        descriptor.title = name;
        descriptor.description = action->description();
        descriptor.category = categoryToString(action->category());
        // Every QuickAction performs a system change, so all are mutating (this alone
        // forces the per-action human gate). `destructive` stays false: none of the
        // built-in QuickActions are irreversible data-loss, and QuickAction exposes no
        // destructive flag today. If an irreversible action is ever added, give
        // QuickAction an isDestructive() and map it here so Unattended mode forces a
        // fresh restore point per invocation instead of the once-per-session offer.
        descriptor.mutating = true;
        descriptor.requires_admin = action->requiresAdmin();

        // QPointer so a controller destroyed before this stored thunk is ever invoked
        // fails closed (runQuickActionSync returns "No action controller") instead of
        // dereferencing a dangling raw pointer.
        const QPointer<QuickActionController> controller_ptr = &controller;
        AppActionInvoke invoke = [controller_ptr, name](const QJsonObject&) {
            return runQuickActionSync(controller_ptr, name);
        };
        if (registry.registerAction(descriptor, std::move(invoke))) {
            ++registered;
        }
    }
    return registered;
}

}  // namespace sak
