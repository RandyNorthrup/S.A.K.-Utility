// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_offline_profile_source.cpp
/// @brief Unit tests for the offline profile rescue seams: pure decisions, a temp-tree
///        Users scan with a junction canary, and the recovery-key staging lifecycle.

#include "sak/app_paths.h"
#include "sak/offline_profile_source.h"

#include "../support/junction_fixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstring>
#include <string>

#include <windows.h>

using namespace sak;

namespace {

/// @brief A SAM Domains\Account "F" value laid out as Windows stores it: filler, then a
///        real SID structure (rev 1, authority 5, sub-authorities 21 + three machine
///        dwords) at offset 0x0C, then filler. The layout round-trips through a real
///        reg save/RegLoadKey in readOfflineHives_liveMachineHives; this fixture pins
///        the parser's shape rules.
QByteArray makeSamAccountValue(quint32 a, quint32 b, quint32 c, int total_size = 512) {
    QByteArray value(total_size, '\x7A');
    auto* p = reinterpret_cast<unsigned char*>(value.data());
    p[0x0C] = 1;  // revision
    p[0x0D] = 4;  // four sub-authorities: 21, a, b, c
    // The 6-byte big-endian authority occupies 0x0E..0x13: it must be zeroed apart
    // from the last byte, or the filler makes an authority of 0x7A7A7A7A7A05.
    for (int i = 0x0E; i <= 0x13; ++i) {
        p[i] = 0;
    }
    p[0x13] = 5;  // NT authority
    quint32 subs[4] = {21, a, b, c};
    memcpy(p + 0x14, subs, sizeof(subs));
    return value;
}

OfflineHiveFacts makeFacts(const QString& machine_sid, const QVector<OfflineHiveProfileRow>& rows) {
    OfflineHiveFacts facts;
    facts.machine_sid = machine_sid;
    facts.machine_sid_ok = !machine_sid.isEmpty();
    facts.profile_list = rows;
    facts.profile_list_ok = true;
    return facts;
}

OfflineProfileEntry findEntry(const QVector<OfflineProfileEntry>& entries, const QString& name) {
    for (const auto& entry : entries) {
        if (entry.folder_name.compare(name, Qt::CaseInsensitive) == 0) {
            return entry;
        }
    }
    return {};
}

}  // namespace

class TestOfflineProfileSource : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalizeVolumeRoot_trailingSlash();
    void isVolumeGuidPath_shape();
    void isLiveSystemRoot_sameVolumeSpelledDifferently();
    void isReservedProfileFolderName_builtins();
    void machineSidFromSamAccountValue_parses();
    void machineSidFromSamAccountValue_rejectsGarbage();
    void sidBelongsToMachine_prefixAndEmpty();
    void profileSkipReason_everyCase();
    void normalizeRecoveryKey_canonicalAndRejects();
    void bitLockerUnlockScript_carriesKeyPathNotKey();
    void bitLockerUnlockScript_rejectsBadInputs();
    void unlockEncryptedVolume_stagesKeyAndShredsIt();
    void unlockEncryptedVolume_runnerFailureStillShreds();
    void scanUsersDirectory_tempTreeDecisions();
    void scanUsersDirectory_junctionRefusedCanaryIntact();
    void scanUsersDirectory_fallbackModeIncludesWithWarning();
    void scanUsersDirectory_missingUsersIsGenuineEmpty();
    void discover_unelevatedRefuses();
    void discoverVolume_rejectsNonGuidAndLiveSystem();
    void readOfflineHives_liveMachineHives();
    void unmountRunDirectory_refusesForeignPath();
    void mountVolumeToRunDirectory_requiresGuidAndSessionTemp();
};

void TestOfflineProfileSource::normalizeVolumeRoot_trailingSlash() {
    QCOMPARE(OfflineProfileSource::normalizeVolumeRoot(QStringLiteral("E:")),
             QStringLiteral("E:\\"));
    QCOMPARE(OfflineProfileSource::normalizeVolumeRoot(QStringLiteral("E:/")),
             QStringLiteral("E:\\"));
    QCOMPARE(OfflineProfileSource::normalizeVolumeRoot(QStringLiteral("E:\\\\")),
             QStringLiteral("E:\\"));
}

