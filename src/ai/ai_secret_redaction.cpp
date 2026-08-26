// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_secret_redaction.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>

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

// Generic "password=", "passwd=", "secret=", "token=", "api[_-]?key=" values, INCLUDING the
// quoted-JSON form "password":"..." -- the optional "? after the key name lets the closing quote
// of a JSON key sit between the name and the ':' separator.
// Three value forms, because the single-quoted one was missed entirely and the double-quoted one
// broke on a space:
//   "..."  any characters including spaces -- password="my secret" used to fail on the space and
//          leak the whole assignment
//   '...'  the value class excluded the quote itself, so password='mysecret' matched NOTHING and
//          was printed verbatim
//   bare   unquoted, still length-bounded so ordinary prose ("token: ok") is not swallowed
// A quoted value is redacted at ANY length: quoting it is what makes the intent explicit, and a
// four-character floor would leak a short password.
// The catalogue of names that mark a value as a secret, written ONCE. It is consumed in two
// places -- the assignment regex below and the JSON key test in redactSecretsInJson -- and two
// copies of this list would drift, which is how a name ends up recognised in a log line and
// missed in a tool result.
constexpr auto kSecretNamePattern = R"((password|passwd|secret|token|api[_\-]?key))";

void redactAssignmentSecrets(QString& result) {
    static const QRegularExpression kAssignmentSecret(
        QStringLiteral(R"RX((?i)\b)RX") + QLatin1String(kSecretNamePattern) +
        QStringLiteral(R"RX("?\s*[:=]\s*(?:"[^"]*"|'[^']*'|[^\s"';,]{4,})"?)RX"));
    QRegularExpressionMatchIterator it = kAssignmentSecret.globalMatch(result);
    QString rewritten;
    rewritten.reserve(result.size());
    int last_end = 0;
    while (it.hasNext()) {
        const auto match = it.next();
        rewritten.append(result.mid(last_end, match.capturedStart(0) - last_end));
        rewritten.append(match.captured(1));
        rewritten.append(QStringLiteral("=[redacted]"));
        last_end = static_cast<int>(match.capturedEnd(0));
    }
    rewritten.append(result.mid(last_end));
    if (!rewritten.isEmpty() || last_end > 0) {
        result = rewritten;
    }
}

}  // namespace

QString redactSecrets(const QString& text) {
    QString result = text;
    redactVendorPrefixedKeys(result);
    redactBearerTokens(result);
    redactAssignmentSecrets(result);
    return result;
}

namespace {

// Does this JSON key NAME a secret? Anchored so it matches the whole key: a field called
// "token" or "api-key" is a secret, while "token_count" or "secretary" is not, and redacting
// those would destroy useful records without protecting anything.
bool jsonKeyNamesASecret(const QString& key) {
    static const QRegularExpression kSecretKey(
        QStringLiteral("^") + QLatin1String(kSecretNamePattern) + QStringLiteral("$"),
        QRegularExpression::CaseInsensitiveOption);
    return kSecretKey.match(key).hasMatch();
}

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
        if (jsonKeyNamesASecret(it.key())) {
            // The KEY already says this is a secret, so the value goes wholesale -- no length
            // floor and no pattern matching. A field named "password" holding "ab" is still a
            // password, and a non-string value there (a number, an object) is redacted too rather
            // than passed through because it did not look like text.
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


}  // namespace sak::ai
