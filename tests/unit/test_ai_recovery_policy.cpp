// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_recovery_policy.h"

#include <QtTest/QtTest>

class AiRecoveryPolicyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cancelledAborts();
    void missingInputAsksHuman();
    void ambiguousPackageAsksHuman();
    void transientFailureRetries();
    void cleanupFailureContinuesDegraded();
    void packageLookupFailureContinuesDegraded();
    void offlineDownloaderFailureFallsBack();
    void riskyMutationFailureAsksHuman();
    void decisionRoundTripsJson();
};

void AiRecoveryPolicyTests::cancelledAborts() {
    sak::ai::AiFailureContext context;
    context.user_cancelled = true;
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    // Two branches return Abort (this cancel gate and the terminal "no safe path" fallthrough)
    // and both leave safe_to_continue false, so the action+flag pair cannot show which fired.
    // Pin the branch reason; an empty error_message means reasonWithCause appends no cause.
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::Abort);
    QCOMPARE(decision.reason, QStringLiteral("User or parent run cancelled this work."));
    QVERIFY(!decision.safe_to_continue);
    QVERIFY(!decision.requires_human);
    QVERIFY(!decision.retry_allowed);
    QVERIFY(decision.preserve_artifacts);

    // The other half of the cancel gate: cancellation reported only through the error text.
    sak::ai::AiFailureContext text_context;
    text_context.error_message = QStringLiteral("Run cancelled by parent");
    const auto text_decision = sak::ai::AiRecoveryPolicy::classifyFailure(text_context);
    QCOMPARE(text_decision.action, sak::ai::AiRecoveryAction::Abort);
    QCOMPARE(text_decision.reason,
             QStringLiteral("User or parent run cancelled this work. "
                            "Underlying failure: Run cancelled by parent"));
    QVERIFY(!text_decision.safe_to_continue);
}

void AiRecoveryPolicyTests::missingInputAsksHuman() {
    sak::ai::AiFailureContext context;
    context.error_message = QStringLiteral("Missing required input: app_name");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    // THREE branches return AskHuman (missing input, policy gate, risky risk label) and all
    // three set requires_human, so the action+flag pair cannot show which one fired. The reason
    // names the branch and carries the underlying cause through reasonWithCause.
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(decision.reason,
             QStringLiteral("Missing or ambiguous required input. "
                            "Underlying failure: Missing required input: app_name"));
    QVERIFY(decision.requires_human);
    // A phase that needs a human is NEVER safe to walk past and is never auto-retried.
    QVERIFY(!decision.safe_to_continue);
    QVERIFY(!decision.retry_allowed);
    QVERIFY(decision.preserve_artifacts);
}

void AiRecoveryPolicyTests::ambiguousPackageAsksHuman() {
    sak::ai::AiFailureContext context;
    context.error_message =
        QStringLiteral("Ambiguous package match for 'chrome'. Choose an exact package_id.");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(decision.reason,
             QStringLiteral("Missing or ambiguous required input. Underlying failure: "
                            "Ambiguous package match for 'chrome'. Choose an exact package_id."));
    QVERIFY(decision.requires_human);
    QVERIFY(!decision.safe_to_continue);
    QVERIFY(!decision.retry_allowed);

    // The error above satisfies BOTH the "ambiguous" and the "choose" needle, so the needle this
    // case is NAMED for can be deleted without reddening it. Probe an ambiguity-ONLY error.
    sak::ai::AiFailureContext ambiguous_only;
    ambiguous_only.error_message = QStringLiteral("Ambiguous package match for 'chrome'.");
    const auto ambiguous_decision = sak::ai::AiRecoveryPolicy::classifyFailure(ambiguous_only);
    QCOMPARE(ambiguous_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(ambiguous_decision.reason,
             QStringLiteral("Missing or ambiguous required input. Underlying failure: "
                            "Ambiguous package match for 'chrome'."));
    QVERIFY(!ambiguous_decision.safe_to_continue);
}

void AiRecoveryPolicyTests::transientFailureRetries() {
    sak::ai::AiFailureContext context;
    context.error_message = QStringLiteral("Connection closed");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::Retry);
    // TWO branches return Retry (transient failure and malformed model output) and both set
    // retry_allowed; the reason names which one fired.
    QCOMPARE(decision.reason,
             QStringLiteral("Transient network/model/tool failure. "
                            "Underlying failure: Connection closed"));
    QVERIFY(decision.retry_allowed);
    // A retry is NOT permission to walk past the failed phase.
    QVERIFY(!decision.safe_to_continue);
    QVERIFY(!decision.requires_human);
    QVERIFY(decision.preserve_artifacts);
}

