// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace sak::ai {

struct AiWorkflowPhaseContext;

/// @brief Controls how a substituted placeholder value is escaped for its
/// destination. `Raw` inserts the value verbatim; `PowerShellSingleQuoted`
/// doubles embedded single quotes so the value is safe inside a PowerShell
/// single-quoted string literal.
enum class WorkflowPlaceholderMode {
    Raw,
    PowerShellSingleQuoted,
};

/// @brief Resolves a single workflow input key to a display string. Strings are
/// trimmed; string arrays are joined with ", "; missing/empty values return
/// @p fallback.
[[nodiscard]] QString workflowInputValue(const AiWorkflowPhaseContext& context,
                                         const QString& key,
                                         const QString& fallback = {});

/// @brief Replaces every `${key}` token in @p text with its resolved value.
/// Recognized keys: `user_message`, `workflow_id`, `run_id`,
/// `result_<phase>_<field>` (prior phase results), and any workflow input key.
/// Unknown keys resolve to empty. See @ref WorkflowPlaceholderMode for escaping.
[[nodiscard]] QString substituteWorkflowPlaceholders(
    const QString& text,
    const AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw);

/// @brief Recursively substitutes placeholders in every string leaf of a JSON
/// object (used for workflow tool-call argument objects).
[[nodiscard]] QJsonObject substituteWorkflowPlaceholdersInObject(
    const QJsonObject& object,
    const AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw);

/// @brief Recursively substitutes placeholders in a JSON value of any type.
[[nodiscard]] QJsonValue substituteWorkflowPlaceholdersInValue(
    const QJsonValue& value,
    const AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw);

/// @brief Validate that a run_powershell workflow command template is safe for
/// PowerShellSingleQuoted substitution: every `${key}` placeholder (using the same
/// `[A-Za-z0-9_]+` grammar the substitutor recognizes) must sit INSIDE a single-quoted
/// literal. In that mode the substitutor only doubles embedded single quotes; a placeholder
/// placed outside a `'...'` literal would inject its value UNESCAPED. Returns false and sets
/// @p error on the first offending placeholder. Pure; unit-testable.
[[nodiscard]] bool powerShellCommandTemplateIsSingleQuoteSafe(const QString& command,
                                                              QString* error = nullptr);

}  // namespace sak::ai
