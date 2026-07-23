// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_tool_policy.h"

#include <QRegularExpression>
#include <QStringList>

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
    for (const auto& needle : needles) {
        if (text.contains(needle, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
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
                           QStringLiteral("don't "),
                           QStringLiteral("dont "),
                           QStringLiteral("never "),
                           QStringLiteral("instead of "),
                           QStringLiteral("rather than "),
                           QStringLiteral("without "),
                           QStringLiteral("no need to "),
                           QStringLiteral("avoid ")});
}

bool hasExplicitPackageMutationIntent(const AiToolCallRequest& request) {
    const QString text = norm(request.user_message);
    if (text.isEmpty()) {
        return false;
    }
    if (hasNegatedActionIntent(text)) {
        return false;
    }
    const QString op = norm(request.operation);
    if (op == QLatin1String("install") || op == QLatin1String("install_bundle")) {
        return textMatchesAny(text,
                              {QStringLiteral("install "),
                               QStringLiteral(" install"),
                               QStringLiteral("reinstall"),
                               QStringLiteral("set up"),
                               QStringLiteral("setup "),
                               QStringLiteral("repair install")});
    }
    if (op == QLatin1String("upgrade")) {
        return textMatchesAny(text,
                              {QStringLiteral("upgrade"),
                               QStringLiteral(" update "),
                               QStringLiteral("update "),
                               QStringLiteral("bring up to date")});
    }
    if (op == QLatin1String("uninstall")) {
        return textMatchesAny(text,
                              {QStringLiteral("uninstall"),
                               QStringLiteral("remove "),
                               QStringLiteral(" remove"),
                               QStringLiteral("delete app"),
                               QStringLiteral("get rid of")});
    }
    if (op == QLatin1String("build_bundle")) {
        return textMatchesAny(text,
                              {QStringLiteral("build bundle"),
                               QStringLiteral("create bundle"),
                               QStringLiteral("offline bundle")});
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

struct ToolPolicyContext {
    bool shell{false};
    bool package_tool{false};
    bool provider_gateway{false};
    bool mutating_package{false};
    bool risky{false};
    bool catastrophic{false};
};

ToolPolicyContext policyContext(const AiToolCallRequest& request) {
    ToolPolicyContext context;
    context.shell = isShellTool(request.tool_name);
    context.package_tool = isPackageTool(request.tool_name);
    context.provider_gateway = isReadOnlyProviderOperation(request);
    context.mutating_package = context.package_tool &&
                               isMutatingPackageOperation(request.operation);
    context.catastrophic = context.shell && commandLooksCatastrophic(request.command_preview);
    context.risky = request.requires_admin ||
                    (context.shell && commandLooksRiskyChange(request.command_preview)) ||
                    context.mutating_package || isMutatingProviderOperation(request);
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

AiToolPolicyDecision evaluateReadOnlyPolicy(const AiToolCallRequest& request,
                                            bool shell,
                                            bool provider_gateway,
                                            bool risky) {
    if (risky) {
        auto decision = block(QStringLiteral("Read-only PC policy blocked mutating command"));
        decision.risky_change = true;
        return decision;
    }
    if (!shell && !provider_gateway && !isSessionSearchTool(request.tool_name) &&
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
            request, context.shell, context.provider_gateway, context.risky);
    case AiToolPolicy::PackageToolsOnly:
        return evaluatePackageOnlyPolicy(context.package_tool, context.mutating_package);
    case AiToolPolicy::DownloadOnly:
        return evaluateDownloadOnlyPolicy(request);
    case AiToolPolicy::MutatingRequiresLease:
        return evaluateMutatingPolicy(context.risky, false);
    case AiToolPolicy::ExclusiveMutatingExecutor:
        return evaluateMutatingPolicy(context.risky, true);
    }
    return block(QStringLiteral("Unsupported tool policy"));
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
           isDelegateSubagentTool(name) || isRunWorkflowTool(name);
}

bool isMutatingPackageOperation(const QString& operation) {
    const QString op = norm(operation);
    return op == QLatin1String("install") || op == QLatin1String("uninstall") ||
           op == QLatin1String("upgrade") || op == QLatin1String("build_bundle") ||
           op == QLatin1String("install_bundle");
}

bool commandLooksObfuscated(const QString& preview) {
    // Encoded/indirection shapes that hide the real command from the risk regex.
    // PowerShell -EncodedCommand/-enc, base64 decode, Invoke-Expression, in-memory
    // remote download-and-run (Net.WebClient / Invoke-WebRequest piped to iex),
    // and LOLBin decoders (certutil -decode/-urlcache).
    static const QRegularExpression obfuscated(
        QStringLiteral(
            R"((\s-enc(odedcommand)?\b|\s-e\s+[A-Za-z0-9+/]{24,}={0,2}|\bfrombase64string\b|\b(iex|invoke-expression)\b|\bdownloadstring\b|\bdownloadfile\b|\b(new-object\s+)?net\.webclient\b|\binvoke-webrequest\b.*\|\s*(iex|invoke-expression)\b|\bcertutil\b.*\s-(decode|urlcache|f)\b|\bconvert\]::frombase64))"),
        QRegularExpression::CaseInsensitiveOption);
    return obfuscated.match(preview).hasMatch();
}

bool commandLooksCatastrophic(const QString& preview) {
    // Irreversible, system/data-destroying operations. These must never run
    // ungated; callers force an explicit human confirmation even in unattended
    // mode. Kept tight to avoid false positives on ordinary mutating commands.
    static const QRegularExpression catastrophic(
        QStringLiteral(
            R"((\bformat\b\s+[a-z]:|\bformat-volume\b|\bdiskpart\b|\bclear-disk\b|\bremove-partition\b|\bclear-volume\b|\breset-physicaldisk\b|\binitialize-disk\b|\bbcdedit\b|\bvssadmin\b\s+delete|\bwbadmin\b\s+delete|\bwevtutil\b\s+cl\b|\bcipher\b\s+/w|\breg\b\s+delete\s+hk|\bremove-item\b[^\n]*\bhk(lm|cu|cr|u):|\bset-executionpolicy\b\s+\S*(unrestricted|bypass)|\bremove-item\b(?=[^\n]*-recurse)(?=[^\n]*(\$env:systemroot|\$env:windir|c:\\windows|c:\\program files))|\b(rd|rmdir)\b\s+/s\b(?=[^\n]*(c:\\windows|c:\\program files|%systemroot%|%windir%))|\bmkfs\b|\bdd\b\s+if=))"),
        QRegularExpression::CaseInsensitiveOption);
    return catastrophic.match(preview).hasMatch();
}

bool commandLooksRiskyChange(const QString& preview) {
    // A blacklist is fail-open, so it must at least cover the common mutating
    // cmdlets/commands. Earlier revisions omitted rename/move/copy/content-writing
    // verbs, so Rename-Item / Move-Item / Copy-Item / Add-Content / Out-File and
    // the cmd.exe equivalents executed under the read-only lease. Obfuscated and
    // catastrophic shapes also count as risky so they never slip through as safe.
    static const QRegularExpression risky(
        QStringLiteral(
            R"((\bremove-\w+|\bclear-\w+|\bset-\w+|\bnew-\w+|\brename-\w+|\bmove-\w+|\bcopy-\w+|\badd-content\b|\bout-file\b|\btee-object\b|\bdelete\b|\bdel\b|\berase\b|\brd\b|\brmdir\b|\bmkdir\b|\bmd\b|\bmove\b|\bren\b|\brename\b|\bcopy\b|\bxcopy\b|\brobocopy\b|\bformat\b|\bclean\b|\breset\b|\brepair\b|\brestorehealth\b|\bchkdsk\b.*\s/[frx]|\bsfc\b|\bdism\b|\bmsiexec\b|\bwinget\s+(install|uninstall|upgrade)|\bchoco\s+(install|uninstall|upgrade)|\buninstall\b|\binstall\b|\bdisable-\w+|\benable-\w+|\bstop-service\b|\bstart-service\b|\bset-itemproperty\b|\bnew-itemproperty\b|\bremove-item\b))"),
        QRegularExpression::CaseInsensitiveOption);
    return risky.match(preview).hasMatch() || commandLooksObfuscated(preview) ||
           commandLooksCatastrophic(preview);
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
    if (context.catastrophic && decision.allowed) {
        // Force the full mutating treatment regardless of which policy allowed it;
        // the panel additionally requires an explicit human gate for these.
        decision.catastrophic_change = true;
        decision.risky_change = true;
        decision.requires_lease = true;
        decision.restore_point_recommended = true;
    }
    return decision;
}

int toolPolicyRank(AiToolPolicy policy) {
    switch (policy) {
    case AiToolPolicy::NoLocalExecution:
        return 0;
    case AiToolPolicy::ReadOnlyPc:
        return 1;
    case AiToolPolicy::DownloadOnly:
    case AiToolPolicy::PackageToolsOnly:
        return 2;
    case AiToolPolicy::MutatingRequiresLease:
        return 3;
    case AiToolPolicy::ExclusiveMutatingExecutor:
        return 4;
    }
    return 0;
}

AiToolPolicy clampToolPolicy(AiToolPolicy requested, AiToolPolicy ceiling) {
    // A strictly-more-restrictive request is honored; otherwise fall back to the
    // session ceiling. Equal-rank-but-different-axis (e.g. DownloadOnly vs
    // PackageToolsOnly) resolves to the ceiling so a sub-agent never gains a
    // capability the session itself was not granted.
    return toolPolicyRank(requested) < toolPolicyRank(ceiling) ? requested : ceiling;
}

}  // namespace sak::ai
