// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_lease_manager.h"
#include "sak/ai/ai_tool_policy.h"

#include <QHash>
#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

/// @brief Routes structured tool calls through an AiToolPolicy gate before
/// invoking the panel-supplied handler.
///
/// The panel registers a handler per tool name. The dispatcher evaluates the
/// active AiToolPolicy first; if the call is blocked, it returns a structured
/// `policy_denied` payload without invoking the handler. Allowed calls reach
/// the handler with the parsed JSON arguments and the policy decision so the
/// handler can attach lease/restore-point context to its result.
class AiToolDispatcher {
public:
    using Handler = std::function<QJsonObject(const QJsonObject& arguments,
                                              const AiToolPolicyDecision& decision)>;

    struct DispatchOutcome {
        QJsonObject result;
        AiToolPolicyDecision policy_decision;
        bool dispatched{false};
        bool handler_missing{false};
        bool lease_denied{false};
        QString lease_id;
    };

    AiToolDispatcher() = default;

    void registerHandler(const QString& tool_name, Handler handler);
    void clearHandlers();
    [[nodiscard]] bool hasHandler(const QString& tool_name) const;
    [[nodiscard]] int handlerCount() const;

    void setLeaseManager(AiLeaseManager* manager);
    [[nodiscard]] AiLeaseManager* leaseManager() const { return m_lease_manager; }

    [[nodiscard]] DispatchOutcome dispatch(AiToolPolicy policy,
                                           const AiToolCallRequest& request,
                                           const QJsonObject& arguments,
                                           const QString& agent_id = {}) const;

    [[nodiscard]] static QJsonObject policyDeniedResult(const AiToolCallRequest& request,
                                                        const AiToolPolicyDecision& decision);
    [[nodiscard]] static QJsonObject handlerMissingResult(const AiToolCallRequest& request);
    [[nodiscard]] static QJsonObject leaseDeniedResult(const AiToolCallRequest& request,
                                                       const QString& reason);

private:
    QHash<QString, Handler> m_handlers;
    AiLeaseManager* m_lease_manager{nullptr};
};

}  // namespace sak::ai
