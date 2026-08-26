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
#include <array>

namespace sak::ai {

namespace {

// Cap on how much of a rejected tool name is echoed back in the refusal message.
constexpr int kToolNameErrorMaxChars = 64;

// Code points below this are C0 control characters; 0x7f is the DEL control character.
constexpr char16_t kFirstPrintableCodePoint = 0x20;
constexpr char16_t kDeleteControlChar = 0x7f;
// C1 controls (0x80-0x9F) include NEL (0x85), which several renderers break lines on.
constexpr char16_t kFirstC1Control = 0x80;
constexpr char16_t kLastC1Control = 0x9f;
// Render an escaped code point as four hexadecimal digits (<U+XXXX>).
constexpr int kControlEscapeHexWidth = 4;
constexpr int kHexadecimalBase = 16;

// Every code point that can change what the reader SEES without being visible itself. The old
// set was only C0 plus DEL, which left the whole class of Unicode display attacks open against
// the one string a human is asked to approve:
//   U+0085 NEL, U+2028 LINE SEPARATOR, U+2029 PARAGRAPH SEPARATOR -- QTextDocument breaks lines
//     on these, so a "single-line" command can push its dangerous tail out of a fixed-height,
//     no-wrap preview box entirely.
//   U+200E/U+200F and U+202A-U+202E and U+2066-U+2069 -- bidi marks, embeddings, overrides and
//     isolates. RIGHT-TO-LEFT OVERRIDE reverses the VISUAL order of everything after it, which
//     is the classic way to make a destructive command read as a harmless one.
//   U+200B-U+200D, U+FEFF, U+00AD -- zero-width and soft-hyphen characters, invisible by
//     definition, able to split a word the reader is scanning for ("rm" vs "r<ZWSP>m").
// A table rather than a switch: eighteen equal alternatives are DATA, and written as control
// flow they cost one cyclomatic branch each (CCN 23, over the gate's limit of 10) while telling
// the reader nothing a list would not. Sorted so the lookup is a binary search.
constexpr std::array<char16_t, 18> kNonC0DisplayHazards = {
    0x00ad,  // SOFT HYPHEN
    0x200b,  // ZERO WIDTH SPACE
    0x200c,  // ZERO WIDTH NON-JOINER
    0x200d,  // ZERO WIDTH JOINER
    0x200e,  // LEFT-TO-RIGHT MARK
    0x200f,  // RIGHT-TO-LEFT MARK
    0x2028,  // LINE SEPARATOR
    0x2029,  // PARAGRAPH SEPARATOR
    0x202a,  // LEFT-TO-RIGHT EMBEDDING
    0x202b,  // RIGHT-TO-LEFT EMBEDDING
    0x202c,  // POP DIRECTIONAL FORMATTING
    0x202d,  // LEFT-TO-RIGHT OVERRIDE
    0x202e,  // RIGHT-TO-LEFT OVERRIDE
    0x2066,  // LEFT-TO-RIGHT ISOLATE
    0x2067,  // RIGHT-TO-LEFT ISOLATE
    0x2068,  // FIRST STRONG ISOLATE
    0x2069,  // POP DIRECTIONAL ISOLATE
    0xfeff,  // ZERO WIDTH NO-BREAK SPACE / BOM
};

bool isDisplayHazardChar(QChar ch) {
    const char16_t code = ch.unicode();
    if (code < kFirstPrintableCodePoint || code == kDeleteControlChar) {
        return true;
    }
    if (code >= kFirstC1Control && code <= kLastC1Control) {
        return true;
    }
    return std::ranges::binary_search(kNonC0DisplayHazards, code);
}

// Renders a hazardous character visibly so it cannot forge the displayed confirmation command.
//
// THE FORM IS BRACKETED, NOT BACKSLASH-ESCAPED, and that is deliberate. A backslash escape
// (\n, \x0a) is ambiguous the moment a value contains the LITERAL two characters '\' and 'n' --
// the reader cannot tell a real newline from text that merely looks like one. Escaping the
// backslash to disambiguate would be worse here than the disease: Windows commands are full of
// paths, and doubling every separator ("C:\\Windows\\System32") makes the preview harder to read
// in exactly the situation where careful reading matters. The bracketed form cannot be produced
// by escaping, so no doubling is needed; a value containing the literal text "<LF>" merely makes
// the reader suspect a newline that is not there, which errs toward caution rather than away.
QString escapeDisplayHazardChar(QChar ch) {
    if (ch == QLatin1Char('\t')) {
        return QStringLiteral("<TAB>");
    }
    if (ch == QLatin1Char('\n')) {
        return QStringLiteral("<LF>");
    }
    if (ch == QLatin1Char('\r')) {
        return QStringLiteral("<CR>");
    }
    return QStringLiteral("<U+%1>")
        .arg(static_cast<uint>(ch.unicode()),
             kControlEscapeHexWidth,
             kHexadecimalBase,
             QLatin1Char('0'))
        .toUpper();
}

// Makes control chars visible and escapes embedded double quotes (the preview's
// own delimiter) so the rendered command is unambiguous.
QString sanitizeForPreview(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (const QChar ch : value) {
        if (isDisplayHazardChar(ch)) {
            out += escapeDisplayHazardChar(ch);
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
    return std::ranges::any_of(value, [](QChar ch) {
        return ch.isSpace() || isDisplayHazardChar(ch) || ch == QLatin1Char('"');
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
    const QString resolved = sak::resolveBareExecutable(trimmed);
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

// Populate @p plan's request/label/preview from the tool name. Returns false when the name is
// not one of the three canonical tools, having already filled in the refusal.
//
// The command tools are a CLOSED set of EXACT names. The router (and every policy helper)
// matches tool names case- and whitespace-insensitively, so a variant such as "RUN_POWERSHELL"
// is authorized as a shell tool; letting it fall through to the process branch would parse and
// launch a model-supplied executable instead of the shell that was authorized. Anything that is
// not one of the three canonical names is refused outright.
bool populateRequestForTool(const QString& tool_name,
                            const QJsonObject& args,
                            AiCommandToolPlan& plan,
                            bool& direct_process) {
    if (tool_name == QLatin1String("run_powershell")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = plan.request.command;
        return true;
    }
    if (tool_name == QLatin1String("run_cmd")) {
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("cmd.exe");
        plan.preview = plan.request.command;
        return true;
    }
    if (tool_name == QLatin1String("run_process")) {
        direct_process = true;
        plan.request = ExecutionBroker::processRequestFromJson(args);
        plan.shell_label = QStringLiteral("Process");
        resolveRequestProcessProgram(plan.request);
        plan.preview = buildProcessPreview(plan.request.program, plan.request.arguments);
        return true;
    }
    plan.shell_label = QStringLiteral("Process");
    plan.risky_change = true;
    plan.request.validation_error =
        QStringLiteral("Unsupported command tool: %1")
            .arg(sanitizeForPreview(tool_name.left(kToolNameErrorMaxChars)));
    plan.guard_block_error = plan.request.validation_error;
    plan.policy_request.tool_name = tool_name;
    plan.display_preview = plan.preview;
    return false;
}

}  // namespace

AiCommandToolPlan AiCommandToolPlanner::buildPlan(const QString& tool_name,
                                                  const QJsonObject& args,
                                                  AiToolPolicy policy,
                                                  Options options) {
    AiCommandToolPlan plan;
    bool direct_process = false;
    if (!populateRequestForTool(tool_name, args, plan, direct_process)) {
        return plan;  // policy_decision stays default-denied.
    }
    // THE HUMAN-FACING COPY, sanitised once, here, for EVERY branch. The two shell branches
    // assign the model's command string straight through, and that string is what the approval
    // dialog used to render: a raw newline (or U+2028, which QTextDocument also breaks on)
    // pushes the dangerous tail out of a fixed-height no-wrap preview box, and a bidi override
    // reverses the visual order of what is left. The process branch was already safe because
    // buildProcessPreview quotes each argument through sanitizeForPreview -- the defence existed
    // and simply was not applied to the branches a model actually uses.
    plan.display_preview = sanitizeForPreview(plan.preview);

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
        // The header promises that ANY call failing the broker's typed validation comes back with
        // risky_change SET, and the sibling unsupported-tool branch above does exactly that. This
        // path did not: a shell command whose arguments were malformed but whose text did not
        // look destructive returned risky_change == false, so a caller honouring the documented
        // contract would skip the risky-command presentation and the restore-point offer for a
        // request it could not even parse. Arguments that failed validation are arguments nothing
        // understood -- fail closed.
        plan.risky_change = true;
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
