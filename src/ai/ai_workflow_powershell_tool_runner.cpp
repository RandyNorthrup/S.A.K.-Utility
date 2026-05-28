// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_powershell_tool_runner.h"

#include "sak/ai/ai_command_guard.h"
#include "sak/ai/ai_credential_store.h"

#include <algorithm>

namespace sak::ai {

namespace {

constexpr qsizetype kSessionMemoryPreviewChars = 500;

QJsonObject toolError(const QString& message) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error_message")] = message;
    return result;
}

QJsonObject resultJson(const AiCommandRequest& request,
                       const QString& command_id,
                       const QString& preview,
                       const AiCommandResult& result) {
    QJsonObject result_json = result.toJson();
    result_json[QStringLiteral("success")] = result.started && !result.cancelled &&
                                             !result.timed_out && result.exit_code == 0;
    result_json[QStringLiteral("command_id")] = command_id;
    result_json[QStringLiteral("command")] = CredentialStore::redactSecrets(request.command);
    result_json[QStringLiteral("preview")] = CredentialStore::redactSecrets(preview);
    result_json[QStringLiteral("requires_admin")] = request.requires_admin;
    return result_json;
}

QJsonObject validateWorkflowCallbacks(const AiWorkflowPowerShellToolCallbacks& callbacks) {
    if (!callbacks.allocate_command_id) {
        return toolError(
            QStringLiteral("Workflow PowerShell command-id allocator is not configured"));
    }
    if (!callbacks.execute_powershell) {
        return toolError(QStringLiteral("Workflow PowerShell executor is not configured"));
    }
    return {};
}

QJsonObject enforceWorkflowGuard(const AiCommandRequest& request,
                                 const QString& preview,
                                 const AiWorkflowPowerShellToolCallbacks& callbacks) {
    const QString block_error = commandGuardBlockError(request, preview);
    if (!block_error.isEmpty()) {
        if (callbacks.append_local_event) {
            callbacks.append_local_event(
                QStringLiteral("Workflow PowerShell guard blocked command: %1").arg(block_error));
        }
        return toolError(block_error);
    }

    const QString approval_reason = commandGuardApprovalReason(request, preview);
    if (approval_reason.isEmpty()) {
        return {};
    }
    if (!callbacks.confirm) {
        return toolError(
            QStringLiteral("Sensitive workflow command confirmation callback is not configured"));
    }
    const QString approval_preview = QStringLiteral("%1\n\n%2").arg(approval_reason, preview);
    if (callbacks.confirm(QStringLiteral("Sensitive Workflow Command"), approval_preview, true)) {
        return {};
    }
    if (callbacks.append_local_event) {
        callbacks.append_local_event(QStringLiteral("User declined sensitive workflow command"));
    }
    return toolError(QStringLiteral("User declined sensitive workflow command"));
}

void logWorkflowStart(const AiCommandRequest& request,
                      const QString& command_id,
                      const QString& admin_suffix,
                      const AiWorkflowPowerShellToolCallbacks& callbacks) {
    if (callbacks.append_local_event) {
        callbacks.append_local_event(QStringLiteral("Running workflow PowerShell%1 tool phase %2")
                                         .arg(admin_suffix, command_id));
    }
    if (callbacks.log_output) {
        callbacks.log_output(
            CredentialStore::redactSecrets(QStringLiteral("[%1 workflow PowerShell%2] $ %3")
                                               .arg(command_id, admin_suffix, request.command)));
    }
}

void recordWorkflowCompletion(const QString& command_id,
                              const QString& preview,
                              const AiCommandResult& command_result,
                              const QJsonObject& json,
                              const AiWorkflowPowerShellToolCallbacks& callbacks) {
    if (callbacks.record_command) {
        callbacks.record_command(preview, json);
    }
    if (callbacks.append_session_memory) {
        callbacks.append_session_memory(QStringLiteral("Tool"),
                                        QStringLiteral("Workflow PowerShell finished"),
                                        QStringLiteral("%1 exit=%2 cancelled=%3 timed_out=%4")
                                            .arg(preview.left(kSessionMemoryPreviewChars))
                                            .arg(command_result.exit_code)
                                            .arg(command_result.cancelled)
                                            .arg(command_result.timed_out));
    }
    if (callbacks.append_local_event) {
        callbacks.append_local_event(
            QStringLiteral("Workflow PowerShell %1 finished (exit %2%3)")
                .arg(command_id)
                .arg(command_result.exit_code)
                .arg(command_result.cancelled   ? QStringLiteral(", cancelled")
                     : command_result.timed_out ? QStringLiteral(", timed out")
                                                : QString()));
    }
}

}  // namespace

QJsonObject AiWorkflowPowerShellToolRunner::run(const QJsonObject& args,
                                                const QString& command_preview,
                                                const AiWorkflowPowerShellToolCallbacks& callbacks,
                                                AiWorkflowPowerShellToolOptions options) {
    AiCommandRequest request = ExecutionBroker::requestFromJson(args);
    request.max_output_bytes =
        std::max(args.value(QStringLiteral("max_output_bytes")).toInt(options.default_output_bytes),
                 options.min_output_bytes);
    if (request.command.trimmed().isEmpty()) {
        return toolError(
            QStringLiteral("Workflow PowerShell phase requires explicit arguments.command"));
    }
    QJsonObject callback_error = validateWorkflowCallbacks(callbacks);
    if (!callback_error.isEmpty()) {
        return callback_error;
    }

    const QString command_id = callbacks.allocate_command_id();
    const QString preview = command_preview.trimmed().isEmpty() ? request.command
                                                                : command_preview.trimmed();
    QJsonObject guard_error = enforceWorkflowGuard(request, preview, callbacks);
    if (!guard_error.isEmpty()) {
        return guard_error;
    }

    const QString admin_suffix = request.requires_admin ? QStringLiteral(" *ADMIN*") : QString();
    logWorkflowStart(request, command_id, admin_suffix, callbacks);

    const AiCommandResult command_result = callbacks.execute_powershell(request, command_id);
    QJsonObject json = resultJson(request, command_id, preview, command_result);
    recordWorkflowCompletion(command_id, preview, command_result, json, callbacks);
    return json;
}

}  // namespace sak::ai
