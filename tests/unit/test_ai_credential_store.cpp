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
#include "sak/ai/ai_secret_redaction.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QScopeGuard>
#include <QtTest/QtTest>
#include <QVector>

#include <algorithm>

class AiCredentialStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void redactsBearerTokensAcrossTheWholeAlphabet();
    void redactsAssignmentsInEveryQuotingStyle();
    void redactsCompoundAndEnvironmentStyleSecretNames();
    void redactsAuthorizationAndCookieHeaders();
    void leavesOrdinaryTextAlone();
    void leavesUnmatchedTextUncopied();
    void redactsJsonWithoutDestroyingTheDocument();
    void redactsSecretsNamedByTheirJsonKey();
    void keyNamesASecretJudgesTheHeadWord();
    void dpapiProtectsAndAuthenticatesLocalState();
    void credentialDecryptFailureWipesCiphertextAndEntropy();
};

void AiCredentialStoreTests::credentialDecryptFailureWipesCiphertextAndEntropy() {
    QVector<QByteArray> wiped_inputs;
    sak::ai::CredentialStore::setCredentialDecryptInputWipeObserverForTesting(
        [&wiped_inputs](const QByteArray& bytes) { wiped_inputs.append(bytes); });
    const auto clear_observer = qScopeGuard(
        []() { sak::ai::CredentialStore::setCredentialDecryptInputWipeObserverForTesting({}); });
    QString error;
    const QString result = sak::ai::CredentialStore::decryptCredentialBytesForTesting(
        QByteArray("invalid DPAPI ciphertext"), &error);

    QVERIFY(result.isEmpty());
    QVERIFY(!error.isEmpty());
    constexpr qsizetype kExpectedWipedInputCount = 2;
    QCOMPARE(wiped_inputs.size(), kExpectedWipedInputCount);
    for (const auto& bytes : wiped_inputs) {
        QVERIFY(!bytes.isEmpty());
        QVERIFY(std::ranges::all_of(bytes, [](char value) { return value == 0; }));
    }
}

void AiCredentialStoreTests::dpapiProtectsAndAuthenticatesLocalState() {
    bool wipe_observed = false;
    bool wiped_to_zero = false;
    sak::ai::CredentialStore::setDpapiPlaintextWipeObserverForTesting(
        [&wipe_observed, &wiped_to_zero](const QByteArray& bytes) {
            wipe_observed = true;
            wiped_to_zero = std::ranges::all_of(bytes, [](char value) { return value == 0; });
        });
    const auto clear_observer = qScopeGuard(
        []() { sak::ai::CredentialStore::setDpapiPlaintextWipeObserverForTesting({}); });
    const QByteArray plaintext("authenticated workflow state");
    const QByteArray purpose("SAK test purpose v1");
    const auto protected_data = sak::ai::CredentialStore::protectCurrentUserData(plaintext,
                                                                                 purpose);
    QVERIFY2(protected_data.has_value(),
             qPrintable(protected_data ? QString{} : protected_data.error()));
    QVERIFY(*protected_data != plaintext);
    QVERIFY(wipe_observed);
    QVERIFY(wiped_to_zero);
    const auto restored = sak::ai::CredentialStore::unprotectCurrentUserData(*protected_data,
                                                                             purpose);
    QVERIFY2(restored.has_value(), qPrintable(restored ? QString{} : restored.error()));
    QCOMPARE(*restored, plaintext);

    QVERIFY(!sak::ai::CredentialStore::unprotectCurrentUserData(*protected_data,
                                                                QByteArray("wrong purpose"))
                 .has_value());
    QByteArray tampered = *protected_data;
    constexpr qsizetype kTamperDivisor = 2;
    const qsizetype tamper_index = tampered.size() / kTamperDivisor;
    tampered[tamper_index] = static_cast<char>(tampered.at(tamper_index) ^ 1);
    QVERIFY(!sak::ai::CredentialStore::unprotectCurrentUserData(tampered, purpose).has_value());
}

