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

// At namespace scope, not inside the function. A raw-string regex in a function body defeats
// lizard's C++ tokenizer: it stops seeing where that function ends and measures its length and
// complexity against everything that follows, failing the gate for a file that is fine.
const QRegularExpression kReadCommandRegex(
    QStringLiteral(R"((^|\s|[;&|])(?:get-content|gc|cat|type)\s+)"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kBinaryExtensionRegex(
    QStringLiteral(
        R"(\.(?:exe|dll|bin|db|db3|sqlite|sdb|zip|7z|msi|cab|sys|png|jpe?g|gif|webp|pdf)(?:['"`\s;)]|$))"),
    QRegularExpression::CaseInsensitiveOption);

bool commandContainsBinaryContentRead(const QString& command) {
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
    // The hyphen is OPTIONAL between every word, and the trailing 's' is optional too. The
    // previous version listed fixed spellings and claimed "the singular spellings also catch the
    // plural switches" -- but that only held for the UNHYPHENATED singular. `--ignore-checksum`,
    // the hyphenated singular, matched none of them and passed the guard unblocked, while
    // Chocolatey accepts it perfectly well.
    //
    // CHECKSUM SUBSTITUTION is included, because the refusal message has always claimed to stop
    // it ("Do not pass --ignore-checksums, substitute checksums, ...") while nothing checked for
    // it. Supplying --checksum/--download-checksum means the download is verified against a value
    // the CALLER chose, which defeats verification exactly as thoroughly as skipping it.
    //
    // A switch prefix is required (- / -- / /), so ordinary prose that merely contains the word
    // "checksum" in a preview or a comment does not trip the guard.
    static const QRegularExpression kBypassSwitch(
        QStringLiteral(R"((?:^|[\s;|&'"`(])[-/]{1,2}(?:ignore-?checksums?)"
                       R"(|allow-?empty-?checksums?|skip-?checksums?)"
                       R"(|download-?checksums?(?:-?x64)?|checksums?(?:64)?)"
                       R"(|checksum-?type)\b)"),
        QRegularExpression::CaseInsensitiveOption);
    if (kBypassSwitch.match(command).hasMatch()) {
        return true;
    }
    // The environment / config spellings of the same bypass carry no switch prefix.
    return command.contains(QStringLiteral("ChocolateyIgnoreChecksums"), Qt::CaseInsensitive) ||
           command.contains(QStringLiteral("ChocolateyAllowEmptyChecksums"), Qt::CaseInsensitive) ||
           command.contains(QStringLiteral("ignorechecksum"), Qt::CaseInsensitive) ||
           command.contains(QStringLiteral("allowemptychecksum"), Qt::CaseInsensitive) ||
           command.contains(QStringLiteral("skipchecksum"), Qt::CaseInsensitive);
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
