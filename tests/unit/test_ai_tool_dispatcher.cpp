// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_policy.h"

#include <QtTest/QtTest>

class AiToolDispatcherTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void policyDeniedReturnsStructuredBlock();
    void allowedCallReachesHandler();
    void packageInstallReportsLeaseRequirement();
    void missingHandlerReportsMissingFlag();
    void exclusiveLeaseBlocksConcurrentMutating();
    void leaseReleasedAfterDispatch();
};

void AiToolDispatcherTests::policyDeniedReturnsStructuredBlock() {
    sak::ai::AiToolDispatcher dispatcher;
    bool handler_called = false;
    dispatcher.registerHandler(QStringLiteral("run_powershell"),
                               [&handler_called](const QJsonObject&,
                                                 const sak::ai::AiToolPolicyDecision&) {
                                   handler_called = true;
                                   return QJsonObject{};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("run_powershell");
    request.command_preview = QStringLiteral("Remove-Item C:\\temp\\x -Recurse");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {});
    QVERIFY(!outcome.dispatched);
    QVERIFY(!outcome.policy_decision.allowed);
    QVERIFY(outcome.policy_decision.risky_change);
    QVERIFY(!handler_called);
    QVERIFY(outcome.result.value(QStringLiteral("policy_denied")).toBool(false));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(), request.tool_name);
}

void AiToolDispatcherTests::allowedCallReachesHandler() {
    sak::ai::AiToolDispatcher dispatcher;
    QJsonObject received_args;
    dispatcher.registerHandler(QStringLiteral("take_screenshot"),
                               [&received_args](const QJsonObject& args,
                                                const sak::ai::AiToolPolicyDecision&) {
                                   received_args = args;
                                   QJsonObject result;
                                   result[QStringLiteral("success")] = true;
                                   result[QStringLiteral("path")] = QStringLiteral("a.png");
                                   return result;
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("take_screenshot");

    QJsonObject args;
    args[QStringLiteral("reason")] = QStringLiteral("snapshot");
    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, args);
    QVERIFY(outcome.dispatched);
    QVERIFY(outcome.policy_decision.allowed);
    QCOMPARE(received_args.value(QStringLiteral("reason")).toString(), QStringLiteral("snapshot"));
    QVERIFY(outcome.result.value(QStringLiteral("success")).toBool(false));
}

void AiToolDispatcherTests::packageInstallReportsLeaseRequirement() {
    sak::ai::AiToolDispatcher dispatcher;
    sak::ai::AiToolPolicyDecision captured_decision;
    dispatcher.registerHandler(QStringLiteral("sak_package_manager"),
                               [&captured_decision](const QJsonObject&,
                                                    const sak::ai::AiToolPolicyDecision& decision) {
                                   captured_decision = decision;
                                   return QJsonObject{};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::PackageToolsOnly, request, {});
    QVERIFY(outcome.dispatched);
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(outcome.policy_decision.risky_change);
    QVERIFY(outcome.policy_decision.requires_lease);
    QVERIFY(outcome.policy_decision.restore_point_recommended);
    QVERIFY(captured_decision.requires_lease);
}

void AiToolDispatcherTests::missingHandlerReportsMissingFlag() {
    sak::ai::AiToolDispatcher dispatcher;
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("take_screenshot");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {});
    QVERIFY(!outcome.dispatched);
    QVERIFY(outcome.handler_missing);
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(outcome.result.value(QStringLiteral("handler_missing")).toBool(false));
}

void AiToolDispatcherTests::exclusiveLeaseBlocksConcurrentMutating() {
    sak::ai::AiToolDispatcher dispatcher;
    sak::ai::AiLeaseManager leases;
    dispatcher.setLeaseManager(&leases);
    dispatcher.registerHandler(QStringLiteral("run_cmd"),
                               [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) {
                                   QJsonObject ok;
                                   ok[QStringLiteral("success")] = true;
                                   return ok;
                               });

    const auto acquire = leases.acquire(QStringLiteral("other_agent"),
                                        QStringList{QStringLiteral("run_powershell")},
                                        QStringLiteral("system_change"),
                                        true);
    QVERIFY(acquire.granted);

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("run_cmd");
    request.command_preview = QStringLiteral("choco install git -y");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ExclusiveMutatingExecutor,
                                             request,
                                             {},
                                             QStringLiteral("repair_agent"));
    QVERIFY(!outcome.dispatched);
    QVERIFY(outcome.lease_denied);
    QVERIFY(outcome.result.value(QStringLiteral("lease_denied")).toBool(false));

    leases.release(acquire.lease.lease_id);
}

void AiToolDispatcherTests::leaseReleasedAfterDispatch() {
    sak::ai::AiToolDispatcher dispatcher;
    sak::ai::AiLeaseManager leases;
    dispatcher.setLeaseManager(&leases);
    dispatcher.registerHandler(QStringLiteral("sak_package_manager"),
                               [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) {
                                   QJsonObject ok;
                                   ok[QStringLiteral("success")] = true;
                                   return ok;
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");

    const auto outcome = dispatcher.dispatch(
        sak::ai::AiToolPolicy::PackageToolsOnly, request, {}, QStringLiteral("package_agent"));
    QVERIFY(outcome.dispatched);
    QVERIFY(!outcome.lease_id.isEmpty());
    QCOMPARE(leases.activeLeaseCount(), 0);
}

QTEST_GUILESS_MAIN(AiToolDispatcherTests)
#include "test_ai_tool_dispatcher.moc"
