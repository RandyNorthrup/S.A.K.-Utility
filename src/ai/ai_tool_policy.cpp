// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_policy.h"

#include <QRegularExpression>
#include <QStringList>

#include <algorithm>
#include <optional>

namespace sak::ai {

namespace {

QString norm(const QString& value) {
    return value.trimmed().toLower();
}

bool isShellTool(const QString& tool_name) {
    const QString name = norm(tool_name);
    return name == QLatin1String("run_powershell") || name == QLatin1String("run_cmd") ||
           name == QLatin1String("run_process");
}

bool isPackageTool(const QString& tool_name) {
    const QString name = norm(tool_name);
    return name == QLatin1String("sak_package_manager") ||
           name == QLatin1String("sak_offline_downloader");
}

bool isProviderGatewayTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("sak_provider_gateway");
}

bool isSessionSearchTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("sak_session_search");
}

bool isSkillTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("sak_skill");
}

bool isDelegateSubagentTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("delegate_subagent");
}

bool isRunWorkflowTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("run_workflow");
}

bool isAppActionTool(const QString& tool_name) {
    return norm(tool_name) == QLatin1String("sak_app_action");
}

bool isReadOnlyProviderOperation(const AiToolCallRequest& request) {
    if (!isProviderGatewayTool(request.tool_name)) {
        return false;
    }
    const QString operation = norm(request.operation);
    return operation == QLatin1String("providers") ||
           operation == QLatin1String("provider_status") ||
           operation == QLatin1String("docs_query") || operation == QLatin1String("app_manifest") ||
           operation == QLatin1String("app_capabilities") ||
           operation == QLatin1String("app_run_action_plan");
}

bool isMutatingProviderOperation(const AiToolCallRequest& request) {
    if (!isProviderGatewayTool(request.tool_name)) {
        return false;
    }
    const QString operation = norm(request.operation);
    return operation == QLatin1String("app_run_action") ||
           operation == QLatin1String("win32_mcp_call");
}

bool isDownloadTool(const AiToolCallRequest& request) {
    const QString name = norm(request.tool_name);
    const QString operation = norm(request.operation);
    return name == QLatin1String("download_file") ||
           (name == QLatin1String("sak_offline_downloader") &&
            (operation == QLatin1String("search") ||
             operation == QLatin1String("direct_download")));
}

bool textMatchesAny(const QString& text, const QStringList& needles) {
    return std::ranges::any_of(needles, [&text](const auto& needle) {
        return text.contains(needle, Qt::CaseInsensitive);
    });
}

// Word-boundary variant: the action VERB that authorizes a mutation must appear as
// a whole word, so "list installed apps" does not satisfy the "install" intent via
// the substring inside "installed". Phrases ("set up") are matched with boundaries
// on each end.
bool textContainsAnyWord(const QString& text, const QStringList& words) {
    return std::ranges::any_of(words, [&text](const auto& word) {
        const QRegularExpression re(QStringLiteral("\\b") + QRegularExpression::escape(word) +
                                        QStringLiteral("\\b"),
                                    QRegularExpression::CaseInsensitiveOption);
        return re.match(text).hasMatch();
    });
}

bool hasScanIntent(const QString& user_message) {
    const QString text = norm(user_message);
    if (text.isEmpty()) {
        return false;
    }
    return textMatchesAny(text,
                          {QStringLiteral("quick scan"),
                           QStringLiteral("full scan"),
                           QStringLiteral("malware scan"),
                           QStringLiteral("virus scan"),
                           QStringLiteral("spyware scan"),
                           QStringLiteral("defender scan"),
                           QStringLiteral("run a scan"),
                           QStringLiteral("do a scan"),
                           QStringLiteral("start a scan"),
                           QStringLiteral("scan my"),
                           QStringLiteral("scan with"),
                           QStringLiteral("scan using")});
}

bool hasNegatedActionIntent(const QString& text) {
    // Substring intent inference cannot tell "install X" from "do not install X".
    // If the message forbids or disclaims the action, do not treat it as explicit
    // affirmative intent; the caller then falls through to the stricter gate.
    return textMatchesAny(text,
                          {QStringLiteral("do not "),
                           // Bare " not " / "n't " catch the forms the phrase list missed --
                           // "can you not install Foo?", "I can't install Foo" -- which
                           // carry a request marker and the verb and were therefore read as
                           // consent. No consent here means the stricter gate blocks.
                           QStringLiteral(" not "),
                           QStringLiteral("n't "),
                           QStringLiteral("don't "),
                           QStringLiteral("dont "),
                           QStringLiteral("never "),
                           QStringLiteral("instead of "),
                           QStringLiteral("rather than "),
                           QStringLiteral("without "),
                           QStringLiteral("no need to "),
                           QStringLiteral("avoid ")});
}

bool startsWithAny(const QString& text, const QStringList& prefixes) {
    return std::ranges::any_of(prefixes, [&text](const auto& prefix) {
        return text.startsWith(prefix, Qt::CaseInsensitive);
    });
}

bool hasDirectedRequestMarker(const QString& text) {
    // Second-person directive addressed to the assistant. This is what separates a
    // command ("please install X", "can you install X", "go ahead and install") or an
    // explicit affirmative ("yes, install it") from a bare topical mention or a question
    // ("how do I install X"), which substring matching cannot tell apart on its own.
    return textMatchesAny(text,
                          {QStringLiteral("please "),
                           QStringLiteral("can you "),
                           QStringLiteral("could you "),
                           QStringLiteral("would you "),
                           QStringLiteral("will you "),
                           QStringLiteral("go ahead"),
                           QStringLiteral("i want you"),
                           QStringLiteral("i need you"),
                           QStringLiteral("i would like you"),
                           QStringLiteral("i'd like you"),
                           QStringLiteral("id like you"),
                           QStringLiteral("you should "),
                           QStringLiteral("you can "),
                           QStringLiteral("let's "),
                           QStringLiteral("lets "),
                           QStringLiteral("yes, "),
                           QStringLiteral("yes ")});
}

