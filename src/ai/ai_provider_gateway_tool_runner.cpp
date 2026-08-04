// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_gateway_tool_runner.h"

#include "sak/ai/ai_app_action_planner.h"
#include "sak/ai/ai_credential_store.h"
#include "sak/ai/ai_win32_gui_runner.h"

#include <QJsonDocument>
#include <QJsonParseError>

namespace sak::ai {

namespace {

constexpr qsizetype kSessionMemoryPreviewChars = 500;

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
        if (error_message) {
            *error_message = QStringLiteral(
                "Provider gateway arguments must be a JSON object string; use {} when unused");
        }
        return {};
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        if (error_message) {
            *error_message =
                QStringLiteral("Provider gateway arguments must be a JSON object string: %1")
                    .arg(parse_error.errorString());
        }
        return {};
    }
    if (!doc.isObject()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Provider gateway arguments JSON must decode to an object");
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

QJsonObject normalizedGatewayArgs(QJsonObject args, QString* error_message) {
    const QJsonValue arguments = args.value(QStringLiteral("arguments"));
    if (arguments.isUndefined() || arguments.isObject()) {
        if (error_message) {
            error_message->clear();
        }
        return args;
    }
    if (!arguments.isString()) {
        if (error_message) {
            *error_message = QStringLiteral(
                "Provider gateway arguments must be an object or JSON object string");
        }
        return {};
    }

    const QJsonObject parsed = parseArgumentsString(arguments.toString(), error_message);
    if (parsed.isEmpty() && error_message && !error_message->isEmpty()) {
        return {};
    }
    args[QStringLiteral("arguments")] = parsed;
    return args;
}

// Browser input injection (click/type/press-key/scroll) drives the user's logged-in
// browser. It demands an explicit human confirmation in EVERY non-chat access mode --
// including Unattended, where other win32 automation would otherwise auto-run or merely
// offer a restore point -- so an autonomous or injected model cannot silently act as the
// user. The prompt is the app's trusted UI; nothing the page or model controls can
// satisfy this gate. Returns empty on approval, a tool error on refusal / misconfig.
QJsonObject requireInputConfirmation(const AiProviderGateway::Win32McpCallPlan& plan,
                                     const AiProviderGatewayToolCallbacks& callbacks) {
    if (!callbacks.confirm) {
        return toolError(QStringLiteral("Win32 MCP confirmation callback is not configured"));
    }
    if (!callbacks.confirm(QStringLiteral("Browser input action"), plan.preview, /*risky=*/true)) {
        return toolError(QStringLiteral("User declined the browser input action"));
    }
    return {};
}

QJsonObject authorizeWin32McpCall(const AiProviderGateway::Win32McpCallPlan& plan,
                                  AiProviderGatewayToolAccess access,
                                  const AiProviderGatewayToolCallbacks& callbacks) {
    if (plan.requires_confirmation) {
        return requireInputConfirmation(plan, callbacks);
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

QJsonObject runWin32McpCall(const QJsonObject& args,
                            const AiProviderGateway* gateway,
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
    if (result.value(QStringLiteral("mcp_is_error")).toBool(false)) {
        const QString text = result.value(QStringLiteral("result_text")).toString();
        return toolError(text.isEmpty() ? QStringLiteral("Win32 MCP tool reported an error")
                                        : QStringLiteral("Win32 MCP tool error: %1").arg(text));
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
                                            .arg(plan.preview.left(kSessionMemoryPreviewChars))
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

// Backs one win32_gui recipe step with the real desktop-control engine: plan the tool call
// (validating it against the live provider manifest + risk classification) then execute it over
// the win32 stdio pipe. High-risk steps are flagged (not executed) so the step runner can reject
// the whole recipe. The recipe already has its single action-level authorization; individual
// vetted-manifest steps are not re-confirmed.
Win32StepOutcome executeWin32GuiStep(const QJsonObject& step, const AiProviderGateway* gateway) {
    Win32StepOutcome outcome;
    QJsonObject inner{
        {QStringLiteral("tool_name"), step.value(QStringLiteral("tool")).toString().trimmed()},
        {QStringLiteral("tool_arguments"), step.value(QStringLiteral("arguments")).toObject()}};
    if (step.contains(QStringLiteral("timeout_ms"))) {
        inner[QStringLiteral("timeout_ms")] = step.value(QStringLiteral("timeout_ms"));
    }
    const QJsonObject call_args{{QStringLiteral("arguments"), inner}};

    QString error;
    const AiProviderGateway::Win32McpCallPlan call_plan = gateway->planWin32McpCall(call_args,
                                                                                    &error);
    if (!error.isEmpty()) {
        outcome.error = error;
        return outcome;
    }
    outcome.planned = true;
    outcome.high_risk = call_plan.high_risk;
    if (outcome.high_risk) {
        return outcome;  // rejected by executeWin32GuiSteps; do not run
    }
    // A recipe may only contain read-only tools and the vetted input-tier desktop tools. A
    // middle-tier tool (planned, not high-risk, but still requires_confirmation via the fail-closed
    // default -- browser cookies/http-auth/download/storage/permission, extension install/
    // uninstall, clipboard_write, ...) demands a per-call human confirmation on the direct path, so
    // it must not auto-run here. Flag it (do not execute) for executeWin32GuiSteps to reject.
    if (!call_plan.read_only && !AiProviderGateway::isWin32InputTool(call_plan.tool_name)) {
        outcome.disallowed = true;
        outcome.error = QStringLiteral(
                            "tool '%1' is not permitted in a win32_gui recipe; only read-only and "
                            "input-tier desktop tools may run without a per-call confirmation")
                            .arg(call_plan.tool_name);
        return outcome;
    }

    QJsonObject result = gateway->callWin32Mcp(call_plan, &error);
    if (!error.isEmpty()) {
        outcome.tool_error = true;
        outcome.error = error;
        return outcome;
    }
    outcome.tool_error = result.value(QStringLiteral("mcp_is_error")).toBool(false);
    outcome.result_text = result.value(QStringLiteral("result_text")).toString();

    // A wait_for_* step whose condition never came true (found / satisfied / idle == false) is a
    // failed step even though the tool returned no error: the recipe's expected state was not
    // reached (e.g. a scan-complete marker never appeared before the timeout), so downstream steps
    // would run against the wrong screen. Treat it as a tool error (optional steps still tolerate
    // it). The classification lives in the pure, unit-tested win32WaitExpectationFailure.
    if (!outcome.tool_error) {
        // A wait_for_* step MUST prove its awaited condition held: require a satisfied flag so a
        // truncated/garbled result_text (which parses to {}) fails closed instead of passing.
        const bool is_wait_step = call_plan.tool_name.contains(QLatin1String("wait_for"),
                                                               Qt::CaseInsensitive);
        const QJsonObject payload = QJsonDocument::fromJson(outcome.result_text.toUtf8()).object();
        if (const std::optional<QString> failure = win32WaitExpectationFailure(payload,
                                                                               is_wait_step)) {
            outcome.tool_error = true;
            outcome.error = *failure;
        }
    }
    return outcome;
}

QJsonObject runWin32GuiAction(const AiAppActionPlan& plan,
                              const QString& app_id,
                              const QString& action,
                              const AiProviderGateway* gateway,
                              const AiProviderGatewayToolCallbacks& callbacks) {
    if (callbacks.append_local_event) {
        callbacks.append_local_event(QStringLiteral("Running app GUI action %1/%2 (%3 steps)")
                                         .arg(app_id, action)
                                         .arg(plan.steps.size()));
    }
    const QJsonObject steps_result =
        executeWin32GuiSteps(plan.steps, [gateway](const QJsonObject& step) {
            return executeWin32GuiStep(step, gateway);
        });

    QJsonObject result = steps_result;
    result[QStringLiteral("app_id")] = app_id;
    result[QStringLiteral("action")] = action;
    result[QStringLiteral("display_name")] = plan.display_name;
    result[QStringLiteral("manifest_method")] = plan.method;
    result[QStringLiteral("preview")] = plan.preview;
    result[QStringLiteral("evidence")] = plan.evidence;
    result[QStringLiteral("operation")] = QStringLiteral("app_run_action");
    if (callbacks.record_command) {
        callbacks.record_command(plan.preview, result);
    }
    return result;
}

QJsonObject runAppAction(const QJsonObject& args,
                         const AiProviderGateway* gateway,
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

    // GUI recipes drive the desktop-control engine step by step instead of running a shell
    // command, so they take the win32 path rather than the PowerShell executor.
    if (plan.method == QLatin1String("win32_gui")) {
        return runWin32GuiAction(plan, app_id, action, gateway, callbacks);
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
                                             const AiProviderGateway* gateway,
                                             AiProviderGatewayToolAccess access,
                                             const AiProviderGatewayToolCallbacks& callbacks,
                                             AiProviderGatewayToolOptions options) {
    if (!gateway) {
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
    if (operation == QLatin1String("app_run_action")) {
        return runAppAction(normalized_args, gateway, access, callbacks, options);
    }
    return runGatewayReadOperation(operation, normalized_args, gateway);
}

}  // namespace sak::ai
