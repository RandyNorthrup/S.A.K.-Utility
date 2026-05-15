// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_dispatcher.h"

namespace sak::ai {

namespace {

QString normalizeName(const QString& tool_name) {
    return tool_name.trimmed().toLower();
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

void AiToolDispatcher::clearHandlers() {
    m_handlers.clear();
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
    const auto it = m_handlers.constFind(name);
    if (it == m_handlers.constEnd() || !it.value()) {
        outcome.handler_missing = true;
        outcome.result = handlerMissingResult(request);
        return outcome;
    }

    QString lease_id;
    if (m_lease_manager && outcome.policy_decision.requires_lease) {
        QStringList scope;
        scope << request.tool_name;
        const auto acquire =
            m_lease_manager->acquire(agent_id,
                                     scope,
                                     QStringLiteral("system_change"),
                                     outcome.policy_decision.requires_exclusive_lease);
        if (!acquire.granted) {
            outcome.lease_denied = true;
            outcome.result = leaseDeniedResult(request, acquire.reason);
            return outcome;
        }
        lease_id = acquire.lease.lease_id;
        outcome.lease_id = lease_id;
    }

    outcome.result = it.value()(arguments, outcome.policy_decision);
    outcome.dispatched = true;
    if (!lease_id.isEmpty() && m_lease_manager) {
        m_lease_manager->release(lease_id);
    }
    return outcome;
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

}  // namespace sak::ai
