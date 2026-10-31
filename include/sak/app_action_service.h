// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_action_registry.h"
#include "sak/quick_action.h"

#include <QEventLoop>
#include <QObject>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>

/// @file app_action_service.h
/// @brief Bridges the app's headless QuickAction subsystem into the
/// AppActionRegistry so the AI assistant can run those actions headless.
///
/// Two pieces:
///  - runQuickActionSync(): a generic, GUI-free bridge that runs one
///    QuickActionController action to completion SYNCHRONOUSLY (the assistant
///    tool handler already runs on a worker thread and expects a value back),
///    without blocking the controller's own (GUI) thread. It marshals the
///    async executeAction() onto the controller's thread and pumps a local
///    event loop on the caller's thread until the completion/error signal.
///  - AppActionService: constructs an app-scoped QuickActionController with the
///    built-in technician QuickActions and registers each into an
///    AppActionRegistry with a thunk that calls runQuickActionSync().
class WorkerBase;  // sak/worker_base.h (global namespace)

namespace sak {

class QuickActionController;
class BackupBitlockerKeysAction;
class GenerateSystemReportAction;
class ScreenshotSettingsAction;

/// How often a running app action polls the caller's cancel route (AppActionCancelCheck).
inline constexpr int kAppActionCancelPollMs = 100;

/// After a Stop reaches a running app action, how long the action may take to confirm it with
/// its own result. Past this the wait ends with appActionCancelledResult() and the caller's
/// teardown stops the work, so a Stop never waits out the action's whole timeout.
inline constexpr int kAppActionStopGraceMs = 15'000;

/// Run @p action_name on @p controller to completion and return its result.
/// Confirmation is suppressed (require_confirmation=false): the assistant applies
/// its own human-gate/restore-point BEFORE invoking, so the controller must not
/// pop a GUI dialog. Returns a failed result on unknown action or timeout.
/// Safe to call from a non-GUI worker thread; does not block the controller's thread.
/// When @p should_cancel reads true the action is cancelled on the controller (which reaches
/// the elevated helper for an administrator action) and its own result is returned once it
/// unwinds, or appActionCancelledResult() after kAppActionStopGraceMs.
[[nodiscard]] AppActionResult runQuickActionSync(QuickActionController* controller,
                                                 const QString& action_name,
                                                 int timeout_ms = kDefaultQuickActionTimeoutMs,
                                                 const AppActionCancelCheck& should_cancel = {});

/// Register every action currently in @p controller into @p registry as an
/// AppAction. Each descriptor's id is "action.<slug-of-name>", category is mapped
/// from the QuickAction category, mutating is set (these run system changes), and
/// requires_admin is taken from the action. The invoke thunk runs the action headless
/// via runQuickActionSync(), bounded by the action's executionTimeoutMs() and stopped by the
/// caller's cancel route (each is registered cancellable). Returns the number registered.
/// Action-agnostic (no dependency on the concrete built-in actions), so it is unit-testable with
/// probes.
[[nodiscard]] int registerQuickActionsInto(QuickActionController& controller,
                                           AppActionRegistry& registry);

class AppActionService : public QObject {
    Q_OBJECT

public:
    /// @param backup_dir output/backup location handed to the actions that write
    /// files (system report, settings screenshots, BitLocker key backup).
    explicit AppActionService(const QString& backup_dir, QObject* parent = nullptr);
    ~AppActionService() override;

    AppActionService(const AppActionService&) = delete;
    AppActionService& operator=(const AppActionService&) = delete;
    AppActionService(AppActionService&&) = delete;
    AppActionService& operator=(AppActionService&&) = delete;

    /// Register every built-in QuickAction into @p registry as an AppAction whose
    /// invoke thunk runs it headless via runQuickActionSync(). Returns the count
    /// registered. Ids are stable ("action.<slug>").
    [[nodiscard]] int registerInto(AppActionRegistry& registry);

