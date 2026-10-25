// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_security.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <sddl.h>

using sak::win32mcp::browserBridgePipeName;
using sak::win32mcp::buildBridgePipeSecurity;
using sak::win32mcp::currentUserSidString;
using sak::win32mcp::generateBridgeToken;
using sak::win32mcp::readRendezvousRecord;
using sak::win32mcp::RendezvousRecord;
using sak::win32mcp::writeRendezvousRecord;

namespace {

// Round-trip the built descriptor back to SDDL so the test can assert on the DACL/SACL.
QString descriptorToSddl(PSECURITY_DESCRIPTOR descriptor) {
    LPWSTR sddl = nullptr;
    const SECURITY_INFORMATION info = DACL_SECURITY_INFORMATION | LABEL_SECURITY_INFORMATION;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            descriptor, SDDL_REVISION_1, info, &sddl, nullptr) == FALSE) {
        return {};
    }
    const QString result = QString::fromWCharArray(sddl);
    LocalFree(sddl);
    return result;
}

// The trustee text SDDL writes for this SID. Well-known accounts render as an alias rather than
// as their S-1-... string -- the built-in Administrator (RID 500, which a hosted CI runner's
// account is) renders as "LA" -- so a DACL assertion has to compare in the same form.
QString sddlTrusteeFor(const QString& sid) {
    PSECURITY_DESCRIPTOR owner_only = nullptr;
    const QString owner_sddl = QStringLiteral("O:") + sid;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(owner_sddl.utf16()), SDDL_REVISION_1, &owner_only, nullptr) ==
        FALSE) {
        return {};
    }
    LPWSTR rendered = nullptr;
    QString result;
    if (ConvertSecurityDescriptorToStringSecurityDescriptorW(
            owner_only, SDDL_REVISION_1, OWNER_SECURITY_INFORMATION, &rendered, nullptr) != FALSE) {
        result = QString::fromWCharArray(rendered);
        LocalFree(rendered);
    }
    LocalFree(owner_only);
    return result.startsWith(QStringLiteral("O:")) ? result.mid(2) : QString();
}

// This machine's built-in Administrator account SID: the local account domain SID plus RID 500.
QString builtInAdministratorSid() {
    wchar_t computer[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD computer_length = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(computer, &computer_length) == FALSE) {
        return {};
    }
    BYTE domain_sid[SECURITY_MAX_SID_SIZE] = {};
    DWORD domain_sid_size = sizeof(domain_sid);
    wchar_t domain[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD domain_length = MAX_COMPUTERNAME_LENGTH + 1;
    SID_NAME_USE use{};
    if (LookupAccountNameW(
            nullptr, computer, domain_sid, &domain_sid_size, domain, &domain_length, &use) ==
            FALSE ||
        use != SidTypeDomain) {
        return {};
    }
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(domain_sid, &text) == FALSE) {
        return {};
    }
    const QString result = QString::fromWCharArray(text) + QStringLiteral("-500");
    LocalFree(text);
    return result;
}

}  // namespace

class BrowserBridgeSecurityTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void currentUserSid_isAWellFormedUserSid();
    void pipeName_isScopedNoncedAndUnique();
    void token_is128BitHexAndUnique();
    void rendezvous_roundTripsAllFields();
    void rendezvous_readMissingFileFails();
    void rendezvous_invalidAppPidFailsClosed();
    void rendezvous_overCapFileFailsClosed();    // R5-G10-9
    void rendezvous_malformedJsonFailsClosed();  // R5-G10-9
    void security_daclIsCurrentUserOnlyWithMediumLabel();
    void security_userTrusteeMatchesSddlAliases();
};

// The DACL assertion above once searched the SDDL for the user's S-1-... string, which fails for
// the built-in Administrator (a hosted CI runner's account) because SDDL writes that SID as "LA".
// Pin the comparison form on every machine with this machine's RID-500 SID, SYSTEM, and an
// ordinary account SID that has no alias.
void BrowserBridgeSecurityTests::security_userTrusteeMatchesSddlAliases() {
    const QString administrator = builtInAdministratorSid();
    QVERIFY2(administrator.startsWith(QStringLiteral("S-1-5-21-")), qPrintable(administrator));
    QCOMPARE(sddlTrusteeFor(administrator), QStringLiteral("LA"));
    QCOMPARE(sddlTrusteeFor(QStringLiteral("S-1-5-18")), QStringLiteral("SY"));
    const QString ordinary = QStringLiteral("S-1-5-21-1111111111-2222222222-3333333333-1001");
    QCOMPARE(sddlTrusteeFor(ordinary), ordinary);

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const QString dacl = QStringLiteral("D:P(A;;GA;;;%1)(A;;GA;;;SY)").arg(administrator);
    QVERIFY(ConvertStringSecurityDescriptorToSecurityDescriptorW(
                reinterpret_cast<LPCWSTR>(dacl.utf16()), SDDL_REVISION_1, &descriptor, nullptr) !=
            FALSE);
    const QString rendered = descriptorToSddl(descriptor);
    LocalFree(descriptor);
    QVERIFY2(!rendered.contains(administrator), qPrintable(rendered));
    QVERIFY2(rendered.contains(QStringLiteral("(A;;GA;;;%1)").arg(sddlTrusteeFor(administrator))),
             qPrintable(rendered));
}

