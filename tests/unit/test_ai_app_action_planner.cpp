// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_app_action_planner.h"

#include <QJsonArray>
#include <QtTest/QtTest>

namespace {

QJsonObject actionManifest(QJsonObject profile, bool supported = true) {
    profile[QStringLiteral("supported")] = supported;
    return QJsonObject{
        {QStringLiteral("id"), QStringLiteral("sample_app")},
        {QStringLiteral("display_name"), QStringLiteral("Sample App")},
        {QStringLiteral("requested_action"), QStringLiteral("quick_scan")},
        {QStringLiteral("requested_action_supported"), supported},
        {QStringLiteral("requested_action_profile"), profile},
    };
}

}  // namespace

class AiAppActionPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void buildsSupportedPowerShellActionPlan();
    void blocksUnsupportedManifestAction();
    void blocksUnsupportedMethod();
    void clampsOutputAndTimeout();
    void carriesGuardBlockForChecksumBypass();
};

void AiAppActionPlannerTests::buildsSupportedPowerShellActionPlan() {
    const QJsonObject manifest = actionManifest(
        QJsonObject{{QStringLiteral("method"), QStringLiteral("powershell")},
                    {QStringLiteral("requires_admin"), true},
                    {QStringLiteral("command"), QStringLiteral("Start-MpScan -ScanType QuickScan")},
                    {QStringLiteral("timeout_seconds"), 7200},
                    {QStringLiteral("evidence"),
                     QJsonArray{QStringLiteral("process_exit_code"),
                                QStringLiteral("Get-MpThreatDetection")}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("windows_defender"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(plan.ok());
    QCOMPARE(plan.display_name, QStringLiteral("Sample App"));
    QCOMPARE(plan.method, QStringLiteral("powershell"));
    QCOMPARE(plan.request.command, QStringLiteral("Start-MpScan -ScanType QuickScan"));
    QVERIFY(plan.request.requires_admin);
    QCOMPARE(plan.request.timeout_seconds, 7200);
    QCOMPARE(plan.evidence.size(), 2);
    QVERIFY(plan.risky);
    QVERIFY(plan.preview.contains(QStringLiteral("quick_scan")));
    QVERIFY(plan.guard_block_error.isEmpty());
}

void AiAppActionPlannerTests::blocksUnsupportedManifestAction() {
    const QJsonObject manifest = actionManifest(
        QJsonObject{{QStringLiteral("method"), QStringLiteral("manual_gui_required")},
                    {QStringLiteral("reason"), QStringLiteral("Manual GUI only")}},
        false);

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message, QStringLiteral("Manual GUI only"));
}

void AiAppActionPlannerTests::blocksUnsupportedMethod() {
    const QJsonObject manifest = actionManifest(
        QJsonObject{{QStringLiteral("method"), QStringLiteral("manual_gui_required")},
                    {QStringLiteral("command"), QStringLiteral("open-gui")}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("sample_app"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message,
             QStringLiteral("app_run_action supports powershell/cli manifest actions only"));
}

void AiAppActionPlannerTests::clampsOutputAndTimeout() {
    const QJsonObject manifest =
        actionManifest(QJsonObject{{QStringLiteral("method"), QStringLiteral("powershell")},
                                   {QStringLiteral("command"), QStringLiteral("Get-Date")}});
    const QJsonObject arguments{{QStringLiteral("timeout_seconds"), 99'999},
                                {QStringLiteral("max_output_bytes"), 99'999'999}};

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("sample_app"),
        QStringLiteral("quick_scan"),
        manifest,
        arguments,
        sak::ai::AiAppActionPlanner::Options{2048, 1024, 4096});

    QVERIFY(plan.ok());
    QCOMPARE(plan.request.timeout_seconds, 14'400);
    QCOMPARE(plan.request.max_output_bytes, 4096);
}

void AiAppActionPlannerTests::carriesGuardBlockForChecksumBypass() {
    const QJsonObject manifest = actionManifest(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("powershell")},
        {QStringLiteral("command"), QStringLiteral("choco install pkg -y --ignore-checksums")}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("sample_app"), QStringLiteral("install"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QVERIFY(plan.guard_block_error.contains(QStringLiteral("checksum bypass")));
    QCOMPARE(plan.error_message, plan.guard_block_error);
    QVERIFY(plan.guard_approval_reason.isEmpty());
}

QTEST_GUILESS_MAIN(AiAppActionPlannerTests)
#include "test_ai_app_action_planner.moc"
