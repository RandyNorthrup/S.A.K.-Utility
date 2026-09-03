// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_tool_planner.h"

#include "sak/ai/ai_command_guard.h"
#include "sak/elevation_manager.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QLatin1Char>
#include <QLatin1String>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <array>
#include <utility>

namespace sak::ai {

namespace {

// Cap on how much of a rejected tool name is echoed back in the refusal message.
constexpr int kToolNameErrorMaxChars = 64;

// Render an escaped code point as four hexadecimal digits (<U+XXXX>).
constexpr int kControlEscapeHexWidth = 4;
constexpr int kHexadecimalBase = 16;
constexpr qsizetype kUnicodeEscapeTokenChars = 8;
constexpr qsizetype kUnicodeEscapePlusIndex = 2;
constexpr qsizetype kUnicodeEscapeHexStart = 3;
constexpr qsizetype kUnicodeEscapeHexEnd = 7;
constexpr qsizetype kUnicodeEscapeEndIndex = 7;

bool isDisplayHazardChar(QChar ch) {
    // Categories, not a hand-maintained code-point list. That covers every C0/C1 control,
    // every line/paragraph separator, every bidi/zero-width format character (including
    // U+061C ARABIC LETTER MARK, which the old list missed), and unpaired UTF-16 surrogates.
    constexpr std::array kHazardCategories{QChar::Mark_NonSpacing,
                                           QChar::Mark_SpacingCombining,
                                           QChar::Mark_Enclosing,
                                           QChar::Separator_Space,
                                           QChar::Separator_Line,
                                           QChar::Separator_Paragraph,
                                           QChar::Other_Control,
                                           QChar::Other_Format,
                                           QChar::Other_PrivateUse,
                                           QChar::Other_NotAssigned,
                                           QChar::Other_Surrogate};
    return ch != QLatin1Char(' ') &&
           std::ranges::find(kHazardCategories, ch.category()) != kHazardCategories.end();
}

// Renders a hazardous character visibly so it cannot forge the displayed confirmation command.
//
// THE FORM IS BRACKETED, NOT BACKSLASH-ESCAPED, and that is deliberate. Windows commands are
// full of backslashes, so doubling every path separator would make the approval harder to read.
// Literal text that already looks like one of these tokens is disambiguated below by escaping
// its opening '<'; therefore the rendering is injective rather than merely caution-inducing.
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

bool isUpperHexDigit(QChar ch) {
    return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
           (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
}

// True when @p value contains text at @p index that could be mistaken for an escape this file
// generates. Escaping only that opening '<' keeps ordinary shell input redirection readable.
bool generatedEscapeTokenAt(const QString& value, qsizetype index) {
    const QStringView suffix = QStringView(value).sliced(index);
    if (suffix.startsWith(QLatin1String("<TAB>")) || suffix.startsWith(QLatin1String("<LF>")) ||
        suffix.startsWith(QLatin1String("<CR>"))) {
        return true;
    }
    if (suffix.size() < kUnicodeEscapeTokenChars || suffix.at(1) != QLatin1Char('U') ||
        suffix.at(kUnicodeEscapePlusIndex) != QLatin1Char('+') ||
        suffix.at(kUnicodeEscapeEndIndex) != QLatin1Char('>')) {
        return false;
    }
    for (qsizetype i = kUnicodeEscapeHexStart; i < kUnicodeEscapeHexEnd; ++i) {
        if (!isUpperHexDigit(suffix.at(i))) {
            return false;
        }
    }
    return true;
}

// Makes display-affecting characters visible. Ordinary visible shell syntax, including quotes,
// stays byte-for-byte readable because this is the entire command, not quoted argv content.
QString sanitizeForPreview(const QString& value) {
    QString out;
    out.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
        const QChar ch = value.at(i);
        if (isDisplayHazardChar(ch)) {
            out += escapeDisplayHazardChar(ch);
        } else if (ch == QLatin1Char('<') && generatedEscapeTokenAt(value, i)) {
            out += escapeDisplayHazardChar(ch);
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
    QString sanitized = sanitizeForPreview(value);
    // Here quotes ARE delimiters around one argv element, unlike a whole shell command.
    sanitized.replace(QLatin1Char('"'), QStringLiteral("\\\""));
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
// kept as given; a bare name is resolved via System32 then absolute PATH; a relative path with
// separators has no defensible base directory and is refused rather than launched CWD-relative.
//
// This function decides the NAME only. Proving that the name refers to a real, examinable file
// -- and recording WHICH file, so the launch can detect a swap -- is resolveRequestProcessProgram's
// job immediately below. That split matters: the absolute branch here deliberately performs no
// filesystem access, so its refusals stay purely textual and testable without a filesystem.
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
        return;
    }
    // BIND THE PLAN TO A FILE, not just to a path. resolveProcessProgram produces a name; the
    // launch happens minutes later, on the far side of an approval dialog, a UAC prompt and
    // restore-point creation. Recording which file that name meant NOW is what lets the broker
    // prove at launch that nothing was swapped underneath the approval.
    //
    // This is also the first existence check the ABSOLUTE-path branch has ever had. It returned
    // the caller's string untouched and left proof of existence to the broker, so a plan naming
    // a file that was not there was still rendered, risk-classified and put in front of a human
    // to approve -- an approval spent on something that could not run. An unreadable program is
    // now refused at plan time.
    request.program_identity = sak::executableIdentity(request.program);
    if (!request.program_identity.valid) {
        request.validation_error =
            QStringLiteral(
                "Cannot examine program '%1'; refusing to plan a launch of a file "
                "that cannot be identified")
                .arg(sanitizeForPreview(request.program.left(kToolNameErrorMaxChars)));
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
                            AiCommandTarget& target) {
    if (tool_name == QLatin1String("run_powershell")) {
        target = AiCommandTarget::PowerShell;
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("PowerShell");
        plan.preview = plan.request.command;
        return true;
    }
    if (tool_name == QLatin1String("run_cmd")) {
        target = AiCommandTarget::Cmd;
        plan.request = ExecutionBroker::requestFromJson(args);
        plan.shell_label = QStringLiteral("cmd.exe");
        plan.preview = plan.request.command;
        return true;
    }
    if (tool_name == QLatin1String("run_process")) {
        target = AiCommandTarget::Process;
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

// Record the BROKER'S OWN PRE-LAUNCH RULES against the plan, while it is still cheap to refuse.
//
// An empty shell command, an elevated cmd.exe or direct-process launch, and an out-of-domain
// output budget are all refused at the broker's door -- but only AFTER the plan has been
// rendered into a preview, risk-classified, put in front of a human for approval, and granted
// an execution lease. Every one of those steps was spent on a command that could never run,
// and an approval dialog whose answer changes nothing is how an operator learns to click
// through it.
//
// Sharing the broker's function rather than restating its rules here is the point: a second
// copy would be free to drift, which is exactly how the output ceiling this checks came to be
// wrong in the first place (see kAiCommandOutputBytesCeiling). Recorded through the existing
// validation_error channel so buildPlan keeps ONE fail-closed refusal path rather than two.
void applyBrokerPreconditions(AiCommandTarget target, AiCommandRequest& request) {
    if (!request.validation_error.isEmpty()) {
        return;  // A parse failure is already recorded; do not overwrite the specific message.
    }
    request.validation_error = aiCommandPreconditionError(target, request);
}

}  // namespace

AiCommandToolPlan AiCommandToolPlanner::buildPlan(const QString& tool_name,
                                                  const QJsonObject& args,
                                                  AiToolPolicy policy) {
    return buildPlan(tool_name, args, std::move(policy), Options{});
}

AiCommandToolPlan AiCommandToolPlanner::buildPlan(const QString& tool_name,
                                                  const QJsonObject& args,
                                                  AiToolPolicy policy,
                                                  Options options) {
    AiCommandToolPlan plan;
    // Defaults to the strictest target so a refusal path that never reaches the dispatch below
    // cannot leave an uninitialised one behind; it matches the "Process" shell_label the
    // unsupported-tool branch already records.
    AiCommandTarget target = AiCommandTarget::Process;
    if (!populateRequestForTool(tool_name, args, plan, target)) {
        return plan;  // policy_decision stays default-denied.
    }
    const bool direct_process = target == AiCommandTarget::Process;
    // Shell previews are raw and need one display-safety pass. Process previews were already
    // built argv-element by argv-element through quoteArgForPreview; sanitising that serialized
    // form again would turn its quote delimiters into backslashes and its generated <TAB>/<LF>
    // markers into text that claims they were literal input.
    plan.display_preview = direct_process ? plan.preview : sanitizeForPreview(plan.preview);

    plan.request.max_output_bytes = options.max_output_bytes;
    // A direct process launch is an arbitrary executable: its effect lives in the binary,
    // not in any text the risk classifier can read, so a launch can never be PROVEN
    // non-destructive. Fail closed and treat every direct launch as a risky change; the
    // shell branches keep their text classification.
    plan.risky_change = direct_process ||
                        isPotentiallyDestructiveCommand(plan.request, plan.preview);

    applyBrokerPreconditions(target, plan.request);

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
        plan.policy_request.host_elevated = ElevationManager::isElevated();
        return plan;  // policy_decision stays default-denied.
    }

    const AiCommandGuardResult guard = evaluateCommandGuard(plan.request);
    plan.guard_block_error = guard.block_error;
    plan.guard_approval_reason = guard.approval_reason;

    plan.policy_request.tool_name = tool_name;
    plan.policy_request.command_preview = plan.preview;
    plan.policy_request.requires_admin = plan.request.requires_admin;
    plan.policy_request.host_elevated = ElevationManager::isElevated();
    plan.policy_decision = evaluateToolPolicy(policy, plan.policy_request);
    // Policy classification is the fail-closed authority. Its read-only allowlist marks an
    // unknown shell command risky even when the blacklist above has never heard of that mutator.
    // Dropping this bit let such a command take a lease but skip risky presentation/restore-point
    // handling in the caller.
    plan.risky_change = plan.risky_change || plan.policy_decision.risky_change;
    return plan;
}

bool AiCommandToolPlanner::isPotentiallyDestructiveCommand(const AiCommandRequest& request,
                                                           const QString& preview) {
    const QString command =
        QStringLiteral("%1 %2 %3").arg(request.command, request.program, preview).toLower();
    return request.requires_admin || commandLooksRiskyChange(command);
}

}  // namespace sak::ai
