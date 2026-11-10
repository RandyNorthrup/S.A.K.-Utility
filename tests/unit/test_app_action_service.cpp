// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/actions/generate_system_report_action.h"
#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"
#include "sak/worker_base.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>
#include <QVector>

#include <atomic>
#include <expected>
#include <memory>

using sak::AppActionDescriptor;
using sak::AppActionRegistry;
using sak::AppActionResult;
using sak::AsyncActionInvocation;
using sak::QuickAction;
using sak::QuickActionController;
using sak::registerQuickActionsInto;
using sak::runQuickActionSync;

namespace {

// Fake signal-completed op for exercising AsyncActionInvocation deterministically.
// Terminal signals are emitted via a timer so the bridge's local event loop is
// genuinely entered before completion, as with a real worker.
class FakeAsyncOp : public QObject {
    Q_OBJECT

public:
    void scheduleDone(int delay_ms, int value) {
        QTimer::singleShot(delay_ms, this, [this, value]() { Q_EMIT done(value); });
    }
    void scheduleFail(int delay_ms, const QString& error) {
        QTimer::singleShot(delay_ms, this, [this, error]() { Q_EMIT failed(error); });
    }
    // Emit both terminal signals back-to-back to prove finish() is first-wins.
    void scheduleDoneThenFail(int delay_ms, int value, const QString& error) {
        QTimer::singleShot(delay_ms, this, [this, value, error]() {
            Q_EMIT done(value);
            Q_EMIT failed(error);
        });
    }
    void scheduleCancelled(int delay_ms) {
        QTimer::singleShot(delay_ms, this, [this]() { Q_EMIT cancelled(); });
    }
    void scheduleBatchesThenDone(int delay_ms) {
        QTimer::singleShot(delay_ms, this, [this]() {
            Q_EMIT batch(10);
            Q_EMIT batch(20);
            Q_EMIT done(2);
        });
    }

Q_SIGNALS:
    void done(int value);
    void failed(const QString& error);
    void batch(int value);
    void cancelled();
};

}  // namespace

namespace {

// Minimal headless QuickAction for exercising the async->sync bridge. execute()
// runs on the controller's worker thread; it blocks until released so the timeout
// path can be tested deterministically without a teardown race.
class ProbeAction : public QuickAction {
public:
    explicit ProbeAction(QString name,
                         bool succeed = true,
                         bool admin = false,
                         bool read_only = false,
                         int timeout_ms = sak::kDefaultQuickActionTimeoutMs)
        : m_name(std::move(name))
        , m_succeed(succeed)
        , m_admin(admin)
        , m_read_only(read_only)
        , m_timeout_ms(timeout_ms) {}

    QString name() const override { return m_name; }
    QString description() const override { return QStringLiteral("probe action"); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    QIcon icon() const override { return {}; }
    bool requiresAdmin() const override { return m_admin; }
    bool isReadOnly() const override { return m_read_only; }
    int executionTimeoutMs() const override { return m_timeout_ms; }

    void release() { m_released.store(true); }

    void execute() override {
        const int run = m_runs.fetch_add(1) + 1;
        m_seen_folder = outputFolder();
        m_seen_warning_accepted = outputFolderWarningAccepted();
        while (!m_released.load() && !isCancelled()) {
            QThread::msleep(2);
        }
        // A run ended by cancel reports itself cancelled, like the real actions do.
        const bool cancelled = isCancelled() && !m_released.load();
        ExecutionResult result;
        result.success = m_succeed && !cancelled;
        result.message = cancelled   ? QStringLiteral("probe cancelled")
                         : m_succeed ? QStringLiteral("probe ok")
                                     : QStringLiteral("probe fail");
        if (m_number_runs) {
            result.message += QStringLiteral(" (run %1)").arg(run);
        }
        result.files_processed = 7;
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
    }

    /// The output folder the latest run started with (read after it finished).
    [[nodiscard]] QString seenFolder() const { return m_seen_folder; }
    [[nodiscard]] bool seenWarningAccepted() const { return m_seen_warning_accepted; }
    void numberRuns() { m_number_runs = true; }

private:
    QString m_name;
    bool m_succeed;
    bool m_admin;
    bool m_read_only;
    int m_timeout_ms;
    std::atomic_bool m_released{false};
    std::atomic<int> m_runs{0};
    QString m_seen_folder;
    bool m_seen_warning_accepted{false};
    bool m_number_runs{false};
};

// A worker that runs until it is asked to stop -- what a long scan looks like to its owner.
class SpinUntilStoppedWorker : public WorkerBase {
public:
    ~SpinUntilStoppedWorker() override { stopAndJoin(); }

protected:
    auto execute() -> std::expected<void, sak::error_code> override {
        while (!stopRequested()) {
            QThread::msleep(2);
        }
        return {};
    }
};

}  // namespace

class AppActionServiceTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void runQuickActionSync_returnsResultSynchronously();
    void runQuickActionSync_reportsUnknownAction();
    void runQuickActionSync_nullControllerFailsCleanly();
    void runQuickActionSync_timesOutWhenActionStalls();
    void resultFromExecution_mapsFieldsIntoData();
    void registerQuickActionsInto_buildsSluggedDescriptorsWithAdminFlags();
    void registerQuickActionsInto_readOnlyActionNeedsNoApproval();
    void registerQuickActionsInto_thunkWaitsTheActionsOwnTimeout();

