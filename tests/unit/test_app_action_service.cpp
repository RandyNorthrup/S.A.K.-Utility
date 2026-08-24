// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"

#include <QJsonObject>
#include <QTimer>
#include <QtTest/QtTest>
#include <QVector>

#include <atomic>
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
};

}  // namespace

namespace {

// Minimal headless QuickAction for exercising the async->sync bridge. execute()
// runs on the controller's worker thread; it blocks until released so the timeout
// path can be tested deterministically without a teardown race.
class ProbeAction : public QuickAction {
public:
    explicit ProbeAction(QString name, bool succeed = true, bool admin = false)
        : m_name(std::move(name)), m_succeed(succeed), m_admin(admin) {}

    QString name() const override { return m_name; }
    QString description() const override { return QStringLiteral("probe action"); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    QIcon icon() const override { return {}; }
    bool requiresAdmin() const override { return m_admin; }

    void release() { m_released.store(true); }

    void scan() override {
        setScanResult({});
        Q_EMIT scanComplete({});
    }

    void execute() override {
        while (!m_released.load() && !isCancelled()) {
            QThread::msleep(2);
        }
        ExecutionResult result;
        result.success = m_succeed;
        result.message = m_succeed ? QStringLiteral("probe ok") : QStringLiteral("probe fail");
        result.files_processed = 7;
        setExecutionResult(result);
        Q_EMIT executionComplete(result);
    }

private:
    QString m_name;
    bool m_succeed;
    bool m_admin;
    std::atomic_bool m_released{false};
};

}  // namespace

class AppActionServiceTests : public QObject {
    Q_OBJECT

private slots:
    void runQuickActionSync_returnsResultSynchronously();
    void runQuickActionSync_reportsUnknownAction();
    void runQuickActionSync_nullControllerFailsCleanly();
    void runQuickActionSync_timesOutWhenActionStalls();
    void resultFromExecution_mapsFieldsIntoData();
    void registerQuickActionsInto_buildsSluggedDescriptorsWithAdminFlags();

    void asyncInvocation_deliversCompletion();
    void asyncInvocation_deliversFailure();
    void asyncInvocation_timesOut();
    void asyncInvocation_firstWins();
    void asyncInvocation_accumulatesBatchesThenFinishes();
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
                     &QuickActionController::actionExecutionComplete,
                     &controller,
                     [completed](QuickAction*) { ++(*completed); });

    const AppActionResult result =
        runQuickActionSync(&controller, QStringLiteral("Slow Probe"), 60);
    QVERIFY(!result.success);
    QCOMPARE(result.message, QStringLiteral("Action timed out after 60 ms"));
    // Proves the fixture really reached the TIMEOUT guard: the action was still running.
    QCOMPARE(*completed, 0);

    // Drain WITHOUT release(): the timeout must have queued cancelCurrentAction() on the
    // controller (src/core/app_action_bridge.cpp:170-173), which sets the action's cancel
    // flag so execute() unwinds and the controller stops being busy. Without that request
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

QTEST_MAIN(AppActionServiceTests)
#include "test_app_action_service.moc"
