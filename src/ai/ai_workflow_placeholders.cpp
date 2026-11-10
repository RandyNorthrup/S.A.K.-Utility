// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_placeholders.h"

#include "sak/ai/ai_orchestrator.h"

#include <QJsonArray>
#include <QLatin1Char>
#include <QLatin1String>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>

namespace sak::ai {

namespace {

// 2^63: the first double above the qint64 range (and exactly representable).
constexpr double kTwoPow63 = 9223372036854775808.0;

// A backtick escape or a doubled quote ('' or "") is a two-character lexical unit; the scanner
// consumes both characters at once and advances past the pair.
constexpr qsizetype kTwoCharSequenceLength = 2;

QString scalarJsonValueToString(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isDouble()) {
        // Every numeric result field this resolves (size_bytes, exit_code) is an integer.
        // A fractional, non-finite, or out-of-qint64-range number is therefore malformed,
        // and the old 'f',0 formatting silently ROUNDED it -- injecting a different number
        // than the tool reported into the next phase's command. Fail closed instead.
        const double number = value.toDouble();
        if (!std::isfinite(number) || number != std::trunc(number) || number < -kTwoPow63 ||
            number >= kTwoPow63) {
            return {};
        }
        return QString::number(static_cast<qint64>(number));
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

QString workflowPhaseResultValue(const AiWorkflowPhaseContext& context, const QString& key) {
    if (!key.startsWith(QStringLiteral("result_"))) {
        return {};
    }
    const QString tail = key.mid(7);
    // LONGEST field name FIRST, and kept that way: `result_<phase>_<field>` is ambiguous
    // whenever one field name is a suffix of another (`path` inside `stdout_path`), and the
    // first matching suffix wins. Longest-first makes the winner the most specific field, so
    // ${result_backup_stdout_path} resolves phase "backup" field "stdout_path" -- never phase
    // "backup_stdout" field "path". A candidate whose phase id is unknown resolves to empty
    // (fail closed) rather than being re-interpreted as a shorter field of a different phase.
    const QStringList known_fields{
        QStringLiteral("error_message"),
        QStringLiteral("stdout_path"),
        QStringLiteral("stderr_path"),
        QStringLiteral("source_url"),
        QStringLiteral("size_bytes"),
        QStringLiteral("exit_code"),
        QStringLiteral("success"),
        QStringLiteral("sha256"),
        QStringLiteral("path"),
    };
    for (const auto& field : known_fields) {
        const QString suffix = QStringLiteral("_%1").arg(field);
        if (!tail.endsWith(suffix)) {
            continue;
        }
        const QString phase_id = tail.left(tail.size() - suffix.size());
        const QJsonObject phase_result = context.phase_results.value(phase_id).toObject();
        if (phase_result.isEmpty()) {
            return {};
        }
        const QString direct = scalarJsonValueToString(phase_result.value(field));
        if (!direct.isEmpty()) {
            return direct;
        }
        return scalarJsonValueToString(
            phase_result.value(QStringLiteral("tool_result")).toObject().value(field));
    }
    return {};
}

QString workflowPlaceholderValue(const AiWorkflowPhaseContext& context,
                                 const QString& key,
                                 WorkflowPlaceholderMode mode) {
    QString value;
    if (key == QLatin1String("user_message")) {
        value = context.user_message.trimmed();
    } else if (key == QLatin1String("workflow_id")) {
        value = context.workflow_id.trimmed();
    } else if (key == QLatin1String("run_id")) {
        value = context.run_id.trimmed();
    } else if (key.startsWith(QStringLiteral("result_"))) {
        value = workflowPhaseResultValue(context, key);
    } else {
        value = workflowInputValue(context, key);
    }
    if (mode == WorkflowPlaceholderMode::PowerShellSingleQuoted) {
        // A PowerShell single-quoted literal treats every character literally except the
        // single quote, which is escaped by doubling; newlines and every metacharacter are
        // inert. This escaping is therefore COMPLETE if and only if the template places the
        // placeholder inside a single-quoted literal (the B1-05 remediation audited every
        // bundled template + splat to guarantee this). It intentionally does not wrap the
        // value: wrapping here would double-quote the templates that already wrap. Templates
        // MUST keep ${...} inside '...' in this mode.
        value.replace(QLatin1Char('\''), QStringLiteral("''"));
    }
    return value;
}

// Lexical state of the minimal PowerShell scanner below.
enum class PowerShellScanState {
    Code,          // command position: a quote OPENS a literal/string
    SingleQuoted,  // '...' literal: every character is literal, '' is an escaped quote
    DoubleQuoted,  // "..." string: ` escapes the next character, "" is an escaped quote
};

struct PowerShellScanResult {
    // False when the scanned prefix used a construct this scanner deliberately does not
    // lex. The caller then REJECTS the template instead of guessing the placeholder's
    // quoting context (the naive quote-toggle it replaced guessed, and guessed wrong).
    bool m_provable{false};
    bool m_inside_single_quoted{false};
    QString m_blocking_construct;
};

[[nodiscard]] QChar characterAt(const QString& text, qsizetype index) {
    return index < text.size() ? text.at(index) : QChar();
}

// One character of PowerShell in command position. Returns the number of characters
// consumed, or 0 when the construct is outside the accepted grammar.
qsizetype scanCodeCharacter(const QString& text,
                            qsizetype index,
                            PowerShellScanState* state,
                            QString* blocking_construct) {
    const QChar ch = text.at(index);
    const QChar next = characterAt(text, index + 1);
    if (ch == QLatin1Char('`')) {
        return kTwoCharSequenceLength;  // escapes the next character, so that character is never a
                                        // delimiter
    }
    if (ch == QLatin1Char('#') || (ch == QLatin1Char('<') && next == QLatin1Char('#'))) {
        *blocking_construct = QStringLiteral("a comment");
        return 0;
    }
    if (ch == QLatin1Char('@') && (next == QLatin1Char('\'') || next == QLatin1Char('"'))) {
        *blocking_construct = QStringLiteral("a here-string");
        return 0;
    }
    if (ch == QLatin1Char('\'')) {
        *state = PowerShellScanState::SingleQuoted;
    } else if (ch == QLatin1Char('"')) {
        *state = PowerShellScanState::DoubleQuoted;
    }
    return 1;
}

// One character inside a '...' literal: nothing is special except the quote, and a
// doubled quote is an escaped quote that keeps the literal open.
qsizetype scanSingleQuotedCharacter(const QString& text,
                                    qsizetype index,
                                    PowerShellScanState* state) {
    if (text.at(index) != QLatin1Char('\'')) {
        return 1;
    }
    if (characterAt(text, index + 1) == QLatin1Char('\'')) {
        return kTwoCharSequenceLength;
    }
    *state = PowerShellScanState::Code;
    return 1;
}

// One character inside a "..." string. A $( ) subexpression re-enters command position,
// which this scanner does not track, so it is refused rather than mis-lexed.
qsizetype scanDoubleQuotedCharacter(const QString& text,
                                    qsizetype index,
                                    PowerShellScanState* state,
                                    QString* blocking_construct) {
    const QChar ch = text.at(index);
    const QChar next = characterAt(text, index + 1);
    if (ch == QLatin1Char('`')) {
        return kTwoCharSequenceLength;
    }
    if (ch == QLatin1Char('$') && next == QLatin1Char('(')) {
        *blocking_construct = QStringLiteral("a $( ) subexpression inside a double-quoted string");
        return 0;
    }
    if (ch != QLatin1Char('"')) {
        return 1;
    }
    if (next == QLatin1Char('"')) {
        return kTwoCharSequenceLength;
    }
    *state = PowerShellScanState::Code;
    return 1;
}

// Lexes text[0, offset) and reports which quoting context @p offset lands in.
//
// This is a real (if small) PowerShell lexer for the constructs the workflow grammar
// ACCEPTS -- single-quoted literals, double-quoted strings, and backtick escapes -- and a
// hard refusal for everything else it would have to guess at (comments, here-strings,
// nested subexpressions inside a double-quoted string). Narrowing the accepted grammar is
// what makes the answer provable: a bigger regex could not tell an apostrophe inside
// "it's" from a literal delimiter, which is exactly how the previous quote-toggle scan was
// bypassed by a template such as $x="'"; ${user_message}.
PowerShellScanResult scanPowerShellPrefix(const QString& text, qsizetype offset) {
    PowerShellScanResult result;
    PowerShellScanState state = PowerShellScanState::Code;
    const qsizetype limit = std::min(offset, text.size());
    qsizetype index = 0;
    while (index < limit) {
        qsizetype consumed = 0;
        switch (state) {
        case PowerShellScanState::Code:
            consumed = scanCodeCharacter(text, index, &state, &result.m_blocking_construct);
            break;
        case PowerShellScanState::SingleQuoted:
            consumed = scanSingleQuotedCharacter(text, index, &state);
            break;
        case PowerShellScanState::DoubleQuoted:
            consumed = scanDoubleQuotedCharacter(text, index, &state, &result.m_blocking_construct);
            break;
        }
        if (consumed == 0) {
            return result;  // provable stays false; blocking_construct names the refusal
        }
        index += consumed;
    }
    result.m_provable = true;
    result.m_inside_single_quoted = state == PowerShellScanState::SingleQuoted;
    return result;
}

void setSingleQuoteError(QString* error, const QString& placeholder, const QString& construct) {
    if (error == nullptr) {
        return;
    }
    if (construct.isEmpty()) {
        *error = QStringLiteral(
                     "PowerShell workflow command places placeholder '%1' outside a "
                     "single-quoted literal, so its value would be injected unescaped")
                     .arg(placeholder);
        return;
    }
    *error = QStringLiteral(
                 "PowerShell workflow command uses %1 before placeholder '%2', so the "
                 "placeholder's quoting context cannot be proven")
                 .arg(construct, placeholder);
}

// The SAME placeholder grammar the substitutor recognizes, so PowerShell's own unbraced
// $vars (and ${...} spellings the substitutor ignores, e.g. containing ':') are not flagged.
// One definition keeps the validators and the substitutor from drifting apart.
const QRegularExpression& workflowPlaceholderPattern() {
    static const QRegularExpression kPattern(QStringLiteral(R"(\$\{([A-Za-z0-9_]+)\})"));
    return kPattern;
}

// A malformed member makes the WHOLE array unresolvable. Dropping the non-string or blank
// members and rendering the survivors turned "act on these five items" into "act on three"
// with no error on any channel -- a silently incomplete enumeration driving real work. The
// caller gets @p fallback (absent), never a partial list.
QString workflowArrayInputValue(const QJsonArray& array, const QString& fallback) {
    QStringList parts;
    for (const auto& item : array) {
        if (!item.isString()) {
            return fallback;
        }
        const QString text = item.toString().trimmed();
        if (text.isEmpty()) {
            return fallback;
        }
        parts << text;
    }
    return parts.isEmpty() ? fallback : parts.join(QStringLiteral(", "));
}

}  // namespace

bool powerShellCommandTemplateIsSingleQuoteSafe(const QString& command, QString* error) {
    auto matches = workflowPlaceholderPattern().globalMatch(command);
    while (matches.hasNext()) {
        const auto match = matches.next();
        const PowerShellScanResult scan = scanPowerShellPrefix(command, match.capturedStart());
        if (!scan.m_provable || !scan.m_inside_single_quoted) {
            setSingleQuoteError(error, match.captured(), scan.m_blocking_construct);
            return false;
        }
    }
    return true;
}

bool cmdCommandTemplateIsPlaceholderFree(const QString& command, QString* error) {
    // cmd.exe offers NO literal-quoting construct: inside "..." the shell still expands
    // %VAR% (and !VAR! under delayed expansion), a bare " ends the quoted run, and '^'
    // escapes only outside quotes. There is therefore no substitution context in which an
    // arbitrary value can be proven inert, so a run_cmd command template that embeds a
    // placeholder is refused outright rather than escaped-and-hoped. The PowerShell path
    // keeps its placeholders because a '...' literal IS a provable inert context.
    const auto match = workflowPlaceholderPattern().match(command);
    if (!match.hasMatch()) {
        return true;
    }
    if (error != nullptr) {
        *error = QStringLiteral(
                     "cmd.exe workflow command embeds placeholder '%1'; cmd.exe has no literal "
                     "quoting that can make an arbitrary value inert, so run_cmd command "
                     "templates must not use placeholders")
                     .arg(match.captured());
    }
    return false;
}

QString workflowInputValue(const AiWorkflowPhaseContext& context,
                           const QString& key,
                           const QString& fallback) {
    const auto value = context.input_values.value(key);
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        return text.isEmpty() ? fallback : text;
    }
    if (value.isArray()) {
        return workflowArrayInputValue(value.toArray(), fallback);
    }
    if (value.isDouble() || value.isBool()) {
        // A required numeric/bool input passes the clarifier's presence check, but without this
        // branch it rendered to the (empty) fallback here -- silently injecting an empty value
        // into the phase while clarification was skipped. scalarJsonValueToString renders an
        // integer or bool and itself fails closed (empty) on a fractional/non-finite/out-of-range
        // number, which then falls through to the fallback rather than a silently rounded value.
        const QString text = scalarJsonValueToString(value);
        return text.isEmpty() ? fallback : text;
    }
    return fallback;
}

QString substituteWorkflowPlaceholders(const QString& text,
                                       const AiWorkflowPhaseContext& context,
                                       WorkflowPlaceholderMode mode) {
    QString result;
    result.reserve(text.size());
    qsizetype cursor = 0;
    auto matches = workflowPlaceholderPattern().globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(cursor, match.capturedStart() - cursor);
        result += workflowPlaceholderValue(context, match.captured(1), mode);
        cursor = match.capturedEnd();
    }
    result += text.mid(cursor);
    return result;
}

QJsonObject substituteWorkflowPlaceholdersInObject(const QJsonObject& object,
                                                   const AiWorkflowPhaseContext& context,
                                                   WorkflowPlaceholderMode mode) {
    QJsonObject substituted;
    for (auto it = object.begin(); it != object.end(); ++it) {
        substituted.insert(it.key(),
                           substituteWorkflowPlaceholdersInValue(it.value(), context, mode));
    }
    return substituted;
}

QJsonValue substituteWorkflowPlaceholdersInValue(const QJsonValue& value,
                                                 const AiWorkflowPhaseContext& context,
                                                 WorkflowPlaceholderMode mode) {
    if (value.isString()) {
        return substituteWorkflowPlaceholders(value.toString(), context, mode);
    }
    if (value.isArray()) {
        QJsonArray array;
        const auto values = value.toArray();
        for (const auto& item : values) {
            array.append(substituteWorkflowPlaceholdersInValue(item, context, mode));
        }
        return array;
    }
    if (value.isObject()) {
        return substituteWorkflowPlaceholdersInObject(value.toObject(), context, mode);
    }
    return value;
}

}  // namespace sak::ai
