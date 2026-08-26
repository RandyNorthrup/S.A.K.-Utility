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
    void buildsWin32GuiActionPlan();
    void blocksWin32GuiWithoutSteps();
    void blocksWin32GuiStepMissingTool();
    void blocksWin32GuiNonObjectStep();
    void blocksWin32GuiNonObjectArguments();
    void mistypedSafetyFlagStaysRisky();
    void flagsCatastrophicManifestCommand();
    void blocksWin32GuiNonBoolOptionalStep();
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
    // 7200 is REDUCED to the executor's ceiling, not passed through. This assertion used to
    // expect 7200 verbatim, which pinned a promise execution never kept: every app-action
    // plan runs through ExecutionBroker::launchProcess, which clamps to
    // kAiCommandMaxTimeoutSeconds (3600) SILENTLY -- so a plan reviewed and approved as a
    // two-hour action was killed at one hour and reported as a timeout. The planner and the
    // broker now share one constant, so a plan cannot describe an execution that will not
    // happen.
    QCOMPARE(plan.request.timeout_seconds, sak::ai::kAiCommandMaxTimeoutSeconds);
    // No max_output_bytes argument, so the default Options budget (256 KiB) survives the clamp.
    QCOMPARE(plan.request.max_output_bytes, 262'144);
    // The evidence array is copied verbatim from the manifest profile, so pin the whole ordered
    // array -- a size-only check cannot catch a reordered, renamed or substituted entry.
    QCOMPARE(plan.evidence,
             QJsonArray(
                 {QStringLiteral("process_exit_code"), QStringLiteral("Get-MpThreatDetection")}));
    QVERIFY(plan.risky);
    QCOMPARE(plan.preview,
             QStringLiteral(
                 "Run Sample App action 'quick_scan': Start-MpScan -ScanType QuickScan"));
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
             QStringLiteral(
                 "app_run_action supports powershell/cli/win32_gui manifest actions only"));
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
    // Same correction: the old ceiling of 14'400 was four times what the broker honours.
    QCOMPARE(plan.request.timeout_seconds, sak::ai::kAiCommandMaxTimeoutSeconds);
    // And the two bounds are the SAME constant, which is what stops them drifting again.
    QVERIFY(sak::ai::kAiCommandMaxTimeoutSeconds == 3600);
    QCOMPARE(plan.request.max_output_bytes, 4096);
}

void AiAppActionPlannerTests::carriesGuardBlockForChecksumBypass() {
    const QJsonObject manifest = actionManifest(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("powershell")},
        {QStringLiteral("command"), QStringLiteral("choco install pkg -y --ignore-checksums")}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("sample_app"), QStringLiteral("install"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.guard_block_error,
             QStringLiteral("Blocked package checksum bypass. Do not pass --ignore-checksums, "
                            "substitute checksums, or run cached installers after a package "
                            "checksum mismatch."));
    QCOMPARE(plan.error_message, plan.guard_block_error);
    QVERIFY(plan.guard_approval_reason.isEmpty());
}

void AiAppActionPlannerTests::flagsCatastrophicManifestCommand() {
    // CODEX_REVIEW_4 M-B1-5: a format/wipe manifest command must be flagged catastrophic so the
    // provider-gateway app_run path forces the hard human confirm in every mode (not merely a
    // restore-point offer in Unattended). Format-Volume passes the command guard, so the plan is
    // ok() and reaches the catastrophic classification.
    const QJsonObject destructive = actionManifest(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("powershell")},
        {QStringLiteral("command"), QStringLiteral("Format-Volume -DriveLetter D -Force")}});
    const auto destroy = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("some_app"), QStringLiteral("wipe"), destructive, QJsonObject{});
    QVERIFY2(destroy.ok(), qPrintable(destroy.error_message));
    QVERIFY(destroy.catastrophic);
    // Catastrophic implies risky (the destructive-command classifier also fires), and the
    // preview is the exact text the confirm dialog shows for this action.
    QVERIFY(destroy.risky);
    QCOMPARE(destroy.preview,
             QStringLiteral("Run Sample App action 'wipe': Format-Volume -DriveLetter D -Force"));

    const QJsonObject benign =
        actionManifest(QJsonObject{{QStringLiteral("method"), QStringLiteral("powershell")},
                                   {QStringLiteral("command"), QStringLiteral("Get-Date")}});
    const auto safe = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("some_app"), QStringLiteral("time"), benign, QJsonObject{});
    QVERIFY(safe.ok());
    QVERIFY(!safe.catastrophic);
    // Non-vacuity for the pair above: with no safety flags in the profile and a benign command,
    // BOTH classifications stay clear -- so `destroy.risky` above is the destructive command, not
    // a flag every plan carries.
    QVERIFY(!safe.risky);
}

