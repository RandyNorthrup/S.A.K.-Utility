// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_dispatcher.h"

#include <QElapsedTimer>
#include <QStringList>

#include <exception>

namespace sak::ai {

namespace {

QString normalizeName(const QString& tool_name) {
    return tool_name.trimmed().toLower();
}

QJsonObject emptyHandlerResult(const AiToolCallRequest& request) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] = QStringLiteral("Tool handler returned no data");
    return obj;
}

QJsonObject handlerThrewResult(const AiToolCallRequest& request, const QString& what) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("failure_class")] = QStringLiteral("handler_exception");
    obj[QStringLiteral("error_message")] =
        QStringLiteral("Tool handler threw an exception: %1").arg(what);
    return obj;
}

bool resultSucceeded(const QJsonObject& result) {
    // Fail closed: a handler result that omits (or mistypes) "success" is NOT proof of
    // success. Recording such a result as healthy would let a malformed result poison the
    // health ledger as if the tool were working. Only an explicit success:true counts.
    return result.value(QStringLiteral("success")).toBool(false);
}

QString resultErrorMessage(const QJsonObject& result) {
    const QString error = result.value(QStringLiteral("error_message")).toString().trimmed();
    if (!error.isEmpty()) {
        return error;
    }
    return result.value(QStringLiteral("message")).toString().trimmed();
}

QString providerGatewayDiscriminator(const QJsonObject& arguments) {
    const QString operation =
        arguments.value(QStringLiteral("operation")).toString().trimmed().toLower();
    const QString provider_id =
        arguments.value(QStringLiteral("provider_id")).toString().trimmed().toLower();
    const QString app_id = arguments.value(QStringLiteral("app_id")).toString().trimmed().toLower();
    const QString action = arguments.value(QStringLiteral("action")).toString().trimmed().toLower();
    const QJsonObject extra = arguments.value(QStringLiteral("arguments")).toObject();
    const QString extra_provider =
        extra.value(QStringLiteral("provider_id")).toString().trimmed().toLower();
    const QString extra_tool =
        extra.value(QStringLiteral("tool_name")).toString().trimmed().toLower();

    QStringList parts;
    if (!operation.isEmpty()) {
        parts << operation;
    }
    if (!provider_id.isEmpty()) {
        parts << provider_id;
    } else if (!extra_provider.isEmpty()) {
        parts << extra_provider;
    }
    if (!app_id.isEmpty()) {
        parts << app_id;
    }
    if (!action.isEmpty()) {
        parts << action;
    }
    if (!extra_tool.isEmpty()) {
        parts << extra_tool;
    }
    return parts.join(QLatin1Char(':'));
}

QString healthKeyFor(const AiToolCallRequest& request, const QJsonObject& arguments) {
    const QString name = normalizeName(request.tool_name);
    if (name == QLatin1String("sak_provider_gateway")) {
        const QString discriminator = providerGatewayDiscriminator(arguments);
        if (!discriminator.isEmpty()) {
            return QStringLiteral("%1:%2").arg(name, discriminator);
        }
    }
    return name;
}

}  // namespace

void AiToolDispatcher::registerHandler(const QString& tool_name, Handler handler) {
    const QString name = normalizeName(tool_name);
    if (name.isEmpty()) {
        return;
    }
    if (handler) {
        m_handlers.insert(name, std::move(handler));
    } else {
        m_handlers.remove(name);
    }
}

void AiToolDispatcher::registerAvailabilityChecker(const QString& tool_name,
                                                   AvailabilityChecker checker) {
    const QString name = normalizeName(tool_name);
    if (name.isEmpty()) {
        return;
    }
    if (checker) {
        m_availability_checkers.insert(name, std::move(checker));
    } else {
        m_availability_checkers.remove(name);
    }
}

void AiToolDispatcher::clearHandlers() {
    m_handlers.clear();
    m_availability_checkers.clear();
}