void TestOfflineProfileSource::isVolumeGuidPath_shape() {
    const QString good = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    QVERIFY(OfflineProfileSource::isVolumeGuidPath(good));
    QVERIFY(!OfflineProfileSource::isVolumeGuidPath(good + QLatin1Char('\\')));  // trailing slash
    QVERIFY(!OfflineProfileSource::isVolumeGuidPath(QStringLiteral("E:\\")));
    QVERIFY(!OfflineProfileSource::isVolumeGuidPath(
        QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcde}")));   // short
    QVERIFY(!OfflineProfileSource::isVolumeGuidPath(
        QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdeg}")));  // non-hex
    QVERIFY(!OfflineProfileSource::isVolumeGuidPath(
        QStringLiteral("\\\\?\\Volume{1234567890abcdef0123456789abcdef}")));      // no dashes
}

void TestOfflineProfileSource::isLiveSystemRoot_sameVolumeSpelledDifferently() {
    const QString guid = QStringLiteral("\\\\?\\Volume{aaaa0000-0000-0000-0000-000000000001}");
    QVERIFY(OfflineProfileSource::isLiveSystemRoot(guid, guid));
    QVERIFY(OfflineProfileSource::isLiveSystemRoot(guid + QLatin1Char('\\'), guid));
    QVERIFY(OfflineProfileSource::isLiveSystemRoot(guid.toLower(), guid.toUpper()));
    QVERIFY(!OfflineProfileSource::isLiveSystemRoot(guid, QStringLiteral("C:\\")));
    QVERIFY(!OfflineProfileSource::isLiveSystemRoot(QString(), guid));  // empty never matches
}

void TestOfflineProfileSource::isReservedProfileFolderName_builtins() {
    QVERIFY(OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("Public")));
    QVERIFY(OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("public")));
    QVERIFY(OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("Default")));
    QVERIFY(OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("Default User")));
    QVERIFY(OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("All Users")));
    QVERIFY(!OfflineProfileSource::isReservedProfileFolderName(QStringLiteral("alice")));
}

void TestOfflineProfileSource::machineSidFromSamAccountValue_parses() {
    const QString sid = OfflineProfileSource::machineSidFromSamAccountValue(
        makeSamAccountValue(0x11'22'33'44, 0x55'66'77'88, 0x99'aa'bb'cc));
    // Pinned as literal decimals, not recomputed from the hex inputs: the decimal
    // form is what the manifest and the wizard carry. 0x11223344 = 287454020,
    // 0x55667788 = 1432778632, 0x99aabbcc = 2578103244.
    QCOMPARE(sid, QStringLiteral("S-1-5-21-287454020-1432778632-2578103244"));
    // The value Windows really carries is long; a truncated one parses to nothing.
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(makeSamAccountValue(1, 2, 3, 0x18))
                .isEmpty());
}

void TestOfflineProfileSource::machineSidFromSamAccountValue_rejectsGarbage() {
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(QByteArray()).isEmpty());
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(QByteArray(512, '\x00')).isEmpty());
    QByteArray wrong_revision = makeSamAccountValue(1, 2, 3);
    wrong_revision[0x0C] = 2;
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(wrong_revision).isEmpty());
    QByteArray wrong_authority = makeSamAccountValue(1, 2, 3);
    wrong_authority[0x13] = 18;  // authority 18, not 5
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(wrong_authority).isEmpty());
    QByteArray wrong_identity = makeSamAccountValue(1, 2, 3);
    quint32 subs[4] = {16, 1, 2, 3};  // first sub-authority is not 21
    memcpy(wrong_revision.data() + 0x14, subs, sizeof(subs));
    memcpy(wrong_authority.data() + 0x14, subs, sizeof(subs));
    QByteArray identity = makeSamAccountValue(1, 2, 3);
    memcpy(identity.data() + 0x14, subs, sizeof(subs));
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountValue(identity).isEmpty());
}

