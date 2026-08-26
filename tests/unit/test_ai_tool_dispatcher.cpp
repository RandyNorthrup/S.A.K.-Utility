// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_lease_manager.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_policy.h"

#include <QJsonDocument>
#include <QtTest/QtTest>

class AiToolDispatcherTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void policyDeniedReturnsStructuredBlock();
    void allowedCallReachesHandler();
    void packageInstallReportsLeaseRequirement();
    void missingHandlerReportsMissingFlag();
    void emptyHandlerResultIsStructuredFailure();
    void exclusiveLeaseBlocksConcurrentMutating();
    void leaseReleasedAfterDispatch();
    void availabilityCheckerBlocksBeforeHandler();
    void availabilityCheckerEmptyResultDeniesCall();
    void requiresLeaseFailsClosedWithoutLeaseManager();
    void healthLedgerSuppressesRepeatedFailures();
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
    // ReadOnlyPc has THREE distinct refusal reasons (risky-mutation, not-on-the-allowlist,
    // non-shell-tool); pin the exact one so a regression that re-routes this input to a
    // sibling guard cannot hide behind the allowed==false bool.
    QCOMPARE(outcome.policy_decision.reason,
             QStringLiteral("Read-only PC policy blocked mutating command"));
    // A refusal must not request a lease or a restore point -- nothing is going to run.
    QVERIFY(!outcome.policy_decision.requires_lease);
    QVERIFY(!outcome.policy_decision.requires_exclusive_lease);
    QVERIFY(!outcome.policy_decision.restore_point_recommended);
    QVERIFY(!outcome.policy_decision.catastrophic_change);
    QVERIFY(!handler_called);
    QVERIFY(outcome.result.value(QStringLiteral("policy_denied")).toBool(false));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(), request.tool_name);
    // The structured block carries the reason verbatim to the caller.
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Read-only PC policy blocked mutating command"));
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
    QCOMPARE(outcome.policy_decision.reason, QStringLiteral("Read-only tool allowed"));
    // The dispatcher forwards the caller's argument object UNCHANGED -- pin the whole object so
    // an injected, renamed or dropped key cannot pass a single-key check.
    QCOMPARE(received_args, args);
    // Likewise the handler's result is passed back untouched (no empty-result substitution, no
    // lease/health stamping on this path), so pin the whole payload.
    const QJsonObject expected_result{{QStringLiteral("success"), true},
                                      {QStringLiteral("path"), QStringLiteral("a.png")}};
    QCOMPARE(outcome.result, expected_result);
}

void AiToolDispatcherTests::packageInstallReportsLeaseRequirement() {
    sak::ai::AiToolDispatcher dispatcher;
    // A mutating call requires a wired lease manager to dispatch (fail closed otherwise --
    // see requiresLeaseFailsClosedWithoutLeaseManager); this test verifies the decision
    // carries the lease requirement on the normal, lease-manager-present path.
    sak::ai::AiLeaseManager leases;
    dispatcher.setLeaseManager(&leases);
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
    request.user_message = QStringLiteral("Install the selected package");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::PackageToolsOnly, request, {});
    QVERIFY(outcome.dispatched);
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(outcome.policy_decision.risky_change);
    QVERIFY(outcome.policy_decision.requires_lease);
    QVERIFY(outcome.policy_decision.restore_point_recommended);
    QCOMPARE(outcome.policy_decision.reason, QStringLiteral("Package tool allowed"));
    // requires_exclusive_lease is the flag the dispatcher forwards as AiLeaseManager::acquire's
    // `exclusive` argument: the package policy never sets it, and an unnoticed regression to true
    // would silently serialize every package op against all other mutating agents.
    QVERIFY(!outcome.policy_decision.requires_exclusive_lease);
    QVERIFY(!outcome.policy_decision.catastrophic_change);
    // The handler must see the SAME decision the outcome reports -- pin every field, since a
    // default-constructed or partially populated decision would still pass a single-bool check.
    QVERIFY(captured_decision.allowed);
    QVERIFY(captured_decision.risky_change);
    QVERIFY(captured_decision.requires_lease);
    QVERIFY(!captured_decision.requires_exclusive_lease);
    QVERIFY(captured_decision.restore_point_recommended);
    QVERIFY(!captured_decision.catastrophic_change);
    QCOMPARE(captured_decision.reason, QStringLiteral("Package tool allowed"));
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
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(), request.tool_name);
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("No handler registered for tool 'take_screenshot'"));
}