bool AiToolDispatcher::hasHandler(const QString& tool_name) const {
    return m_handlers.contains(normalizeName(tool_name));
}

int AiToolDispatcher::handlerCount() const {
    return static_cast<int>(m_handlers.size());
}

void AiToolDispatcher::setLeaseManager(AiLeaseManager* manager) {
    m_lease_manager = manager;
}

void AiToolDispatcher::setHealthLedger(AiToolHealthLedger* ledger) {
    m_health_ledger = ledger;
}

void AiToolDispatcher::invokeHandler(const Handler& handler,
                                     const AiToolCallRequest& request,
                                     const QJsonObject& arguments,
                                     DispatchOutcome* outcome) const {
    QElapsedTimer timer;
    timer.start();
    try {
        outcome->result = handler(arguments, outcome->policy_decision);
    } catch (const std::exception& ex) {
        // Fail closed: a throwing handler must not leak the mutation lease -- an unreleased
        // lease blocks every future mutating call until its TTL expires -- nor skip health
        // recording. Convert the throw into a structured failure so the caller's normal
        // record-health / release-lease path still runs.
        outcome->result = handlerThrewResult(request, QString::fromUtf8(ex.what()));
    } catch (...) {
        outcome->result = handlerThrewResult(request, QStringLiteral("unknown exception"));
    }
    outcome->latency_ms = timer.elapsed();
    outcome->dispatched = true;
    if (outcome->result.isEmpty()) {
        outcome->result = emptyHandlerResult(request);
    }
}

AiToolDispatcher::DispatchOutcome AiToolDispatcher::dispatch(AiToolPolicy policy,
                                                             const AiToolCallRequest& request,
                                                             const QJsonObject& arguments,
                                                             const QString& agent_id) const {
    DispatchOutcome outcome;
    outcome.policy_decision = evaluateToolPolicy(policy, request);
    if (!outcome.policy_decision.allowed) {
        outcome.result = policyDeniedResult(request, outcome.policy_decision);
        return outcome;
    }

    const QString name = normalizeName(request.tool_name);
    outcome.health_key = healthKeyFor(request, arguments);
    if (!applyHealthGate(&outcome, request)) {
        return outcome;
    }
    if (!applyAvailabilityGate(&outcome, request, arguments, name)) {
        return outcome;
    }

    const Handler handler = resolveHandler(&outcome, request, name);
    if (!handler) {
        return outcome;
    }

    QString lease_id;
    if (!acquireLeaseForDispatch(&outcome, request, agent_id, &lease_id)) {
        return outcome;
    }

    invokeHandler(handler, request, arguments, &outcome);
    // Release BEFORE recording health: a release that reports the lease was already gone means
    // the TTL sweep reclaimed it mid-op, which fails the result closed -- and the health ledger
    // must then record that corrected failure, not the handler's (now untrustworthy) success.
    releaseLeaseForDispatch(&outcome, request, lease_id);
    recordHealthForResult(outcome);
    return outcome;
}

bool AiToolDispatcher::applyHealthGate(DispatchOutcome* outcome,
                                       const AiToolCallRequest& request) const {
    if (m_health_ledger == nullptr) {
        return true;
    }
    const AiToolAvailability availability = m_health_ledger->check(outcome->health_key);
    if (availability.available) {
        return true;
    }
    outcome->health_suppressed = true;
    outcome->result = healthSuppressedResult(request, availability);
    return false;
}