void AiAppActionPlannerTests::buildsWin32GuiActionPlan() {
    const QJsonObject manifest = actionManifest(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("win32_gui")},
        {QStringLiteral("steps"),
         QJsonArray{
             QJsonObject{{QStringLiteral("tool"), QStringLiteral("focus_window")},
                         {QStringLiteral("arguments"),
                          QJsonObject{{QStringLiteral("window_title"), QStringLiteral("App")}}}},
             QJsonObject{{QStringLiteral("tool"), QStringLiteral("click_text")},
                         {QStringLiteral("arguments"),
                          QJsonObject{{QStringLiteral("text"), QStringLiteral("Quick Scan")}}}}}},
        {QStringLiteral("evidence"), QJsonArray{QStringLiteral("Items Detected")}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(plan.ok());
    QCOMPARE(plan.method, QStringLiteral("win32_gui"));
    // The steps array is the recipe the desktop step-runner executes, copied verbatim from the
    // manifest: pin the whole ordered array so a dropped/reordered/rewritten step cannot pass a
    // size-only check.
    QCOMPARE(plan.steps,
             manifest.value(QStringLiteral("requested_action_profile"))
                 .toObject()
                 .value(QStringLiteral("steps"))
                 .toArray());
    QCOMPARE(plan.steps.size(), 2);
    QCOMPARE(plan.steps.at(0).toObject().value(QStringLiteral("tool")).toString(),
             QStringLiteral("focus_window"));
    QCOMPARE(plan.steps.at(1).toObject().value(QStringLiteral("tool")).toString(),
             QStringLiteral("click_text"));
    QVERIFY(plan.risky);  // GUI input injection is always at least input-tier
    QVERIFY(plan.request.command.isEmpty());
    QCOMPARE(plan.preview,
             QStringLiteral("Drive Sample App GUI action 'quick_scan' (2 desktop-control steps)"));
    // Evidence is copied before the win32_gui branch, so a GUI recipe carries it too.
    QCOMPARE(plan.evidence, QJsonArray({QStringLiteral("Items Detected")}));
    QVERIFY(plan.error_message.isEmpty());
}

void AiAppActionPlannerTests::blocksWin32GuiWithoutSteps() {
    const QJsonObject manifest =
        actionManifest(QJsonObject{{QStringLiteral("method"), QStringLiteral("win32_gui")},
                                   {QStringLiteral("steps"), QJsonArray{}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message,
             QStringLiteral("win32_gui action requires a non-empty 'steps' array in the manifest"));
}

void AiAppActionPlannerTests::blocksWin32GuiStepMissingTool() {
    const QJsonObject manifest = actionManifest(
        QJsonObject{{QStringLiteral("method"), QStringLiteral("win32_gui")},
                    {QStringLiteral("steps"),
                     QJsonArray{QJsonObject{{QStringLiteral("arguments"), QJsonObject{}}}}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    // The refusal names the offending step INDEX, so the exact message also proves the loop
    // reports which step failed rather than a generic recipe error.
    QCOMPARE(plan.error_message, QStringLiteral("win32_gui step 0 is missing a 'tool' name"));
}

void AiAppActionPlannerTests::blocksWin32GuiNonObjectStep() {
    // A step that is not an object (a bare value) is a malformed recipe.
    const QJsonObject manifest =
        actionManifest(QJsonObject{{QStringLiteral("method"), QStringLiteral("win32_gui")},
                                   {QStringLiteral("steps"), QJsonArray{42}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message, QStringLiteral("win32_gui step 0 must be an object"));
}

void AiAppActionPlannerTests::blocksWin32GuiNonObjectArguments() {
    // 'arguments', when present, must be an object -- a scalar is rejected.
    const QJsonObject manifest = actionManifest(
        QJsonObject{{QStringLiteral("method"), QStringLiteral("win32_gui")},
                    {QStringLiteral("steps"),
                     QJsonArray{QJsonObject{{QStringLiteral("tool"), QStringLiteral("click_text")},
                                            {QStringLiteral("arguments"), 5}}}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message, QStringLiteral("win32_gui step 0 'arguments' must be an object"));
}

void AiAppActionPlannerTests::mistypedSafetyFlagStaysRisky() {
    // A mistyped safety flag (the string "true" instead of a JSON bool) must NOT silently
    // downgrade to false and strip the risk/admin treatment. The command itself (Get-Date)
    // is benign, so risky here can only come from the fail-closed flag reading.
    const QJsonObject manifest =
        actionManifest(QJsonObject{{QStringLiteral("method"), QStringLiteral("powershell")},
                                   {QStringLiteral("command"), QStringLiteral("Get-Date")},
                                   {QStringLiteral("high_risk"), QStringLiteral("true")},
                                   {QStringLiteral("requires_admin"), QStringLiteral("yes")}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("sample_app"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(plan.ok());
    QVERIFY(plan.risky);                   // present-but-mistyped high_risk -> treated as set
    QVERIFY(plan.request.requires_admin);  // present-but-mistyped requires_admin -> treated as set
}

void AiAppActionPlannerTests::blocksWin32GuiNonBoolOptionalStep() {
    // 'optional', when present, must be a boolean -- a mistyped value is a malformed recipe.
    const QJsonObject manifest = actionManifest(QJsonObject{
        {QStringLiteral("method"), QStringLiteral("win32_gui")},
        {QStringLiteral("steps"),
         QJsonArray{QJsonObject{{QStringLiteral("tool"), QStringLiteral("click_text")},
                                {QStringLiteral("optional"), QStringLiteral("yes")}}}}});

    const auto plan = sak::ai::AiAppActionPlanner::buildPlan(
        QStringLiteral("superantispyware"), QStringLiteral("quick_scan"), manifest, QJsonObject{});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message, QStringLiteral("win32_gui step 0 'optional' must be a boolean"));
}

QTEST_GUILESS_MAIN(AiAppActionPlannerTests)
#include "test_ai_app_action_planner.moc"