void AiToolDispatcherTests::emptyHandlerResultIsStructuredFailure() {
    sak::ai::AiToolDispatcher dispatcher;
    dispatcher.registerHandler(QStringLiteral("take_screenshot"),
                               [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) {
                                   return QJsonObject{};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("take_screenshot");

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {});
    QVERIFY(outcome.dispatched);
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(), request.tool_name);
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Tool handler returned no data"));
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
    // The refusal is a LEASE denial on an ALLOWED, exclusive-lease-requiring decision -- pin the
    // decision so a policy regression that blocked the call outright (also lease_denied==false's
    // sibling failure mode) cannot masquerade as this contention path.
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(outcome.policy_decision.risky_change);
    QVERIFY(outcome.policy_decision.requires_lease);
    QVERIFY(outcome.policy_decision.requires_exclusive_lease);
    QVERIFY(outcome.policy_decision.restore_point_recommended);
    QCOMPARE(outcome.policy_decision.reason,
             QStringLiteral("Known local tool allowed with exclusive mutation policy"));
    QVERIFY(outcome.result.value(QStringLiteral("lease_denied")).toBool(false));
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(),
             QStringLiteral("run_cmd"));
    // The blocking holder is named in the error the caller sees -- by LABEL, not by the live
    // lease id. THIS ASSERTION PREVIOUSLY PINNED A LEAK, and pinned it at its worst point: this
    // error_message is the tool result handed back to the MODEL and written into the persisted
    // transcript, so asserting the full id here was asserting that the bearer token authorizing
    // release() of that lease gets published to the agent we just refused. The label still names
    // which lease is blocking; the token must not appear anywhere in the result.
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Active mutating lease '%1' held by 'other_agent' blocks new lease")
                 .arg(sak::ai::AiLeaseManager::publicLabel(acquire.lease.lease_id)));
    const QString serialized_result =
        QString::fromUtf8(QJsonDocument(outcome.result).toJson(QJsonDocument::Compact));
    QVERIFY2(!serialized_result.contains(acquire.lease.lease_id), qPrintable(serialized_result));

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
    request.user_message = QStringLiteral("Install the selected package");

    const auto outcome = dispatcher.dispatch(
        sak::ai::AiToolPolicy::PackageToolsOnly, request, {}, QStringLiteral("package_agent"));
    QVERIFY(outcome.dispatched);
    // A clean release leaves the handler's result untouched; the reclaimed-midop path would have
    // stamped success:false + lease_reclaimed_midop over the top, so pinning the whole payload
    // proves this dispatch took the normal branch.
    QVERIFY(!outcome.lease_reclaimed_midop);
    const QJsonObject expected_result{{QStringLiteral("success"), true}};
    QCOMPARE(outcome.result, expected_result);
    // The id is minted "lease_<4-digit counter>_<16 hex random>": the counter half is
    // deterministic for the first lease of a fresh manager, the token half only in width.
    QVERIFY2(outcome.lease_id.startsWith(QStringLiteral("lease_0001_")),
             qPrintable(outcome.lease_id));
    QCOMPARE(outcome.lease_id.size(), 27);
    QCOMPARE(leases.activeLeaseCount(), 0);
}

void AiToolDispatcherTests::availabilityCheckerBlocksBeforeHandler() {
    sak::ai::AiToolDispatcher dispatcher;
    bool handler_called = false;
    dispatcher.registerAvailabilityChecker(
        QStringLiteral("download_file"),
        [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) {
            return QJsonObject{{QStringLiteral("success"), false},
                               {QStringLiteral("failure_class"), QStringLiteral("invalid_request")},
                               {QStringLiteral("error_message"), QStringLiteral("bad url")}};
        });
    dispatcher.registerHandler(QStringLiteral("download_file"),
                               [&handler_called](const QJsonObject&,
                                                 const sak::ai::AiToolPolicyDecision&) {
                                   handler_called = true;
                                   return QJsonObject{{QStringLiteral("success"), true}};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("download_file");
    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::DownloadOnly, request, {});
    QVERIFY(outcome.availability_denied);
    QVERIFY(!outcome.dispatched);
    QVERIFY(!handler_called);
    QVERIFY(outcome.result.value(QStringLiteral("availability_denied")).toBool(false));
    QCOMPARE(outcome.result.value(QStringLiteral("failure_class")).toString(),
             QStringLiteral("invalid_request"));
    // The checker's own object is carried through: its failure_class AND message survive
    // (the dispatcher only fills those keys when the checker left them ABSENT), and success is
    // forced false regardless of what the checker said.
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(),
             QStringLiteral("download_file"));
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("bad url"));
}

void AiToolDispatcherTests::availabilityCheckerEmptyResultDeniesCall() {
    // A registered availability checker that returns an empty object is NOT evidence of
    // availability: the call must be denied fail-closed, not passed through.
    sak::ai::AiToolDispatcher dispatcher;
    bool handler_called = false;
    dispatcher.registerAvailabilityChecker(
        QStringLiteral("download_file"),
        [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) { return QJsonObject{}; });
    dispatcher.registerHandler(QStringLiteral("download_file"),
                               [&handler_called](const QJsonObject&,
                                                 const sak::ai::AiToolPolicyDecision&) {
                                   handler_called = true;
                                   return QJsonObject{{QStringLiteral("success"), true}};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("download_file");
    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::DownloadOnly, request, {});
    QVERIFY(outcome.availability_denied);
    QVERIFY(!outcome.dispatched);
    QVERIFY(!handler_called);
    // The empty checker object supplies neither field, so BOTH dispatcher-side defaults are
    // exercised here -- the sibling of availabilityCheckerBlocksBeforeHandler, where the
    // checker's own values win.
    QVERIFY(outcome.result.value(QStringLiteral("availability_denied")).toBool(false));
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("failure_class")).toString(),
             QStringLiteral("availability_failed"));
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Tool availability check failed"));
}