    void asyncInvocation_deliversCompletion();
    void asyncInvocation_deliversFailure();
    void asyncInvocation_timesOut();
    void asyncInvocation_firstWins();
    void asyncInvocation_accumulatesBatchesThenFinishes();
    void asyncInvocation_stopBeforeStartNeverStartsTheWork();
    void asyncInvocation_stopAsksTheOpAndTakesItsOwnResult();
    void asyncInvocation_unansweredStopSettlesAfterTheGrace();
    void asyncInvocation_stopWithoutStopRequestSettlesAtOnce();
    void runQuickActionSync_stopCancelsTheRunningAction();
    void registerQuickActionsInto_stopReachesTheAction();
    void stopWorkerOnCancel_settlesOnTheWorkersCancelled();
    void runQuickActionSync_carriesItsOutputFolder();
    void runQuickActionSync_neverAnswersAWarningForAPerson();
    void registerQuickActionsInto_readsTheFolderWhenTheRunIsRequested();
    void outputDirectoryScope_isThePerCallFolderOfItsThread();
    void runQuickActionSync_waitsForItsOwnRunNotAnotherCallers();
};

void AppActionServiceTests::runQuickActionSync_returnsResultSynchronously() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Probe One"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    raw->release();  // let execute() finish immediately

    const AppActionResult result = runQuickActionSync(&controller, QStringLiteral("Probe One"));
    QVERIFY(result.success);
    QCOMPARE(result.message, QStringLiteral("probe ok"));
    QCOMPARE(result.data.value(QStringLiteral("files_processed")).toInt(), 7);
}

void AppActionServiceTests::runQuickActionSync_reportsUnknownAction() {
    QuickActionController controller;
    const AppActionResult result = runQuickActionSync(&controller, QStringLiteral("nope"));
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Unknown action: nope"));
}

void AppActionServiceTests::runQuickActionSync_nullControllerFailsCleanly() {
    const AppActionResult result = runQuickActionSync(nullptr, QStringLiteral("x"));
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("No action controller"));
}

void AppActionServiceTests::runQuickActionSync_timesOutWhenActionStalls() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Slow Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    // Do NOT release yet: execute() blocks, so the 60ms timeout must fire.

    // The controller must report this action finished exactly once, and only because the
    // timeout REQUESTED cancellation -- the probe is deliberately never released first.
    auto completed = std::make_shared<int>(0);
    QObject::connect(&controller,
                     &QuickActionController::runFinished,
                     &controller,
                     [completed](const sak::QuickActionRun&,
                                 const QuickAction::ExecutionResult&,
                                 QuickAction::ActionStatus) { ++(*completed); });

    const AppActionResult result =
        runQuickActionSync(&controller, QStringLiteral("Slow Probe"), 60);
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action timed out after 60 ms"));
    // Proves the fixture really reached the TIMEOUT guard: the action was still running.
    QCOMPARE(*completed, 0);

    // Drain WITHOUT release(): the timeout must have queued cancelRunsOf(<this call's owner>) on
    // the controller, which sets the action's cancel flag so execute() unwinds and the
    // controller stops being busy. Without that request
    // this never completes, instead of passing on a status that never moves off Idle.
    QTRY_VERIFY_WITH_TIMEOUT(*completed == 1, 5000);
    QCOMPARE(*completed, 1);
    QCOMPARE(raw->lastExecutionResult().files_processed, static_cast<qint64>(7));
    raw->release();  // no-op safety: the worker is already out of its loop

    // Drain: release the worker and let it finish before the controller tears down,
    // so the still-running execute() cannot outlive its action.
    raw->release();
    QTRY_VERIFY_WITH_TIMEOUT(raw->status() == QuickAction::ActionStatus::Success ||
                                 raw->status() == QuickAction::ActionStatus::Idle,
                             5000);
}