void TestOfflineProfileSource::sidBelongsToMachine_prefixAndEmpty() {
    const QString machine = QStringLiteral("S-1-5-21-1-2-3");
    QVERIFY(
        OfflineProfileSource::sidBelongsToMachine(QStringLiteral("S-1-5-21-1-2-3-1001"), machine));
    QVERIFY(OfflineProfileSource::sidBelongsToMachine(QStringLiteral("s-1-5-21-1-2-3-500"),
                                                      machine.toUpper()));
    // Domain account: same shape, different authority.
    QVERIFY(
        !OfflineProfileSource::sidBelongsToMachine(QStringLiteral("S-1-5-21-9-9-9-1001"), machine));
    // A machine that is a SID PREFIX of another machine is NOT a member of it: -4 is a
    // different machine, not a RID of this one.
    QVERIFY(!OfflineProfileSource::sidBelongsToMachine(QStringLiteral("S-1-5-21-1-2-34"), machine));
    QVERIFY(!OfflineProfileSource::sidBelongsToMachine(QString(), machine));
    QVERIFY(!OfflineProfileSource::sidBelongsToMachine(QStringLiteral("S-1-5-21-1-2-3-1001"),
                                                       QString()));
}

void TestOfflineProfileSource::profileSkipReason_everyCase() {
    const QString machine = QStringLiteral("S-1-5-21-1-2-3");
    QVERIFY(
        OfflineProfileSource::profileSkipReason(QStringLiteral("Public"), QString(), machine, true)
            .contains(QStringLiteral("Built-in")));
    // Hive fallback (ruling: honest folder names, no fake SIDs): everything not reserved passes.
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("alice"), QString(), QString(), false)
                .isEmpty());
    // Hive read, folder with no account record: skipped, not guessed.
    QVERIFY(
        OfflineProfileSource::profileSkipReason(QStringLiteral("ghost"), QString(), machine, true)
            .contains(QStringLiteral("No single account record")));
    // Hive read, ProfileList fine but SAM unreadable: cannot PROVE local, so skipped.
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("alice"), QStringLiteral("S-1-5-21-1-2-3-1001"), QString(), true)
                .contains(QStringLiteral("SAM")));
    // Domain profile (ruling 4): skipped with its reason.
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("jsmith"), QStringLiteral("S-1-5-21-9-9-9-1001"), machine, true)
                .contains(QStringLiteral("Domain")));
    // Local profile: included.
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("alice"), QStringLiteral("S-1-5-21-1-2-3-1001"), machine, true)
                .isEmpty());
}

void TestOfflineProfileSource::normalizeRecoveryKey_canonicalAndRejects() {
    const QString typed =
        QStringLiteral("  111111-222222-333333-444444-555555-666666-777777-888888  ");
    auto good = OfflineProfileSource::normalizeRecoveryKey(typed);
    QVERIFY(good.has_value());
    QCOMPARE(*good, QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"));
    // Spaces and stray punctuation between groups are the shape people actually type.
    auto spaced = OfflineProfileSource::normalizeRecoveryKey(
        QStringLiteral("111111 222222 333333 444444 555555 666666 777777 888888"));
    QVERIFY(spaced.has_value());
    QCOMPARE(*spaced, *good);
    // 47 digits: refused, and the error counts without echoing digits.
    auto short_key = OfflineProfileSource::normalizeRecoveryKey(
        QStringLiteral("11111122222233333344444455555566666677777788888"));
    QVERIFY(!short_key.has_value());
    QVERIFY(short_key.error().contains(QStringLiteral("47")));
    QVERIFY(!short_key.error().contains(QStringLiteral("111111")));
    auto letters = OfflineProfileSource::normalizeRecoveryKey(
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-88888A"));
    QVERIFY(!letters.has_value());
    QVERIFY(letters.error().contains(QStringLiteral("letters")));
}

void TestOfflineProfileSource::bitLockerUnlockScript_carriesKeyPathNotKey() {
    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    auto script = OfflineProfileSource::bitLockerUnlockScript(
        volume, QStringLiteral("C:\\sak\\data\\temp\\run-1\\sak-bitlocker-recovery-x.key"));
    QVERIFY(script.has_value());
    QVERIFY(script->contains(QStringLiteral("Unlock-BitLocker")));
    QVERIFY(
        script->contains(QStringLiteral("-MountPoint '\\\\?\\Volume{12345678-90ab-cdef-0123-"
                                        "456789abcdef}\\'")));
    QVERIFY(script->contains(QStringLiteral("sak-bitlocker-recovery-x.key")));
    QVERIFY(script->contains(QStringLiteral("SAK-OFFLINE-UNLOCK-OK")));
    // A folder name with an apostrophe must not be able to close the single-quoted literal.
    auto quoted = OfflineProfileSource::bitLockerUnlockScript(
        volume, QStringLiteral("C:\\temp\\O'Brien\\key.key"));
    QVERIFY(quoted.has_value());
    QVERIFY(quoted->contains(QStringLiteral("O''Brien")));
    // The script is built from validated inputs only -- it never takes the key at all.
    // A volume GUID legitimately holds long digit runs, so the check is for the
    // RECOVERY-KEY shape (six digits, dash, six digits), which no GUID can produce.
    QVERIFY(!script->contains(QRegularExpression(QStringLiteral("\\d{6}-\\d{6}"))));
}

void TestOfflineProfileSource::bitLockerUnlockScript_rejectsBadInputs() {
    const QString path = QStringLiteral("C:\\temp\\key.key");
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(QStringLiteral("E:"), path).has_value());
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(QStringLiteral("\\\\?\\Volume{bad-guid}"),
                                                         path)
                 .has_value());
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(
                 QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}"),
                 QStringLiteral("relative\\key.key"))
                 .has_value());
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(
                 QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}"), QString())
                 .has_value());
}