bool hasExplanatoryFraming(const QString& text) {
    // A how-to / explanatory question mentions the action verb but asks to be TOLD ABOUT
    // the action, not directed to perform it. "Can you explain how to uninstall X" carries
    // a request marker ("can you") and the verb, yet consents to nothing -- the
    // "explain"/"how to"/"what happens" framing is interrogative. Treat it as no consent so
    // the caller falls through to the stricter gate instead of authorizing the mutation.
    return textMatchesAny(text,
                          {QStringLiteral("how to "),
                           QStringLiteral("how do i"),
                           QStringLiteral("how do you"),
                           QStringLiteral("how can i"),
                           QStringLiteral("how would i"),
                           QStringLiteral("how does "),
                           QStringLiteral("explain "),
                           QStringLiteral("what does "),
                           QStringLiteral("what happens"),
                           QStringLiteral("what would happen"),
                           QStringLiteral("tell me about"),
                           QStringLiteral("tell me how"),
                           QStringLiteral("walk me through"),
                           QStringLiteral("is it safe")});
}

bool hasDirectedIntentFor(const QString& text,
                          const QStringList& verb_keywords,
                          const QStringList& imperative_prefixes) {
    // Consent requires (a) not disclaiming the action, (b) not an explanatory/how-to
    // question about it, (c) the action verb is actually present, and (d) the message is
    // phrased as a directive to the assistant -- an imperative opening ("install X") or a
    // request marker ("please/can you ... install X"). A bare substring mention, a
    // question, or a how-to request fails and is not treated as consent.
    if (hasNegatedActionIntent(text) || hasExplanatoryFraming(text) ||
        !textContainsAnyWord(text, verb_keywords)) {
        return false;
    }
    return startsWithAny(text, imperative_prefixes) || hasDirectedRequestMarker(text);
}

bool hasExplicitPackageMutationIntent(const AiToolCallRequest& request) {
    const QString text = norm(request.user_message);
    if (text.isEmpty()) {
        return false;
    }
    const QString op = norm(request.operation);
    if (op == QLatin1String("install") || op == QLatin1String("install_bundle")) {
        return hasDirectedIntentFor(text,
                                    {QStringLiteral("install"),
                                     QStringLiteral("reinstall"),
                                     QStringLiteral("set up"),
                                     QStringLiteral("setup"),
                                     QStringLiteral("repair install")},
                                    {QStringLiteral("install"),
                                     QStringLiteral("reinstall"),
                                     QStringLiteral("set up"),
                                     QStringLiteral("setup")});
    }
    if (op == QLatin1String("upgrade")) {
        return hasDirectedIntentFor(
            text,
            {QStringLiteral("upgrade"),
             QStringLiteral("update "),
             QStringLiteral("bring up to date")},
            {QStringLiteral("upgrade"), QStringLiteral("update"), QStringLiteral("bring")});
    }
    if (op == QLatin1String("uninstall")) {
        return hasDirectedIntentFor(text,
                                    {QStringLiteral("uninstall"),
                                     QStringLiteral("remove "),
                                     QStringLiteral(" remove"),
                                     QStringLiteral("delete app"),
                                     QStringLiteral("get rid of")},
                                    {QStringLiteral("uninstall"),
                                     QStringLiteral("remove"),
                                     QStringLiteral("delete"),
                                     QStringLiteral("get rid")});
    }
    if (op == QLatin1String("build_bundle")) {
        return hasDirectedIntentFor(
            text,
            {QStringLiteral("build bundle"),
             QStringLiteral("create bundle"),
             QStringLiteral("offline bundle")},
            {QStringLiteral("build"), QStringLiteral("create"), QStringLiteral("make")});
    }
    return false;
}

bool packageMutationContradictsScanRequest(const AiToolCallRequest& request) {
    return isPackageTool(request.tool_name) && isMutatingPackageOperation(request.operation) &&
           hasScanIntent(request.user_message) && !hasExplicitPackageMutationIntent(request);
}

bool packageMutationMissingExplicitIntent(const AiToolCallRequest& request) {
    return isPackageTool(request.tool_name) && isMutatingPackageOperation(request.operation) &&
           !hasExplicitPackageMutationIntent(request);
}

// Defined below with the read-only allowlist machinery: true ONLY when the command is
// positively proven read-only by that allowlist, never merely "absent from the blacklist".
bool shellCommandProvenReadOnly(const QString& preview);

struct ToolPolicyContext {
    bool m_shell{false};
    bool m_package_tool{false};
    bool m_provider_gateway{false};
    bool m_mutating_package{false};
    // A shell/process command the read-only allowlist cannot PROVE non-mutating.
    bool m_shell_unproven{false};
    bool m_risky{false};
    bool m_catastrophic{false};
};