void AppActionServiceTests::resultFromExecution_mapsFieldsIntoData() {
    const AppActionResult r = sak::appActionResultFromExecution(
        {true, QStringLiteral("done"), 100, 5, 250, QStringLiteral("C:/out/report.html")});
    QVERIFY(r.success);
    QCOMPARE(r.message, QStringLiteral("done"));
    QCOMPARE(r.data.value(QStringLiteral("bytes_processed")).toInt(), 100);
    QCOMPARE(r.data.value(QStringLiteral("files_processed")).toInt(), 5);
    QCOMPARE(r.data.value(QStringLiteral("duration_ms")).toInt(), 250);
    QCOMPARE(r.data.value(QStringLiteral("output_path")).toString(),
             QStringLiteral("C:/out/report.html"));
    QCOMPARE(static_cast<int>(r.data.size()), 4);  // exactly those four keys, no strays

    // The other arm of the output_path guard (src/core/app_action_bridge.cpp:112): an EMPTY
    // path is OMITTED, never emitted as an empty string the model would try to open.
    const AppActionResult no_path = sak::appActionResultFromExecution(
        {false, QStringLiteral("nothing written"), 0, 0, 12, QString()});
    QVERIFY(!no_path.success);
    QCOMPARE(no_path.message, QStringLiteral("nothing written"));
    QVERIFY(!no_path.data.contains(QStringLiteral("output_path")));
    QCOMPARE(static_cast<int>(no_path.data.size()), 3);
    QCOMPARE(no_path.data.value(QStringLiteral("duration_ms")).toInt(), 12);
}

void AppActionServiceTests::registerQuickActionsInto_buildsSluggedDescriptorsWithAdminFlags() {
    QuickActionController controller;
    controller.registerAction(
        std::make_unique<ProbeAction>(QStringLiteral("Verify System Files"), true, /*admin=*/true));
    controller.registerAction(std::make_unique<ProbeAction>(
        QStringLiteral("Optimize Power Settings"), true, /*admin=*/false));
    // Exercises every slugify() arm the two plain names above leave dead: leading
    // non-alnum skipped, a RUN of non-alnum collapsed to ONE underscore, trailing
    // underscores chopped (src/core/app_action_bridge.cpp:37-54).
    controller.registerAction(std::make_unique<ProbeAction>(
        QStringLiteral("  Reset Network (Wi-Fi) -- TCP/IP!  "), true, /*admin=*/false));

    AppActionRegistry registry;
    const int registered = registerQuickActionsInto(controller, registry);
    QCOMPARE(registered, 3);
    QCOMPARE(registry.count(), 3);

    // Ids are "action.<slug>": lowercased, spaces -> single underscore.
    QVERIFY(registry.contains(QStringLiteral("action.verify_system_files")));
    QVERIFY(registry.contains(QStringLiteral("action.optimize_power_settings")));
    QVERIFY(registry.contains(QStringLiteral("action.reset_network_wi_fi_tcp_ip")));

    const auto verify = registry.descriptor(QStringLiteral("action.verify_system_files"));
    QVERIFY(verify.has_value());
    QVERIFY(verify->requires_admin);  // flows through from the action
    QVERIFY(verify->mutating);        // actions perform system changes
    QVERIFY(!verify->read_only);
    QCOMPARE(verify->category, QStringLiteral("maintenance"));
    // Model-facing text is the action's OWN name/description, not the slugged id.
    QCOMPARE(verify->title, QStringLiteral("Verify System Files"));
    QCOMPARE(verify->description, QStringLiteral("probe action"));
    // Risk flags the assistant gates on: mutating alone forces the human gate, while
    // destructive/catastrophic stay FALSE for every QuickAction until one maps a real
    // irreversible flag (src/core/app_action_bridge.cpp:196-204).
    QVERIFY(!verify->destructive);
    QVERIFY(!verify->catastrophic);
    // No arguments: an empty schema is what tells the model to invoke with {}.
    QVERIFY(verify->params_schema.isEmpty());

    const auto power = registry.descriptor(QStringLiteral("action.optimize_power_settings"));
    QVERIFY(power.has_value());
    QVERIFY(!power->requires_admin);
    // Per-action descriptor, not a copy of the first one: the label tracks THIS action while
    // the shared risk flags still apply to it (src/core/app_action_bridge.cpp:192-204).
    QCOMPARE(power->title, QStringLiteral("Optimize Power Settings"));
    QVERIFY(power->mutating);
    QVERIFY(!power->destructive);
}

