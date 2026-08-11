// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_guard.h"

#include <QRegularExpression>

namespace sak::ai {

namespace {

QString combinedCommandText(const AiCommandRequest& request, const QString& preview) {
    return QStringLiteral("%1 %2 %3 %4")
        .arg(request.command, request.program, request.arguments.join(QLatin1Char(' ')), preview)
        .toLower();
}

bool commandContainsBinaryContentRead(const QString& command) {
    static const QRegularExpression kReadCommandRegex(
        QStringLiteral(R"((^|\s|[;&|])(?:get-content|gc|cat|type)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression kBinaryExtensionRegex(
        QStringLiteral(
            R"(\.(?:exe|dll|bin|db|db3|sqlite|sdb|zip|7z|msi|cab|sys|png|jpe?g|gif|webp|pdf)(?:['"`\s;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return kReadCommandRegex.match(command).hasMatch() &&
           kBinaryExtensionRegex.match(command).hasMatch();
}

bool commandContainsBroadRegistryRecursion(const QString& command) {
    static const QRegularExpression kRegistryRootRegex(
        QStringLiteral(R"((hklm|hkcu):\\software(?:['"`\s,;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    // The narrow-scope exception only counts when it is part of an actual registry path
    // in the command. Matching it anywhere in the combined text failed open: a trailing
    // comment or a dummy argument naming the uninstall/vendor key unblocked an otherwise
    // broad recursive scan of the whole SOFTWARE hive.
    static const QRegularExpression kNarrowScopeRegex(
        QStringLiteral(R"((hklm|hkcu):\\software\\[^\s'"`;,)]*)"
                       R"((?:currentversion\\uninstall|superantispyware\.com))"),
        QRegularExpression::CaseInsensitiveOption);
    if (!command.contains(QStringLiteral("-recurse")) ||
        !kRegistryRootRegex.match(command).hasMatch()) {
        return false;
    }
    return !kNarrowScopeRegex.match(command).hasMatch();
}

bool commandContainsPowerShellPidMutation(const QString& command) {
    static const QRegularExpression kPidAssignmentRegex(
        QStringLiteral(R"((^|[;\s])(?:\[[^\]]+\])?\$pid\s*=|\[ref\]\s*\$pid\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return kPidAssignmentRegex.match(command).hasMatch();
}

bool commandContainsChecksumBypass(const QString& command) {
    // Substring matches, not switch parsing: the singular spellings also catch the plural
    // switches, the "--" prefixed forms, and the ChocolateyIgnoreChecksums /
    // ChocolateyAllowEmptyChecksums environment and config spellings of the same bypass.
    return command.contains(QStringLiteral("ignore-checksums")) ||
           command.contains(QStringLiteral("ignorechecksum")) ||
           command.contains(QStringLiteral("allow-empty-checksum")) ||
           command.contains(QStringLiteral("allowemptychecksum")) ||
           command.contains(QStringLiteral("skip-checksum")) ||
           command.contains(QStringLiteral("skipchecksum"));
}

bool commandContainsCachedPackageInstallerRun(const QString& command) {
    static const QRegularExpression kInstallerExtensionRegex(
        QStringLiteral(R"(\.(?:exe|msi|msix|msixbundle|appx)(?:['"`\s;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    const bool package_cache_path =
        command.contains(QStringLiteral("\\data\\temp\\chocolatey\\")) ||
        command.contains(QStringLiteral("/data/temp/chocolatey/")) ||
        command.contains(QStringLiteral("\\chocolatey\\lib-bad\\")) ||
        command.contains(QStringLiteral("/chocolatey/lib-bad/"));
    // Any reference to a package-cache installer needs approval. Requiring one of a fixed
    // set of launch verbs failed open: request.program can name the installer directly,
    // and "msiexec /i", &"path" without a space, a tab-separated call operator or an
    // environment-variable path all launch it without matching any of those verbs.
    return package_cache_path && kInstallerExtensionRegex.match(command).hasMatch();
}

}  // namespace

QString commandGuardBlockError(const AiCommandRequest& request, const QString& preview) {
    const QString command = combinedCommandText(request, preview);
    if (commandContainsBinaryContentRead(command)) {
        return QStringLiteral(
            "Blocked binary file dump. Use Get-Item, Get-FileHash, Authenticode signature checks, "
            "or Format-Hex -Count for a small sample instead of Get-Content/cat/type.");
    }
    if (commandContainsBroadRegistryRecursion(command)) {
        return QStringLiteral(
            "Blocked broad recursive registry scan. Query exact uninstall/vendor keys instead and "
            "cap output with Select-Object -First.");
    }
    if (commandContainsPowerShellPidMutation(command)) {
        return QStringLiteral(
            "Blocked PowerShell $PID mutation. $PID is a read-only automatic variable; use a "
            "different variable such as $processId or $windowProcessId.");
    }
    if (commandContainsChecksumBypass(command)) {
        return QStringLiteral(
            "Blocked package checksum bypass. Do not pass --ignore-checksums, substitute "
            "checksums, or run cached installers after a package checksum mismatch.");
    }
    return {};
}

QString commandGuardApprovalReason(const AiCommandRequest& request, const QString& preview) {
    const QString command = combinedCommandText(request, preview);
    if (commandContainsCachedPackageInstallerRun(command)) {
        return QStringLiteral(
            "Cached package installer execution requested after package-manager handling. "
            "Continue only with explicit user approval and verification evidence.");
    }
    return {};
}

AiCommandGuardResult evaluateCommandGuard(const AiCommandRequest& request, const QString& preview) {
    AiCommandGuardResult result;
    result.block_error = commandGuardBlockError(request, preview);
    if (result.block_error.isEmpty()) {
        result.approval_reason = commandGuardApprovalReason(request, preview);
    }
    result.evaluated = true;
    return result;
}

}  // namespace sak::ai
