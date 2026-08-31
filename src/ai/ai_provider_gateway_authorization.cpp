// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway_authorization.h"

namespace sak::ai {

namespace {

QJsonObject toolError(const QString& message) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error_message")] = message;
    return result;
}

bool assisted(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::AssistedFullAccess;
}

bool unattended(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::UnattendedFullAccess;
}

// Tools carrying requires_confirmation span browser/desktop input, clipboard writes,
// extension changes, and unclassified operations. They demand explicit human confirmation in
// every non-chat access mode, including Unattended. Use a neutral identity: the exact tool and
// arguments remain in plan.preview, while the trusted dialog title must never mislabel a desktop
// or extension action as browser input. Returns empty on approval, a tool error on refusal.
QJsonObject requireHardConfirmation(const AiProviderGateway::Win32McpCallPlan& plan,
                                    const AiProviderGatewayToolCallbacks& callbacks) {
    if (!callbacks.confirm) {
        return toolError(QStringLiteral("Win32 MCP confirmation callback is not configured"));
    }
    if (!callbacks.confirm(QStringLiteral("Win32 MCP action"), plan.preview, /*risky=*/true)) {
        return toolError(QStringLiteral("User declined Win32 MCP action"));
    }
    return {};
}

}  // namespace

QJsonObject authorizeWin32McpCall(const AiProviderGateway::Win32McpCallPlan& plan,
                                  AiProviderGatewayToolAccess access,
                                  const AiProviderGatewayToolCallbacks& callbacks) {
    if (plan.requires_confirmation) {
        return requireHardConfirmation(plan, callbacks);
    }
    if (assisted(access) && !plan.read_only) {
        if (!callbacks.confirm) {
            return toolError(QStringLiteral("Win32 MCP confirmation callback is not configured"));
        }
        if (!callbacks.confirm(
                QStringLiteral("Win32 MCP automation"), plan.preview, plan.high_risk)) {
            return toolError(QStringLiteral("User declined Win32 MCP automation"));
        }
    }
    if (unattended(access) && plan.high_risk) {
        if (!callbacks.offer_restore_point) {
            return toolError(QStringLiteral("Win32 MCP restore-point callback is not configured"));
        }
        if (!callbacks.offer_restore_point(plan.preview, true)) {
            return toolError(
                QStringLiteral("Restore point handling cancelled Win32 MCP high-risk automation"));
        }
    }
    return {};
}

}  // namespace sak::ai
