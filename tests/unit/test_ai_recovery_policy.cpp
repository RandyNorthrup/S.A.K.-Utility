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

private:
    static void verifyHumanGateOutranksRiskGate();

private Q_SLOTS:
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

    // reasonWithCause must keep the persisted run record -- and the operator-facing
    // "Phase <id> needs human input: <reason>" message -- SINGLE-LINE and BOUNDED. Every cause
    // used anywhere in this suite is a short single-line string, so neither sanitation step is
    // observable: a build that stopped collapsing newlines, or stopped truncating, stays green.
    sak::ai::AiFailureContext multiline_context;
    multiline_context.user_cancelled = true;
    multiline_context.error_message = QStringLiteral("cancelled\r\nby operator");
    const auto multiline_decision = sak::ai::AiRecoveryPolicy::classifyFailure(multiline_context);
    QCOMPARE(multiline_decision.reason,
             QStringLiteral("User or parent run cancelled this work. "
                            "Underlying failure: cancelled  by operator"));

    sak::ai::AiFailureContext long_context;
    long_context.user_cancelled = true;
    long_context.error_message = QString(500, QChar::fromLatin1('x'));
    const auto long_decision = sak::ai::AiRecoveryPolicy::classifyFailure(long_context);
    QCOMPARE(long_decision.reason,
             QStringLiteral("User or parent run cancelled this work. Underlying failure: ") +
                 QString(400, QChar::fromLatin1('x')) + QStringLiteral("..."));
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

    // The human gate has a SECOND branch -- a policy/approval refusal -- that returns the same
    // action and the same flags, so nothing above can show it is alive; it can be deleted whole
    // (the context then falls through to the terminal abort, losing the human gate entirely)
    // without reddening anything. Pin its own reason, and probe two needles separately.
    sak::ai::AiFailureContext policy_context;
    policy_context.error_message = QStringLiteral("Approval required before this action");
    const auto policy_decision = sak::ai::AiRecoveryPolicy::classifyFailure(policy_context);
    QCOMPARE(policy_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(policy_decision.reason,
             QStringLiteral("Policy gate or approval blocked the action. "
                            "Underlying failure: Approval required before this action"));
    QVERIFY(policy_decision.requires_human);
    QVERIFY(!policy_decision.safe_to_continue);
    QVERIFY(!policy_decision.retry_allowed);
    QVERIFY(policy_decision.preserve_artifacts);

    sak::ai::AiFailureContext restore_point_context;
    restore_point_context.error_message = QStringLiteral("Restore point could not be created");
    const auto restore_point_decision =
        sak::ai::AiRecoveryPolicy::classifyFailure(restore_point_context);
    QCOMPARE(restore_point_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(restore_point_decision.reason,
             QStringLiteral("Policy gate or approval blocked the action. "
                            "Underlying failure: Restore point could not be created"));
    QVERIFY(!restore_point_decision.safe_to_continue);
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

    // The comment above names TWO Retry branches but only the transient one is ever reached, so
    // the malformed-model-output branch can be deleted whole -- the context then falls through
    // to the terminal abort -- without reddening anything. Probe two of its needles separately.
    sak::ai::AiFailureContext malformed_context;
    malformed_context.error_message = QStringLiteral("Model returned invalid json");
    const auto malformed_decision = sak::ai::AiRecoveryPolicy::classifyFailure(malformed_context);
    QCOMPARE(malformed_decision.action, sak::ai::AiRecoveryAction::Retry);
    QCOMPARE(malformed_decision.reason,
             QStringLiteral("Model output did not match expected schema. "
                            "Underlying failure: Model returned invalid json"));
    QVERIFY(malformed_decision.retry_allowed);
    QVERIFY(!malformed_decision.safe_to_continue);
    QVERIFY(!malformed_decision.requires_human);
    QVERIFY(malformed_decision.preserve_artifacts);

    sak::ai::AiFailureContext no_output_context;
    no_output_context.error_message = QStringLiteral("Model produced no output text");
    const auto no_output_decision = sak::ai::AiRecoveryPolicy::classifyFailure(no_output_context);
    QCOMPARE(no_output_decision.action, sak::ai::AiRecoveryAction::Retry);
    QCOMPARE(no_output_decision.reason,
             QStringLiteral("Model output did not match expected schema. "
                            "Underlying failure: Model produced no output text"));
    QVERIFY(no_output_decision.retry_allowed);
    QVERIFY(!no_output_decision.safe_to_continue);
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

    // The fixture above leaves risk EMPTY, which clears the risk gate through the isEmpty()
    // disjunct alone. The shipped workflow catalog labels these phases with real non-mutating
    // risks, and each is a SEPARATE disjunct of isNonMutatingRisk that no case in this file
    // reaches: drop any one of them and that label's phases start demanding a human.
    sak::ai::AiFailureContext none_risk = context;
    none_risk.risk = QStringLiteral("none");
    const auto none_decision = sak::ai::AiRecoveryPolicy::classifyFailure(none_risk);
    QCOMPARE(none_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(none_decision.reason,
             QStringLiteral("Cleanup failed; preserve artifacts and report cleanup debt. "
                            "Underlying failure: locked file"));

    sak::ai::AiFailureContext artifact_cleanup_risk = context;
    artifact_cleanup_risk.risk = QStringLiteral("artifact_cleanup");
    const auto artifact_cleanup_decision =
        sak::ai::AiRecoveryPolicy::classifyFailure(artifact_cleanup_risk);
    QCOMPARE(artifact_cleanup_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(artifact_cleanup_decision.reason,
             QStringLiteral("Cleanup failed; preserve artifacts and report cleanup debt. "
                            "Underlying failure: locked file"));

    sak::ai::AiFailureContext web_read_only_risk = context;
    web_read_only_risk.risk = QStringLiteral("web_read_only");
    const auto web_read_only_decision =
        sak::ai::AiRecoveryPolicy::classifyFailure(web_read_only_risk);
    QCOMPARE(web_read_only_decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QCOMPARE(web_read_only_decision.reason,
             QStringLiteral("Cleanup failed; preserve artifacts and report cleanup debt. "
                            "Underlying failure: locked file"));
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

    // The last gate before that terminal abort matches the critic ROLE as a whole token. An
    // agent id that merely EMBEDS those letters must still abort: a substring match would hand
    // a model-named agent an automatic review reassignment with safe_to_continue set. Neither
    // side is reachable from any other case here, and no case here sets agent_id at all.
    sak::ai::AiFailureContext embedded_critic = other_tool;
    embedded_critic.agent_id = QStringLiteral("criticality_triage_agent");
    const auto embedded_decision = sak::ai::AiRecoveryPolicy::classifyFailure(embedded_critic);
    QCOMPARE(embedded_decision.action, sak::ai::AiRecoveryAction::Abort);
    QCOMPARE(embedded_decision.reason,
             QStringLiteral("No safe automatic recovery path. "
                            "Underlying failure: Package search returned no candidates"));
    QVERIFY(embedded_decision.suggested_agent.isEmpty());
    QVERIFY(!embedded_decision.safe_to_continue);

    sak::ai::AiFailureContext critic_context = other_tool;
    critic_context.agent_id = QStringLiteral("code-critic");
    const auto critic_decision = sak::ai::AiRecoveryPolicy::classifyFailure(critic_context);
    QCOMPARE(critic_decision.action, sak::ai::AiRecoveryAction::Reassign);
    QCOMPARE(critic_decision.reason,
             QStringLiteral("Critic failed; reassign review to overseer/report agent. "
                            "Underlying failure: Package search returned no candidates"));
    QCOMPARE(critic_decision.suggested_agent, QStringLiteral("overseer"));
    QVERIFY(critic_decision.safe_to_continue);
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

    // The risk gate fails CLOSED on any label the non-mutating allow-list does not name. Both of
    // these ship in resources/ai/workflows and both are near-misses for an allow-list member, so
    // widening the list to admit them (or to a contains() check) would wave them through -- and
    // the packaging one is on a phase whose tool and name would then continue degraded.
    sak::ai::AiFailureContext packaged_download = context;
    packaged_download.tool_name = QStringLiteral("sak_offline_downloader");
    packaged_download.phase_id = QStringLiteral("build_bundle");
    packaged_download.risk = QStringLiteral("download_and_package");
    const auto packaged_decision = sak::ai::AiRecoveryPolicy::classifyFailure(packaged_download);
    QCOMPARE(packaged_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(packaged_decision.reason,
             QStringLiteral("Risky or mutating action failed; human decision needed. "
                            "Underlying failure: installer failed"));
    QVERIFY(!packaged_decision.safe_to_continue);
    QVERIFY(!packaged_decision.retry_allowed);

    sak::ai::AiFailureContext artifact_write_context = context;
    artifact_write_context.risk = QStringLiteral("artifact_write");
    const auto artifact_write_decision =
        sak::ai::AiRecoveryPolicy::classifyFailure(artifact_write_context);
    QCOMPARE(artifact_write_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(artifact_write_decision.reason,
             QStringLiteral("Risky or mutating action failed; human decision needed. "
                            "Underlying failure: installer failed"));
    QVERIFY(!artifact_write_decision.safe_to_continue);

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

    verifyHumanGateOutranksRiskGate();
}

// The OTHER half of the ordering: the human gate is classified BEFORE the risk gate so the more
// specific reason survives. Every sibling case has either an empty risk or an error the human
// gate ignores, so swapping the two gates changes nothing they assert -- yet it would replace
// "Missing or ambiguous required input" with the generic risk reason in the operator-facing gate
// message, losing the one thing that tells them WHAT to supply.
void AiRecoveryPolicyTests::verifyHumanGateOutranksRiskGate() {
    sak::ai::AiFailureContext risky_missing_input;
    risky_missing_input.risk = QStringLiteral("system_change");
    risky_missing_input.error_message = QStringLiteral("Missing required input: app_name");
    const auto risky_missing_decision =
        sak::ai::AiRecoveryPolicy::classifyFailure(risky_missing_input);
    QCOMPARE(risky_missing_decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QCOMPARE(risky_missing_decision.reason,
             QStringLiteral("Missing or ambiguous required input. "
                            "Underlying failure: Missing required input: app_name"));
    QVERIFY(!risky_missing_decision.safe_to_continue);
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

    // The abort above cannot isolate the safe_to_continue rule: the abort rule clears the SAME
    // flag, so the rule this assertion is aimed at is dead weight as far as the suite can tell.
    // Nor does any case prove retry_allowed SURVIVES on a genuine retry, so a blanket clear --
    // the obvious over-clamp -- passes everything above. A retry that claims it may be walked
    // past isolates both: the flag it may keep, and the flag it may not.
    QJsonObject retry_object = json;
    retry_object[QStringLiteral("action")] = QStringLiteral("retry");
    retry_object[QStringLiteral("retry_allowed")] = true;
    retry_object[QStringLiteral("safe_to_continue")] = true;
    const auto retry_clamped = sak::ai::AiRecoveryDecision::fromJson(retry_object);
    QCOMPARE(retry_clamped.action, sak::ai::AiRecoveryAction::Retry);
    QVERIFY(retry_clamped.retry_allowed);
    QVERIFY(!retry_clamped.safe_to_continue);
    QVERIFY(!retry_clamped.requires_human);
    QVERIFY(retry_clamped.preserve_artifacts);

    // Unknown action text resolves to the most restrictive action, not a permissive one.
    QJsonObject unknown = json;
    unknown[QStringLiteral("action")] = QStringLiteral("proceed_anyway");
    const auto fallback = sak::ai::AiRecoveryDecision::fromJson(unknown);
    QCOMPARE(fallback.action, sak::ai::AiRecoveryAction::Abort);
    QVERIFY(!fallback.safe_to_continue);
}

QTEST_GUILESS_MAIN(AiRecoveryPolicyTests)
#include "test_ai_recovery_policy.moc"
