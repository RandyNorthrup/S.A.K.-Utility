// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include "sak/ai/ai_command_guard.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>

namespace sak::ai {

namespace {

// Cap on how much of a rejected tool name is echoed back in the refusal message.
constexpr int kToolNameErrorMaxChars = 64;

// Code points below this are C0 control characters; 0x7f is the DEL control character.
constexpr char16_t kFirstPrintableCodePoint = 0x20;
constexpr char16_t kDeleteControlChar = 0x7f;
// Render an escaped control byte as two hexadecimal digits (\xNN).
constexpr int kControlEscapeHexWidth = 2;
constexpr int kHexadecimalBase = 16;

bool isControlChar(QChar ch) {
    return ch.unicode() < kFirstPrintableCodePoint || ch.unicode() == kDeleteControlChar;
}

// Renders a control character as a visible escape so a newline/tab embedded in an
// argument cannot forge the displayed confirmation command.
QString escapeControlChar(QChar ch) {
    if (ch == QLatin1Char('\t')) {
        return QStringLiteral("\\t");
    }
    if (ch == QLatin1Char('\n')) {
        return QStringLiteral("\\n");
    }
    if (ch == QLatin1Char('\r')) {
        return QStringLiteral("\\r");
    }
    return QStringLiteral("\\x%1").arg(static_cast<uint>(ch.unicode()),
                                       kControlEscapeHexWidth,
                                       kHexadecimalBase,
                                       QLatin1Char('0'));
}

// Makes control chars visible and escapes embedded double quotes (the preview's
// own delimiter) so the rendered command is unambiguous.
QString sanitizeForPreview(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (isControlChar(ch)) {
            out += escapeControlChar(ch);
        } else if (ch == QLatin1Char('"')) {
            out += QStringLiteral("\\\"");
        } else {
            out += ch;
        }
    }
    return out;
}

bool previewNeedsQuoting(const QString& value) {
    if (value.isEmpty()) {
        return true;
    }
    return std::any_of(value.cbegin(), value.cend(), [](QChar ch) {
        return ch.isSpace() || isControlChar(ch) || ch == QLatin1Char('"');
    });
}

// One argv element rendered so its boundaries match QProcess argument boundaries:
// quoted (with contents escaped/visible) whenever it holds whitespace, a quote, or
// a control char, so it can never silently merge into or split from a neighbor.
QString quoteArgForPreview(const QString& value) {
    const QString sanitized = sanitizeForPreview(value);
    if (previewNeedsQuoting(value)) {
        return QLatin1Char('"') + sanitized + QLatin1Char('"');
    }
    return sanitized;
}

// Resolve a bare program name (no path separator) to an absolute path. System32 wins first,
// then only the ABSOLUTE entries of PATH (never "." or a relative entry, which resolve
// against the working directory). Empty return -> caller fails closed. This mirrors the
// execution broker's launch-time resolver so the plan-time preview names the SAME binary
// the broker would run, closing the window where the approved name and the launched target
// could differ.
[[nodiscard]] QString resolveBareProgram(const QString& name) {
#ifdef Q_OS_WIN
    const QString system32 = sak::system32Path(name);
    if (!system32.isEmpty() && QFileInfo(system32).isFile()) {
        return system32;
    }
#endif
    QStringList search;
    const QStringList entries = qEnvironmentVariable("PATH").split(QDir::listSeparator(),
                                                                   Qt::SkipEmptyParts);
    for (const QString& entry : entries) {
        const QString entry_trimmed = entry.trimmed();
        if (entry_trimmed.isEmpty() || !QDir::isAbsolutePath(entry_trimmed)) {
            continue;
        }
        search.append(entry_trimmed);
    }
    if (search.isEmpty()) {
        return {};
    }
    return QStandardPaths::findExecutable(name, search);
}

// Rewrite an AI-supplied run_process program into an ABSOLUTE, verified path AT PLAN TIME so
// the human approval preview shows exactly which binary will launch, and so the broker never
// has to resolve a bare name against the process search order AFTER the user has approved.
// Returns a non-empty error (leaving @p program untouched) so buildPlan() fails closed.
//
// CreateProcess searches the CURRENT DIRECTORY ahead of PATH, so a bare name -- or any
// relative path -- lets a binary planted in the working directory win. An absolute path is
// kept as given (its existence is proven when the broker launches it, exactly as today); a
// bare name is resolved via System32 then absolute PATH; a relative path with separators has
// no defensible base directory and is refused rather than launched CWD-relative.
[[nodiscard]] QString resolveProcessProgram(QString& program) {
    const QString trimmed = program.trimmed();
    if (trimmed.isEmpty()) {
        return QStringLiteral("program must not be empty");
    }
    if (QDir::isAbsolutePath(trimmed)) {
        program = trimmed;
        return {};
    }
    if (trimmed.contains(QLatin1Char('/')) || trimmed.contains(QLatin1Char('\\'))) {
        return QStringLiteral(
                   "program must be an absolute path or a bare executable name, not a "
                   "working-directory-relative path: %1")
            .arg(sanitizeForPreview(trimmed.left(kToolNameErrorMaxChars)));
    }
    const QString resolved = resolveBareProgram(trimmed);
    if (resolved.isEmpty()) {
        return QStringLiteral(
                   "Cannot resolve program '%1' to an absolute path; refusing to launch a bare "
                   "name")
            .arg(sanitizeForPreview(trimmed.left(kToolNameErrorMaxChars)));
    }
    program = resolved;
    return {};
}

QString buildProcessPreview(const QString& program, const QStringList& arguments) {
    QStringList parts;
    parts.reserve(arguments.size() + 1);
    parts << quoteArgForPreview(program);
    for (const QString& arg : arguments) {
        parts << quoteArgForPreview(arg);
    }
    return parts.join(QLatin1Char(' '));
}

// Resolve the request's program to an absolute, verified path BEFORE the preview is built so
// the human approval shows exactly which binary will launch and the broker receives an
// already-absolute path -- never a bare name it would resolve against the process search
// order only after the user approved. Skips when argument parsing already failed; that error
// is surfaced unchanged by buildPlan's validation_error branch. Fails the request closed by
// recording the resolver's error.
void resolveRequestProcessProgram(AiCommandRequest& request) {
    if (!request.validation_error.isEmpty()) {
        return;
    }
    const QString resolve_error = resolveProcessProgram(request.program);
    if (!resolve_error.isEmpty()) {
        request.validation_error = resolve_error;
    }
}

}  // namespace

