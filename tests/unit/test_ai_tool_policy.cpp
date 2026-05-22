// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_policy.h"

#include <QtTest/QtTest>

class AiToolPolicyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void readOnlyPolicyBlocksRiskyCommands();
    void readOnlyPolicyAllowsProviderGatewayStatus();
    void readOnlyPolicyAllowsSessionSearch();
    void packagePolicyRequiresLeaseForInstall();
    void packageMutationBlockedWhenUserAskedForScan();
    void packageMutationRequiresExplicitIntent_data();
    void packageMutationRequiresExplicitIntent();
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

void AiToolPolicyTests::readOnlyPolicyAllowsProviderGatewayStatus() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_provider_gateway");
    request.operation = QStringLiteral("provider_status");

    auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);

    QVERIFY(decision.allowed);
    QVERIFY(!decision.risky_change);
    QVERIFY(!decision.requires_lease);

    request.operation = QStringLiteral("docs_query");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);

    QVERIFY(decision.allowed);
    QVERIFY(!decision.risky_change);
    QVERIFY(!decision.requires_lease);

    request.operation = QStringLiteral("app_run_action");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);
    QVERIFY(!decision.allowed);

    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::MutatingRequiresLease, request);
    QVERIFY(decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);

    request.operation = QStringLiteral("win32_mcp_call");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);
    QVERIFY(!decision.allowed);
    QVERIFY(decision.risky_change);

    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::MutatingRequiresLease, request);
    QVERIFY(decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);
}

void AiToolPolicyTests::readOnlyPolicyAllowsSessionSearch() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_session_search");
    request.operation = QStringLiteral("search");

    const auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::ReadOnlyPc, request);
    QVERIFY(decision.allowed);
    QVERIFY(!decision.risky_change);
}

void AiToolPolicyTests::packagePolicyRequiresLeaseForInstall() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");
    request.user_message = QStringLiteral("install git");

    const auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::PackageToolsOnly,
                                                      request);

    QVERIFY(decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);
    QVERIFY(decision.restore_point_recommended);
}

void AiToolPolicyTests::packageMutationBlockedWhenUserAskedForScan() {
    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");
    request.user_message = QStringLiteral("can you run a SUPERAntiSpyware quick scan?");

    auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::PackageToolsOnly, request);

    QVERIFY(!decision.allowed);
    QVERIFY(decision.risky_change);
    QVERIFY(decision.requires_lease);
    QVERIFY(decision.reason.contains(QStringLiteral("scan"), Qt::CaseInsensitive));

    request.user_message = QStringLiteral("install SUPERAntiSpyware then run a scan");
    decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::PackageToolsOnly, request);
    QVERIFY(decision.allowed);
}

void AiToolPolicyTests::packageMutationRequiresExplicitIntent_data() {
    QTest::addColumn<QString>("operation");
    QTest::addColumn<QString>("user_message");
    QTest::addColumn<bool>("allowed");

    QTest::newRow("install-empty") << QStringLiteral("install") << QString() << false;
    QTest::newRow("install-scan") << QStringLiteral("install")
                                  << QStringLiteral("run a malware scan") << false;
    QTest::newRow("install-download-only")
        << QStringLiteral("install") << QStringLiteral("download firefox") << false;
    QTest::newRow("install-explicit")
        << QStringLiteral("install") << QStringLiteral("install firefox") << true;
    QTest::newRow("upgrade-explicit")
        << QStringLiteral("upgrade") << QStringLiteral("upgrade firefox") << true;
    QTest::newRow("uninstall-explicit")
        << QStringLiteral("uninstall") << QStringLiteral("uninstall firefox") << true;
}

void AiToolPolicyTests::packageMutationRequiresExplicitIntent() {
    QFETCH(QString, operation);
    QFETCH(QString, user_message);
    QFETCH(bool, allowed);

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = operation;
    request.user_message = user_message;

    const auto decision = sak::ai::evaluateToolPolicy(sak::ai::AiToolPolicy::PackageToolsOnly,
                                                      request);
    QCOMPARE(decision.allowed, allowed);
    if (!allowed) {
        QVERIFY(decision.reason.contains(QStringLiteral("explicitly request")) ||
                decision.reason.contains(QStringLiteral("scan")));
    }
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