void TestOfflineProfileSource::unlockEncryptedVolume_stagesKeyAndShredsIt() {
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    const QString session = sak::app_paths::sessionTempDirectory();
    QVERIFY(!session.isEmpty());

    // A QVERIFY/QFAIL inside a lambda that returns std::expected is a compile
    // error (the macro's failure branch is a bare `return;`), so the runner
    // records what it saw and the slot asserts on it after the call.
    QString captured_script;
    QString seen_key_file;
    QString staged_path;
    bool key_file_opened = false;
    OfflineProfileSource::ElevatedScriptRunner runner =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        captured_script = script;
        // Read the staged file the way the script does: the KEY must be there at run time,
        // in a file inside this run's own temp folder -- and nowhere in the script text.
        const QString anchor = QStringLiteral("ReadAllText('");
        const int start = script.indexOf(anchor);
        const int end = start < 0 ? -1 : script.indexOf(QLatin1Char('\''), start + anchor.size());
        if (start >= 0 && end > start) {
            staged_path = script.mid(start + anchor.size(), end - start - anchor.size())
                              .replace(QStringLiteral("''"), QStringLiteral("'"));
            QFile staged(staged_path);
            key_file_opened = staged.open(QIODevice::ReadOnly);
            if (key_file_opened) {
                seen_key_file = QString::fromUtf8(staged.readAll()).trimmed();
                staged.close();
            }
        }
        return QStringLiteral("Unlock-BitLocker: OK\nSAK-OFFLINE-UNLOCK-OK");
    };

    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    const auto result = OfflineProfileSource::unlockEncryptedVolume(
        volume, QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"), runner);
    QVERIFY(result.has_value());
    QVERIFY(!staged_path.isEmpty());
    QVERIFY(staged_path.startsWith(sak::app_paths::sessionTempDirectory()));
    QVERIFY(key_file_opened);
    QCOMPARE(seen_key_file,
             QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"));
    // The digits never entered the script text; only the path did.
    QVERIFY(!captured_script.contains(QStringLiteral("111111")));
    QVERIFY(!captured_script.contains(QStringLiteral("888888")));

    // After the run the staged key file is gone (zeroed and removed).
    QDir temp(session);
    QVERIFY(temp.entryList(QStringList{QStringLiteral("sak-bitlocker-recovery-*.key")}, QDir::Files)
                .isEmpty());
}

void TestOfflineProfileSource::unlockEncryptedVolume_runnerFailureStillShreds() {
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    OfflineProfileSource::ElevatedScriptRunner failing =
        [](const QString&, const QString&) -> std::expected<QString, QString> {
        return std::unexpected(QStringLiteral("helper refused"));
    };
    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    const auto result = OfflineProfileSource::unlockEncryptedVolume(
        volume, QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"), failing);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), QStringLiteral("helper refused"));
    QDir temp(sak::app_paths::sessionTempDirectory());
    QVERIFY(temp.entryList(QStringList{QStringLiteral("sak-bitlocker-recovery-*.key")}, QDir::Files)
                .isEmpty());
}

void TestOfflineProfileSource::scanUsersDirectory_tempTreeDecisions() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("Users\\alice\\Documents")));
    QVERIFY(root.mkpath(QStringLiteral("Users\\Public")));
    QVERIFY(root.mkpath(QStringLiteral("Users\\ghost")));
    QFile doc(root.filePath(QStringLiteral("Users\\alice\\Documents\\a.txt")));
    QVERIFY(doc.open(QIODevice::WriteOnly));
    doc.write("hello");  // 5 bytes, the one file the size walk will see
    doc.close();

    const QString machine = QStringLiteral("S-1-5-21-1-2-3");
    const OfflineHiveFacts facts = makeFacts(
        machine,
        {
            {QStringLiteral("S-1-5-21-1-2-3-1001"), QStringLiteral("\\Users\\alice")},
            {QStringLiteral("S-1-5-21-9-9-9-1001"), QStringLiteral("\\Users\\domainuser")},
        });

    QString error;
    const auto entries = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(root.filePath(QStringLiteral("Users"))) + '\\', facts, error);
    QVERIFY(error.isEmpty());

    const OfflineProfileEntry alice = findEntry(entries, QStringLiteral("alice"));
    QVERIFY(alice.included);
    QVERIFY(alice.skip_reason.isEmpty());
    QCOMPARE(alice.sid, QStringLiteral("S-1-5-21-1-2-3-1001"));
    QVERIFY(alice.sid_from_hive);
    QCOMPARE(alice.estimated_size_bytes, static_cast<qint64>(5));

    QVERIFY(!findEntry(entries, QStringLiteral("Public")).included);
    QVERIFY(findEntry(entries, QStringLiteral("Public"))
                .skip_reason.contains(QStringLiteral("Built-in")));

    // A folder no hive row names is skipped, not rescued under a guessed identity.
    const OfflineProfileEntry ghost = findEntry(entries, QStringLiteral("ghost"));
    QVERIFY(!ghost.included);
    QVERIFY(ghost.skip_reason.contains(QStringLiteral("No single account record")));

    // A hive row no folder answers to is simply not a discoverable profile: it cannot be
    // listed as skipped either (there is nothing on disk to point at).
    QVERIFY(findEntry(entries, QStringLiteral("domainuser")).folder_name.isEmpty());
}