void AiRecoveryPolicyTests::cleanupFailureContinuesDegraded() {
    sak::ai::AiFailureContext context;
    context.phase_type = QStringLiteral("cleanup");
    context.error_message = QStringLiteral("locked file");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    // THREE branches return ContinueDegraded (cleanup, package lookup, download) and all three
    // set safe_to_continue; the reason names the cleanup branch.
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(decision.reason,
             QStringLiteral("Cleanup failed; preserve artifacts and report cleanup debt. "
                            "Underlying failure: locked file"));
    QVERIFY(decision.safe_to_continue);
    // The whole point of this branch: the artifacts survive the failed cleanup.
    QVERIFY(decision.preserve_artifacts);
    QVERIFY(!decision.requires_human);
    QVERIFY(!decision.retry_allowed);
}

void AiRecoveryPolicyTests::packageLookupFailureContinuesDegraded() {
    sak::ai::AiFailureContext context;
    context.tool_name = QStringLiteral("sak_package_manager");
    context.risk = QStringLiteral("read_only");
    context.phase_id = QStringLiteral("precheck");
    context.error_message = QStringLiteral("Package search returned no candidates");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(decision.reason,
             QStringLiteral("Built-in package lookup failed; continue degraded and surface the "
                            "lookup failure. Underlying failure: Package search returned no "
                            "candidates"));
    QVERIFY(decision.safe_to_continue);
    QVERIFY(!decision.requires_human);
    QVERIFY(!decision.retry_allowed);
    QVERIFY(decision.preserve_artifacts);

    // The degraded-continue is granted to a READ-ONLY package lookup, not to the tool name
    // alone: another non-mutating risk label on the same tool/phase has no recovery path.
    sak::ai::AiFailureContext download_only = context;
    download_only.risk = QStringLiteral("download_only");
    const auto download_only_decision = sak::ai::AiRecoveryPolicy::classifyFailure(download_only);
    QCOMPARE(download_only_decision.action, sak::ai::AiRecoveryAction::Abort);
    QCOMPARE(download_only_decision.reason,
             QStringLiteral("No safe automatic recovery path. "
                            "Underlying failure: Package search returned no candidates"));
    QVERIFY(!download_only_decision.safe_to_continue);

    // ... and it is granted to the package tool, not to any read-only step that fails.
    sak::ai::AiFailureContext other_tool = context;
    other_tool.tool_name = QStringLiteral("sak_system_report");
    const auto other_tool_decision = sak::ai::AiRecoveryPolicy::classifyFailure(other_tool);
    QCOMPARE(other_tool_decision.action, sak::ai::AiRecoveryAction::Abort);
    QCOMPARE(other_tool_decision.reason,
             QStringLiteral("No safe automatic recovery path. "
                            "Underlying failure: Package search returned no candidates"));
    QVERIFY(!other_tool_decision.safe_to_continue);
}

