// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_secret_redaction.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSet>
#include <QStringList>
#include <QStringView>

#include <algorithm>
#include <utility>

namespace sak::ai {

namespace {

// Vendor keys that announce themselves with a fixed prefix. Each is its own pattern rather than
// one alternation so the replacement can NAME which vendor's credential was found -- a log
// reader who sees [redacted-aws-key] knows which secret to rotate.
void redactVendorPrefixedKeys(QString& result) {
    static const QRegularExpression kOpenAiKey(QStringLiteral(R"(\bsk-[A-Za-z0-9_\-]{12,}\b)"),
                                               QRegularExpression::UseUnicodePropertiesOption);
    result.replace(kOpenAiKey, QStringLiteral("sk-...[redacted]"));

    static const QRegularExpression kContext7Key(QStringLiteral(R"(\bctx7sk-[A-Za-z0-9\-]{12,}\b)"),
                                                 QRegularExpression::CaseInsensitiveOption);
    result.replace(kContext7Key, QStringLiteral("[redacted-context7-key]"));

    // GitHub personal/oauth/server/app/refresh tokens (ghp_, gho_, ghu_, ghs_, ghr_)
    static const QRegularExpression kGitHubToken(
        QStringLiteral(R"(\bgh[pousr]_[A-Za-z0-9]{20,}\b)"));
    result.replace(kGitHubToken, QStringLiteral("[redacted-github-token]"));

    // GitHub fine-grained PATs (github_pat_...) and Slack app-level tokens (xapp-...), which the
    // prefix-specific patterns above do not cover.
    static const QRegularExpression kGitHubFineGrained(
        QStringLiteral(R"(\bgithub_pat_[A-Za-z0-9_]{20,}\b)"));
    result.replace(kGitHubFineGrained, QStringLiteral("[redacted-github-token]"));
    static const QRegularExpression kSlackAppToken(
        QStringLiteral(R"(\bxapp-[A-Za-z0-9\-]{12,}\b)"));
    result.replace(kSlackAppToken, QStringLiteral("[redacted-slack-token]"));

    // AWS access key IDs (AKIA/ASIA + 16 alnum) and Google API keys (AIza...)
    static const QRegularExpression kAwsAccessKey(
        QStringLiteral(R"(\b(?:AKIA|ASIA)[A-Z0-9]{16}\b)"));
    result.replace(kAwsAccessKey, QStringLiteral("[redacted-aws-key]"));
    static const QRegularExpression kGoogleApiKey(QStringLiteral(R"(\bAIza[A-Za-z0-9_\-]{35}\b)"));
    result.replace(kGoogleApiKey, QStringLiteral("[redacted-google-key]"));

    // Slack tokens (xox[bopas]-...) and Stripe keys (sk_live_..., rk_live_...)
    static const QRegularExpression kSlackToken(
        QStringLiteral(R"(\bxox[boapsr]-[A-Za-z0-9\-]{12,}\b)"));
    result.replace(kSlackToken, QStringLiteral("[redacted-slack-token]"));
    static const QRegularExpression kStripeKey(
        QStringLiteral(R"(\b(?:sk|rk)_(?:live|test)_[A-Za-z0-9]{16,}\b)"));
    result.replace(kStripeKey, QStringLiteral("[redacted-stripe-key]"));
}

// The token class must cover STANDARD base64 ('+', '/' and the '=' padding) and the '~' that
// opaque provider tokens use, not just base64url. It did not, so a standard-base64 bearer token
// matched only up to its first '+' or '/': the prefix was replaced and THE REST WAS PRINTED.
// A partial redaction is worse than none -- it reads as though the secret was handled.
// The scheme word is kept so the reader still sees that a bearer token was present.
void redactBearerTokens(QString& result) {
    static const QRegularExpression kBearer(QStringLiteral(
                                                R"((Bearer\s+)[A-Za-z0-9_\-\.~+/=]{12,})"),
                                            QRegularExpression::CaseInsensitiveOption);
    result.replace(kBearer, QStringLiteral("\\1[redacted]"));
}

// A Cookie / Set-Cookie HEADER line carries a list of `name=value` pairs separated by "; ", and
// every one of them may be a session credential. The assignment rule below stops a bare value at
// the first ';', so on its own it would redact the first cookie and print the rest -- a partial
// redaction, which reads as handled. The header form (name, then ':') is therefore redacted to
// the end of its line first. A quoted JSON value ("Cookie":"...") is left to the assignment rule,
// which already takes the whole quoted string.
//
// This regex and the assignment one below live at namespace scope and spell every literal double
// quote as \x22 (the same character to the regex engine). A quote inside a raw string makes
// lizard's C++ tokenizer lose its place, after which it measures none of the functions that
// follow -- the complexity gate then passes code it never saw.
const QRegularExpression kCookieHeaderRegex(
    QStringLiteral(R"((?<![A-Za-z0-9_\-])((?:set-)?cookie)[ \t]*:[ \t]*[^\s\x22'][^\r\n]*)"),
    QRegularExpression::CaseInsensitiveOption);

void redactCookieHeaders(QString& result) {
    result.replace(kCookieHeaderRegex, QStringLiteral("\\1: [redacted]"));
}

// `name=value` and `name: value` assignments whose NAME keyNamesASecret() accepts, INCLUDING the
// quoted-JSON form "password":"..." -- the optional "? after the name lets the closing quote of a
// JSON key sit between the name and the ':' separator.
//
// THE NAME IS CAPTURED WHOLE AND JUDGED BY THE SHARED CATALOGUE. The previous rule matched five
// fixed words after a \b, and '_' is a word character, so \b never fires inside an identifier:
// `access_token=`, `client_secret=`, `AWS_SECRET_ACCESS_KEY=` and `X_API_KEY=` all printed their
// values. Capturing the identifier and asking keyNamesASecret() covers every compound spelling the
// JSON key test covers, with one list instead of two.
//
// Three value forms, because the single-quoted one was missed entirely and the double-quoted one
// broke on a space:
//   "..."  any characters including spaces -- password="my secret" used to fail on the space and
//          leak the whole assignment
//   '...'  the value class excluded the quote itself, so password='mysecret' matched NOTHING and
//          was printed verbatim
//   bare   unquoted, still length-bounded so ordinary prose ("token: ok") is not swallowed
// A quoted value is redacted at ANY length: quoting it is what makes the intent explicit, and a
// four-character floor would leak a short password.
//
// A bare value may open with an HTTP authorization scheme word (`Authorization: Basic dXNl...`).
// The scheme is kept and the credential after it redacted: matching only the scheme word as the
// value would print the credential beside a marker that claims it was handled.
constexpr int kAssignmentNameGroup = 1;
constexpr int kAssignmentSeparatorGroup = 2;
constexpr int kAssignmentSchemeGroup = 3;
constexpr int kAssignmentBareValueGroup = 4;
const QRegularExpression kSecretAssignmentRegex(
    QStringLiteral(R"((?<![A-Za-z0-9_])([A-Za-z][A-Za-z0-9_.\-]*)(\x22?\s*[:=]\s*))"
                   R"((?:\x22[^\x22]*\x22|'[^']*'|)"
                   R"((?:(basic|bearer|digest|negotiate|ntlm)\s+)?([^\s\x22';,]{4,}))\x22?)"),
    QRegularExpression::CaseInsensitiveOption);

// A bare true or false is a FACT about a secret, never one: Get-LocalUser prints
// `UserMayChangePassword : True` and Active Directory `CannotChangePassword : False` for every
// account, and their head word is "password". A boolean carries no credential. A QUOTED "true"
// is still redacted, like any quoted value.
bool assignsABoolean(const QRegularExpressionMatch& match) {
    const QString bare = match.captured(kAssignmentBareValueGroup);
    return match.captured(kAssignmentSchemeGroup).isEmpty() &&
           (bare.compare(QLatin1String("true"), Qt::CaseInsensitive) == 0 ||
            bare.compare(QLatin1String("false"), Qt::CaseInsensitive) == 0);
}

// A quoted JSON key with a quoted value -- "password" : "..." -- keeps its own punctuation and gets
// a quoted marker, so a JSON fragment in plain text still parses after redaction: a browser page
// showing a config, or a tool payload quoted in a message. Normalising it to name=[redacted] left
// the key's opening quote unmatched. Every other form is normalised to name=[redacted].
bool isQuotedJsonMember(const QString& text, const QRegularExpressionMatch& match) {
    const qsizetype name_start = match.capturedStart(kAssignmentNameGroup);
    const qsizetype value_start = match.capturedEnd(kAssignmentSeparatorGroup);
    const QString separator = match.captured(kAssignmentSeparatorGroup);
    return name_start > 0 && text.at(name_start - 1) == QLatin1Char('"') &&
           separator.startsWith(QLatin1Char('"')) && separator.contains(QLatin1Char(':')) &&
           value_start < text.size() && text.at(value_start) == QLatin1Char('"');
}

void appendRedactedAssignment(QString& rewritten,
                              const QString& text,
                              const QRegularExpressionMatch& match) {
    rewritten.append(match.captured(kAssignmentNameGroup));
    if (isQuotedJsonMember(text, match)) {
        rewritten.append(match.captured(kAssignmentSeparatorGroup));
        rewritten.append(QStringLiteral("\"[redacted]\""));
        return;
    }
    rewritten.append(QLatin1Char('='));
    const QString scheme = match.captured(kAssignmentSchemeGroup);
    if (!scheme.isEmpty()) {
        rewritten.append(scheme);
        rewritten.append(QLatin1Char(' '));
    }
    rewritten.append(QStringLiteral("[redacted]"));
}

void redactAssignmentSecrets(QString& result) {
    QString rewritten;
    qsizetype copied_to = 0;
    qsizetype offset = 0;
    bool redacted_any = false;
    while (offset < result.size()) {
        const QRegularExpressionMatch match = kSecretAssignmentRegex.match(result, offset);
        if (!match.hasMatch()) {
            break;
        }
        if (!keyNamesASecret(match.captured(kAssignmentNameGroup)) || assignsABoolean(match)) {
            // Resume right after the NAME, not after the value: in `x=password=hunter2` the
            // secret assignment is the value of a harmless one.
            offset = match.capturedEnd(kAssignmentNameGroup);
            continue;
        }
        rewritten.append(QStringView(result).mid(copied_to, match.capturedStart(0) - copied_to));
        appendRedactedAssignment(rewritten, result, match);
        copied_to = match.capturedEnd(0);
        offset = copied_to;
        redacted_any = true;
    }
    // Nothing matched: leave the string as it is instead of rebuilding an identical copy.
    if (!redacted_any) {
        return;
    }
    rewritten.append(QStringView(result).mid(copied_to));
    result = std::move(rewritten);
}

// Head nouns that name a secret. A compound name's HEAD is its last word -- "access token",
// "client secret", "recovery password", "proxy authorization" -- while the words before it only
// qualify it. Judging the head is what keeps Get-LocalUser's `PasswordRequired : True` and
// `PasswordLastSet : ...`, or `token_count` and `max_tokens`, readable: they are facts ABOUT a
// password or tokens, not one.
//
// `pwd`, `accesskey` and `clientsecret` came from the win32 MCP server's own list when it was
// folded into this one (NEW-10): a connection string's `Pwd=`, and the run-together spellings its
// `access[_-]?key` / `client[_-]?secret` pattern matched, which split into no words here.
bool isSecretNoun(const QString& word) {
    static const QSet<QString> kSecretNouns{
        QStringLiteral("password"),
        QStringLiteral("passwd"),
        QStringLiteral("pwd"),
        QStringLiteral("passphrase"),
        QStringLiteral("secret"),
        QStringLiteral("secrets"),
        QStringLiteral("token"),
        QStringLiteral("authorization"),
        QStringLiteral("cookie"),
        QStringLiteral("cookies"),
        QStringLiteral("credential"),
        QStringLiteral("credentials"),
        QStringLiteral("ciphertext"),
        QStringLiteral("bearer"),
        QStringLiteral("apikey"),
        QStringLiteral("accesskey"),
        QStringLiteral("clientsecret"),
    };
    return kSecretNouns.contains(word);
}

// `pass` is short for password in a command switch (`--pass`), a script variable (`$pass`) and a
// dotted or hyphenated setting (`smtp.pass`, `db-pass`), all of which the win32 MCP server's
// `\bpass\b` caught. As the head of a compound it is ambiguous -- `recovery_pass` is the
// orchestrator's iteration flag, nginx's `proxy_pass` holds a URL -- so it counts only as the
// name's whole final token, where a token runs over letters, digits and '_': exactly the server's
// rule. `recovery_pass`, `bypass` and `passes` are not secrets.
bool endsInPassAbbreviation(const QString& key) {
    qsizetype start = key.size();
    while (start > 0 &&
           (key.at(start - 1).isLetterOrNumber() || key.at(start - 1) == QLatin1Char('_'))) {
        --start;
    }
    return QStringView(key).mid(start).compare(QLatin1String("pass"), Qt::CaseInsensitive) == 0;
}

// A trailing word that names a CONTAINER of the noun before it: `password_hash`,
// `authorization_header`, `secret_value`, `password_confirmation` still hold the secret.
bool isSecretContainerWord(const QString& word) {
    static const QSet<QString> kContainerWords{
        QStringLiteral("value"),
        QStringLiteral("header"),
        QStringLiteral("hash"),
        QStringLiteral("confirm"),
        QStringLiteral("confirmation"),
    };
    return kContainerWords.contains(word);
}

// Words that turn a trailing "key" into a secret one: api_key, secret_access_key, private_key,
// subscription_key, access_key (the win32 MCP server's list) and an Azure Storage connection
// string's AccountKey. A bare "key" is a keystroke or a registry key, not a credential.
bool isSecretKeyQualifier(const QString& word) {
    static const QSet<QString> kKeyQualifiers{
        QStringLiteral("api"),
        QStringLiteral("secret"),
        QStringLiteral("private"),
        QStringLiteral("subscription"),
        QStringLiteral("access"),
        QStringLiteral("account"),
    };
    return kKeyQualifiers.contains(word);
}

// True between "aB" (lower or digit, then upper) and inside "AWSSecret" (the last capital of an
// acronym that a lowercase letter follows): the camelCase word boundaries.
bool startsCamelCaseWord(const QString& key, qsizetype index) {
    const QChar previous = key.at(index - 1);
    if (!previous.isLetterOrNumber()) {
        return false;
    }
    if (previous.isLower() || previous.isDigit()) {
        return true;
    }
    return index + 1 < key.size() && key.at(index + 1).isLower();
}

// Lower-cased words of @p key, split at every non-alphanumeric character and at camelCase
// boundaries: "AWS_SECRET_ACCESS_KEY", "x-api-key" and "awsSecretAccessKey" all become words.
QStringList keyWords(const QString& key) {
    QStringList words;
    QString current;
    for (qsizetype index = 0; index < key.size(); ++index) {
        const QChar ch = key.at(index);
        const bool boundary = !ch.isLetterOrNumber() || (ch.isUpper() && !current.isEmpty() &&
                                                         startsCamelCaseWord(key, index));
        if (boundary && !current.isEmpty()) {
            words.append(current.toLower());
            current.clear();
        }
        if (ch.isLetterOrNumber()) {
            current.append(ch);
        }
    }
    if (!current.isEmpty()) {
        words.append(current.toLower());
    }
    return words;
}

}  // namespace

bool keyNamesASecret(const QString& key) {
    if (endsInPassAbbreviation(key)) {
        return true;
    }
    QStringList words = keyWords(key);
    if (words.size() > 1 && isSecretContainerWord(words.last())) {
        words.removeLast();
    }
    if (words.isEmpty()) {
        return false;
    }
    const QString& head = words.last();
    if (isSecretNoun(head)) {
        return true;
    }
    return head == QLatin1String("key") &&
           std::any_of(words.cbegin(), words.cend() - 1, isSecretKeyQualifier);
}

QString redactSecrets(const QString& text) {
    QString result = text;
    redactVendorPrefixedKeys(result);
    redactBearerTokens(result);
    redactCookieHeaders(result);
    redactAssignmentSecrets(result);
    return result;
}

namespace {

QJsonValue redactJsonValue(const QJsonValue& value);

QJsonArray redactJsonArray(const QJsonArray& array) {
    QJsonArray out;
    for (const QJsonValue& element : array) {
        out.append(redactJsonValue(element));
    }
    return out;
}

QJsonObject redactJsonObject(const QJsonObject& object) {
    QJsonObject out;
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (keyNamesASecret(it.key()) && !it.value().isBool()) {
            // The KEY already says this is a secret, so the value goes wholesale -- no length
            // floor and no pattern matching. A field named "password" holding "ab" is still a
            // password, and a non-string value there (a number, an object) is redacted too rather
            // than passed through because it did not look like text. A boolean is the exception:
            // `"UserMayChangePassword": true` is a fact about a password and carries no secret.
            out.insert(it.key(), QStringLiteral("[redacted]"));
            continue;
        }
        out.insert(it.key(), redactJsonValue(it.value()));
    }
    return out;
}

QJsonValue redactJsonValue(const QJsonValue& value) {
    if (value.isString()) {
        return redactSecrets(value.toString());
    }
    if (value.isObject()) {
        return redactJsonObject(value.toObject());
    }
    if (value.isArray()) {
        return redactJsonArray(value.toArray());
    }
    return value;
}

}  // namespace

QJsonObject redactSecretsInJson(const QJsonObject& object) {
    return redactJsonObject(object);
}

QJsonArray redactSecretsInJson(const QJsonArray& array) {
    return redactJsonArray(array);
}

}  // namespace sak::ai