void TestOfflineProfileSource::scanUsersDirectory_junctionRefusedCanaryIntact() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("Users\\real")));
    // createJunction makes the link folder itself (CreateDirectoryW), so "trojan"
    // must NOT exist yet -- mkpath'ing it would fail the junction creation.
    // The junction's TARGET lives outside Users and holds a canary: following it would
    // read (or worse, hand over) files no profile owns.
    QVERIFY(root.mkpath(QStringLiteral("outside")));
    QVERIFY(sak::test::createCanary(root.filePath(QStringLiteral("outside")),
                                    QStringLiteral("canary.bin"),
                                    QByteArray("SECRET")));
    QVERIFY(sak::test::createJunction(root.filePath(QStringLiteral("Users\\trojan")),
                                      root.filePath(QStringLiteral("outside"))));

    const QString machine = QStringLiteral("S-1-5-21-1-2-3");
    const OfflineHiveFacts facts = makeFacts(machine, {});
    QString error;
    const auto entries = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(root.filePath(QStringLiteral("Users"))) + '\\', facts, error);
    QVERIFY(error.isEmpty());

    const OfflineProfileEntry trojan = findEntry(entries, QStringLiteral("trojan"));
    QVERIFY(!trojan.included);
    QVERIFY(trojan.skip_reason.contains(QStringLiteral("link")));
    QVERIFY(sak::test::canaryIntact(root.filePath(QStringLiteral("outside")),
                                    QStringLiteral("canary.bin"),
                                    QByteArray("SECRET")));
}