bool AiToolDispatcher::applyAvailabilityGate(DispatchOutcome* outcome,
                                             const AiToolCallRequest& request,
                                             const QJsonObject& arguments,
                                             const QString& normalized_name) const {
    const auto checker_it = m_availability_checkers.constFind(normalized_name);
    if (checker_it == m_availability_checkers.constEnd() || !checker_it.value()) {
        return true;
    }
    // Copy the checker out of the hash before invoking it. Invoking through the live
    // iterator would dangle if the callback re-entrantly registers or clears a checker:
    // a QHash rehash destroys the referenced std::function mid-call (use-after-free).
    const AvailabilityChecker checker = checker_it.value();
    QJsonObject availability;
    try {
        availability = checker(arguments, outcome->policy_decision);
    } catch (const std::exception& ex) {
        // Fail closed: a throwing checker denies (no explicit success below).
        availability[QStringLiteral("error_message")] =
            QStringLiteral("Availability check threw: %1").arg(QString::fromUtf8(ex.what()));
    } catch (...) {
        availability[QStringLiteral("error_message")] =
            QStringLiteral("Availability check threw an unknown exception");
    }
    // Fail closed: a registered checker that returns success:true passes; anything else --
    // including an empty/malformed object with no explicit success -- denies the call. An
    // empty result is not evidence of availability.
    if (availability.value(QStringLiteral("success")).toBool(false)) {
        return true;
    }
    outcome->availability_denied = true;
    outcome->result = availabilityDeniedResult(request, availability);
    if (m_health_ledger != nullptr) {
        m_health_ledger->recordFailure(
            outcome->health_key,
            outcome->result.value(QStringLiteral("failure_class"))
                .toString(QStringLiteral("availability_failed")),
            outcome->result.value(QStringLiteral("error_message")).toString(),
            0);
    }
    return false;
}

AiToolDispatcher::Handler AiToolDispatcher::resolveHandler(DispatchOutcome* outcome,
                                                           const AiToolCallRequest& request,
                                                           const QString& normalized_name) const {
    const auto it = m_handlers.constFind(normalized_name);
    if (it != m_handlers.constEnd() && it.value()) {
        return it.value();
    }
    outcome->handler_missing = true;
    outcome->result = handlerMissingResult(request);
    if (m_health_ledger != nullptr) {
        m_health_ledger->recordFailure(
            outcome->health_key,
            QStringLiteral("handler_missing"),
            outcome->result.value(QStringLiteral("error_message")).toString(),
            0);
    }
    return {};
}

bool AiToolDispatcher::acquireLeaseForDispatch(DispatchOutcome* outcome,
                                               const AiToolCallRequest& request,
                                               const QString& agent_id,
                                               QString* lease_id) const {
    if (!outcome->policy_decision.requires_lease) {
        return true;
    }
    if (m_lease_manager == nullptr) {
        // Fail closed: a call that requires a mutation lease must NOT run when no lease
        // manager is wired. Proceeding would let a mutating tool execute with no
        // cross-agent serialization -- the exact guarantee the lease exists to provide.
        outcome->lease_denied = true;
        outcome->result = leaseDeniedResult(
            request, QStringLiteral("Mutating lease required but no lease manager is configured"));
        return false;
    }
    const auto acquire =
        m_lease_manager->acquire(agent_id,
                                 QStringList{request.tool_name},
                                 QStringLiteral("system_change"),
                                 outcome->policy_decision.requires_exclusive_lease);
    if (acquire.granted) {
        *lease_id = acquire.lease.lease_id;
        outcome->lease_id = *lease_id;
        return true;
    }
    outcome->lease_denied = true;
    outcome->result = leaseDeniedResult(request, acquire.reason);
    return false;
}

