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

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QProcess>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest/QtTest>
#include <QVector>

#include <algorithm>
#include <string>
#include <vector>

#include <windows.h>

#include <aclapi.h>
#include <dpapi.h>

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
    void keyFileRoundTripsAndAbsentFileIsNotAnError();
    void emptyObjectKeyFileIsReportedNotHidden();
    void nonObjectKeyFileGetsItsOwnError();
    void keyFileVersionIsChecked();
    void decryptedKeyIsValidatedLikeASavedOne();
    void savedKeyFileIsRestrictedToItsUser();
    void keyFileSavesThroughAJunctionedFolder();
};

namespace {

// A realistic key, assembled at run time so the source holds no matchable credential.
QString fixtureApiKey() {
    return QStringLiteral("sk-") + QStringLiteral("test") + QStringLiteral("Q9x7Lm2Rw4Zp8Kd3");
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate) &&
           file.write(bytes) == bytes.size();
}

QJsonObject readKeyFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

// A DPAPI blob of @p plaintext under the store's own entropy, i.e. what a same-user writer that
// is NOT this store could plant. The entropy string is the file-format constant
// ai_credential_store.cpp writes; a test fixture has to name it to forge the format.
QByteArray dpapiBlobFor(const QString& plaintext) {
    QByteArray bytes = plaintext.toUtf8();
    QByteArray entropy("SAK Utility/OpenAI API Key/v1");
    DATA_BLOB input{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    DATA_BLOB salt{static_cast<DWORD>(entropy.size()), reinterpret_cast<BYTE*>(entropy.data())};
    DATA_BLOB output{};
    if (CryptProtectData(
            &input, nullptr, &salt, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &output) == 0) {
        return {};
    }
    QByteArray blob(reinterpret_cast<const char*>(output.pbData), static_cast<int>(output.cbData));
    LocalFree(output.pbData);
    return blob;
}

std::vector<BYTE> currentUserSid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<BYTE> token_user(needed);
    const bool read = needed != 0 &&
                      GetTokenInformation(token, TokenUser, token_user.data(), needed, &needed) !=
                          0;
    CloseHandle(token);
    if (!read) {
        return {};
    }
    const PSID user = reinterpret_cast<const TOKEN_USER*>(token_user.data())->User.Sid;
    std::vector<BYTE> sid(GetLengthSid(user));
    CopySid(static_cast<DWORD>(sid.size()), sid.data(), user);
    return sid;
}

std::vector<BYTE> wellKnownSid(WELL_KNOWN_SID_TYPE type) {
    std::vector<BYTE> sid(SECURITY_MAX_SID_SIZE);
    DWORD size = static_cast<DWORD>(sid.size());
    if (CreateWellKnownSid(type, nullptr, sid.data(), &size) == 0) {
        return {};
    }
    sid.resize(size);
    return sid;
}

struct AllowedSids {
    std::vector<BYTE> user;
    std::vector<BYTE> system;
    std::vector<BYTE> admins;
};

// Empty when @p ace is a direct (not inherited) allow entry for the user, SYSTEM or
// Administrators; *@p is_user says whether it is the user's.
QString aceProblem(void* ace, const AllowedSids& allowed, bool* is_user) {
    const auto* header = static_cast<const ACE_HEADER*>(ace);
    if (header->AceType != ACCESS_ALLOWED_ACE_TYPE || (header->AceFlags & INHERITED_ACE) != 0) {
        return QStringLiteral("an entry is not a direct allow entry");
    }
    PSID sid = &static_cast<ACCESS_ALLOWED_ACE*>(ace)->SidStart;
    *is_user = EqualSid(sid, const_cast<BYTE*>(allowed.user.data())) != 0;
    if (!*is_user && EqualSid(sid, const_cast<BYTE*>(allowed.system.data())) == 0 &&
        EqualSid(sid, const_cast<BYTE*>(allowed.admins.data())) == 0) {
        return QStringLiteral("an account other than the user, SYSTEM or Administrators");
    }
    return {};
}