void TestOfflineProfileSource::scanUsersDirectory_fallbackModeIncludesWithWarning() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("Users\\alice")));

    OfflineHiveFacts facts;  // nothing readable: the honest folder-name fallback
    facts.errors.append(QStringLiteral("SOFTWARE could not be loaded"));
    QString error;
    const auto entries = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(root.filePath(QStringLiteral("Users"))) + '\\', facts, error);
    QVERIFY(error.isEmpty());
    QCOMPARE(entries.size(), 1);
    QVERIFY(entries.at(0).included);
    QVERIFY(entries.at(0).sid.isEmpty());
    QVERIFY(entries.at(0).skip_reason.isEmpty());
}

void TestOfflineProfileSource::scanUsersDirectory_missingUsersIsGenuineEmpty() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QString error;
    const auto entries = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(dir.path()) + QStringLiteral("\\Users\\"),
        makeFacts(QStringLiteral("S-1-5-21-1-2-3"), {}),
        error);
    QVERIFY(entries.isEmpty());
    QVERIFY(error.isEmpty());  // empty WITH no error = genuine, not a failure
}

void TestOfflineProfileSource::discover_unelevatedRefuses() {
    const auto result = OfflineProfileSource::discover(false);
    QVERIFY(!result.enumeration_ok);  // an empty list must never read as a scan
    QCOMPARE(result.error, OfflineProfileSource::elevationRequiredMessage());
    QVERIFY(!OfflineProfileSource::elevationRequiredMessage().isEmpty());
    QVERIFY(result.volumes.isEmpty());
}

void TestOfflineProfileSource::discoverVolume_rejectsNonGuidAndLiveSystem() {
    // discoverVolume reads hives from the mounted root and never launches the
    // elevated helper, so there is nothing to inject here; the no-launch
    // property belongs to mount/unmount/unlock, which do take a runner.
    const auto unknown =
        OfflineProfileSource::discoverVolume(QStringLiteral("\\\\.\\SakNoSuchMount-9f31"), true);
    QVERIFY(!unknown.readable);
    QVERIFY(unknown.unread_reason.contains(QStringLiteral("not a volume")));

    const auto unelevated = OfflineProfileSource::discoverVolume(
        QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}"), false);
    QVERIFY(!unelevated.readable);
    QCOMPARE(unelevated.unread_reason, OfflineProfileSource::elevationRequiredMessage());

    const QString live = OfflineProfileSource::liveSystemVolumeName();
    if (!live.isEmpty()) {  // on a real Windows, the live volume is refused by name
        QVERIFY(OfflineProfileSource::isVolumeGuidPath(live));  // resolved, never "C:"
        const auto live_source = OfflineProfileSource::discoverVolume(live, true);
        QVERIFY(!live_source.readable);
        QVERIFY(live_source.unread_reason.contains(QStringLiteral("running Windows")));
        // The same volume spelled as its drive letter is the SAME refusal: a
        // technician typing C: must not be told to scan the running system.
        const QString system_drive = qEnvironmentVariable("SystemDrive").trimmed();  // e.g. "C:"
        if (!system_drive.isEmpty()) {
            const auto by_letter = OfflineProfileSource::discoverVolume(system_drive, true);
            QVERIFY(!by_letter.readable);
            QVERIFY(by_letter.unread_reason.contains(QStringLiteral("running Windows")));
            QCOMPARE(by_letter.volume_name, live);
        }
    }
}

namespace {

/// @brief True when this process holds an elevated token, the only token that can
///        enable SeRestorePrivilege and call RegLoadKey.
bool processIsElevated() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated =
        GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size) != 0 &&
        elevation.TokenIsElevated != 0;
    CloseHandle(token);
    return elevated;
}