void BrowserBridgeSecurityTests::currentUserSid_isAWellFormedUserSid() {
    QString error;
    const QString sid = currentUserSidString(&error);
    QVERIFY2(!sid.isEmpty(), qPrintable(error));
    QVERIFY(sid.startsWith(QStringLiteral("S-1-")));
}

void BrowserBridgeSecurityTests::pipeName_isScopedNoncedAndUnique() {
    QString error;
    const QString sid = currentUserSidString(&error);
    const QString a = browserBridgePipeName(&error);
    const QString b = browserBridgePipeName(&error);
    QVERIFY2(!a.isEmpty(), qPrintable(error));
    QVERIFY(a.startsWith(QStringLiteral("\\\\.\\pipe\\SAK_BrowserBridge_")));
    QVERIFY(a.contains(sid));  // scoped to this user
    QVERIFY(a != b);           // nonce makes each name unique
}

void BrowserBridgeSecurityTests::token_is128BitHexAndUnique() {
    const QString a = generateBridgeToken();
    const QString b = generateBridgeToken();
    QCOMPARE(a.size(), 32);  // 128 bits as hex
    QVERIFY(a != b);
    for (const QChar c : a) {
        QVERIFY(c.isDigit() || (c >= QLatin1Char('a') && c <= QLatin1Char('f')));
    }
}

void BrowserBridgeSecurityTests::rendezvous_roundTripsAllFields() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("sub/browser_bridge.json"));  // nested: mkpath
    const RendezvousRecord in{QStringLiteral("\\\\.\\pipe\\SAK_BrowserBridge_x"),
                              QStringLiteral("deadbeefdeadbeefdeadbeefdeadbeef"),
                              1,
                              4242};
    QString error;
    QVERIFY2(writeRendezvousRecord(path, in, &error), qPrintable(error));

    RendezvousRecord out;
    QVERIFY2(readRendezvousRecord(path, &out, &error), qPrintable(error));
    QCOMPARE(out.pipe_name, in.pipe_name);
    QCOMPARE(out.token, in.token);
    QCOMPARE(out.protocol, in.protocol);
    QCOMPARE(out.app_pid, in.app_pid);
}

void BrowserBridgeSecurityTests::rendezvous_readMissingFileFails() {
    RendezvousRecord out;
    QString error;
    QVERIFY(!readRendezvousRecord(QStringLiteral("Z:/nope/does_not_exist.json"), &out, &error));
    // Pin the file-open guard specifically (the errorString() tail is OS/locale-variant).
    QVERIFY(error.startsWith(QStringLiteral("Cannot open Z:/nope/does_not_exist.json: ")));
}

void BrowserBridgeSecurityTests::rendezvous_invalidAppPidFailsClosed() {
    // F26: app_pid must be a positive integer within the exact-integer double range. A record
    // carrying an out-of-range app_pid must FAIL the read (casting such a double to qint64 is
    // UB) rather than yielding a garbage pid the relay would then try to bind against.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("bad.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    // 1e19 is beyond both int64 and the exact-integer double range.
    file.write(R"({"pipe_name":"\\\\.\\pipe\\x","token":"deadbeef","protocol":1,"app_pid":1e19})");
    file.close();

    RendezvousRecord out;
    QString error;
    QVERIFY(!readRendezvousRecord(path, &out, &error));
    QCOMPARE(error, QStringLiteral("Rendezvous record has an invalid app_pid."));
}

void BrowserBridgeSecurityTests::rendezvous_overCapFileFailsClosed() {
    // The on-disk rendezvous record is untrusted: a hostile same-user process can rewrite it.
    // readRendezvousRecord bounds the read at 64 KiB and refuses an over-cap file BEFORE the JSON
    // parse, so a multi-megabyte record cannot force an unbounded allocation. The existing tests
    // cover a missing file and an out-of-range app_pid, never an over-cap file.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("oversized.json"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    // 65 KiB > the 64 KiB cap. The size check fires before any parse, so the content need not be
    // valid JSON.
    file.write(QByteArray(65 * 1024, 'x'));
    file.close();

    RendezvousRecord out;
    QString error;
    QVERIFY(!readRendezvousRecord(path, &out, &error));
    QCOMPARE(error, QStringLiteral("Rendezvous record is too large."));
}