ToolPolicyContext policyContext(const AiToolCallRequest& request) {
    ToolPolicyContext context;
    context.m_shell = isShellTool(request.tool_name);
    context.m_package_tool = isPackageTool(request.tool_name);
    context.m_provider_gateway = isReadOnlyProviderOperation(request);
    context.m_mutating_package = context.m_package_tool &&
                                 isMutatingPackageOperation(request.operation);
    // commandLooksRiskyChange is a blacklist, and a blacklist is fail-open: "fsutil file
    // setzerodata", "powershell -File attacker.ps1", a vendor wipe utility, or any mutator
    // invented after this list was written match none of its patterns, so under the mutating
    // policies they took NO lease, NO exclusive lease and NO restore point. Under those
    // policies a shell command therefore counts as mutating unless the read-only allowlist
    // proves otherwise (fail closed); the read-only policy keeps its own allowlist path so it
    // still reports precisely why a command was refused.
    context.m_shell_unproven = context.m_shell &&
                               !shellCommandProvenReadOnly(request.command_preview);
    // An obfuscated/indirected shell command (encoded, concatenated verb, in-memory
    // download-and-run) hides its real effect from commandLooksCatastrophic, so it
    // cannot be proven non-catastrophic. Treat it as catastrophic and force the hard
    // human confirmation rather than downgrading it to a mere restore-point (a wiped
    // volume behind '& (\'For\'+\'mat-Volume\')' must not slip through unattended).
    context.m_catastrophic = context.m_shell &&
                             (commandLooksCatastrophic(request.command_preview) ||
                              commandLooksObfuscated(request.command_preview));
    context.m_risky = request.requires_admin ||
                      (context.m_shell && commandLooksRiskyChange(request.command_preview)) ||
                      context.m_mutating_package || isMutatingProviderOperation(request);
    return context;
}

AiToolPolicyDecision block(const QString& reason) {
    AiToolPolicyDecision decision;
    decision.reason = reason;
    return decision;
}

AiToolPolicyDecision allow(const QString& reason) {
    AiToolPolicyDecision decision;
    decision.allowed = true;
    decision.reason = reason;
    return decision;
}