// Empty when @p path carries a PROTECTED DACL (nothing inherited) that grants only this user,
// SYSTEM and Administrators, and grants this user; otherwise what is wrong with it.
QString daclRestrictionProblem(const QString& path) {
    const std::wstring native = QDir::toNativeSeparators(path).toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    PACL dacl = nullptr;
    if (GetNamedSecurityInfoW(native.c_str(),
                              SE_FILE_OBJECT,
                              DACL_SECURITY_INFORMATION,
                              nullptr,
                              nullptr,
                              &dacl,
                              nullptr,
                              &descriptor) != ERROR_SUCCESS) {
        return QStringLiteral("the DACL could not be read");
    }
    const auto free_descriptor = qScopeGuard([descriptor]() { LocalFree(descriptor); });
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (GetSecurityDescriptorControl(descriptor, &control, &revision) == 0 ||
        (control & SE_DACL_PROTECTED) == 0 || dacl == nullptr) {
        return QStringLiteral("the key file's DACL is not protected (it inherits)");
    }
    const AllowedSids allowed{.user = currentUserSid(),
                              .system = wellKnownSid(WinLocalSystemSid),
                              .admins = wellKnownSid(WinBuiltinAdministratorsSid)};
    bool user_allowed = false;
    for (DWORD index = 0; index < dacl->AceCount; ++index) {
        void* ace = nullptr;
        if (GetAce(dacl, index, &ace) == 0) {
            return QStringLiteral("an entry could not be read");
        }
        bool is_user = false;
        const QString problem = aceProblem(ace, allowed, &is_user);
        if (!problem.isEmpty()) {
            return problem;
        }
        user_allowed = user_allowed || is_user;
    }
    return user_allowed ? QString() : QStringLiteral("the user is not granted access");
}

}  // namespace

void AiCredentialStoreTests::keyFileRoundTripsAndAbsentFileIsNotAnError() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("credentials/key.json"));

    // No file is the "no key configured" state: no key AND no error.
    QString error = QStringLiteral("stale");
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.isEmpty(), qPrintable(error));

    QVERIFY2(sak::ai::CredentialStore::saveApiKeyToPathForTesting(path, fixtureApiKey(), &error),
             qPrintable(error));
    QCOMPARE(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error), fixtureApiKey());
    QVERIFY2(error.isEmpty(), qPrintable(error));
    // The written format is what the reader now checks.
    QCOMPARE(readKeyFile(path).value(QStringLiteral("version")), QJsonValue(1));
}

void AiCredentialStoreTests::emptyObjectKeyFileIsReportedNotHidden() {
    // CRED-3. An absent file and a file holding `{}` used to produce the same empty object, so
    // a key file truncated or overwritten to `{}` read as "no key configured" with no error --
    // the technician saw a missing key, not a damaged one.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("key.json"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("{}")));

    QString error;
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("empty JSON object")), qPrintable(error));
}

void AiCredentialStoreTests::nonObjectKeyFileGetsItsOwnError() {
    // CRED-6. `[]` is valid JSON, so the parser reports NO error; sharing its branch printed
    // "Encrypted API key file is invalid: no error occurred".
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("key.json"));
    QVERIFY(writeBytes(path, QByteArrayLiteral("[]")));

    QString error;
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("not an object")), qPrintable(error));
    QVERIFY2(!error.contains(QStringLiteral("no error occurred")), qPrintable(error));
}

void AiCredentialStoreTests::keyFileVersionIsChecked() {
    // CRED-7. `version` was written and never read, so a future format reusing the provider name
    // would have been decrypted as this one. The file below is a VALID saved key with only its
    // version changed, so the refusal can only come from the version.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("key.json"));
    QString error;
    QVERIFY2(sak::ai::CredentialStore::saveApiKeyToPathForTesting(path, fixtureApiKey(), &error),
             qPrintable(error));
    const QJsonObject saved = readKeyFile(path);

    QJsonObject future = saved;
    future[QStringLiteral("version")] = 2;
    QVERIFY(writeBytes(path, QJsonDocument(future).toJson()));
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("version 2 is unsupported")), qPrintable(error));

    QJsonObject unversioned = saved;
    unversioned.remove(QStringLiteral("version"));
    QVERIFY(writeBytes(path, QJsonDocument(unversioned).toJson()));
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("no version")), qPrintable(error));

    QJsonObject text_version = saved;
    text_version[QStringLiteral("version")] = QStringLiteral("1");
    QVERIFY(writeBytes(path, QJsonDocument(text_version).toJson()));
    QVERIFY(sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error).isEmpty());
    QVERIFY2(error.contains(QStringLiteral("not a number")), qPrintable(error));
}