void AiToolDispatcher::releaseLeaseForDispatch(DispatchOutcome* outcome,
                                               const AiToolCallRequest& request,
                                               const QString& lease_id) const {
    if (lease_id.isEmpty() || (m_lease_manager == nullptr)) {
        return;
    }
    if (m_lease_manager->release(lease_id)) {
        // Normal path: the lease this dispatch acquired was still held and is now released.
        return;
    }
    // release() reports false only when the id is no longer held. A lease id minted for this
    // dispatch carries an unguessable token no other agent can present, so the only way it can
    // vanish while THIS handler was running is the manager's TTL sweep reclaiming it as
    // "abandoned" -- the exact hazard behind this finding: a still-executing handler is not
    // proof the holder stopped, yet the TTL treated it as such and freed the lease. A second
    // agent may have acquired a fresh lease and mutated concurrently, so the exclusive-mutation
    // guarantee was broken for THIS call. Fail closed: never report a mutation whose exclusivity
    // was violated mid-flight as a clean success. Preserve any handler diagnostics but stamp the
    // breach over the top so triage and the health circuit breaker both see the real failure.
    outcome->lease_reclaimed_midop = true;
    outcome->result[QStringLiteral("success")] = false;
    outcome->result[QStringLiteral("lease_reclaimed_midop")] = true;
    outcome->result[QStringLiteral("failure_class")] = QStringLiteral("lease_reclaimed_midop");
    outcome->result[QStringLiteral("tool_name")] = request.tool_name;
    outcome->result[QStringLiteral("operation")] = request.operation;
    outcome->result[QStringLiteral("error_message")] =
        QStringLiteral(
            "Mutating lease '%1' was reclaimed by TTL while the handler was still "
            "running; exclusive access could not be guaranteed for this call")
            .arg(lease_id);
}

void AiToolDispatcher::recordHealthForResult(const DispatchOutcome& outcome) const {
    if (m_health_ledger == nullptr) {
        return;
    }
    if (resultSucceeded(outcome.result)) {
        m_health_ledger->recordSuccess(outcome.health_key, outcome.latency_ms);
        return;
    }
    m_health_ledger->recordFailure(outcome.health_key,
                                   AiToolHealthLedger::classifyResult(outcome.result),
                                   resultErrorMessage(outcome.result),
                                   outcome.latency_ms);
}

QJsonObject AiToolDispatcher::leaseDeniedResult(const AiToolCallRequest& request,
                                                const QString& reason) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("lease_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] =
        reason.isEmpty() ? QStringLiteral("Mutating lease unavailable") : reason;
    return obj;
}

QJsonObject AiToolDispatcher::policyDeniedResult(const AiToolCallRequest& request,
                                                 const AiToolPolicyDecision& decision) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("policy_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("risky_change")] = decision.risky_change;
    obj[QStringLiteral("requires_lease")] = decision.requires_lease;
    obj[QStringLiteral("requires_exclusive_lease")] = decision.requires_exclusive_lease;
    obj[QStringLiteral("restore_point_recommended")] = decision.restore_point_recommended;
    obj[QStringLiteral("error_message")] =
        decision.reason.isEmpty() ? QStringLiteral("Tool policy denied call") : decision.reason;
    return obj;
}

QJsonObject AiToolDispatcher::handlerMissingResult(const AiToolCallRequest& request) {
    QJsonObject obj;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("handler_missing")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("error_message")] =
        QStringLiteral("No handler registered for tool '%1'").arg(request.tool_name);
    return obj;
}

QJsonObject AiToolDispatcher::availabilityDeniedResult(const AiToolCallRequest& request,
                                                       const QJsonObject& availability) {
    QJsonObject obj = availability;
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("availability_denied")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    if (!obj.contains(QStringLiteral("failure_class"))) {
        obj[QStringLiteral("failure_class")] = QStringLiteral("availability_failed");
    }
    if (!obj.contains(QStringLiteral("error_message"))) {
        obj[QStringLiteral("error_message")] = QStringLiteral("Tool availability check failed");
    }
    return obj;
}

QJsonObject AiToolDispatcher::healthSuppressedResult(const AiToolCallRequest& request,
                                                     const AiToolAvailability& availability) {
    QJsonObject obj = availability.toJson();
    obj[QStringLiteral("success")] = false;
    obj[QStringLiteral("health_suppressed")] = true;
    obj[QStringLiteral("tool_name")] = request.tool_name;
    obj[QStringLiteral("operation")] = request.operation;
    obj[QStringLiteral("error_message")] =
        availability.reason.isEmpty() ? QStringLiteral("Tool/provider temporarily disabled")
                                      : availability.reason;
    return obj;
}

}  // namespace sak::ai
