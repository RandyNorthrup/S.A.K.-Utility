// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway_tool_runner.h"

#include "sak/ai/ai_app_action_planner.h"
#include "sak/ai/ai_credential_store.h"

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

bool chatOnly(AiProviderGatewayToolAccess access) {
    return access == AiProviderGatewayToolAccess::ChatAndResearch;
}

QJsonObject finalizeResult(QJsonObject result, const QString& operation) {
    if (result.isEmpty()) {
        return toolError(QStringLiteral("Provider gateway returned no data"));
    }
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = operation;
    return result;
}

QJsonObject authorizeWin32McpCall(const AiProviderGateway::Win32McpCallPlan& plan,
                                  AiProviderGatewayToolAccess access,
                                  const AiProviderGatewayToolCallbacks& callbacks) {
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

QJsonObject runWin32McpCall(const QJsonObject& args,
                            AiProviderGateway* gateway,
                            AiProviderGatewayToolAccess access,
                            const AiProviderGatewayToolCallbacks& callbacks) {
    if (chatOnly(access)) {
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
    return finalizeResult(std::move(result), QStringLiteral("win32_mcp_call"));
}

QJsonObject validateAppActionArgs(const QString& app_id, const QString& action) {
    if (app_id.isEmpty() || action.isEmpty()) {
        return toolError(QStringLiteral("app_run_action requires app_id and action"));
    }
    return {};
}

QJsonObject authorizeSensitiveAppAction(const AiAppActionPlan& plan,
                                        const AiProviderGatewayToolCallbacks& callbacks) {
    if (plan.guard_approval_reason.isEmpty()) {
        return {};
    }
    const QString preview =
        QStringLiteral("%1\n\n%2").arg(plan.guard_approval_reason, plan.preview);
    if (!callbacks.confirm) {
        return toolError(QStringLiteral("App action confirmation callback is not configured"));
    }
    return callbacks.confirm(QStringLiteral("Sensitive App Action"), preview, true)
               ? QJsonObject{}
               : toolError(QStringLiteral("User declined sensitive app action"));
}

QJsonObject authorizeAssistedAppAction(const AiAppActionPlan& plan,
                                       AiProviderGatewayToolAccess access,
                                       const AiProviderGatewayToolCallbacks& callbacks) {
    if (!assisted(access) || !plan.guard_approval_reason.isEmpty()) {
        return {};
    }
    if (!callbacks.confirm) {
        return toolError(QStringLiteral("App action confirmation callback is not configured"));
    }
    return callbacks.confirm(QStringLiteral("App Action"), plan.preview, plan.risky)
               ? QJsonObject{}
               : toolError(QStringLiteral("User declined app action"));
}

QJsonObject authorizeUnattendedRiskyAppAction(const AiAppActionPlan& plan,
                                              AiProviderGatewayToolAccess access,
                                              const AiProviderGatewayToolCallbacks& callbacks) {
    if (!unattended(access) || !plan.risky || !plan.guard_approval_reason.isEmpty()) {
        return {};
    }
    if (!callbacks.offer_restore_point) {
        return toolError(QStringLiteral("App action restore-point callback is not configured"));
    }
    return callbacks.offer_restore_point(plan.preview, true)
               ? QJsonObject{}
               : toolError(QStringLiteral("Restore point handling cancelled app action"));
}

QJsonObject authorizeAppAction(const AiAppActionPlan& plan,
                               AiProviderGatewayToolAccess access,
                               const AiProviderGatewayToolCallbacks& callbacks) {
    for (const QJsonObject& error : {authorizeSensitiveAppAction(plan, callbacks),
                                     authorizeAssistedAppAction(plan, access, callbacks),
                                     authorizeUnattendedRiskyAppAction(plan, access, callbacks)}) {
        if (!error.isEmpty()) {
            return error;
        }
    }
    return {};
}

QJsonObject requireAppActionExecutor(const AiProviderGatewayToolCallbacks& callbacks) {
    if (!callbacks.allocate_command_id) {
        return toolError(QStringLiteral("App action command-id allocator is not configured"));
    }
    if (!callbacks.execute_powershell) {
        return toolError(QStringLiteral("App action PowerShell executor is not configured"));
    }
    return {};
}

void logAppActionStart(const AiAppActionPlan& plan,
                       const QString& command_id,
                       const QString& admin_suffix,
                       const AiProviderGatewayToolCallbacks& callbacks) {
    if (callbacks.append_local_event) {
        callbacks.append_local_event(QStringLiteral("Running app action PowerShell%1 tool %2")
                                         .arg(admin_suffix, command_id));
    }
    if (callbacks.log_output) {
        callbacks.log_output(CredentialStore::redactSecrets(
            QStringLiteral("[%1 app action PowerShell%2] $ %3")
                .arg(command_id, admin_suffix, plan.request.command)));
    }
}

QJsonObject appActionResultJson(const AiCommandResult& command_result,
                                const AiAppActionPlan& plan,
                                const QString& app_id,
                                const QString& action,
                                const QString& command_id) {
    QJsonObject result = command_result.toJson();
    result[QStringLiteral("command_id")] = command_id;
    result[QStringLiteral("command")] = CredentialStore::redactSecrets(plan.request.command);
    result[QStringLiteral("preview")] = CredentialStore::redactSecrets(plan.preview);
    result[QStringLiteral("requires_admin")] = plan.request.requires_admin;
    result[QStringLiteral("app_id")] = app_id;
    result[QStringLiteral("action")] = action;
    result[QStringLiteral("display_name")] = plan.display_name;
    result[QStringLiteral("manifest_method")] = plan.method;
    result[QStringLiteral("evidence")] = plan.evidence;
    result[QStringLiteral("success")] = command_result.started && !command_result.cancelled &&
                                        !command_result.timed_out && command_result.exit_code == 0;
    result[QStringLiteral("operation")] = QStringLiteral("app_run_action");
    return result;
}

void recordAppActionCompletion(const AiCommandResult& command_result,
                               const AiAppActionPlan& plan,
                               const QString& command_id,
                               const QJsonObject& result,
                               const AiProviderGatewayToolCallbacks& callbacks) {
    if (callbacks.record_command) {
        callbacks.record_command(plan.preview, result);
    }
    if (callbacks.append_session_memory) {
        callbacks.append_session_memory(QStringLiteral("Tool"),
                                        QStringLiteral("App action finished"),
                                        QStringLiteral("%1 exit=%2 cancelled=%3 timed_out=%4")
                                            .arg(plan.preview.left(500))
                                            .arg(command_result.exit_code)
                                            .arg(command_result.cancelled)
                                            .arg(command_result.timed_out));
    }
    if (callbacks.append_local_event) {
        callbacks.append_local_event(
            QStringLiteral("App action %1 finished (exit %2%3)")
                .arg(command_id)
                .arg(command_result.exit_code)
                .arg(command_result.cancelled   ? QStringLiteral(", cancelled")
                     : command_result.timed_out ? QStringLiteral(", timed out")
                                                : QString()));
    }
}

QJsonObject appActionPlanResult(const QString& operation,
                                QJsonObject result,
                                const QString& error) {
    if (!error.isEmpty()) {
        return toolError(error);
    }
    const bool supported = result.value(QStringLiteral("requested_action_supported")).toBool(false);
    result[QStringLiteral("execution_supported")] = supported;
    result[QStringLiteral("execution_message")] =
        supported ? QStringLiteral(
                        "Use the manifest-supported control path; do not improvise raw GUI or "
                        "helper executable probing.")
                  : QStringLiteral(
                        "Requested app action is not supported by bundled manifest. Stop and ask "
                        "user for manual GUI action or another scanner/control path.");
    return finalizeResult(std::move(result), operation);
}

QJsonObject runGatewayReadOperation(const QString& operation,
                                    const QJsonObject& args,
                                    AiProviderGateway* gateway) {
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
    } else if (operation == QLatin1String("app_manifest")) {
        result = gateway->appManifest(args.value(QStringLiteral("app_id")).toString(), &error);
    } else if (operation == QLatin1String("app_capabilities")) {
        result = gateway->appCapabilities(args.value(QStringLiteral("app_id")).toString(),
                                          args.value(QStringLiteral("action")).toString(),
                                          &error);
    } else if (operation == QLatin1String("app_run_action_plan")) {
        result = gateway->appCapabilities(args.value(QStringLiteral("app_id")).toString(),
                                          args.value(QStringLiteral("action")).toString(),
                                          &error);
        return appActionPlanResult(operation, std::move(result), error);
    } else {
        return toolError(
            QStringLiteral("Unsupported provider gateway operation: %1").arg(operation));
    }

    return error.isEmpty() ? finalizeResult(std::move(result), operation) : toolError(error);
}

QJsonObject runAppAction(const QJsonObject& args,
                         AiProviderGateway* gateway,
                         AiProviderGatewayToolAccess access,
                         const AiProviderGatewayToolCallbacks& callbacks,
                         AiProviderGatewayToolOptions options) {
    if (chatOnly(access)) {
        return toolError(QStringLiteral("App action execution is disabled in Chat mode"));
    }

    const QString app_id = args.value(QStringLiteral("app_id")).toString().trimmed();
    const QString action = args.value(QStringLiteral("action")).toString().trimmed();
    QJsonObject validation_error = validateAppActionArgs(app_id, action);
    if (!validation_error.isEmpty()) {
        return validation_error;
    }

    QString error;
    const QJsonObject manifest = gateway->appCapabilities(app_id, action, &error);
    if (!error.isEmpty()) {
        return toolError(error);
    }
    const AiAppActionPlan plan =
        AiAppActionPlanner::buildPlan(app_id,
                                      action,
                                      manifest,
                                      args.value(QStringLiteral("arguments")).toObject(),
                                      AiAppActionPlanner::Options{options.default_output_bytes,
                                                                  options.min_output_bytes,
                                                                  options.max_output_bytes});
    if (!plan.ok()) {
        return toolError(plan.error_message);
    }

    QJsonObject authorization_error = authorizeAppAction(plan, access, callbacks);
    if (!authorization_error.isEmpty()) {
        return authorization_error;
    }
    QJsonObject executor_error = requireAppActionExecutor(callbacks);
    if (!executor_error.isEmpty()) {
        return executor_error;
    }

    const QString command_id = callbacks.allocate_command_id();
    const QString admin_suffix = plan.request.requires_admin ? QStringLiteral(" *ADMIN*")
                                                             : QString();
    logAppActionStart(plan, command_id, admin_suffix, callbacks);
    const AiCommandResult command_result = callbacks.execute_powershell(plan.request, command_id);
    const QJsonObject result =
        appActionResultJson(command_result, plan, app_id, action, command_id);
    recordAppActionCompletion(command_result, plan, command_id, result, callbacks);
    return result;
}

}  // namespace

QJsonObject AiProviderGatewayToolRunner::run(const QJsonObject& args,
                                             AiProviderGateway* gateway,
                                             AiProviderGatewayToolAccess access,
                                             const AiProviderGatewayToolCallbacks& callbacks,
                                             AiProviderGatewayToolOptions options) {
    if (!gateway) {
        return toolError(QStringLiteral("Provider gateway is not configured"));
    }

    const QString operation =
        args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    if (operation.isEmpty()) {
        return toolError(QStringLiteral("Provider gateway requires operation"));
    }

    if (operation == QLatin1String("win32_mcp_call")) {
        return runWin32McpCall(args, gateway, access, callbacks);
    }
    if (operation == QLatin1String("app_run_action")) {
        return runAppAction(args, gateway, access, callbacks, options);
    }
    return runGatewayReadOperation(operation, args, gateway);
}

}  // namespace sak::ai