/// @brief Copies a loaded live registry tree into a hive file -- the shape an offline
///        disk hands the scanner. Returns the failure text, empty on success.
QString saveLiveTreeAsHive(const QString& key_name, const QString& hive_path) {
    QProcess reg;
    reg.start(QStringLiteral("reg.exe"),
              {QStringLiteral("save"),
               QStringLiteral("HKLM\\") + key_name,
               hive_path,
               QStringLiteral("/y")});
    if (!reg.waitForStarted(5000)) {
        return QStringLiteral("could not start reg.exe");
    }
    if (!reg.waitForFinished(120'000)) {
        reg.kill();
        reg.waitForFinished(5000);
        return QStringLiteral("reg.exe timed out saving %1").arg(key_name);
    }
    if (reg.exitStatus() != QProcess::NormalExit || reg.exitCode() != 0) {
        return QStringLiteral("reg save %1 failed: %2")
            .arg(key_name, QString::fromLocal8Bit(reg.readAllStandardError()).trimmed());
    }
    return {};
}

/// @brief Writes the SAM Domains\Account "F" shape the unit fixture pins under a
///        scratch key, saves it as a hive file, and removes the scratch key.
///
///        The live SAM hive is kernel-protected from Administrators too, so it has no
///        copy to save; this also proves the SAM parse survives a real save/RegLoadKey
///        round trip, not just the fixture bytes. The scratch key lives under SOFTWARE
///        because a few security products in the wild block NEW keys directly at the
///        HKLM root; SOFTWARE is open ground. Returns the failure text, empty on
///        success.
QString writeSamShapedHive(const QString& hive_path) {
    const QString key_name =
        QStringLiteral("SOFTWARE\\SAK-OfflineHiveTest-%1").arg(QCoreApplication::applicationPid());
    const std::wstring key_w = key_name.toStdWString();
    const auto delete_key = qScopeGuard([&] { RegDeleteTreeW(HKEY_LOCAL_MACHINE, key_w.c_str()); });
    HKEY branch = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_LOCAL_MACHINE,
                                     (key_w + L"\\Domains\\Account").c_str(),
                                     0,
                                     nullptr,
                                     0,
                                     KEY_WRITE,
                                     nullptr,
                                     &branch,
                                     nullptr);
    if (status != ERROR_SUCCESS) {
        return QStringLiteral("create the SAM-shaped key failed: %1").arg(status);
    }
    const QByteArray account_value = makeSamAccountValue(7'000'001, 7'000'002, 7'000'003);
    status = RegSetValueExW(branch,
                            L"F",
                            0,
                            REG_BINARY,
                            reinterpret_cast<const BYTE*>(account_value.constData()),
                            static_cast<DWORD>(account_value.size()));
    RegCloseKey(branch);
    if (status != ERROR_SUCCESS) {
        return QStringLiteral("write the SAM F value failed: %1").arg(status);
    }
    return saveLiveTreeAsHive(key_name, hive_path);
}

}  // namespace