AiCommandToolPlan AiCommandToolPlanner::buildPlan(const QString& tool_name,
                                                  const QJsonObject& args,
                                                  AiToolPolicy policy,
                                                  Options options) {
    AiCommandToolPlan plan;
    // The command tools are a CLOSED set of EXACT names. The router (and every policy
    // helper) matches tool names case- and whitespace-insensitively, so a variant such as
    // "RUN_POWERSHELL" is authorized as a shell tool; letting it fall through to the
    // process branch here would parse and launch a model-supplied executable instead of
    // the shell that was authorized. Anything that is not one of the three canonical
    // names is refused outright.
    bool direct_process = false;
    if (tool_name == QLatin1String("run_powershell")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = plan.request.command;
    } else if (tool_name == QLatin1String("run_cmd")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("cmd.exe");
        plan.preview = plan.request.command;
    } else if (tool_name == QLatin1String("run_process")) {
        direct_process = true;
        plan.request = ExecutionBroker::processRequestFromJson(args);
        plan.shell_label = QStringLiteral("Process");
        resolveRequestProcessProgram(plan.request);
        plan.preview = buildProcessPreview(plan.request.program, plan.request.arguments);
    } else {
        plan.shell_label = QStringLiteral("Process");
        plan.risky_change = true;
        plan.request.validation_error =
            QStringLiteral("Unsupported command tool: %1")
                .arg(sanitizeForPreview(tool_name.left(kToolNameErrorMaxChars)));
        plan.guard_block_error = plan.request.validation_error;
        plan.policy_request.tool_name = tool_name;
        return plan;  // policy_decision stays default-denied.
    }

    plan.request.max_output_bytes = options.max_output_bytes;
    // A direct process launch is an arbitrary executable: its effect lives in the binary,
    // not in any text the risk classifier can read, so a launch can never be PROVEN
    // non-destructive. Fail closed and treat every direct launch as a risky change; the
    // shell branches keep their text classification.
    plan.risky_change = direct_process ||
                        isPotentiallyDestructiveCommand(plan.request, plan.preview);

    // Malformed / wrong-typed / out-of-domain arguments are rejected HERE rather than
    // repaired into defaults and carried through the confirmation, lease, and launch path
    // only to be refused at the broker's entry point.
    if (!plan.request.validation_error.isEmpty()) {
        plan.guard_block_error = plan.request.validation_error;
        plan.policy_request.tool_name = tool_name;
        plan.policy_request.command_preview = plan.preview;
        plan.policy_request.requires_admin = plan.request.requires_admin;
        return plan;  // policy_decision stays default-denied.
    }

    const AiCommandGuardResult guard = evaluateCommandGuard(plan.request, plan.preview);
    plan.guard_block_error = guard.block_error;
    plan.guard_approval_reason = guard.approval_reason;

    plan.policy_request.tool_name = tool_name;
    plan.policy_request.command_preview = plan.preview;
    plan.policy_request.requires_admin = plan.request.requires_admin;
    plan.policy_decision = evaluateToolPolicy(policy, plan.policy_request);
    return plan;
}

bool AiCommandToolPlanner::isPotentiallyDestructiveCommand(const AiCommandRequest& request,
                                                           const QString& preview) {
    const QString command =
        QStringLiteral("%1 %2 %3").arg(request.command, request.program, preview).toLower();
    return request.requires_admin || commandLooksRiskyChange(command);
}

}  // namespace sak::ai