void AiCredentialStoreTests::decryptedKeyIsValidatedLikeASavedOne() {
    // CRED-4. The save side refuses control characters and implausible lengths; the load side
    // returned any non-empty plaintext, and the key goes straight into the Authorization header.
    // These blobs are what a same-user writer other than this store could plant.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("key.json"));
    struct Case {
        QString plaintext;
        QString reason;
    };
    const QList<Case> cases = {
        {fixtureApiKey() + QStringLiteral("\r\nX-Injected: 1"),
         QStringLiteral("control characters")},
        {QString(8193, QLatin1Char('k')), QStringLiteral("implausibly long")},
        {QStringLiteral("   "), QStringLiteral("empty")},
    };
    for (const Case& c : cases) {
        const QByteArray blob = dpapiBlobFor(c.plaintext);
        QVERIFY(!blob.isEmpty());
        const QJsonObject root{
            {QStringLiteral("version"), 1},
            {QStringLiteral("provider"), QStringLiteral("dpapi-current-user-v1")},
            {QStringLiteral("ciphertext"), QString::fromLatin1(blob.toBase64())}};
        QVERIFY(writeBytes(path, QJsonDocument(root).toJson()));
        QString error;
        const QString key = sak::ai::CredentialStore::loadApiKeyFromPathForTesting(path, &error);
        QVERIFY2(key.isEmpty(), qPrintable(c.reason));
        QVERIFY2(error.startsWith(QStringLiteral("Stored API key is invalid")), qPrintable(error));
        QVERIFY2(error.contains(c.reason), qPrintable(error));
    }
}

void AiCredentialStoreTests::savedKeyFileIsRestrictedToItsUser() {
    // CRED-5. The "owner-only" step was QFile::setPermissions(ReadOwner | WriteOwner), which on
    // Windows only toggles the read-only attribute: the file kept the folder's INHERITED DACL,
    // so every account that could read the data folder could read the key file. The DACL is now
    // protected (nothing inherited) and names only this user, SYSTEM and Administrators.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("key.json"));
    QString error;
    QVERIFY2(sak::ai::CredentialStore::saveApiKeyToPathForTesting(path, fixtureApiKey(), &error),
             qPrintable(error));
    const QString problem = daclRestrictionProblem(path);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

void AiCredentialStoreTests::keyFileSavesThroughAJunctionedFolder() {
    // A portable copy run from a mapped network drive, a SUBST drive or a junctioned folder
    // reaches its data folder through a path that resolves to another spelling. PermissionManager's
    // apply refuses such a path for an elevated change; for the key file the store has just
    // written, that refusal would only turn into a failed save and a deleted key file. The save
    // must succeed, and the real file must carry the restricted DACL.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString real_folder = dir.filePath(QStringLiteral("real"));
    QVERIFY(QDir().mkpath(real_folder));
    const QString junction = dir.filePath(QStringLiteral("linked"));
    const QString cmd =
        QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32/cmd.exe"));
    QCOMPARE(QProcess::execute(cmd,
                               {QStringLiteral("/d"),
                                QStringLiteral("/c"),
                                QStringLiteral("mklink"),
                                QStringLiteral("/J"),
                                QDir::toNativeSeparators(junction),
                                QDir::toNativeSeparators(real_folder)}),
             0);
    const auto remove_junction = qScopeGuard([junction]() { QDir().rmdir(junction); });

    QString error;
    QVERIFY2(sak::ai::CredentialStore::saveApiKeyToPathForTesting(
                 QDir(junction).filePath(QStringLiteral("key.json")), fixtureApiKey(), &error),
             qPrintable(error));
    const QString real_file = QDir(real_folder).filePath(QStringLiteral("key.json"));
    QVERIFY(QFileInfo::exists(real_file));
    const QString problem = daclRestrictionProblem(real_file);
    QVERIFY2(problem.isEmpty(), qPrintable(problem));
}

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
