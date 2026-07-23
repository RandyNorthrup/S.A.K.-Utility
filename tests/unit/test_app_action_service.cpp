// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_action_registry.h"
#include "sak/app_action_service.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"

#include <QJsonObject>
#include <QtTest/QtTest>

#include <atomic>

using sak::AppActionDescriptor;
using sak::AppActionRegistry;
using sak::AppActionResult;
using sak::QuickAction;
using sak::QuickActionController;
using sak::registerQuickActionsInto;
using sak::runQuickActionSync;

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
    QVERIFY(result.message.contains(QStringLiteral("Unknown action")));
}

void AppActionServiceTests::runQuickActionSync_nullControllerFailsCleanly() {
    const AppActionResult result = runQuickActionSync(nullptr, QStringLiteral("x"));
    QVERIFY(!result.success);
}

void AppActionServiceTests::runQuickActionSync_timesOutWhenActionStalls() {
    QuickActionController controller;
    auto probe = std::make_unique<ProbeAction>(QStringLiteral("Slow Probe"));
    ProbeAction* raw = probe.get();
    controller.registerAction(std::move(probe));
    // Do NOT release yet: execute() blocks, so the 60ms timeout must fire.

    const AppActionResult result =
        runQuickActionSync(&controller, QStringLiteral("Slow Probe"), 60);
    QVERIFY(!result.success);
    QVERIFY(result.message.contains(QStringLiteral("timed out")));

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
}

void AppActionServiceTests::registerQuickActionsInto_buildsSluggedDescriptorsWithAdminFlags() {
    QuickActionController controller;
    controller.registerAction(
        std::make_unique<ProbeAction>(QStringLiteral("Verify System Files"), true, /*admin=*/true));
    controller.registerAction(std::make_unique<ProbeAction>(
        QStringLiteral("Optimize Power Settings"), true, /*admin=*/false));

    AppActionRegistry registry;
    const int registered = registerQuickActionsInto(controller, registry);
    QCOMPARE(registered, 2);

    // Ids are "action.<slug>": lowercased, spaces -> single underscore.
    QVERIFY(registry.contains(QStringLiteral("action.verify_system_files")));
    QVERIFY(registry.contains(QStringLiteral("action.optimize_power_settings")));

    const auto verify = registry.descriptor(QStringLiteral("action.verify_system_files"));
    QVERIFY(verify.has_value());
    QVERIFY(verify->requires_admin);  // flows through from the action
    QVERIFY(verify->mutating);        // actions perform system changes
    QVERIFY(!verify->read_only);
    QCOMPARE(verify->category, QStringLiteral("maintenance"));

    const auto power = registry.descriptor(QStringLiteral("action.optimize_power_settings"));
    QVERIFY(power.has_value());
    QVERIFY(!power->requires_admin);
}

QTEST_MAIN(AppActionServiceTests)
#include "test_app_action_service.moc"
