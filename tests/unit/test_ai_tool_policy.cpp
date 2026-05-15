// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_policy.h"

#include <QtTest/QtTest>

class AiToolPolicyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void readOnlyPolicyBlocksRiskyCommands();
    void packagePolicyRequiresLeaseForInstall();
    void downloadOnlyAllowsDirectDownloadButBlocksInstall();
    void exclusivePolicyMarksRiskyCallsExclusive();
};

void AiToolPolicyTests::readOnlyPolicyBlocksRiskyCommands() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("run_powershell");
    request.command_preview = QStringLiteral("Get-PhysicalDisk");

    auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);
    QVERIFY(decision.allowed);
    QVERIFY(!decision.risky_change);

    request.command_preview = QStringLiteral("Remove-Item C:\\temp\\x -Recurse");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);
    QVERIFY(!decision.allowed);
    QVERIFY(decision.risky_change);
}

void AiToolPolicyTests::packagePolicyRequiresLeaseForInstall() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");

    const auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::PackageToolsOnly,
                                                      request);

    QVERIFY(decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);
    QVERIFY(decision.restore_point_recommended);
}

void AiToolPolicyTests::downloadOnlyAllowsDirectDownloadButBlocksInstall() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_offline_downloader");
    request.operation = QStringLiteral("direct_download");

    auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::DownloadOnly, request);
    QVERIFY(decision.allowed);

    request.operation = QStringLiteral("install_bundle");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::DownloadOnly, request);
    QVERIFY(!decision.allowed);
}

void AiToolPolicyTests::exclusivePolicyMarksRiskyCallsExclusive() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("run_cmd");
    request.command_preview = QStringLiteral("choco install git -y");

    const auto decision =
        sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ExclusiveMutatingExecutor, request);

    QVERIFY(decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);
    QVERIFY(decision.requires_exclusive_lease);
}

QTEST_GUILESS_MAIN(AiToolPolicyTests)
#include "test_ai_tool_policy.moc"
