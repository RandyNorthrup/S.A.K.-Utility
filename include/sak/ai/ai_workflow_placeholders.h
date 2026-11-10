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
/// trimmed; an array is joined with ", " only when EVERY member is a non-empty
/// string -- one malformed member makes the whole array unresolvable, so a caller
/// can never receive a silently truncated subset. Missing, empty, wrong-typed and
/// malformed values all return @p fallback, which callers that drive real work
/// must treat as "absent" and refuse, not as a usable default.
[[nodiscard]] QString workflowInputValue(const AiWorkflowPhaseContext& context,
                                         const QString& key,
                                         const QString& fallback = {});

/// @brief Replaces every `${key}` token in @p text with its resolved value.
/// Recognized keys: `user_message`, `workflow_id`, `run_id`,
/// `result_<phase>_<field>` (prior phase results), and any workflow input key.
/// Unknown keys, unknown phases, and malformed values resolve to EMPTY: this
/// function has no error channel, so a caller that substitutes into a destructive
/// target must itself reject an unresolved placeholder rather than proceed with a
/// blank one. A numeric result field is rendered only when it is an exact integer
/// (never rounded). See @ref WorkflowPlaceholderMode for escaping -- `Raw` is the
/// default because prompts and argv values need no escaping; a SHELL command
/// template must pass the mode for its shell, and is validated at load by
/// @ref powerShellCommandTemplateIsSingleQuoteSafe /
/// @ref cmdCommandTemplateIsPlaceholderFree so an unescaped placeholder in a
/// command cannot survive to substitution time.
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
/// placed outside a `'...'` literal would inject its value UNESCAPED.
///
/// The text before each placeholder is lexed with a minimal PowerShell scanner that
/// understands single-quoted literals, double-quoted strings and backtick escapes. Any
/// construct outside that accepted grammar (a comment, a here-string, or a `$( )`
/// subexpression nested in a double-quoted string) is REFUSED rather than guessed at, so
/// the answer is provable instead of heuristic. Returns false and sets @p error on the
/// first offending placeholder. Pure; unit-testable.
[[nodiscard]] bool powerShellCommandTemplateIsSingleQuoteSafe(const QString& command,
                                                              QString* error = nullptr);

/// @brief Validate that a run_cmd workflow command template embeds no `${key}` placeholder.
/// cmd.exe has no literal-quoting construct that makes an arbitrary value inert (`"..."`
/// still expands `%VAR%`, a bare `"` ends the quoted run, and `^` escapes only outside
/// quotes), so a placeholder in a cmd.exe command cannot be proven safe for its substitution
/// context and is rejected before the command is built. Returns false and sets @p error on
/// the first placeholder found. Pure; unit-testable.
[[nodiscard]] bool cmdCommandTemplateIsPlaceholderFree(const QString& command,
                                                       QString* error = nullptr);

}  // namespace sak::ai