bool commandUsesResolutionIndirection(const QString& preview) {
    // Command-name indirection and string obfuscation that assemble/resolve a command
    // token the contiguous-substring risk/catastrophic regexes never see:
    //   * Get-Command / Invoke-Command / Start-Process / Start-Job / New-Object and the
    //     "&" call operator resolve and invoke an arbitrary command by name/expression;
    //   * quote-adjacent "+" concatenation ('For'+'mat-Volume') splices a dangerous verb
    //     out of harmless-looking fragments so 'format-volume' is never contiguous.
    // Any of these forfeits the read-only allowlist and is classified risky so a hidden
    // mutation cannot run without a lease/restore point (fail closed, no silent bypass).
    static const QRegularExpression kIndirection(
        QStringLiteral(
            R"RX((\bget-command\b|\binvoke-command\b|\bstart-process\b|\bstart-job\b|\bnew-object\b|&\s*[('"$@{]|['"]\s*\+|\+\s*['"]))RX"),
        QRegularExpression::CaseInsensitiveOption);
    return kIndirection.match(preview).hasMatch();
}

bool commandHasUnsafeConstruct(const QString& preview) {
    // Indirection/injection shapes that let a mutation hide inside a command whose
    // leading token looks read-only: static/type member access ([IO.File]::Delete),
    // method invocation (.Terminate()), subexpressions ($( ... )), backtick
    // obfuscation, script blocks ({ ... } can contain any hidden cmdlet), expression
    // evaluation (iex), and file-writing redirection. Stream merges (2>&1) and null
    // discards (>nul / >$null) are NOT writes and stay allowed. Any hit forfeits the
    // read-only allowlist -- this is why a mutation blacklist is fail-open and an
    // allowlist is required here.
    //
    // cmd.exe %VAR% expansion happens BEFORE the line is tokenized, so an
    // attacker-writable environment value containing "& attacker_tool" becomes real
    // command syntax after this check passed on "echo %VAR%". Delayed !VAR! expansion is
    // rejected with it: neither form's real command text is knowable here, so neither can
    // be proven read-only.
    static const QRegularExpression kUnsafe(
        QStringLiteral(
            R"((::|\.\s*\w+\s*\(|`|\$\(|\{|%[^\s%]{1,64}%|![a-z_][a-z0-9_]{0,63}!|\biex\b|\binvoke-expression\b|>>?\s*(?!&|nul\b|\$null\b|/dev/null\b)[^\s>&|]))"),
        QRegularExpression::CaseInsensitiveOption);
    return kUnsafe.match(preview).hasMatch() || commandUsesResolutionIndirection(preview);
}

bool segmentLeadIsReadOnly(const QString& segment) {
    // The leading command token of a single segment must be a known read-only
    // diagnostic: native tools and PowerShell read cmdlets whose EVERY documented
    // form is non-mutating. Mutating-capable programs (wmic, reg, sc, netsh, route,
    // arp, set, diskpart, del, format, python, node, ...) are deliberately absent, so
    // anything not clearly read-only is refused. Get-*/Test-* wildcards are safe (no
    // mutating cmdlet uses those verbs); Format-* is spelled out to exclude the
    // disk-destroying Format-Volume/Format-Table sharing the "format" stem.
    static const QRegularExpression kLead(
        QStringLiteral(
            R"(^\s*\(*\s*(get-\w+|test-\w+|resolve-dnsname|select-object|select-string|sort-object|measure-object|compare-object|group-object|where-object|format-table|format-list|format-wide|out-string|write-output|write-host|convertto-\w+|convertfrom-\w+|ping|tracert|pathping|ipconfig|netstat|nslookup|systeminfo|tasklist|whoami|hostname|ver|getmac|driverquery|type|dir|findstr|find|fc|tree|vol|where|echo)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return kLead.match(segment).hasMatch();
}

// A parenthesis inside a quoted string is literal text, not a group delimiter, so a
// quoted region is skipped wholesale. Returns true once the character has been
// accounted for by quote tracking and the caller needs to do nothing further with it.
bool consumeQuoteChar(QChar c, QChar& quote) {
    if (!quote.isNull()) {
        if (c == quote) {
            quote = QChar();
        }
        return true;
    }
    if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
        quote = c;
        return true;
    }
    return false;
}

// Running state of the top-level paren scan: the groups closed so far, the current
// nesting depth, and where the open top-level group started (-1 when outside one).
struct ParenScan {
    QStringList m_groups;
    int m_depth{0};
    qsizetype m_start{-1};
};

// Fold the delimiter at index i into the scan. A ')' with no matching '(' means the
// grouping is broken and cannot be resolved with confidence, so it returns false and
// the caller fails closed; every other character leaves the scan valid.
bool applyParenBoundary(ParenScan& scan, const QString& s, qsizetype i) {
    const QChar c = s.at(i);
    if (c == QLatin1Char('(')) {
        if (scan.m_depth == 0) {
            scan.m_start = i + 1;
        }
        ++scan.m_depth;
    } else if (c == QLatin1Char(')')) {
        if (scan.m_depth == 0) {
            return false;
        }
        --scan.m_depth;
        if (scan.m_depth == 0 && scan.m_start >= 0) {
            scan.m_groups.append(s.mid(scan.m_start, i - scan.m_start));
            scan.m_start = -1;
        }
    }
    return true;
}

// Contents of each top-level (...) group. PowerShell evaluates a parenthesized
// sub-expression BEFORE the surrounding command, so a read-only-looking lead can
// still run a mutation hidden in a group -- "Write-Output (Restart-Computer -Force)"
// reboots the machine. Those groups must be validated as their own commands.
//
// A parenthesis inside a quoted string is literal text, so quoted regions are skipped: the
// earlier quote-blind scan counted a fake quoted "(" as a real opener, which swallowed the
// following REAL group and hid it from validation, so
// 'Write-Output "(" (attacker_tool)' passed as a read-only Write-Output. Unbalanced
// parentheses or an unterminated quote mean the command cannot be parsed with confidence,
// so nullopt is returned and the caller fails closed.
std::optional<QStringList> topLevelParenGroups(const QString& s) {
    ParenScan scan;
    QChar quote;
    for (qsizetype i = 0; i < s.size(); ++i) {
        const QChar c = s.at(i);
        if (consumeQuoteChar(c, quote)) {
            continue;
        }
        if (!applyParenBoundary(scan, s, i)) {
            return std::nullopt;
        }
    }
    if (scan.m_depth != 0 || !quote.isNull()) {
        return std::nullopt;
    }
    return scan.m_groups;
}

constexpr int kMaxReadOnlyParenDepth = 8;

bool commandIsReadOnlyDiagnostic(const QString& preview, int depth_remaining);

// Require EVERY &/|/;/newline-separated segment to lead with an allowlisted
// read-only command. One non-diagnostic segment (chained mutator, call-operator
// to a binary) refuses the whole command.
bool allSegmentsReadOnly(const QString& normalized) {
    static const QRegularExpression kSep(QStringLiteral(R"([|;&\r\n])"));
    const QStringList segments = normalized.split(kSep, Qt::SkipEmptyParts);
    if (segments.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(segments, [](const QString& segment) {
        return segment.trimmed().isEmpty() || segmentLeadIsReadOnly(segment);
    });
}

// Every parenthesized sub-expression is its own command context PowerShell runs
// first, so it must be read-only too (this is what stops a nested mutator hiding
// behind a read-only lead). Fail closed if the grouping nests deeper than we
// will verify.
bool parenSubExpressionsReadOnly(const QString& normalized, int depth_remaining) {
    const std::optional<QStringList> groups = topLevelParenGroups(normalized);
    if (!groups.has_value()) {
        // Unbalanced parentheses or an unterminated quote: the grouping cannot be resolved,
        // so the command cannot be proven read-only.
        return false;
    }
    if (groups->isEmpty()) {
        return true;
    }
    if (depth_remaining <= 0) {
        return false;
    }
    return std::ranges::all_of(*groups, [depth_remaining](const QString& group) {
        const QString inner = group.trimmed();
        return inner.isEmpty() || commandIsReadOnlyDiagnostic(inner, depth_remaining - 1);
    });
}

bool commandIsReadOnlyDiagnostic(const QString& preview, int depth_remaining) {
    QString normalized = preview.trimmed();
    if (normalized.isEmpty() || commandHasUnsafeConstruct(normalized)) {
        return false;
    }
    // Neutralize stream-merge redirections (2>&1, 1>&2) so their '&' is not mistaken
    // for a command separator.
    static const QRegularExpression kMerge(QStringLiteral(R"([0-9]?>&[0-9]?)"));
    normalized.replace(kMerge, QStringLiteral(" "));
    return allSegmentsReadOnly(normalized) &&
           parenSubExpressionsReadOnly(normalized, depth_remaining);
}

bool shellCommandProvenReadOnly(const QString& preview) {
    return commandIsReadOnlyDiagnostic(preview, kMaxReadOnlyParenDepth);
}

AiToolPolicyDecision evaluateReadOnlyShell(const QString& preview) {
    // Read-only shell is an ALLOWLIST, not a mutation blacklist. A blacklist is
    // fail-open: novel binaries (python -c, node -e), .NET/WMI method calls
    // ([IO.File]::WriteAllText, Invoke-WmiMethod), and unknown mutators slip through.
    // Only commands proven read-only by the allowlist run under the read-only lease.
    if (!shellCommandProvenReadOnly(preview)) {
        return block(QStringLiteral(
            "Read-only PC policy allows only known read-only diagnostic shell commands "
            "(ping, ipconfig, systeminfo, tasklist, netstat, whoami, Get-*/Test-* reads, ...); "
            "this command is not on the read-only allowlist"));
    }
    return allow(QStringLiteral("Read-only diagnostic shell command allowed"));
}

AiToolPolicyDecision evaluateReadOnlyPolicy(const AiToolCallRequest& request,
                                            bool shell,
                                            bool provider_gateway,
                                            bool risky) {
    if (risky) {
        auto decision = block(QStringLiteral("Read-only PC policy blocked mutating command"));
        decision.risky_change = true;
        return decision;
    }
    if (shell) {
        return evaluateReadOnlyShell(request.command_preview);
    }
    if (!provider_gateway && !isSessionSearchTool(request.tool_name) &&
        norm(request.tool_name) != QLatin1String("take_screenshot")) {
        return block(
            QStringLiteral("Read-only PC policy only allows shell diagnostics, screenshots, and "
                           "provider status/capability checks, and AI session search"));
    }
    return allow(QStringLiteral("Read-only tool allowed"));
}

AiToolPolicyDecision evaluatePackageOnlyPolicy(bool package_tool, bool mutating_package) {
    if (!package_tool) {
        return block(QStringLiteral("Package-only policy blocked non-package tool"));
    }
    auto decision = allow(QStringLiteral("Package tool allowed"));
    decision.risky_change = mutating_package;
    decision.requires_lease = mutating_package;
    decision.restore_point_recommended = mutating_package;
    return decision;
}

AiToolPolicyDecision evaluateDownloadOnlyPolicy(const AiToolCallRequest& request) {
    if (!isDownloadTool(request)) {
        return block(QStringLiteral("Download-only policy blocked non-download tool"));
    }
    return allow(QStringLiteral("Download tool allowed"));
}

AiToolPolicyDecision evaluateMutatingPolicy(bool risky, bool exclusive) {
    auto decision = allow(exclusive ? QStringLiteral("Known local tool allowed with exclusive "
                                                     "mutation policy")
                                    : QStringLiteral("Known local tool allowed"));
    decision.risky_change = risky;
    decision.requires_lease = risky;
    decision.requires_exclusive_lease = exclusive && risky;
    decision.restore_point_recommended = risky;
    return decision;
}

AiToolPolicyDecision evaluateKnownPolicy(AiToolPolicy policy,
                                         const AiToolCallRequest& request,
                                         const ToolPolicyContext& context) {
    switch (policy) {
    case AiToolPolicy::NoLocalExecution:
        return block(QStringLiteral("Local execution disabled by tool policy"));
    case AiToolPolicy::ReadOnlyPc:
        return evaluateReadOnlyPolicy(
            request, context.m_shell, context.m_provider_gateway, context.m_risky);
    case AiToolPolicy::PackageToolsOnly:
        return evaluatePackageOnlyPolicy(context.m_package_tool, context.m_mutating_package);
    case AiToolPolicy::DownloadOnly:
        return evaluateDownloadOnlyPolicy(request);
    case AiToolPolicy::MutatingRequiresLease:
        return evaluateMutatingPolicy(context.m_risky || context.m_shell_unproven, false);
    case AiToolPolicy::ExclusiveMutatingExecutor:
        return evaluateMutatingPolicy(context.m_risky || context.m_shell_unproven, true);
    }
    return block(QStringLiteral("Unsupported tool policy"));
}

bool isReadOnlyAppActionOperation(const QString& operation) {
    // sak_app_action exposes exactly "list" (a pure in-memory catalog read) and "run"
    // (executes a technician action whose descriptor may be mutating, destructive,
    // catastrophic, or admin). An empty/omitted operation defaults to "list" in the
    // handler; anything else is treated as a run, fail-closed.
    const QString op = norm(operation);
    return op.isEmpty() || op == QLatin1String("list");
}

AiToolPolicyDecision evaluateAppActionPolicy(AiToolPolicy policy,
                                             const AiToolCallRequest& request) {
    if (isReadOnlyAppActionOperation(request.operation)) {
        return allow(QStringLiteral("App action catalog listing allowed (read-only)"));
    }
    // Running a technician action is a mutating capability. The policy layer cannot see
    // the resolved descriptor's risk flags (they resolve later in the run handler), so it
    // fails closed: only the mutating policies may run one, and it ALWAYS takes a lease so
    // it cannot execute concurrently with another mutation. The per-action human gate in
    // the handler still applies on top -- it is not sufficient on its own to bound a
    // delegated sub-agent to its clamped policy ceiling.
    const bool exclusive = policy == AiToolPolicy::ExclusiveMutatingExecutor;
    if (policy != AiToolPolicy::MutatingRequiresLease && !exclusive) {
        auto decision = block(QStringLiteral(
            "App action run blocked: the effective tool policy does not permit system mutation"));
        decision.risky_change = true;
        decision.requires_lease = true;
        return decision;
    }
    auto decision = allow(QStringLiteral("App action run allowed (mutation lease required)"));
    decision.risky_change = true;
    decision.requires_lease = true;
    decision.requires_exclusive_lease = exclusive;
    decision.restore_point_recommended = true;
    return decision;
}

}  // namespace

AiToolPolicy toolPolicyFromString(const QString& value) {
    const QString policy = norm(value);
    if (policy == QLatin1String("read_only_pc")) {
        return AiToolPolicy::ReadOnlyPc;
    }
    if (policy == QLatin1String("package_tools_only")) {
        return AiToolPolicy::PackageToolsOnly;
    }
    if (policy == QLatin1String("download_only")) {
        return AiToolPolicy::DownloadOnly;
    }
    if (policy == QLatin1String("mutating_requires_lease")) {
        return AiToolPolicy::MutatingRequiresLease;
    }
    if (policy == QLatin1String("exclusive_mutating_executor")) {
        return AiToolPolicy::ExclusiveMutatingExecutor;
    }
    return AiToolPolicy::NoLocalExecution;
}

QString toolPolicyToString(AiToolPolicy policy) {
    switch (policy) {
    case AiToolPolicy::NoLocalExecution:
        return QStringLiteral("no_local_execution");
    case AiToolPolicy::ReadOnlyPc:
        return QStringLiteral("read_only_pc");
    case AiToolPolicy::PackageToolsOnly:
        return QStringLiteral("package_tools_only");
    case AiToolPolicy::DownloadOnly:
        return QStringLiteral("download_only");
    case AiToolPolicy::MutatingRequiresLease:
        return QStringLiteral("mutating_requires_lease");
    case AiToolPolicy::ExclusiveMutatingExecutor:
        return QStringLiteral("exclusive_mutating_executor");
    }
    return QStringLiteral("no_local_execution");
}

bool isKnownLocalTool(const QString& tool_name) {
    const QString name = norm(tool_name);
    return isShellTool(name) || name == QLatin1String("take_screenshot") ||
           name == QLatin1String("download_file") || isPackageTool(name) ||
           isProviderGatewayTool(name) || isSessionSearchTool(name) || isSkillTool(name) ||
           isDelegateSubagentTool(name) || isRunWorkflowTool(name) || isAppActionTool(name);
}

bool isMutatingPackageOperation(const QString& operation) {
    const QString op = norm(operation);
    return op == QLatin1String("install") || op == QLatin1String("uninstall") ||
           op == QLatin1String("upgrade") || op == QLatin1String("build_bundle") ||
           op == QLatin1String("install_bundle");
}

static QString shellEscapeStrippedPreview(const QString& preview) {
    // cmd.exe removes its escape character (U+005E) and PowerShell removes its own
    // (U+0060) before parsing. Both are written as hex escapes rather than literal
    // characters so the complexity gate's tokenizer does not mistake them for delimiters.
    //
    // For the PowerShell escape, note that the newline/tab/bell character substitutions
    // apply only INSIDE double-quoted strings; in command position the escape before any
    // character yields that literal character, so a split spelling of vssadmin really does
    // invoke vssadmin. Excluding the escape letters would miss real bypasses, so every
    // intra-word occurrence is stripped.
    static const QRegularExpression kCaretSplit(QStringLiteral("(\\w)\\x5E(\\w)"));
    static const QRegularExpression kEscapeSplit(QStringLiteral("(\\w)\\x60(\\w)"));
    // cmd.exe uses '^' and PowerShell uses U+0060 as escape characters that the shell
    // removes before parsing, so a keyword split by one of them executes exactly as the
    // unsplit keyword. Every risk regex below matches whole words, so a single escape
    // inside a keyword defeated all of them at once: the command was neither catastrophic
    // nor obfuscated nor even risky, and ran with no lease, no restore point and no
    // confirmation.
    //
    // Strip an escape character only where it sits BETWEEN two word characters, which is
    // the keyword-splitting shape. Legitimate uses -- a trailing '^' line continuation, an
    // escape before a quote or a newline -- are not intra-word and stay untouched, so this
    // does not manufacture matches for ordinary commands. The loop handles a keyword split
    // more than once, such as "f^o^rmat".
    constexpr int kMaxEscapeStripPasses = 8;
    QString stripped = preview;
    for (int guard = 0; guard < kMaxEscapeStripPasses; ++guard) {
        const QString previous = stripped;
        stripped.replace(kCaretSplit, QStringLiteral("\\1\\2"));
        stripped.replace(kEscapeSplit, QStringLiteral("\\1\\2"));
        if (stripped == previous) {
            break;
        }
    }
    return stripped;
}

static QString shellCommandNormalizedPreview(const QString& preview) {
    // Windows resolves "format.com", "cipher.exe" and "wbadmin.exe" to exactly the programs
    // the bare names name, but every whole-word pattern below is written for the bare name
    // and expects a space after it, so the suffixed spelling matched NOTHING: "format.com D:"
    // and "cipher.exe /w:c" escaped even the risky tier. Match the escape-stripped form with
    // the executable suffix removed as well.
    QString normalized = shellEscapeStrippedPreview(preview);
    static const QRegularExpression kExeSuffix(QStringLiteral(R"(\.(exe|com|cmd|bat|ps1|msc)\b)"),
                                               QRegularExpression::CaseInsensitiveOption);
    normalized.remove(kExeSuffix);
    return normalized;
}

bool commandLooksObfuscated(const QString& preview) {
    // Encoded/indirection shapes that hide the real command from the risk regex.
    // PowerShell -EncodedCommand/-enc, base64 decode, Invoke-Expression, in-memory
    // remote download-and-run (Net.WebClient / Invoke-WebRequest piped to iex),
    // and LOLBin decoders (certutil -decode/-urlcache).
    static const QRegularExpression kObfuscated(
        QStringLiteral(
            R"((\s-enc(odedcommand)?\b|\s-e\s+[A-Za-z0-9+/]{24,}={0,2}|\bfrombase64string\b|\b(iex|invoke-expression)\b|\bdownloadstring\b|\bdownloadfile\b|\b(new-object\s+)?net\.webclient\b|\binvoke-webrequest\b.*\|\s*(iex|invoke-expression)\b|\bcertutil\b.*\s-(decode|urlcache|f)\b|\bconvert\]::frombase64))"),
        QRegularExpression::CaseInsensitiveOption);
    // Deliberately NOT flagging the mere presence of an intra-word escape: a double-quoted
    // string carrying a newline escape is ordinary output, and escalating it to
    // catastrophic would force a confirmation prompt on it. Instead the stripped form is
    // against the same patterns, so an escape only matters when it reveals a real command.
    const QString stripped = shellEscapeStrippedPreview(preview);
    return kObfuscated.match(preview).hasMatch() || kObfuscated.match(stripped).hasMatch() ||
           commandUsesResolutionIndirection(preview) || commandUsesResolutionIndirection(stripped);
}

bool commandLooksCatastrophic(const QString& preview) {
    // Irreversible, system/data-destroying operations. These must never run
    // ungated; callers force an explicit human confirmation even in unattended
    // mode. Kept tight to avoid false positives on ordinary mutating commands.
    //
    // Switches may precede the drive letter ("format /q D:"); Remove-Item's aliases rm/ri
    // wipe exactly what Remove-Item wipes; PowerShell accepts forward slashes in paths
    // ("C:/Windows"); and dd destroys whichever way round if=/of= are written.
    static const QRegularExpression kCatastrophic(
        QStringLiteral(
            R"((\bformat\b\s+(?:/\S+\s+)*[a-z]:|\bformat-volume\b|\bdiskpart\b|\bclear-disk\b|\bremove-partition\b|\bclear-volume\b|\breset-physicaldisk\b|\binitialize-disk\b|\bbcdedit\b|\bvssadmin\b\s+delete|\bwbadmin\b\s+delete|\bwevtutil\b\s+cl\b|\bcipher\b\s+/w|\breg\b\s+delete\s+hk|\bremove-item\b[^\n]*\bhk(lm|cu|cr|u):|\bset-executionpolicy\b\s+\S*(unrestricted|bypass)|\b(?:remove-item|rm|ri)\b(?=[^\n]*\s-r)(?=[^\n]*(\$env:systemroot|\$env:windir|c:[\\/]windows|c:[\\/]program files))|\b(rd|rmdir)\b\s+/s\b(?=[^\n]*(c:[\\/]windows|c:[\\/]program files|%systemroot%|%windir%))|\bmkfs\b|\bdd\b(?=[^\n]*\b(if|of)=)))"),
        QRegularExpression::CaseInsensitiveOption);
    // Match the shell-escape-stripped form as well, so "fo^rmat C:" and "For`mat-Volume"
    // are classified exactly like the plain commands they execute as, and the
    // executable-suffix-stripped form so "format.com D:" is too.
    return kCatastrophic.match(preview).hasMatch() ||
           kCatastrophic.match(shellEscapeStrippedPreview(preview)).hasMatch() ||
           kCatastrophic.match(shellCommandNormalizedPreview(preview)).hasMatch();
}

bool commandLooksRiskyChange(const QString& preview) {
    // A blacklist is fail-open, so it must at least cover the common mutating
    // cmdlets/commands. Earlier revisions omitted rename/move/copy/content-writing
    // verbs, so Rename-Item / Move-Item / Copy-Item / Add-Content / Out-File and
    // the cmd.exe equivalents executed under the read-only lease. This revision also
    // adds native mutators the PowerShell-cmdlet list missed: reg add, sc / net
    // service control, taskkill, shutdown, schtasks, powercfg, and file-writing output
    // redirection (> / >>). Obfuscated and catastrophic shapes also count as risky so
    // they never slip through as safe.
    static const QRegularExpression kRisky(
        QStringLiteral(
            R"((\bremove-\w+|\bclear-\w+|\bset-\w+|\bnew-\w+|\brename-\w+|\bmove-\w+|\bcopy-\w+|\badd-content\b|\bout-file\b|\btee-object\b|\bdelete\b|\bdel\b|\berase\b|\brd\b|\brmdir\b|\bmkdir\b|\bmd\b|\bmove\b|\bren\b|\brename\b|\bcopy\b|\bxcopy\b|\brobocopy\b|\bformat\b|\bclean\b|\breset\b|\brepair\b|\brestorehealth\b|\bchkdsk\b.*\s/[frx]|\bsfc\b|\bdism\b|\bmsiexec\b|\bwinget\s+(install|uninstall|upgrade)|\bchoco\s+(install|uninstall|upgrade)|\buninstall\b|\binstall\b|\bdisable-\w+|\benable-\w+|\bstop-service\b|\bstart-service\b|\brestart-service\b|\bstop-process\b|\bkill\b|\brestart-computer\b|\bstop-computer\b|(?:^|[|;&\r\n]\s*)r[im]\b|\]\s*::|\.\w+\s*\(|\bset-itemproperty\b|\bnew-itemproperty\b|\bremove-item\b|\breg\b\s+add\b|\bsc(\.exe)?\b\s+(stop|start|config|delete|create|failure|pause|continue)\b|\bnet\b\s+(stop|start)\b|\btaskkill\b|\bshutdown\b|\bschtasks\b\s*/(create|delete|change|run|end)|\bpowercfg\b\s*/(setactive|s\b|import|delete|x\b)|\s\d*>>?\s*(?!&|nul\b|\$null\b|/dev/null\b)[^\s>&|]))"),
        QRegularExpression::CaseInsensitiveOption);
    return kRisky.match(preview).hasMatch() ||
           kRisky.match(shellEscapeStrippedPreview(preview)).hasMatch() ||
           kRisky.match(shellCommandNormalizedPreview(preview)).hasMatch() ||
           commandLooksObfuscated(preview) || commandLooksCatastrophic(preview);
}

AiToolPolicyDecision evaluateToolPolicy(AiToolPolicy policy, const AiToolCallRequest& request) {
    if (!isKnownLocalTool(request.tool_name)) {
        return block(QStringLiteral("Unknown local tool"));
    }
    // Reading bundled/user skill guidance is a pure text lookup with no PC, disk,
    // or network access, so it is allowed under every policy (including read-only
    // and no-local-execution) and never needs a lease or restore point.
    if (isSkillTool(request.tool_name)) {
        return allow(QStringLiteral("Skill guidance lookup allowed"));
    }
    // Delegating to a sub-agent is itself allowed under every mode: it only spawns
    // a scoped reasoner. The sub-agent's OWN tool policy (clamped to this session's
    // ceiling by the caller) gates whatever the sub-agent then tries to execute.
    if (isDelegateSubagentTool(request.tool_name)) {
        return allow(QStringLiteral("Sub-agent delegation allowed (sub-agent policy clamped)"));
    }
    // Launching a catalog workflow is allowed under every mode: each of the
    // workflow's phases is independently policy/lease/human-gated as it runs, so
    // the launch itself grants no capability beyond the session.
    if (isRunWorkflowTool(request.tool_name)) {
        return allow(QStringLiteral("Workflow launch allowed (per-phase gates apply)"));
    }
    // Enumerating the app's own technician actions (operation=list) is a read-only
    // catalog lookup allowed under every mode. RUNNING an action is gated against the
    // EFFECTIVE (already clamped by the caller) policy and takes a mutation lease -- the
    // per-action human gate in the run handler is not sufficient on its own to bound a
    // delegated sub-agent to its policy ceiling or to serialize concurrent mutations.
    if (isAppActionTool(request.tool_name)) {
        return evaluateAppActionPolicy(policy, request);
    }
    if (packageMutationContradictsScanRequest(request)) {
        auto decision =
            block(QStringLiteral("Package install/upgrade/uninstall blocked because the user asked "
                                 "to scan, not install"));
        decision.risky_change = true;
        decision.requires_lease = true;
        return decision;
    }
    if (packageMutationMissingExplicitIntent(request)) {
        auto decision =
            block(QStringLiteral("Package install/upgrade/uninstall blocked because the user did "
                                 "not explicitly request package mutation"));
        decision.risky_change = true;
        decision.requires_lease = true;
        return decision;
    }

    const ToolPolicyContext context = policyContext(request);
    AiToolPolicyDecision decision = evaluateKnownPolicy(policy, request, context);
    if (context.m_catastrophic && decision.allowed) {
        // Force the full mutating treatment regardless of which policy allowed it.
        decision.catastrophic_change = true;
        decision.risky_change = true;
        decision.requires_lease = true;
        decision.restore_point_recommended = true;
        // Machine-enforce the human gate: a catastrophic/irreversible op (disk format,
        // partition wipe, boot-config edit, shadow-copy/backup delete, hive delete,
        // recursive system-dir wipe) is NOT allowed to dispatch until an explicit human
        // confirmation is recorded on the request. Callers detect catastrophic_change,
        // obtain the confirmation, set request.human_confirmed, and re-dispatch. Leaving
        // it to a caller convention meant any path that skipped the panel's confirm --
        // a direct dispatch, a future caller -- could run a wipe unattended (the gap this
        // finding closes). catastrophic_change stays set so the gate still knows to prompt.
        if (!request.human_confirmed) {
            decision.allowed = false;
            decision.reason = QStringLiteral(
                "Catastrophic operation blocked: an explicit human confirmation is required");
        }
    }
    return decision;
}

int toolPolicyRank(AiToolPolicy policy) {
    constexpr int kRankDownloadTier = 2;
    constexpr int kRankMutatingRequiresLease = 3;
    constexpr int kRankExclusiveMutatingExecutor = 4;
    switch (policy) {
    case AiToolPolicy::NoLocalExecution:
        return 0;
    case AiToolPolicy::ReadOnlyPc:
        return 1;
    case AiToolPolicy::DownloadOnly:
    case AiToolPolicy::PackageToolsOnly:
        return kRankDownloadTier;
    case AiToolPolicy::MutatingRequiresLease:
        return kRankMutatingRequiresLease;
    case AiToolPolicy::ExclusiveMutatingExecutor:
        return kRankExclusiveMutatingExecutor;
    }
    return 0;
}

namespace {

// Capability containment. The modes are NOT a single line of increasing permission:
// ReadOnlyPc grants shell diagnostics, screenshots, session search and provider status,
// none of which PackageToolsOnly or DownloadOnly grant. Ranking alone therefore answered
// "is ReadOnlyPc within a PackageToolsOnly ceiling?" with yes (rank 1 < 2) and handed a
// package-only session a shell-capable sub-agent. A request is honored only when the
// ceiling already grants everything the request grants.
bool policyGrantsSubsetOf(AiToolPolicy requested, AiToolPolicy ceiling) {
    if (requested == ceiling || requested == AiToolPolicy::NoLocalExecution) {
        return true;
    }
    if (ceiling == AiToolPolicy::ExclusiveMutatingExecutor) {
        return true;
    }
    if (ceiling == AiToolPolicy::MutatingRequiresLease) {
        // The mutating modes allow every known local tool, so every narrower mode is
        // contained; only the exclusive executor is not (it is the broader lease tier).
        return requested != AiToolPolicy::ExclusiveMutatingExecutor;
    }
    return false;
}

}  // namespace

AiToolPolicy clampToolPolicy(AiToolPolicy requested, AiToolPolicy ceiling) {
    // A request is honored only when the session ceiling already grants everything it
    // grants; otherwise the ceiling stands, so a sub-agent never gains a capability the
    // session itself was not granted. An incomparable pair (DownloadOnly vs
    // PackageToolsOnly, ReadOnlyPc under a package-only ceiling) resolves to the ceiling,
    // never to the union of the two capability sets.
    return policyGrantsSubsetOf(requested, ceiling) ? requested : ceiling;
}

}  // namespace sak::ai