void BrowserBridgeSecurityTests::rendezvous_malformedJsonFailsClosed() {
    // A size-legal (<= 64 KiB) but malformed record body must fail closed at the parse guard,
    // BEFORE pipe_name/token/app_pid (which steer where the relay connects and what token it
    // presents) are trusted. Covers both a JSON parse error and a valid-but-non-object document.
    // The existing tests stop earlier: missing-file at open, over-cap at the size check (before the
    // parse), and invalid-app_pid on a well-formed object.
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    RendezvousRecord out;
    for (const QByteArray& body :
         {QByteArray("{\"pipe_name\":"),       // truncated write: parse error
          QByteArray("[1,2,3]"),               // valid JSON, but not object
          QByteArray("\"just-a-string\"")}) {  // valid JSON, but not object
        const QString path = dir.filePath(QStringLiteral("malformed.json"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(body);
        file.close();
        QString error;
        QVERIFY2(!readRendezvousRecord(path, &out, &error), body.constData());
        // The code-controlled prefix incl. the ": " separator is deterministic (the tail is Qt's
        // locale-variant QJsonParseError text); anchor at the start so dropping ": " is caught.
        QVERIFY2(error.startsWith(QStringLiteral("Malformed rendezvous record: ")),
                 qPrintable(error));
    }
}

void BrowserBridgeSecurityTests::security_daclIsCurrentUserOnlyWithMediumLabel() {
    SECURITY_ATTRIBUTES attributes{};
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    QString error;
    QVERIFY2(buildBridgePipeSecurity(&attributes, &descriptor, &error), qPrintable(error));
    QVERIFY(descriptor != nullptr);
    QCOMPARE(attributes.bInheritHandle, FALSE);
    // nLength is set to sizeof(SECURITY_ATTRIBUTES); dropping that assignment leaves the zero-init
    // value and survives the rest of the test (CreateNamedPipe does not validate it).
    QCOMPARE(attributes.nLength, static_cast<DWORD>(sizeof(SECURITY_ATTRIBUTES)));

    const QString sid = currentUserSidString(&error);
    const QString sddl = descriptorToSddl(descriptor);
    QVERIFY(!sddl.isEmpty());
    QVERIFY(sddl.contains(QStringLiteral("D:P")));  // DACL is protected
    // The fixed SYSTEM ACE (SY/GA are OS-invariant abbreviations); dropping it survives every
    // other check here, so pin its presence explicitly.
    QVERIFY(sddl.contains(QStringLiteral("(A;;GA;;;SY)")));  // SYSTEM full-access ACE present
    // The current user's full-access ACE, in the form SDDL writes that SID (see sddlTrusteeFor).
    const QString user_trustee = sddlTrusteeFor(sid);
    QVERIFY2(!user_trustee.isEmpty(), qPrintable(sid));
    QVERIFY2(sddl.contains(QStringLiteral("(A;;GA;;;%1)").arg(user_trustee)), qPrintable(sddl));
    QVERIFY(sddl.contains(QStringLiteral("NR")));  // NO_READ_UP mandatory-label bit
    QVERIFY(sddl.contains(QStringLiteral("NW")));  // NO_WRITE_UP mandatory-label bit
    // No BUILTIN\Users, Everyone, or Authenticated Users (well-knowns render as SDDL
    // abbreviations BU / WD / AU, not raw SIDs).
    QVERIFY(!sddl.contains(QStringLiteral(";BU)")));
    QVERIFY(!sddl.contains(QStringLiteral(";WD)")));
    QVERIFY(!sddl.contains(QStringLiteral(";AU)")));

    // The descriptor must be one the OS actually accepts on a first-instance,
    // remote-rejecting pipe (a convertible SDDL is not proof CreateNamedPipe accepts
    // it -- the Medium mandatory label in particular must be settable at our own IL).
    const std::wstring name = browserBridgePipeName(&error).toStdWString();
    HANDLE pipe =
        CreateNamedPipeW(name.c_str(),
                         PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                         PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS,
                         1,
                         4096,
                         4096,
                         0,
                         &attributes);
    QVERIFY2(pipe != INVALID_HANDLE_VALUE,
             qPrintable(QStringLiteral("CreateNamedPipe rejected the descriptor (err=%1)")
                            .arg(GetLastError())));
    CloseHandle(pipe);
    LocalFree(descriptor);
}

QTEST_MAIN(BrowserBridgeSecurityTests)
#include "test_browser_bridge_security.moc"
