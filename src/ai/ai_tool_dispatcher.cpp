// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_dispatcher.h"

#include <QElapsedTimer>
#include <QStringList>

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

bool resultSucceeded(const QJsonObject& result) {
    if (result.contains(QStringLiteral("success"))) {
        return result.value(QStringLiteral("success")).toBool(false);
    }
    return true;
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
    return m_handlers.size();
}

void AiToolDispatcher::setLeaseManager(AiLeaseManager* manager) {
    m_lease_manager = manager;
}

void AiToolDispatcher::setHealthLedger(AiToolHealthLedger* ledger) {
    m_health_ledger = ledger;
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

    QElapsedTimer timer;
    timer.start();
    outcome.result = handler(arguments, outcome.policy_decision);
    outcome.latency_ms = timer.elapsed();
    outcome.dispatched = true;
    if (outcome.result.isEmpty()) {
        outcome.result = emptyHandlerResult(request);
    }
    recordHealthForResult(outcome);
    if (!lease_id.isEmpty() && m_lease_manager) {
        m_lease_manager->release(lease_id);
    }
    return outcome;
}

bool AiToolDispatcher::applyHealthGate(DispatchOutcome* outcome,
                                       const AiToolCallRequest& request) const {
    if (!m_health_ledger) {
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
    const auto checker = m_availability_checkers.constFind(normalized_name);
    if (checker == m_availability_checkers.constEnd() || !checker.value()) {
        return true;
    }
    const QJsonObject availability = checker.value()(arguments, outcome->policy_decision);
    if (availability.isEmpty() || availability.value(QStringLiteral("success")).toBool(false)) {
        return true;
    }
    outcome->availability_denied = true;
    outcome->result = availabilityDeniedResult(request, availability);
    if (m_health_ledger) {
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
    if (m_health_ledger) {
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
    if (!m_lease_manager || !outcome->policy_decision.requires_lease) {
        return true;
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

void AiToolDispatcher::recordHealthForResult(const DispatchOutcome& outcome) const {
    if (!m_health_ledger) {
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