void AiToolDispatcherTests::requiresLeaseFailsClosedWithoutLeaseManager() {
    // A call that requires a mutation lease must be denied when no lease manager is wired --
    // running it would grant a mutating tool with no cross-agent serialization (fail open).
    sak::ai::AiToolDispatcher dispatcher;  // no setLeaseManager()
    bool handler_called = false;
    dispatcher.registerHandler(QStringLiteral("sak_package_manager"),
                               [&handler_called](const QJsonObject&,
                                                 const sak::ai::AiToolPolicyDecision&) {
                                   handler_called = true;
                                   return QJsonObject{{QStringLiteral("success"), true}};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("sak_package_manager");
    request.operation = QStringLiteral("install");
    request.user_message = QStringLiteral("install the selected package");

    const auto outcome = dispatcher.dispatch(
        sak::ai::AiToolPolicy::PackageToolsOnly, request, {}, QStringLiteral("agent"));
    QVERIFY(outcome.policy_decision.allowed);
    QVERIFY(outcome.policy_decision.requires_lease);
    QVERIFY(outcome.lease_denied);
    QVERIFY(!outcome.dispatched);
    QVERIFY(!handler_called);
    QVERIFY(outcome.result.value(QStringLiteral("lease_denied")).toBool(false));
    QVERIFY(!outcome.result.value(QStringLiteral("success")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("tool_name")).toString(),
             QStringLiteral("sak_package_manager"));
    QCOMPARE(outcome.result.value(QStringLiteral("operation")).toString(),
             QStringLiteral("install"));
    // The exact reason distinguishes THIS guard (no lease manager wired) from the contention
    // denial in exclusiveLeaseBlocksConcurrentMutating, which shares the lease_denied flag.
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Mutating lease required but no lease manager is configured"));
}

void AiToolDispatcherTests::healthLedgerSuppressesRepeatedFailures() {
    sak::ai::AiToolDispatcher dispatcher;
    sak::ai::AiToolHealthLedger ledger(2, 60'000, 60'000);
    dispatcher.setHealthLedger(&ledger);
    dispatcher.registerHandler(QStringLiteral("take_screenshot"),
                               [](const QJsonObject&, const sak::ai::AiToolPolicyDecision&) {
                                   return QJsonObject{{QStringLiteral("success"), false},
                                                      {QStringLiteral("error_message"),
                                                       QStringLiteral("capture failed")}};
                               });

    sak::ai::AiToolCallRequest request;
    request.tool_name = QStringLiteral("take_screenshot");
    QVERIFY(dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {}).dispatched);
    const auto primed = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {});
    QVERIFY(primed.dispatched);
    QCOMPARE(primed.health_key, QStringLiteral("take_screenshot"));
    // Pin what the two priming dispatches actually recorded: the suppression that follows is
    // only meaningful if the ledger counted exactly two failures of the expected class.
    const auto primed_record = ledger.record(QStringLiteral("take_screenshot"));
    QCOMPARE(primed_record.failure_count, 2);
    QCOMPARE(primed_record.consecutive_failures, 2);
    QCOMPARE(primed_record.success_count, 0);
    QCOMPARE(primed_record.last_failure_class, QStringLiteral("tool_failed"));
    QCOMPARE(primed_record.last_error_message, QStringLiteral("capture failed"));

    const auto outcome = dispatcher.dispatch(sak::ai::AiToolPolicy::ReadOnlyPc, request, {});
    QVERIFY(outcome.health_suppressed);
    QVERIFY(!outcome.dispatched);
    QVERIFY(outcome.result.value(QStringLiteral("health_suppressed")).toBool(false));
    // The suppression payload is the availability record serialized verbatim plus the
    // dispatcher's stamps: everything but the backoff deadline itself is deterministic.
    QVERIFY(!outcome.result.value(QStringLiteral("available")).toBool(true));
    QCOMPARE(outcome.result.value(QStringLiteral("key")).toString(),
             QStringLiteral("take_screenshot"));
    QCOMPARE(outcome.result.value(QStringLiteral("failure_class")).toString(),
             QStringLiteral("health_backoff"));
    const QString disabled_until =
        outcome.result.value(QStringLiteral("disabled_until_utc")).toString();
    QVERIFY(!disabled_until.isEmpty());
    QCOMPARE(outcome.result.value(QStringLiteral("error_message")).toString(),
             QStringLiteral("Tool/provider 'take_screenshot' is temporarily disabled until %1 "
                            "after 2 consecutive failure(s): capture failed")
                 .arg(disabled_until));
}

QTEST_GUILESS_MAIN(AiToolDispatcherTests)
#include "test_ai_tool_dispatcher.moc"
