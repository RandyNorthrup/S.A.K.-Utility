// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_command_guard.h"

#include <QRegularExpression>
#include <QStringList>

namespace sak::ai {

namespace {

// At namespace scope, not inside the function. A raw-string regex in a function body defeats
// lizard's C++ tokenizer: it stops seeing where that function ends and measures its length and
// complexity against everything that follows, failing the gate for a file that is fine.
const QRegularExpression kReadCommandRegex(
    QStringLiteral(R"((?:^|[\s{}()])(?:get-content|gc|cat|type)(?=$|\s))"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kQuotedTextRegex(
    QStringLiteral(R"regex('(?:''|[^'])*(?:'|$)|"(?:`.|""|[^"])*(?:"|$))regex"));
const QRegularExpression kQuotedCallReadRegex(
    QStringLiteral(R"(^\s*&\s*(['"])(?:get-content|gc|cat|type)\1(?=$|\s))"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kInterpolatedReadRegex(
    QStringLiteral(
        R"regex("(?:(?:`.)|[^"`])*(?<!`)\$\([^)]*\b(?:get-content|gc|cat|type)\b[^)]*\)(?:(?:`.)|[^"`])*")regex"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kBinaryExtensionRegex(
    QStringLiteral(
        R"(\.(?:exe|dll|bin|db|db3|sqlite|sdb|zip|7z|msi|cab|sys|png|jpe?g|gif|webp|pdf)(?:['"`\s;)]|$))"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kRegistryRootRegex(
    QStringLiteral(R"((?:(?:hklm|hkcu):\\|registry::hkey_(?:local_machine|)"
                   R"(current_user)\\)software(?:\\)?(?:['"`\s,;)]|$))"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kActiveRecurseRegex(QStringLiteral("(?:^|\\s)-rec(?:u(?:r(?:s(?:e)?)?)?)?"
                                                            "(?:\\s*:\\s*\\$true)?(?:$|[\\s,)])"),
                                             QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kPidAssignmentRegex(
    QStringLiteral(R"((^|[;\s])(?:\[[^\]]+\])?\$pid\s*=|\[ref\]\s*\$pid\b)"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kBypassSwitchRegex(
    QStringLiteral(R"((?:^|[\s;|&'"`(])[-/]{1,2}(?:ignore-?checksums?)"
                   R"(|allow-?empty-?checksums?|skip-?checksums?)"
                   R"(|download-?checksums?(?:-?x64)?|checksums?(?:64)?)"
                   R"(|checksum-?type)\b)"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kChocolateyTempEnvironmentRegex(
    QStringLiteral(R"((?:\$env:temp|\$\{env:temp\}|%temp%)[\\/]chocolatey[\\/])"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kChocolateyInstallCacheRegex(
    QStringLiteral(R"((?:\$env:chocolateyinstall|\$\{env:chocolateyinstall\}|)"
                   R"(%chocolateyinstall%)[\\/](?:lib-bad|cache|temp)[\\/])"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kChocolateyTempPathRegex(
    QStringLiteral(R"([\\/](?:data[\\/])?temp[\\/]chocolatey[\\/])"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kChocolateyRootCacheRegex(
    QStringLiteral(R"([\\/]chocolatey[\\/](?:lib-bad|cache|temp)[\\/])"),
    QRegularExpression::CaseInsensitiveOption);
const QRegularExpression kInstallerExtensionRegex(
    QStringLiteral(R"(\.(?:exe|msi|msix|msixbundle|appx)(?:['"`\s;)]|$))"),
    QRegularExpression::CaseInsensitiveOption);

bool isClauseSeparator(QChar ch) {
    return ch == QLatin1Char(';') || ch == QLatin1Char('|') || ch == QLatin1Char('&') ||
           ch == QLatin1Char('\n') || ch == QLatin1Char('\r');
}

bool beginsCallOperatorClause(const QString& current) {
    const QString trimmed = current.trimmed();
    return trimmed.isEmpty() || QStringLiteral("({=,:").contains(trimmed.back());
}

struct ClauseScanState {
    QChar quote;
    bool escaped{false};
};

void appendCompletedClause(QStringList* clauses, QString* current) {
    const QString clause = current->trimmed();
    if (!clause.isEmpty()) {
        clauses->append(clause);
    }
    current->clear();
}

bool consumeEscapedClauseCharacter(QChar ch, ClauseScanState* state, QString* current) {
    if (state->escaped) {
        current->append(ch);
        state->escaped = false;
        return true;
    }
    if (state->quote != QLatin1Char('\'') && (ch == QLatin1Char('`') || ch == QLatin1Char('^'))) {
        current->append(ch);
        state->escaped = true;
        return true;
    }
    return false;
}

bool consumeClauseQuote(const QString& command,
                        qsizetype* index,
                        ClauseScanState* state,
                        QString* current) {
    const QChar ch = command.at(*index);
    if (ch != QLatin1Char('\'') && ch != QLatin1Char('"')) {
        return false;
    }
    if (state->quote.isNull()) {
        state->quote = ch;
    } else if (state->quote == ch) {
        if (*index + 1 < command.size() && command.at(*index + 1) == ch) {
            current->append(ch);
            ++(*index);
        } else {
            state->quote = QChar();
        }
    }
    current->append(ch);
    return true;
}

QStringList splitCommandClauses(const QString& normalized_command) {
    QStringList clauses;
    QString current;
    ClauseScanState state;
    for (qsizetype index = 0; index < normalized_command.size(); ++index) {
        const QChar ch = normalized_command.at(index);
        if (consumeEscapedClauseCharacter(ch, &state, &current)) {
            continue;
        }
        if (consumeClauseQuote(normalized_command, &index, &state, &current)) {
            continue;
        }
        if (state.quote.isNull() && isClauseSeparator(ch)) {
            if (ch == QLatin1Char('&') && beginsCallOperatorClause(current)) {
                appendCompletedClause(&clauses, &current);
                current.append(ch);
                continue;
            }
            appendCompletedClause(&clauses, &current);
            continue;
        }
        current.append(ch);
    }
    appendCompletedClause(&clauses, &current);
    return clauses;
}

QStringList commandSecurityClauses(const AiCommandRequest& request) {
    const QString normalized_command = request.command.toLower();
    QStringList clauses = splitCommandClauses(normalized_command);
    const QString direct_process = QStringLiteral("%1 %2")
                                       .arg(request.program,
                                            request.arguments.join(QLatin1Char(' ')))
                                       .trimmed();
    if (!direct_process.isEmpty()) {
        clauses.append(direct_process.toLower());
    }
    return clauses;
}

bool commandContainsBinaryContentRead(const QStringList& clauses) {
    for (const QString& clause : clauses) {
        QString command_syntax = clause;
        command_syntax.replace(kQuotedTextRegex, QStringLiteral(" "));
        const bool reads_content = kReadCommandRegex.match(command_syntax).hasMatch() ||
                                   kQuotedCallReadRegex.match(clause).hasMatch() ||
                                   kInterpolatedReadRegex.match(clause).hasMatch();
        if (reads_content && kBinaryExtensionRegex.match(clause).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool commandContainsBroadRegistryRecursion(const QStringList& clauses) {
    for (const QString& clause : clauses) {
        if (kRegistryRootRegex.match(clause).hasMatch() &&
            kActiveRecurseRegex.match(clause).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool commandContainsPowerShellPidMutation(const QStringList& clauses) {
    for (const QString& clause : clauses) {
        if (kPidAssignmentRegex.match(clause).hasMatch()) {
            return true;
        }
    }
    return false;
}

bool commandContainsChecksumBypass(const QStringList& clauses) {
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
    for (const QString& clause : clauses) {
        if (kBypassSwitchRegex.match(clause).hasMatch() ||
            clause.contains(QStringLiteral("chocolateyignorechecksums")) ||
            clause.contains(QStringLiteral("chocolateyallowemptychecksums")) ||
            clause.contains(QStringLiteral("ignorechecksum")) ||
            clause.contains(QStringLiteral("allowemptychecksum")) ||
            clause.contains(QStringLiteral("skipchecksum"))) {
            return true;
        }
    }
    return false;
}

bool clauseContainsChocolateyCachePath(const QString& clause) {
    return kChocolateyTempEnvironmentRegex.match(clause).hasMatch() ||
           kChocolateyInstallCacheRegex.match(clause).hasMatch() ||
           kChocolateyTempPathRegex.match(clause).hasMatch() ||
           kChocolateyRootCacheRegex.match(clause).hasMatch();
}

bool commandContainsCachedPackageInstallerRun(const QStringList& clauses) {
    // Any reference to a package-cache installer needs approval. Requiring one of a fixed
    // set of launch verbs failed open: request.program can name the installer directly,
    // and "msiexec /i", &"path" without a space, a tab-separated call operator or an
    // environment-variable path all launch it without matching any of those verbs.
    for (const QString& clause : clauses) {
        if (clauseContainsChocolateyCachePath(clause) &&
            kInstallerExtensionRegex.match(clause).hasMatch()) {
            return true;
        }
    }
    return false;
}

QString commandGuardBlockError(const QStringList& clauses) {
    if (commandContainsBinaryContentRead(clauses)) {
        return QStringLiteral(
            "Blocked binary file dump. Use Get-Item, Get-FileHash, Authenticode signature checks, "
            "or Format-Hex -Count for a small sample instead of Get-Content/cat/type.");
    }
    if (commandContainsBroadRegistryRecursion(clauses)) {
        return QStringLiteral(
            "Blocked broad recursive registry scan. Query exact uninstall/vendor keys instead and "
            "cap output with Select-Object -First.");
    }
    if (commandContainsPowerShellPidMutation(clauses)) {
        return QStringLiteral(
            "Blocked PowerShell $PID mutation. $PID is a read-only automatic variable; use a "
            "different variable such as $processId or $windowProcessId.");
    }
    if (commandContainsChecksumBypass(clauses)) {
        return QStringLiteral(
            "Blocked package checksum bypass. Do not pass --ignore-checksums, substitute "
            "checksums, or run cached installers after a package checksum mismatch.");
    }
    return {};
}

QString commandGuardApprovalReason(const QStringList& clauses) {
    if (commandContainsCachedPackageInstallerRun(clauses)) {
        return QStringLiteral(
            "Cached package installer execution requested after package-manager handling. "
            "Continue only with explicit user approval and verification evidence.");
    }
    return {};
}

}  // namespace

AiCommandGuardResult evaluateCommandGuard(const AiCommandRequest& request) {
    const QStringList clauses = commandSecurityClauses(request);
    AiCommandGuardResult result;
    result.block_error = commandGuardBlockError(clauses);
    if (result.block_error.isEmpty()) {
        result.approval_reason = commandGuardApprovalReason(clauses);
    }
    result.evaluated = true;
    return result;
}

}  // namespace sak::ai