// System Inventory Report only reads and writes its own report, so it must not reach the assistant
// as a mutating action (which forces an approval and a lease); everything else still does.
void AppActionServiceTests::registerQuickActionsInto_readOnlyActionNeedsNoApproval() {
    QVERIFY(sak::GenerateSystemReportAction(QString()).isReadOnly());
    QuickActionController controller;
    controller.registerAction(std::make_unique<ProbeAction>(
        QStringLiteral("Read Only Probe"), true, /*admin=*/false, /*read_only=*/true));
    controller.registerAction(std::make_unique<ProbeAction>(QStringLiteral("Changing Probe")));
    AppActionRegistry registry;
    QCOMPARE(registerQuickActionsInto(controller, registry), 2);
    const auto reading = registry.descriptor(QStringLiteral("action.read_only_probe"));
    QVERIFY(reading.has_value());
    QVERIFY(reading->read_only);
    QVERIFY(!reading->mutating);
    const auto changing = registry.descriptor(QStringLiteral("action.changing_probe"));
    QVERIFY(changing.has_value());
    QVERIFY(!changing->read_only);
    QVERIFY(changing->mutating);
}
// The registry thunk waits exactly the action's declared bound (executionTimeoutMs), not a generic
// ceiling: SFC/DISM is allowed 60 minutes and a Defender full scan four hours, while a shorter one
// must be stopped at its own bound.
void AppActionServiceTests::registerQuickActionsInto_thunkWaitsTheActionsOwnTimeout() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Bounded Probe"),
                                               true,
                                               /*admin=*/false,
                                               /*read_only=*/false,
                                               /*timeout_ms=*/80);
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    AppActionRegistry registry;
    QCOMPARE(registerQuickActionsInto(controller, registry), 1);

    // The probe finishes successfully on its own after 2 s. A thunk that ignored the action's
    // 80 ms bound would wait for that and report success; the right one stops at 80 ms.
    auto completed = std::make_shared<int>(0);
    QObject::connect(&controller,
                     &QuickActionController::runFinished,
                     &controller,
                     [completed](const sak::QuickActionRun&,
                                 const QuickAction::ExecutionResult&,
                                 QuickAction::ActionStatus) { ++(*completed); });
    QTimer::singleShot(2000, &controller, [raw]() { raw->release(); });
    const AppActionResult result = registry.invoke(QStringLiteral("action.bounded_probe"),
                                                   QJsonObject{});
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action timed out after 80 ms"));

    // Drain: the timeout requested a cancel; let the probe unwind before teardown.
    raw->release();
    QTRY_VERIFY_WITH_TIMEOUT(*completed == 1, 5000);
}
void AppActionServiceTests::asyncInvocation_deliversCompletion() {
    FakeAsyncOp op;
    AsyncActionInvocation inv(5000);
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv](int value) {
        inv.finish({true, QStringLiteral("ok"), QJsonObject{{QStringLiteral("value"), value}}});
    });
    const AppActionResult result = inv.run([&op]() { op.scheduleDone(1, 42); });
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("value")).toInt(), 42);
}

void AppActionServiceTests::asyncInvocation_deliversFailure() {
    FakeAsyncOp op;
    AsyncActionInvocation inv(5000);
    QObject::connect(&op, &FakeAsyncOp::failed, inv.context(), [&inv](const QString& error) {
        inv.finish({false, error, {}});
    });
    const AppActionResult result = inv.run([&op]() { op.scheduleFail(1, QStringLiteral("boom")); });
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("boom"));
}

void AppActionServiceTests::asyncInvocation_timesOut() {
    FakeAsyncOp op;  // never emits
    AsyncActionInvocation inv(30);
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv](int) {
        inv.finish({true, QStringLiteral("ok"), {}});
    });
    const AppActionResult result = inv.run([]() {});
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action timed out after 30 ms"));
}

