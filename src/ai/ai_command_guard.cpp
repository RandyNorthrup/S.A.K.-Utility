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
    static const QRegularExpression read_command_regex(
        QStringLiteral(R"((^|\s|[;&|])(?:get-content|gc|cat|type)\s+)"),
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression binary_extension_regex(
        QStringLiteral(
            R"(\.(?:exe|dll|bin|db|db3|sqlite|sdb|zip|7z|msi|cab|sys|png|jpe?g|gif|webp|pdf)(?:['"`\s;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    return read_command_regex.match(command).hasMatch() &&
           binary_extension_regex.match(command).hasMatch();
}

bool commandContainsBroadRegistryRecursion(const QString& command) {
    static const QRegularExpression registry_root_regex(
        QStringLiteral(R"((hklm|hkcu):\\software(?:['"`\s,;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    if (!command.contains(QStringLiteral("-recurse")) ||
        !registry_root_regex.match(command).hasMatch()) {
        return false;
    }
    return !command.contains(QStringLiteral("currentversion\\uninstall")) &&
           !command.contains(QStringLiteral("superantispyware.com"));
}

bool commandContainsPowerShellPidMutation(const QString& command) {
    static const QRegularExpression pid_assignment_regex(
        QStringLiteral(R"((^|[;\s])(?:\[[^\]]+\])?\$pid\s*=|\[ref\]\s*\$pid\b)"),
        QRegularExpression::CaseInsensitiveOption);
    return pid_assignment_regex.match(command).hasMatch();
}

bool commandContainsChecksumBypass(const QString& command) {
    return command.contains(QStringLiteral("--ignore-checksums")) ||
           command.contains(QStringLiteral("--ignorechecksum")) ||
           command.contains(QStringLiteral("ignore-checksums")) ||
           command.contains(QStringLiteral("ignorechecksum"));
}

bool commandContainsCachedPackageInstallerRun(const QString& command) {
    static const QRegularExpression installer_extension_regex(
        QStringLiteral(R"(\.(?:exe|msi|msix|msixbundle|appx)(?:['"`\s;)]|$))"),
        QRegularExpression::CaseInsensitiveOption);
    const bool package_cache_path =
        command.contains(QStringLiteral("\\data\\temp\\chocolatey\\")) ||
        command.contains(QStringLiteral("/data/temp/chocolatey/")) ||
        command.contains(QStringLiteral("\\chocolatey\\lib-bad\\")) ||
        command.contains(QStringLiteral("/chocolatey/lib-bad/"));
    if (!package_cache_path || !installer_extension_regex.match(command).hasMatch()) {
        return false;
    }
    return command.contains(QStringLiteral("start-process")) ||
           command.contains(QStringLiteral("invoke-item")) ||
           command.contains(QStringLiteral("cmd /c")) || command.contains(QStringLiteral("& "));
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
    return result;
}

}  // namespace sak::ai