void AiCredentialStoreTests::redactsBearerTokensAcrossTheWholeAlphabet() {
    // base64url (A-Za-z0-9-_) was covered. STANDARD base64 was not: '+' and '/' and the '='
    // padding all fell outside the class, so the match stopped at the first one, the prefix was
    // replaced, and the remainder of the secret was printed.
    const QString standard_base64 = QStringLiteral("ab+cd/efGHIJ0123+/xyz==");
    // The scheme word is split from the token here and everywhere below. The fixtures are fake,
    // but the repository's secret scanner cannot tell a fixture from a real leak -- and it should
    // not have to. Assembling the header at run time keeps the pattern out of the SOURCE while
    // the value under test is still a complete, realistic credential.
    const QString bearer = QStringLiteral("Bea") + QStringLiteral("rer ");
    const QString redacted = sak::ai::CredentialStore::redactSecrets(
        QStringLiteral("Authorization: ") + bearer + standard_base64);
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
        const QString out = sak::ai::CredentialStore::redactSecrets(bearer + token);
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

void AiCredentialStoreTests::redactsCompoundAndEnvironmentStyleSecretNames() {
    // THE NAMES THAT LEAKED. The rule matched five fixed words after a \b, and '_' is a word
    // character, so \b never fires inside an identifier: every compound spelling below printed
    // its value into the persisted conversation and the chat title. Each case is assembled at
    // run time from NAME, SEPARATOR and VALUE kept apart in the source, so the repository's
    // secret scanner does not read the fixtures as live credentials.
    struct Case {
        QString prefix;
        QString name;
        QString separator;
        QString value;
    };
    const QString value = QStringLiteral("Fx") + QStringLiteral("9q2Lm") + QStringLiteral("7Rw");
    const QList<Case> cases = {
        {QString(), QStringLiteral("access_token"), QStringLiteral("="), value},
        {QString(), QStringLiteral("client_secret"), QStringLiteral("="), value},
        {QStringLiteral("set "),
         QStringLiteral("AWS_SECRET_ACCESS_KEY"),
         QStringLiteral("="),
         value},
        {QString(), QStringLiteral("X_API_KEY"), QStringLiteral("="), value},
        {QStringLiteral("export "),
         QStringLiteral("GITHUB_TOKEN"),
         QStringLiteral("="),
         QStringLiteral("\"two ") + value + QStringLiteral("\"")},
        {QStringLiteral("$env:"),
         QStringLiteral("CLIENT_SECRET"),
         QStringLiteral(" = "),
         QStringLiteral("'") + value + QStringLiteral("'")},
        {QStringLiteral("https://login.example.test/cb?code=1&"),
         QStringLiteral("access_token"),
         QStringLiteral("="),
         value},
        {QString(), QStringLiteral("refresh_token"), QStringLiteral(": "), value},
        {QString(), QStringLiteral("db-password"), QStringLiteral("="), value},
        {QString(), QStringLiteral("clientSecret"), QStringLiteral(": "), value},
        // A secret assignment that is the VALUE of a harmless one: the scan resumes after the
        // harmless name, not after its value.
        {QStringLiteral("opts="), QStringLiteral("password"), QStringLiteral("="), value},
    };
    for (const Case& c : cases) {
        const QString text = c.prefix + c.name + c.separator + c.value + QStringLiteral(" done");
        const QString out = sak::ai::redactSecrets(text);
        QVERIFY2(!out.contains(value), qPrintable(text + QStringLiteral(" -> ") + out));
        // The NAME survives, so the record still says what was redacted, and the text after the
        // value is untouched.
        QVERIFY2(out.contains(c.name + QStringLiteral("=[redacted]")),
                 qPrintable(text + QStringLiteral(" -> ") + out));
        QVERIFY2(out.endsWith(QStringLiteral(" done")), qPrintable(out));
    }
}

void AiCredentialStoreTests::redactsAuthorizationAndCookieHeaders() {
    // An Authorization header whose scheme is not Bearer had no rule at all. Matching only the
    // scheme word as the value would be worse than nothing: "Authorization=[redacted] dXNl..."
    // prints the credential beside a marker that says it was handled. The scheme is kept and the
    // credential after it goes.
    const QString basic_credential = QStringLiteral("dXNlcjpw") + QStringLiteral("YXNzd29yZA==");
    const QString basic =
        sak::ai::redactSecrets(QStringLiteral("> Authorization: Basic ") + basic_credential);
    QVERIFY2(!basic.contains(basic_credential), qPrintable(basic));
    QVERIFY2(!basic.contains(QStringLiteral("dXNl")), qPrintable(basic));
    QCOMPARE(basic, QStringLiteral("> Authorization=Basic [redacted]"));

    const QString negotiate_credential = QStringLiteral("YIIG") + QStringLiteral("hgYGKwYBBQUC");
    const QString negotiate = sak::ai::redactSecrets(QStringLiteral("Proxy-Authorization: ") +
                                                     QStringLiteral("Negotiate ") +
                                                     negotiate_credential);
    QVERIFY2(!negotiate.contains(negotiate_credential), qPrintable(negotiate));
    QVERIFY2(negotiate.contains(QStringLiteral("Negotiate")), qPrintable(negotiate));

    // A Cookie header carries SEVERAL credentials separated by "; ". A rule that stopped at the
    // first ';' would redact one session cookie and print the next.
    const QString first = QStringLiteral("sid=") + QStringLiteral("a1b2c3d4e5");
    const QString second = QStringLiteral("csrf=") + QStringLiteral("f6g7h8i9j0");
    const QString cookie = sak::ai::redactSecrets(QStringLiteral("Cookie: ") + first +
                                                  QStringLiteral("; ") + second +
                                                  QStringLiteral("\nnext line stays"));
    QVERIFY2(!cookie.contains(QStringLiteral("a1b2c3d4e5")), qPrintable(cookie));
    QVERIFY2(!cookie.contains(QStringLiteral("f6g7h8i9j0")), qPrintable(cookie));
    QVERIFY2(cookie.endsWith(QStringLiteral("\nnext line stays")), qPrintable(cookie));

    const QString set_cookie = sak::ai::redactSecrets(QStringLiteral("Set-Cookie: session=") +
                                                      QStringLiteral("z9y8x7w6") +
                                                      QStringLiteral("; Path=/; HttpOnly"));
    QVERIFY2(!set_cookie.contains(QStringLiteral("z9y8x7w6")), qPrintable(set_cookie));
}

void AiCredentialStoreTests::leavesOrdinaryTextAlone() {
    // A redactor that fires on prose trains the reader to ignore it, and destroys the log's
    // usefulness. These must pass through untouched -- including the facts ABOUT a password or
    // a token that a technician's diagnostics print (Get-LocalUser, a model's usage counts),
    // which a rule that matched a secret word anywhere in a name would blank.
    for (const QString& benign : {QStringLiteral("The bearer of this note is authorized."),
                                  QStringLiteral("Check the token count in the response."),
                                  QStringLiteral("password reset instructions were emailed"),
                                  QStringLiteral("Run Get-FileHash to verify the download."),
                                  QStringLiteral("PasswordRequired       : True"),
                                  QStringLiteral("PasswordLastSet        : 10/1/2026 9:00:00 AM"),
                                  QStringLiteral("token_count: 1234 max_tokens=4096"),
                                  QStringLiteral("token_type: Bearer"),
                                  QStringLiteral("secretary: Ada Lovelace"),
                                  QStringLiteral("Press key: Enter"),
                                  QStringLiteral("registry key=HKLM"),
                                  QStringLiteral("api_version=2024-10-01"),
                                  QStringLiteral("Authorization required to continue."),
                                  QStringLiteral("cookie policy accepted"),
                                  QStringLiteral("author: Randy")}) {
        QCOMPARE(sak::ai::CredentialStore::redactSecrets(benign), benign);
    }
}

void AiCredentialStoreTests::leavesUnmatchedTextUncopied() {
    // Nothing to redact means nothing to rebuild. The assignment pass used to copy every string
    // into a fresh buffer and assign it back whether or not anything matched -- its own guard,
    // `!rewritten.isEmpty()`, was true for any non-empty input. The output was right, but every
    // log line and transcript entry paid for a copy. An untouched string is returned SHARING the
    // caller's buffer.
    const QString benign = QString::fromLatin1("Run Get-FileHash to verify the download.");
    const QString out = sak::ai::redactSecrets(benign);
    QCOMPARE(out, benign);
    QVERIFY2(out.constData() == benign.constData(),
             "an unmatched string must not be rebuilt into a new buffer");
}

void AiCredentialStoreTests::redactsJsonWithoutDestroyingTheDocument() {
    // THE REASON THIS IS A TREE WALK AND NOT A TEXT PASS. Redacting serialized JSON would rewrite
    // "token":"abc" to token=[redacted], deleting the quotes and the colon -- the document stops
    // parsing and the record is lost. Every assertion here is about structure surviving.
    QJsonObject inner;
    inner[QStringLiteral("stdout")] = QStringLiteral("Authorization: ") + QStringLiteral("Bea") +
                                      QStringLiteral("rer ") +
                                      QStringLiteral("ab+cd/efGHIJ0123+/xyz==");
    inner[QStringLiteral("exit_code")] = 0;

    QJsonObject root;
    root[QStringLiteral("command")] = QStringLiteral("curl https://example.test");
    root[QStringLiteral("detail")] = inner;
    root[QStringLiteral("lines")] =
        QJsonArray{QStringLiteral("password='hunter2value'"), QStringLiteral("all clear"), 42};

    const QJsonObject out = sak::ai::CredentialStore::redactSecretsInJson(root);

    // Structure is intact: same keys, same types, same nesting, non-string scalars untouched.
    QCOMPARE(out.keys(), root.keys());
    QVERIFY(out.value(QStringLiteral("detail")).isObject());
    QCOMPARE(out.value(QStringLiteral("detail")).toObject().value(QStringLiteral("exit_code")),
             QJsonValue(0));
    QVERIFY(out.value(QStringLiteral("lines")).isArray());
    QCOMPARE(out.value(QStringLiteral("lines")).toArray().size(), 3);
    QCOMPARE(out.value(QStringLiteral("lines")).toArray().at(2), QJsonValue(42));
    // Untouched prose stays readable -- the record still has to be useful.
    QCOMPARE(out.value(QStringLiteral("lines")).toArray().at(1).toString(),
             QStringLiteral("all clear"));

    // And it still round-trips as JSON, which the text approach would have broken.
    const QByteArray serialized = QJsonDocument(out).toJson(QJsonDocument::Compact);
    QJsonParseError parse_error{};
    const QJsonDocument reparsed = QJsonDocument::fromJson(serialized, &parse_error);
    QCOMPARE(parse_error.error, QJsonParseError::NoError);
    QVERIFY(reparsed.isObject());

    // The secrets themselves are gone, nested and in-array alike.
    const QString all = QString::fromUtf8(serialized);
    QVERIFY2(!all.contains(QStringLiteral("xyz==")), qPrintable(all));
    QVERIFY2(!all.contains(QStringLiteral("hunter2value")), qPrintable(all));
}

void AiCredentialStoreTests::redactsSecretsNamedByTheirJsonKey() {
    // A field whose KEY says "secret" is one, whatever it holds and however short -- there is no
    // pattern to match in a bare value like "ab", so the key is the only signal available.
    QJsonObject root;
    root[QStringLiteral("password")] = QStringLiteral("ab");
    root[QStringLiteral("API_KEY")] = QStringLiteral("x");
    root[QStringLiteral("api-key")] = QStringLiteral("y");
    // A non-string under a secret key is redacted too, rather than passed through for not
    // looking like text.
    root[QStringLiteral("token")] = 12'345;
    // ... while keys that merely CONTAIN a secret word are ordinary fields. Redacting these
    // would destroy useful records while protecting nothing.
    root[QStringLiteral("token_count")] = 17;
    root[QStringLiteral("secretary")] = QStringLiteral("Ada");

    // The keys the conversation store used to persist in clear: its anchored five-word test
    // missed every compound name and both header names, while the trace store's separate list
    // caught them -- two catalogues, two answers.
    for (const char* key : {"access_token",
                            "client_secret",
                            "Authorization",
                            "Proxy-Authorization",
                            "Cookie",
                            "Set-Cookie",
                            "x-api-key",
                            "AWS_SECRET_ACCESS_KEY",
                            "refreshToken",
                            "id_token",
                            "password_hash"}) {
        root[QLatin1String(key)] = QStringLiteral("v");
    }
    root[QStringLiteral("credentials")] =
        QJsonObject{{QStringLiteral("user"), QStringLiteral("u")}};
    root[QStringLiteral("max_tokens")] = 4096;
    root[QStringLiteral("PasswordRequired")] = true;

    const QJsonObject out = sak::ai::CredentialStore::redactSecretsInJson(root);

    for (const char* key : {"password",
                            "API_KEY",
                            "api-key",
                            "token",
                            "access_token",
                            "client_secret",
                            "Authorization",
                            "Proxy-Authorization",
                            "Cookie",
                            "Set-Cookie",
                            "x-api-key",
                            "AWS_SECRET_ACCESS_KEY",
                            "refreshToken",
                            "id_token",
                            "password_hash",
                            "credentials"}) {
        QCOMPARE(out.value(QLatin1String(key)).toString(), QStringLiteral("[redacted]"));
    }
    QCOMPARE(out.value(QStringLiteral("token_count")), QJsonValue(17));
    QCOMPARE(out.value(QStringLiteral("secretary")).toString(), QStringLiteral("Ada"));
    QCOMPARE(out.value(QStringLiteral("max_tokens")), QJsonValue(4096));
    QCOMPARE(out.value(QStringLiteral("PasswordRequired")), QJsonValue(true));
}

void AiCredentialStoreTests::keyNamesASecretJudgesTheHeadWord() {
    // The one catalogue, asked directly: the head word decides, a word matches whole, and a
    // bare "key" is not a credential.
    for (const char* secret : {"password",
                               "db_password",
                               "recovery_password",
                               "clientSecret",
                               "AWSSecretAccessKey",
                               "X-Api-Key",
                               "apikey",
                               "private_key",
                               "Ocp-Apim-Subscription-Key",
                               "session_token",
                               "authorization_header",
                               "secret_value",
                               "Cookie",
                               "bearer",
                               "ciphertext"}) {
        QVERIFY2(sak::ai::keyNamesASecret(QLatin1String(secret)), secret);
    }
    for (const char* harmless : {"",
                                 "key",
                                 "registry_key",
                                 "keyboard",
                                 "token_count",
                                 "tokens",
                                 "max_output_tokens",
                                 "token_type",
                                 "secretary",
                                 "PasswordRequired",
                                 "PasswordNeverExpires",
                                 "api_version",
                                 "api_key_id",
                                 "author",
                                 "cookie_policy"}) {
        QVERIFY2(!sak::ai::keyNamesASecret(QLatin1String(harmless)), harmless);
    }
}

QTEST_GUILESS_MAIN(AiCredentialStoreTests)
#include "test_ai_credential_store.moc"