    void setOutputDirectory(const QString& output_directory);
    [[nodiscard]] QString outputDirectory() const { return m_backup_dir; }

private:
    std::unique_ptr<QuickActionController> m_controller;
    QString m_backup_dir;
    GenerateSystemReportAction* m_report_action{nullptr};
    ScreenshotSettingsAction* m_screenshot_action{nullptr};
    BackupBitlockerKeysAction* m_bitlocker_action{nullptr};
};

/// Generous default for a signal-driven read-only op (some file searches / scans
/// legitimately run for minutes). The assistant tool layer applies its own outer
/// timeout too.
inline constexpr int kDefaultAsyncActionTimeoutMs = 5 * 60 * 1000;

/// Runs an asynchronous, signal-completed app operation to completion
/// SYNCHRONOUSLY from a non-GUI worker thread, WITHOUT a GUI thread in the loop.
///
/// Usage (all on the same worker thread):
///   AsyncActionInvocation inv(timeout_ms);
///   QObject::connect(controller, &C::doneSignal, inv.context(),
///                    [&inv](Payload p){ inv.finish({true, "...", serialize(p)}); });
///   QObject::connect(controller, &C::failSignal, inv.context(),
///                    [&inv](QString e){ inv.finish({false, e, {}}); });
///   return inv.run([controller]{ controller->start(); });
///
/// context() is a QObject with the CALLER thread's affinity: connecting a
/// controller's completion signals to it means the slots are delivered (queued)
/// onto the caller thread and drained by the local event loop, no matter which
/// thread the controller emits from. finish() is idempotent and first-wins, so
/// the completion, error, and timeout paths race safely. run() installs the
/// timeout, invokes @p start, and pumps a local event loop until the first
/// finish(); it returns the recorded result. The controller must be created on
/// (and owned by) the caller thread and outlive the call -- on timeout it should
/// be destroyed after run() returns so its destructor can drain its worker.
///
/// Stop: with a cancel route (@p should_cancel), a Stop before start() means the work never
/// starts; a Stop while it runs calls the stop request (setStopRequest) once and keeps waiting
/// for the operation's own terminal signal for up to @p stop_grace_ms, then settles with
/// appActionCancelledResult(). Without a stop request a Stop settles at once. Either way the
/// caller's post-run teardown (requestStop + the owner's bounded join) stops the work.
class AsyncActionInvocation {
public:
    explicit AsyncActionInvocation(int timeout_ms = kDefaultAsyncActionTimeoutMs,
                                   AppActionCancelCheck should_cancel = {},
                                   int stop_grace_ms = kAppActionStopGraceMs);

    AsyncActionInvocation(const AsyncActionInvocation&) = delete;
    AsyncActionInvocation& operator=(const AsyncActionInvocation&) = delete;
    AsyncActionInvocation(AsyncActionInvocation&&) = delete;
    AsyncActionInvocation& operator=(AsyncActionInvocation&&) = delete;

    /// Receiver context for signal connections (caller-thread affinity).
    [[nodiscard]] QObject* context();

    /// Record the outcome and stop the loop. Idempotent: only the first call wins.
    void finish(const AppActionResult& result);

    /// Whether a terminal outcome has already been recorded.
    [[nodiscard]] bool isDone() const;

    /// What a Stop asks the running operation to do (e.g. worker.requestStop()). Runs once,
    /// on this thread, when the cancel route first reads true.
    void setStopRequest(std::function<void()> request_stop);

    /// Install the timeout, run @p start, pump the local loop until finish().
    [[nodiscard]] AppActionResult run(const std::function<void()>& start);

private:
    void onCancelPoll();

    // SAK-ALLOW-BLOCKING: nested loop on a WORKER thread, never the GUI thread. The
    // assistant's tool handler must return a value, and the op it drives is signal-
    // completed; run() enters this loop only after checking m_done, and finish() is
    // first-wins across the completion, error and timeout paths.
    QEventLoop m_loop;
    QObject m_ctx;
    QTimer m_cancel_poll;
    AppActionResult m_result{false, QStringLiteral("Action did not complete"), {}};
    bool m_done{false};
    bool m_stop_requested{false};
    int m_timeout_ms;
    int m_stop_grace_ms;
    AppActionCancelCheck m_should_cancel;
    std::function<void()> m_request_stop;
};

/// Make a Stop reach a WorkerBase-driven invocation: its stop request is worker.requestStop(),
/// and the worker's own cancelled() settles @p invocation with appActionCancelledResult().
void stopWorkerOnCancel(AsyncActionInvocation& invocation, ::WorkerBase& worker);

/// A QuickAction::ExecutionResult flattened to primitives, so the mapping to an
/// AppActionResult can live in the action-agnostic bridge and be unit-tested there.
struct AppActionExecutionFields {
    bool success{false};
    QString message;
    qint64 bytes{0};
    qint64 files{0};
    qint64 duration_ms{0};
    QString output_path;
};

/// Map an execution into the AppAction result (success/message + a data payload).
/// Exposed for tests.
[[nodiscard]] AppActionResult appActionResultFromExecution(const AppActionExecutionFields& fields);

}  // namespace sak
