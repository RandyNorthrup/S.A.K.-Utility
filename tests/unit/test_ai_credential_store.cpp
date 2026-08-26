// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ai_credential_store.cpp
/// @brief Unit tests for CredentialStore::redactSecrets (R5-LEDGER).
///
/// WHY THIS FILE EXISTS
/// redactSecrets is what keeps API keys, bearer tokens and passwords out of the AI transcript,
/// the log and the persisted conversation -- and it had NO tests of its own. It was only ever
/// exercised incidentally, through callers that assert on something else, which is exactly how
/// two leaks survived in it: a bearer token whose alphabet the pattern did not cover, and two
/// assignment spellings the pattern could not match at all.
///
/// The rule these tests encode: a PARTIAL redaction is worse than none, because it reads as
/// though the secret was handled. Every case below therefore asserts the secret's tail is gone,
/// not merely that the string changed.

#include "sak/ai/ai_credential_store.h"

#include <QtTest/QtTest>

class AiCredentialStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void redactsBearerTokensAcrossTheWholeAlphabet();
    void redactsAssignmentsInEveryQuotingStyle();
    void leavesOrdinaryTextAlone();
};

void AiCredentialStoreTests::redactsBearerTokensAcrossTheWholeAlphabet() {
    // base64url (A-Za-z0-9-_) was covered. STANDARD base64 was not: '+' and '/' and the '='
    // padding all fell outside the class, so the match stopped at the first one, the prefix was
    // replaced, and the remainder of the secret was printed.
    const QString standard_base64 = QStringLiteral("ab+cd/efGHIJ0123+/xyz==");
    const QString redacted = sak::ai::CredentialStore::redactSecrets(
        QStringLiteral("Authorization: Bearer %1").arg(standard_base64));
    QVERIFY2(!redacted.contains(standard_base64), qPrintable(redacted));
    // The TAIL specifically: a partial redaction would have left everything after the '+'.
    QVERIFY2(!redacted.contains(QStringLiteral("xyz==")), qPrintable(redacted));
    QVERIFY2(!redacted.contains(QStringLiteral("efGHIJ")), qPrintable(redacted));
    QVERIFY2(redacted.contains(QStringLiteral("[redacted]")), qPrintable(redacted));
    // The scheme itself is kept: the reader still needs to see that a bearer token was present.
    QVERIFY2(redacted.contains(QStringLiteral("Bearer")), qPrintable(redacted));

    // A JWT (base64url with '.' separators) and a '~'-bearing opaque token.
    for (const QString& token : {QStringLiteral("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NSJ9.abcDEF"),
                                 QStringLiteral("tok~with~tildes~1234567890")}) {
        const QString out =
            sak::ai::CredentialStore::redactSecrets(QStringLiteral("Bearer %1").arg(token));
        QVERIFY2(!out.contains(token), qPrintable(out));
    }
}

void AiCredentialStoreTests::redactsAssignmentsInEveryQuotingStyle() {
    // Each of these leaked before: the value character class excluded the single quote, so a
    // single-quoted secret matched nothing at all, and it excluded whitespace, so a double-quoted
    // value containing a space failed too.
    struct Case {
        QString text;
        QString secret;
    };
    const QList<Case> cases = {
        {QStringLiteral("password='mysecretvalue'"), QStringLiteral("mysecretvalue")},
        {QStringLiteral("password=\"my secret value\""), QStringLiteral("my secret value")},
        {QStringLiteral("api_key='abc123def456'"), QStringLiteral("abc123def456")},
        {QStringLiteral("api-key: 'abc123def456'"), QStringLiteral("abc123def456")},
        {QStringLiteral("secret: \"two words\""), QStringLiteral("two words")},
        {QStringLiteral("token=\"spaced out token\""), QStringLiteral("spaced out token")},
        // The forms that already worked must keep working.
        {QStringLiteral("password=plaintextvalue"), QStringLiteral("plaintextvalue")},
        {QStringLiteral("\"token\":\"jsonstylevalue\""), QStringLiteral("jsonstylevalue")},
    };
    for (const Case& c : cases) {
        const QString out = sak::ai::CredentialStore::redactSecrets(c.text);
        QVERIFY2(!out.contains(c.secret), qPrintable(c.text + QStringLiteral(" -> ") + out));
        QVERIFY2(out.contains(QStringLiteral("[redacted]")),
                 qPrintable(c.text + QStringLiteral(" -> ") + out));
    }

    // A quoted value is redacted at ANY length -- quoting it is what makes the intent explicit,
    // and a four-character minimum would leak a short password.
    const QString shortQuoted =
        sak::ai::CredentialStore::redactSecrets(QStringLiteral("password='ab'"));
    QVERIFY2(!shortQuoted.contains(QStringLiteral("'ab'")), qPrintable(shortQuoted));
}

void AiCredentialStoreTests::leavesOrdinaryTextAlone() {
    // A redactor that fires on prose trains the reader to ignore it, and destroys the log's
    // usefulness. These must pass through untouched.
    for (const QString& benign : {QStringLiteral("The bearer of this note is authorized."),
                                  QStringLiteral("Check the token count in the response."),
                                  QStringLiteral("password reset instructions were emailed"),
                                  QStringLiteral("Run Get-FileHash to verify the download.")}) {
        QCOMPARE(sak::ai::CredentialStore::redactSecrets(benign), benign);
    }
}

QTEST_GUILESS_MAIN(AiCredentialStoreTests)
#include "test_ai_credential_store.moc"