void TestOfflineProfileSource::readOfflineHives_liveMachineHives() {
    // The elevated hive test the header's subkey-path comment refers to. This is the
    // only end-to-end proof of the WHOLE load path: the SeRestorePrivilege lookup and
    // enable (a wrong privilege name failed that lookup on every machine, silently,
    // until 2026-11-23), RegLoadKey, the ProfileList read, the ComputerName read and
    // the SAM machine-SID parse. The live hives are locked by the kernel and cannot
    // be reloaded, so the test saves copies of them first -- exactly the shape an
    // offline disk hands the scanner: hive files read from a dead system.
    if (!processIsElevated()) {
        QSKIP(
            "Loading offline hives needs the SeRestorePrivilege of an elevated token; "
            "an elevated run proves it");
    }

    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString config = QDir::toNativeSeparators(dir.path()) +
                           QStringLiteral("\\System32\\config\\");
    QVERIFY(QDir().mkpath(config));

    const QString software_error = saveLiveTreeAsHive(QStringLiteral("SOFTWARE"),
                                                      config + QStringLiteral("SOFTWARE"));
    QVERIFY2(software_error.isEmpty(), qPrintable(software_error));
    const QString system_error = saveLiveTreeAsHive(QStringLiteral("SYSTEM"),
                                                    config + QStringLiteral("SYSTEM"));
    QVERIFY2(system_error.isEmpty(), qPrintable(system_error));
    const QString sam_error = writeSamShapedHive(config + QStringLiteral("SAM"));
    QVERIFY2(sam_error.isEmpty(), qPrintable(sam_error));

    const OfflineHiveFacts facts = OfflineProfileSource::readOfflineHives(
        QDir::toNativeSeparators(dir.path()) + QLatin1Char('\\'));
    QVERIFY2(facts.errors.isEmpty(), qPrintable(facts.errors.join(QStringLiteral("; "))));
    QVERIFY(facts.profile_list_ok);
    QCOMPARE(facts.machine_sid, QStringLiteral("S-1-5-21-7000001-7000002-7000003"));
    QVERIFY(facts.machine_sid_ok);
    QCOMPARE(facts.computer_name.compare(qEnvironmentVariable("COMPUTERNAME"), Qt::CaseInsensitive),
             0);

    // The current user must appear in the real hive's ProfileList with a SID under
    // the machine's authority: proves ProfileImagePath rows parsed from a real hive.
    const QString user = qEnvironmentVariable("USERNAME").trimmed();
    QVERIFY(!user.isEmpty());
    bool found_current_user = false;
    for (const OfflineHiveProfileRow& row : facts.profile_list) {
        const QFileInfo image(row.profile_image_path);
        if (image.fileName().compare(user, Qt::CaseInsensitive) == 0) {
            QVERIFY(row.sid.startsWith(QStringLiteral("S-1-5-21-")));
            found_current_user = true;
        }
    }
    QVERIFY2(found_current_user,
             qPrintable(QStringLiteral("ProfileList held %1 rows, none for current user")
                            .arg(facts.profile_list.size())));
}

void TestOfflineProfileSource::unmountRunDirectory_refusesForeignPath() {
    bool runner_ran = false;
    OfflineProfileSource::ElevatedScriptRunner runner =
        [&](const QString&, const QString&) -> std::expected<QString, QString> {
        runner_ran = true;
        return QStringLiteral("SAK-OFFLINE-UNMOUNT-OK");
    };
    const auto foreign = OfflineProfileSource::unmountRunDirectory(QStringLiteral("C:\\Windows"),
                                                                   runner);
    QVERIFY(!foreign.has_value());
    QVERIFY(!runner_ran);  // unmount only ever touches a directory this run created
    const auto empty_path = OfflineProfileSource::unmountRunDirectory(QString(), runner);
    QVERIFY(!empty_path.has_value());
    QVERIFY(!runner_ran);
}

void TestOfflineProfileSource::mountVolumeToRunDirectory_requiresGuidAndSessionTemp() {
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    bool runner_ran = false;
    OfflineProfileSource::ElevatedScriptRunner runner =
        [&](const QString&, const QString&) -> std::expected<QString, QString> {
        runner_ran = true;
        return QStringLiteral("SAK-OFFLINE-MOUNT-OK");
    };
    const auto bad = OfflineProfileSource::mountVolumeToRunDirectory(QStringLiteral("E:"), runner);
    QVERIFY(!bad.has_value());
    QVERIFY(!runner_ran);

    // A path inside this run's temp folder named like a mount directory IS one; anything
    // else (even a sibling spelling) is not.
    const QString session = sak::app_paths::sessionTempDirectory();
    QVERIFY(OfflineProfileSource::isRunMountDirectory(
        QDir(session).filePath(QStringLiteral("offline-a1b2c3d4e5f6a7b8c9d0e1f2a3b4c5d6"))));
    QVERIFY(!OfflineProfileSource::isRunMountDirectory(
        QDir(session).filePath(QStringLiteral("notoffline-x"))));
    QVERIFY(
        !OfflineProfileSource::isRunMountDirectory(QStringLiteral("C:\\Windows\\Temp\\offline-x")));
    QVERIFY(!OfflineProfileSource::isRunMountDirectory(
        QDir(session).filePath(QStringLiteral("offline-a\\b"))));
}

QTEST_GUILESS_MAIN(TestOfflineProfileSource)
#include "test_offline_profile_source.moc"