void AppActionServiceTests::asyncInvocation_firstWins() {
    FakeAsyncOp op;
    AsyncActionInvocation inv(5000);
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv](int) {
        inv.finish({true, QStringLiteral("completed"), {}});
    });
    QObject::connect(&op, &FakeAsyncOp::failed, inv.context(), [&inv](const QString& error) {
        inv.finish({false, error, {}});
    });
    // done is emitted first, so the failure that follows must be ignored.
    const AppActionResult result =
        inv.run([&op]() { op.scheduleDoneThenFail(1, 1, QStringLiteral("late failure")); });
    QVERIFY(result.success);
    QCOMPARE(result.message, QStringLiteral("completed"));
    QVERIFY(inv.isDone());

    // Reuse must fail closed with its OWN reason and must NOT launch a second operation
    // nobody waits on -- never silently replay the recorded success
    // (src/core/app_action_bridge.cpp:88-90).
    auto restarted = std::make_shared<bool>(false);
    const AppActionResult reused = inv.run([restarted]() { *restarted = true; });
    QVERIFY(!reused.success);
    QCOMPARE(reused.message, QStringLiteral("AsyncActionInvocation already used"));
    QVERIFY(!*restarted);
}

void AppActionServiceTests::asyncInvocation_accumulatesBatchesThenFinishes() {
    FakeAsyncOp op;
    AsyncActionInvocation inv(5000);
    auto total = std::make_shared<int>(0);
    QObject::connect(&op, &FakeAsyncOp::batch, inv.context(), [total](int value) {
        *total += value;
    });
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv, total](int count) {
        inv.finish(
            {true,
             QStringLiteral("done"),
             QJsonObject{{QStringLiteral("count"), count}, {QStringLiteral("sum"), *total}}});
    });
    const AppActionResult result = inv.run([&op]() { op.scheduleBatchesThenDone(1); });
    QVERIFY(result.success);
    QCOMPARE(result.data.value(QStringLiteral("count")).toInt(), 2);
    QCOMPARE(result.data.value(QStringLiteral("sum")).toInt(), 30);
}

// A Stop that came before the work started: the work is never started at all.
void AppActionServiceTests::asyncInvocation_stopBeforeStartNeverStartsTheWork() {
    AsyncActionInvocation inv(5000, []() { return true; });
    bool started = false;
    const AppActionResult result = inv.run([&started]() { started = true; });
    QVERIFY(!started);
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action cancelled"));
    QVERIFY(result.data.value(QStringLiteral("cancelled")).toBool());
}

