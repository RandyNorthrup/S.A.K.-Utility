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

bool isDownloadTool(const AiToolCallRequest& request) {
    const QString name = norm(request.tool_name);
    const QString operation = norm(request.operation);
    return name == QLatin1String("download_file") ||
           (name == QLatin1String("sak_offline_downloader") &&
            (operation == QLatin1String("search") ||
             operation == QLatin1String("direct_download")));
}

struct ToolPolicyContext {
    bool shell{false};
    bool package_tool{false};
    bool mutating_package{false};
    bool risky{false};
};

ToolPolicyContext policyContext(const AiToolCallRequest& request) {
    ToolPolicyContext context;
    context.shell = isShellTool(request.tool_name);
    context.package_tool = isPackageTool(request.tool_name);
    context.mutating_package = context.package_tool &&
                               isMutatingPackageOperation(request.operation);
    context.risky = request.requires_admin ||
                    (context.shell && commandLooksRiskyChange(request.command_preview)) ||
                    context.mutating_package;
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
                                            bool risky) {
    if (!shell && norm(request.tool_name) != QLatin1String("take_screenshot")) {
        return block(
            QStringLiteral("Read-only PC policy only allows shell diagnostics and screenshots"));
    }
    if (risky) {
        auto decision = block(QStringLiteral("Read-only PC policy blocked mutating command"));
        decision.risky_change = true;
        return decision;
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
        return evaluateReadOnlyPolicy(request, context.shell, context.risky);
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
           name == QLatin1String("download_file") || isPackageTool(name);
}

bool isMutatingPackageOperation(const QString& operation) {
    const QString op = norm(operation);
    return op == QLatin1String("install") || op == QLatin1String("uninstall") ||
           op == QLatin1String("upgrade") || op == QLatin1String("build_bundle") ||
           op == QLatin1String("install_bundle");
}

bool commandLooksRiskyChange(const QString& preview) {
    static const QRegularExpression risky(
        QStringLiteral(
            R"((\bremove-\w+|\bclear-\w+|\bset-\w+|\bnew-\w+|\bdelete\b|\bdel\b|\berase\b|\brd\b|\brmdir\b|\bformat\b|\bclean\b|\breset\b|\brepair\b|\brestorehealth\b|\bchkdsk\b.*\s/[frx]|\bsfc\b|\bdism\b|\bmsiexec\b|\bwinget\s+(install|uninstall|upgrade)|\bchoco\s+(install|uninstall|upgrade)|\buninstall\b|\binstall\b|\bdisable-\w+|\benable-\w+|\bstop-service\b|\bstart-service\b|\bset-itemproperty\b|\bnew-itemproperty\b|\bremove-item\b))"),
        QRegularExpression::CaseInsensitiveOption);
    return risky.match(preview).hasMatch();
}

AiToolPolicyDecision evaluateToolPolicy(AiToolPolicy policy, const AiToolCallRequest& request) {
    if (!isKnownLocalTool(request.tool_name)) {
        return block(QStringLiteral("Unknown local tool"));
    }

    return evaluateKnownPolicy(policy, request, policyContext(request));
}

}  // namespace sak::ai
