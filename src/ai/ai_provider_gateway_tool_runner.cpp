// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway_tool_runner.h"

#include "sak/ai/ai_provider_gateway_authorization.h"
#include "sak/ai/ai_tool_error.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace sak::ai {

namespace {

QJsonObject finalizeResult(QJsonObject result, const QString& operation) {
    if (result.isEmpty()) {
        return toolError(QStringLiteral("Provider gateway returned no data"));
    }
    // Only DEFAULT success to true when the handler did not set it; unconditionally
    // overwriting would clobber a handler's own success:false into a false success.
    if (!result.contains(QStringLiteral("success"))) {
        result[QStringLiteral("success")] = true;
    }
    result[QStringLiteral("operation")] = operation;
    return result;
}

QJsonObject parseArgumentsString(const QString& text, QString* error_message) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral(
                "Provider gateway arguments must be a JSON object string; use {} when unused");
        }
        return {};
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Provider gateway arguments must be a JSON object string: %1")
                    .arg(parse_error.errorString());
        }
        return {};
    }
    if (!doc.isObject()) {
        if (error_message != nullptr) {
            *error_message =
                QStringLiteral("Provider gateway arguments JSON must decode to an object");
        }
        return {};
    }
    if (error_message != nullptr) {
        error_message->clear();
    }
    return doc.object();
}

QJsonObject normalizedGatewayArgs(QJsonObject args, QString* error_message) {
    const QJsonValue arguments = args.value(QStringLiteral("arguments"));
    if (arguments.isUndefined() || arguments.isObject()) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return args;
    }
    if (!arguments.isString()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral(
                "Provider gateway arguments must be an object or JSON object string");
        }
        return {};
    }

    const QJsonObject parsed = parseArgumentsString(arguments.toString(), error_message);
    if (parsed.isEmpty() && (error_message != nullptr) && !error_message->isEmpty()) {
        return {};
    }
    args[QStringLiteral("arguments")] = parsed;
    return args;
}

// authorizeWin32McpCall (the win32 MCP authorization gate) and its requireInputConfirmation helper
// were extracted to ai_provider_gateway_authorization.{h,cpp} so the full authorization matrix is
// unit-testable without linking this TU's AI execution subsystem dependencies. This runner keeps
// calling authorizeWin32McpCall exactly as before.

QJsonObject runWin32McpCall(const QJsonObject& args,
                            const AiProviderGateway* gateway,
                            AiProviderGatewayToolAccess access,
                            const AiProviderGatewayToolCallbacks& callbacks) {
    if (providerGatewayAccessIsChatOnly(access)) {
        return toolError(QStringLiteral("Win32 MCP local automation is disabled in Chat mode"));
    }

    QString error;
    const AiProviderGateway::Win32McpCallPlan plan = gateway->planWin32McpCall(args, &error);
    if (!error.isEmpty()) {
        return toolError(error);
    }
    QJsonObject authorization_error = authorizeWin32McpCall(plan, access, callbacks);
    if (!authorization_error.isEmpty()) {
        return authorization_error;
    }

    QJsonObject result = gateway->callWin32Mcp(plan, &error);
    if (!error.isEmpty()) {
        return toolError(error);
    }
    if (result.value(QStringLiteral("mcp_is_error")).toBool(false)) {
        const QString text = result.value(QStringLiteral("result_text")).toString();
        return toolError(text.isEmpty() ? QStringLiteral("Win32 MCP tool reported an error")
                                        : QStringLiteral("Win32 MCP tool error: %1").arg(text));
    }
    return finalizeResult(std::move(result), QStringLiteral("win32_mcp_call"));
}

QJsonObject runGatewayReadOperation(const QString& operation,
                                    const QJsonObject& args,
                                    const AiProviderGateway* gateway) {
    QString error;
    QJsonObject result;
    if (operation == QLatin1String("providers")) {
        result = gateway->providers(&error);
    } else if (operation == QLatin1String("provider_status")) {
        const QString provider_id = args.value(QStringLiteral("provider_id")).toString().trimmed();
        result = provider_id.isEmpty() ? gateway->providerStatuses(&error)
                                       : gateway->providerStatus(provider_id, &error);
    } else if (operation == QLatin1String("docs_query")) {
        result = gateway->docsQuery(args, &error);
    } else {
        return toolError(
            QStringLiteral("Unsupported provider gateway operation: %1").arg(operation));
    }

    return error.isEmpty() ? finalizeResult(std::move(result), operation) : toolError(error);
}

}  // namespace

QJsonObject AiProviderGatewayToolRunner::run(const QJsonObject& args,
                                             const AiProviderGateway* gateway,
                                             AiProviderGatewayToolAccess access,
                                             const AiProviderGatewayToolCallbacks& callbacks) {
    if (gateway == nullptr) {
        return toolError(QStringLiteral("Provider gateway is not configured"));
    }

    QString argument_error;
    const QJsonObject normalized_args = normalizedGatewayArgs(args, &argument_error);
    if (!argument_error.isEmpty()) {
        return toolError(argument_error);
    }

    const QString operation =
        normalized_args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    if (operation.isEmpty()) {
        return toolError(QStringLiteral("Provider gateway requires operation"));
    }

    if (operation == QLatin1String("win32_mcp_call")) {
        return runWin32McpCall(normalized_args, gateway, access, callbacks);
    }
    return runGatewayReadOperation(operation, normalized_args, gateway);
}

}  // namespace sak::ai