// A Stop while the op runs asks the op to stop and returns the op's OWN terminal result -- the
// truth about what it did -- well inside the timeout.
void AppActionServiceTests::asyncInvocation_stopAsksTheOpAndTakesItsOwnResult() {
    FakeAsyncOp op;  // never completes on its own
    bool stop = false;
    AsyncActionInvocation inv(5000, [&stop]() { return stop; });
    bool stop_requested = false;
    inv.setStopRequest([&op, &stop_requested]() {
        stop_requested = true;
        op.scheduleCancelled(10);
    });
    QObject::connect(&op, &FakeAsyncOp::cancelled, inv.context(), [&inv]() {
        inv.finish({false, QStringLiteral("op stopped after 3 items"), {}});
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const AppActionResult result =
        inv.run([&stop]() { QTimer::singleShot(30, [&stop]() { stop = true; }); });
    QVERIFY(stop_requested);
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("op stopped after 3 items"));
    QVERIFY2(elapsed.elapsed() < 4000, "a Stop must not wait out the action's timeout");
}

// An op that does not answer a Stop is not waited on past the grace: the wait settles as
// cancelled and the caller's teardown stops the work.
void AppActionServiceTests::asyncInvocation_unansweredStopSettlesAfterTheGrace() {
    FakeAsyncOp op;  // never completes, ignores the stop request
    bool stop = false;
    AsyncActionInvocation inv(5000, [&stop]() { return stop; }, /*stop_grace_ms=*/50);
    bool stop_requested = false;
    inv.setStopRequest([&stop_requested]() { stop_requested = true; });
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv](int) {
        inv.finish({true, QStringLiteral("ok"), {}});
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const AppActionResult result =
        inv.run([&stop]() { QTimer::singleShot(30, [&stop]() { stop = true; }); });
    QVERIFY(stop_requested);
    QCOMPARE(result.message, QStringLiteral("Action cancelled"));
    QVERIFY2(elapsed.elapsed() < 4000, "an unanswered Stop must settle after the grace");
}

void AppActionServiceTests::asyncInvocation_stopWithoutStopRequestSettlesAtOnce() {
    FakeAsyncOp op;  // never emits
    bool stop = false;
    AsyncActionInvocation inv(5000, [&stop]() { return stop; });
    QObject::connect(&op, &FakeAsyncOp::done, inv.context(), [&inv](int) {
        inv.finish({true, QStringLiteral("ok"), {}});
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const AppActionResult result =
        inv.run([&stop]() { QTimer::singleShot(30, [&stop]() { stop = true; }); });
    QCOMPARE(result.message, QStringLiteral("Action cancelled"));
    QVERIFY(elapsed.elapsed() < 4000);
}

// The quick-action thunk's Stop cancels the action on its controller (the route that also reaches
// the elevated helper), and the action -- never released by the test -- unwinds and reports
// itself cancelled, which is what the caller is told.
void AppActionServiceTests::runQuickActionSync_stopCancelsTheRunningAction() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Stoppable Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    auto completed = std::make_shared<int>(0);
    QObject::connect(&controller,
                     &QuickActionController::runFinished,
                     &controller,
                     [completed](const sak::QuickActionRun&,
                                 const QuickAction::ExecutionResult&,
                                 QuickAction::ActionStatus) { ++(*completed); });

    bool stop = false;
    QTimer::singleShot(50, &controller, [&stop]() { stop = true; });
    QElapsedTimer elapsed;
    elapsed.start();
    const AppActionResult result = runQuickActionSync(
        &controller, QStringLiteral("Stoppable Probe"), 30'000, [&stop]() { return stop; });
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("probe cancelled"));
    QCOMPARE(*completed, 1);
    QVERIFY2(elapsed.elapsed() < 10'000, "Stop must not wait out the action's own bound");
    raw->release();  // no-op safety: the probe already unwound on the cancel
}
// The quick-action thunk the registry stores carries the Stop through to the controller: the
// same cancel as runQuickActionSync, reached by registry.invoke().
void AppActionServiceTests::registerQuickActionsInto_stopReachesTheAction() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Registered Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    AppActionRegistry registry;
    QCOMPARE(registerQuickActionsInto(controller, registry), 1);
    QVERIFY(registry.isCancellable(QStringLiteral("action.registered_probe")));

    bool stop = false;
    QTimer::singleShot(50, &controller, [&stop]() { stop = true; });
    const AppActionResult result = registry.invoke(QStringLiteral("action.registered_probe"),
                                                   QJsonObject{},
                                                   nullptr,
                                                   [&stop]() { return stop; });
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("probe cancelled"));
    raw->release();
}

// A WorkerBase-driven action settles on the worker's own cancelled() once Stop asks it to stop,
// well before the 15 s grace a silent op would get.
void AppActionServiceTests::stopWorkerOnCancel_settlesOnTheWorkersCancelled() {
    const auto worker = sak::makeWorker<SpinUntilStoppedWorker>();
    bool stop = false;
    AsyncActionInvocation inv(5000, [&stop]() { return stop; });
    sak::stopWorkerOnCancel(inv, *worker);
    QObject::connect(worker.get(), &WorkerBase::finished, inv.context(), [&inv]() {
        inv.finish({true, QStringLiteral("ran to the end"), {}});
    });
    QElapsedTimer elapsed;
    elapsed.start();
    const AppActionResult result = inv.run([&worker, &stop]() {
        worker->start();
        QTimer::singleShot(30, [&stop]() { stop = true; });
    });
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action cancelled"));
    QVERIFY2(elapsed.elapsed() < 4000, "the worker's cancelled() must settle the Stop");
}
// The assistant's staging folder travels with its run (it used to be set on the shared action
// object, where a run queued behind another caller's could pick up the wrong folder).
void AppActionServiceTests::runQuickActionSync_carriesItsOutputFolder() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Folder Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    raw->release();

    const AppActionResult result = runQuickActionSync(
        &controller, QStringLiteral("Folder Probe"), 30'000, {}, QStringLiteral("C:/staging/a"));
    QVERIFY(result.success);
    QCOMPARE(raw->seenFolder(), QStringLiteral("C:/staging/a"));
}

// The assistant's human gate approves running the action. It never shows a warning about the
// folder, and in Unattended no one is there, so a run from here has agreed to nothing, even when a
// panel's run of the same action just did.
void AppActionServiceTests::runQuickActionSync_neverAnswersAWarningForAPerson() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Warning Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    raw->release();
    raw->setOutputFolderWarningAccepted(true);

    const AppActionResult result = runQuickActionSync(
        &controller, QStringLiteral("Warning Probe"), 30'000, {}, QStringLiteral("C:/staging/a"));
    QVERIFY(result.success);
    QVERIFY(!raw->seenWarningAccepted());
}

// The registry thunk asks for the folder when it requests the run, so the folder the assistant
// sets for one call is the one that call's run uses.
void AppActionServiceTests::registerQuickActionsInto_readsTheFolderWhenTheRunIsRequested() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Staged Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    raw->release();
    auto folder = std::make_shared<QString>(QStringLiteral("C:/staging/first"));
    AppActionRegistry registry;
    QCOMPARE(registerQuickActionsInto(controller, registry, [folder]() { return *folder; }), 1);

    QVERIFY(registry.invoke(QStringLiteral("action.staged_probe"), QJsonObject{}).success);
    QCOMPARE(raw->seenFolder(), QStringLiteral("C:/staging/first"));
    *folder = QStringLiteral("C:/staging/second");
    QVERIFY(registry.invoke(QStringLiteral("action.staged_probe"), QJsonObject{}).success);
    QCOMPARE(raw->seenFolder(), QStringLiteral("C:/staging/second"));
}