void AiRecoveryPolicyTests::offlineDownloaderFailureFallsBack() {
    sak::ai::AiFailureContext context;
    context.tool_name = QStringLiteral("sak_offline_downloader");
    context.phase_id = QStringLiteral("direct_download");
    context.error_message = QStringLiteral("package unavailable");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(decision.reason,
             QStringLiteral("Built-in download step failed; continue degraded and surface the "
                            "download failure. Underlying failure: package unavailable"));
    QVERIFY(decision.safe_to_continue);
    QVERIFY(!decision.requires_human);
    QVERIFY(!decision.retry_allowed);
    QVERIFY(decision.preserve_artifacts);

    // The phase id above ALSO contains "download", so the tool-name half of isDownloadFailure --
    // the half this case is NAMED for -- can be deleted without reddening it. Probe each half.
    sak::ai::AiFailureContext tool_only = context;
    tool_only.phase_id = QStringLiteral("step_one");
    const auto tool_only_decision = sak::ai::AiRecoveryPolicy::classifyFailure(tool_only);
    QCOMPARE(tool_only_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(tool_only_decision.reason,
             QStringLiteral("Built-in download step failed; continue degraded and surface the "
                            "download failure. Underlying failure: package unavailable"));

    sak::ai::AiFailureContext phase_only = context;
    phase_only.tool_name = QStringLiteral("sak_package_manager");
    const auto phase_only_decision = sak::ai::AiRecoveryPolicy::classifyFailure(phase_only);
    QCOMPARE(phase_only_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(phase_only_decision.reason,
             QStringLiteral("Built-in download step failed; continue degraded and surface the "
                            "download failure. Underlying failure: package unavailable"));

    // The third disjunct -- a bundle phase on a non-downloader tool -- is otherwise untested.
    sak::ai::AiFailureContext bundle_only = context;
    bundle_only.tool_name = QStringLiteral("sak_package_manager");
    bundle_only.phase_id = QStringLiteral("stage_bundle");
    const auto bundle_decision = sak::ai::AiRecoveryPolicy::classifyFailure(bundle_only);
    QCOMPARE(bundle_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(bundle_decision.reason,
             QStringLiteral("Built-in download step failed; continue degraded and surface the "
                            "download failure. Underlying failure: package unavailable"));
}

void AiRecoveryPolicyTests::riskyMutationFailureAsksHuman() {
    sak::ai::AiFailureContext context;
    context.risk = QStringLiteral("system_change");
    context.error_message = QStringLiteral("installer failed");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(decision.reason,
             QStringLiteral("Risky or mutating action failed; human decision needed. "
                            "Underlying failure: installer failed"));
    QVERIFY(decision.requires_human);
    QVERIFY(!decision.safe_to_continue);
    QVERIFY(!decision.retry_allowed);
    QVERIFY(decision.preserve_artifacts);

    // The risk gate must run BEFORE the automatic retry/degraded paths: a half-applied system
    // change must not be re-run because its error text looks transient, nor walked past because
    // its phase type is cleanup. Neither ordering is observable from the case above.
    sak::ai::AiFailureContext transient_risky;
    transient_risky.risk = QStringLiteral("system_change");
    transient_risky.error_message = QStringLiteral("Connection closed");
    const auto transient_decision = sak::ai::AiRecoveryPolicy::classifyFailure(transient_risky);
    QCOMPARE(transient_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(transient_decision.reason,
             QStringLiteral("Risky or mutating action failed; human decision needed. "
                            "Underlying failure: Connection closed"));
    QVERIFY(!transient_decision.retry_allowed);

    sak::ai::AiFailureContext cleanup_risky;
    cleanup_risky.phase_type = QStringLiteral("cleanup");
    cleanup_risky.risk = QStringLiteral("uninstall");
    cleanup_risky.error_message = QStringLiteral("locked file");
    const auto cleanup_decision = sak::ai::AiRecoveryPolicy::classifyFailure(cleanup_risky);
    QCOMPARE(cleanup_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QVERIFY(!cleanup_decision.safe_to_continue);
}

void AiRecoveryPolicyTests::decisionRoundTripsJson() {
    sak::ai::AiRecoveryDecision decision;
    decision.action = sak::ai::AiRecoveryAction::Reassign;
    decision.reason = QStringLiteral("critic failed");
    decision.suggested_agent = QStringLiteral("overseer");
    decision.safe_to_continue = true;

    // Pin the WIRE KEYS, not just the round trip: the orchestrator persists this object and
    // reads it back by name, so a symmetric key rename would round-trip green while breaking
    // the transcript (see test_ai_orchestrator.cpp reading recovery_decision["reason"]).
    const QJsonObject json = decision.toJson();
    QCOMPARE(json.value(QStringLiteral("action")).toString(), QStringLiteral("reassign"));
    QCOMPARE(json.value(QStringLiteral("reason")).toString(), QStringLiteral("critic failed"));
    QCOMPARE(json.value(QStringLiteral("suggested_agent")).toString(), QStringLiteral("overseer"));
    QCOMPARE(json.value(QStringLiteral("requires_human")).toBool(true), false);
    QCOMPARE(json.value(QStringLiteral("retry_allowed")).toBool(true), false);
    QCOMPARE(json.value(QStringLiteral("safe_to_continue")).toBool(false), true);
    QCOMPARE(json.value(QStringLiteral("preserve_artifacts")).toBool(false), true);

    const auto roundtrip = sak::ai::AiRecoveryDecision::fromJson(json);
    QCOMPARE(roundtrip.action, sak::ai::AiRecoveryAction::Reassign);
    QCOMPARE(roundtrip.reason, decision.reason);
    QCOMPARE(roundtrip.suggested_agent, decision.suggested_agent);
    QVERIFY(roundtrip.safe_to_continue);
    QVERIFY(!roundtrip.requires_human);
    QVERIFY(!roundtrip.retry_allowed);
    QVERIFY(roundtrip.preserve_artifacts);

    // A decision read back from persisted or model-authored JSON is untrusted: a CONTRADICTORY
    // object (an abort that claims it is safe to continue and may retry, and that discards its
    // artifacts) must be clamped to the restrictive side, never honored.
    QJsonObject hostile = json;
    hostile[QStringLiteral("action")] = QStringLiteral("abort");
    hostile[QStringLiteral("safe_to_continue")] = true;
    hostile[QStringLiteral("retry_allowed")] = true;
    hostile[QStringLiteral("preserve_artifacts")] = false;
    const auto clamped = sak::ai::AiRecoveryDecision::fromJson(hostile);
    QCOMPARE(clamped.action, sak::ai::AiRecoveryAction::Abort);
    QVERIFY(!clamped.safe_to_continue);
    QVERIFY(!clamped.retry_allowed);
    QVERIFY(clamped.preserve_artifacts);

    // Unknown action text resolves to the most restrictive action, not a permissive one.
    QJsonObject unknown = json;
    unknown[QStringLiteral("action")] = QStringLiteral("proceed_anyway");
    const auto fallback = sak::ai::AiRecoveryDecision::fromJson(unknown);
    QCOMPARE(fallback.action, sak::ai::AiRecoveryAction::Abort);
    QVERIFY(!fallback.safe_to_continue);
}

QTEST_GUILESS_MAIN(AiRecoveryPolicyTests)
#include "test_ai_recovery_policy.moc"
