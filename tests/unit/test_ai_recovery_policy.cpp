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
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::Abort);
    QVERIFY(!decision.safe_to_continue);
}

void AiRecoveryPolicyTests::missingInputAsksHuman() {
    sak::ai::AiFailureContext context;
    context.error_message = QStringLiteral("Missing required input: app_name");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QVERIFY(decision.requires_human);
}

void AiRecoveryPolicyTests::ambiguousPackageAsksHuman() {
    sak::ai::AiFailureContext context;
    context.error_message =
        QStringLiteral("Ambiguous package match for 'chrome'. Choose an exact package_id.");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QVERIFY(decision.requires_human);
}

void AiRecoveryPolicyTests::transientFailureRetries() {
    sak::ai::AiFailureContext context;
    context.error_message = QStringLiteral("Connection closed");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::Retry);
    QVERIFY(decision.retry_allowed);
}

void AiRecoveryPolicyTests::cleanupFailureContinuesDegraded() {
    sak::ai::AiFailureContext context;
    context.phase_type = QStringLiteral("cleanup");
    context.error_message = QStringLiteral("locked file");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QVERIFY(decision.safe_to_continue);
}

void AiRecoveryPolicyTests::packageLookupFailureContinuesDegraded() {
    sak::ai::AiFailureContext context;
    context.tool_name = QStringLiteral("sak_package_manager");
    context.risk = QStringLiteral("read_only");
    context.phase_id = QStringLiteral("precheck");
    context.error_message = QStringLiteral("Package search returned no candidates");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QVERIFY(decision.safe_to_continue);
}

void AiRecoveryPolicyTests::offlineDownloaderFailureFallsBack() {
    sak::ai::AiFailureContext context;
    context.tool_name = QStringLiteral("sak_offline_downloader");
    context.phase_id = QStringLiteral("direct_download");
    context.error_message = QStringLiteral("package unavailable");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::ContinueDegraded);
    QVERIFY(decision.safe_to_continue);
}

void AiRecoveryPolicyTests::riskyMutationFailureAsksHuman() {
    sak::ai::AiFailureContext context;
    context.risk = QStringLiteral("system_change");
    context.error_message = QStringLiteral("installer failed");
    const auto decision = sak::ai::AiRecoveryPolicy::classifyFailure(context);
    QCOMPARE(decision.action, sak::ai::AiRecoveryAction::AskHuman);
    QVERIFY(decision.requires_human);
}

void AiRecoveryPolicyTests::decisionRoundTripsJson() {
    sak::ai::AiRecoveryDecision decision;
    decision.action = sak::ai::AiRecoveryAction::Reassign;
    decision.reason = QStringLiteral("critic failed");
    decision.suggested_agent = QStringLiteral("overseer");
    decision.safe_to_continue = true;

    const auto roundtrip = sak::ai::AiRecoveryDecision::fromJson(decision.toJson());
    QCOMPARE(roundtrip.action, sak::ai::AiRecoveryAction::Reassign);
    QCOMPARE(roundtrip.reason, decision.reason);
    QCOMPARE(roundtrip.suggested_agent, decision.suggested_agent);
    QVERIFY(roundtrip.safe_to_continue);
}

QTEST_GUILESS_MAIN(AiRecoveryPolicyTests)
#include "test_ai_recovery_policy.moc"