// A call's staging folder belongs to the call's thread: another thread setting its own (or none)
// cannot redirect the run, and outside a scope there is no folder to write into.
void AppActionServiceTests::outputDirectoryScope_isThePerCallFolderOfItsThread() {
    auto controller = std::make_shared<QuickActionController>();
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Scoped Probe"));
    ProbeAction* raw = probe.get();
    controller->registerAction(std::move(probe));
    raw->release();
    sak::AppActionService service(controller);
    AppActionRegistry registry;
    QCOMPARE(service.registerInto(registry), 1);
    QCOMPARE(service.outputDirectory(), QString());

    {
        const sak::AppActionService::OutputDirectoryScope scope(service,
                                                                QStringLiteral("C:/staging/gui"));
        QCOMPARE(service.outputDirectory(), QStringLiteral("C:/staging/gui"));

        QString other_before;
        QString other_inside;
        QString other_after;
        std::unique_ptr<QThread> other(QThread::create([&]() {
            other_before = service.outputDirectory();
            {
                const sak::AppActionService::OutputDirectoryScope inner(
                    service, QStringLiteral("C:/staging/worker"));
                other_inside = service.outputDirectory();
            }
            other_after = service.outputDirectory();
        }));
        other->start();
        QVERIFY(other->wait(10'000));
        QCOMPARE(other_before, QString());
        QCOMPARE(other_inside, QStringLiteral("C:/staging/worker"));
        QCOMPARE(other_after, QString());

        // The other thread's scope came and went; this thread's run still gets its own folder.
        QCOMPARE(service.outputDirectory(), QStringLiteral("C:/staging/gui"));
        QVERIFY(registry.invoke(QStringLiteral("action.scoped_probe"), QJsonObject{}).success);
        QCOMPARE(raw->seenFolder(), QStringLiteral("C:/staging/gui"));
    }
    QCOMPARE(service.outputDirectory(), QString());
    QVERIFY(registry.invoke(QStringLiteral("action.scoped_probe"), QJsonObject{}).success);
    QCOMPARE(raw->seenFolder(), QString());
}

// One controller serves the panels and the assistant. A panel's run of the same action that
// finishes while the assistant's waits must not be taken as the assistant's result (the bridge
// used to match on the action object alone).
void AppActionServiceTests::runQuickActionSync_waitsForItsOwnRunNotAnotherCallers() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Shared Probe"));
    ProbeAction* raw = probe.get();
    raw->numberRuns();
    controller.registerAction(std::move(probe));
    controller.requestRun({QStringLiteral("Shared Probe"), QStringLiteral("diagnostics"), {}});
    QTimer::singleShot(50, &controller, [raw]() { raw->release(); });

    const AppActionResult result =
        runQuickActionSync(&controller, QStringLiteral("Shared Probe"), 30'000);
    QVERIFY(result.success);
    QCOMPARE(result.message, QStringLiteral("probe ok (run 2)"));
}

QTEST_MAIN(AppActionServiceTests)
#include "test_app_action_service.moc"
