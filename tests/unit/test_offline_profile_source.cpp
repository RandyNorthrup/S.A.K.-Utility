// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_offline_profile_source.cpp
/// @brief Unit tests for the offline profile rescue seams: pure decisions, a temp-tree
///        Users scan with a junction canary, and the recovery-key staging lifecycle.

#include "sak/app_paths.h"
#include "sak/offline_profile_source.h"

#include "../support/junction_fixture.h"
#include "../support/test_hardware_guard.h"

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

#include <ntsecapi.h>
#include <sddl.h>

using namespace sak;

namespace {

/// @brief A SAM\Domains\Account "V" value as Windows stores it: variable-length data whose
///        last 24 bytes are the account domain's SID (revision 1, four sub-authorities,
///        authority 5, then 21 and three machine dwords). That layout is pinned against a SAM
///        saved from the live machine by readOfflineHives_liveMachineHives; this fixture pins
///        the parser's shape rules.
QByteArray makeSamAccountV(quint32 a, quint32 b, quint32 c, int leading_size = 0x100) {
    constexpr int kSidBytes = 24;
    QByteArray sid(kSidBytes, '\0');
    auto* p = reinterpret_cast<unsigned char*>(sid.data());
    p[0] = 1;  // revision
    p[1] = 4;  // four sub-authorities: 21, a, b, c
    p[7] = 5;  // the 6-byte big-endian authority (bytes 2..7): NT
    const quint32 subs[4] = {21, a, b, c};
    memcpy(p + 8, subs, sizeof(subs));
    return QByteArray(leading_size, '\x7A') + sid;
}

/// @brief Offset of the SID inside a makeSamAccountV value.
int samSidOffset(const QByteArray& value) {
    return static_cast<int>(value.size()) - 24;
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
    void machineSidFromSamAccountV_parsesTrailingSid();
    void machineSidFromSamAccountV_rejectsGarbage();
    void controlSetKeyName_padsAndRejects();
    void matchVolumeRequest_exactOnly();
    void sidBelongsToMachine_prefixAndEmpty();
    void profileSkipReason_everyCase();
    void normalizeRecoveryKey_canonicalAndRejects();
    void bitLockerUnlockScript_carriesPlaceholderNotKey();
    void bitLockerUnlockScript_rejectsBadInputs();
    void unlockEncryptedVolume_stagesKeyAndShredsIt();
    void unlockEncryptedVolume_runnerFailureStillShreds();
    void unlockEncryptedVolume_keyThatLeavesItLockedIsRefused();
    void scanUsersDirectory_tempTreeDecisions();
    void scanUsersDirectory_junctionRefusedCanaryIntact();
    void scanUsersDirectory_unreadableHivesSkipEveryFolder();
    void scanUsersDirectory_missingUsersIsGenuineEmpty();
    void discover_unelevatedRefuses();
    void discoverVolume_rejectsNonGuidAndLiveSystem();
    void readOfflineHives_liveMachineHives();
    void unmountRunDirectory_refusesForeignPath();
    void mountVolumeToRunDirectory_requiresGuidAndSessionTemp();
    void mountAndUnmount_realVolumeWithoutLetter();
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

void TestOfflineProfileSource::machineSidFromSamAccountV_parsesTrailingSid() {
    const QString sid = OfflineProfileSource::machineSidFromSamAccountV(
        makeSamAccountV(0x11'22'33'44, 0x55'66'77'88, 0x99'aa'bb'cc));
    // Pinned as literal decimals, not recomputed from the hex inputs: the decimal
    // form is what the manifest and the wizard carry. 0x11223344 = 287454020,
    // 0x55667788 = 1432778632, 0x99aabbcc = 2578103244.
    QCOMPARE(sid, QStringLiteral("S-1-5-21-287454020-1432778632-2578103244"));
    // Exactly the SID is enough; one byte short is not.
    QCOMPARE(OfflineProfileSource::machineSidFromSamAccountV(makeSamAccountV(1, 2, 3, 0)),
             QStringLiteral("S-1-5-21-1-2-3"));
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(makeSamAccountV(1, 2, 3, 0).mid(1))
                .isEmpty());
    // The layout an earlier synthesized fixture used -- a SID at offset 0x0C of "F" with
    // filler after it -- is not where Windows keeps it, and parses to nothing.
    QByteArray synthesized_f(512, '\x7A');
    const QByteArray sid_bytes = makeSamAccountV(1, 2, 3, 0);
    synthesized_f.replace(0x0C, sid_bytes.size(), sid_bytes);
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(synthesized_f).isEmpty());
}

void TestOfflineProfileSource::machineSidFromSamAccountV_rejectsGarbage() {
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(QByteArray()).isEmpty());
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(QByteArray(512, '\x00')).isEmpty());
    QByteArray wrong_revision = makeSamAccountV(1, 2, 3);
    wrong_revision[samSidOffset(wrong_revision)] = 2;
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(wrong_revision).isEmpty());
    QByteArray wrong_count = makeSamAccountV(1, 2, 3);
    wrong_count[samSidOffset(wrong_count) + 1] = 5;
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(wrong_count).isEmpty());
    QByteArray wrong_authority = makeSamAccountV(1, 2, 3);
    wrong_authority[samSidOffset(wrong_authority) + 7] = 18;  // authority 18, not 5
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(wrong_authority).isEmpty());
    QByteArray wrong_identity = makeSamAccountV(1, 2, 3);
    const quint32 subs[4] = {16, 1, 2, 3};  // first sub-authority is not 21
    memcpy(wrong_identity.data() + samSidOffset(wrong_identity) + 8, subs, sizeof(subs));
    QVERIFY(OfflineProfileSource::machineSidFromSamAccountV(wrong_identity).isEmpty());
}

void TestOfflineProfileSource::controlSetKeyName_padsAndRejects() {
    // Select\Default is a REG_DWORD; 1 names ControlSet001, never "ControlSet1".
    QCOMPARE(OfflineProfileSource::controlSetKeyName(1), QStringLiteral("ControlSet001"));
    QCOMPARE(OfflineProfileSource::controlSetKeyName(2), QStringLiteral("ControlSet002"));
    QCOMPARE(OfflineProfileSource::controlSetKeyName(12), QStringLiteral("ControlSet012"));
    QCOMPARE(OfflineProfileSource::controlSetKeyName(999), QStringLiteral("ControlSet999"));
    QVERIFY(OfflineProfileSource::controlSetKeyName(0).isEmpty());
    QVERIFY(OfflineProfileSource::controlSetKeyName(1000).isEmpty());
}

void TestOfflineProfileSource::matchVolumeRequest_exactOnly() {
    OfflineSourceDiscovery discovery;
    discovery.enumeration_ok = true;
    OfflineVolumeSource lettered;
    lettered.root_path = QStringLiteral("D:\\");
    lettered.volume_name = QStringLiteral("\\\\?\\Volume{11111111-2222-3333-4444-555555555555}");
    lettered.display_name = QStringLiteral("Data (D:)");
    OfflineVolumeSource unlettered;
    unlettered.volume_name = QStringLiteral("\\\\?\\Volume{eeeeeeee-eeee-eeee-eeee-eeeeeeeeeeee}");
    unlettered.display_name = QStringLiteral("Old disk");
    discovery.volumes = {lettered, unlettered};
    const QString label = QStringLiteral("test");

    for (const QString& spelling : {QStringLiteral("D"),
                                    QStringLiteral("d:"),
                                    QStringLiteral("D:\\"),
                                    QStringLiteral("D:/")}) {
        const auto found = OfflineProfileSource::matchVolumeRequest(discovery, spelling, label);
        QVERIFY2(found.has_value(), qPrintable(spelling));
        QCOMPARE(found->volume_name, lettered.volume_name);
    }
    for (const QString& spelling : {unlettered.volume_name,
                                    unlettered.volume_name + QLatin1Char('\\'),
                                    unlettered.volume_name.toUpper()}) {
        const auto found = OfflineProfileSource::matchVolumeRequest(discovery, spelling, label);
        QVERIFY2(found.has_value(), qPrintable(spelling));
        QCOMPARE(found->volume_name, unlettered.volume_name);
    }
    QVERIFY(OfflineProfileSource::matchVolumeRequest(discovery, QStringLiteral("Old disk"), label)
                .has_value());
    // A short request must not select a volume because it occurs inside a GUID or a label.
    for (const QString& spelling : {QStringLiteral("E"),
                                    QStringLiteral("e:"),
                                    QStringLiteral("eeee"),
                                    QStringLiteral("\\\\?\\Volume{eeeeeeee"),
                                    QStringLiteral("Old"),
                                    QStringLiteral("D:\\Users")}) {
        QVERIFY2(!OfflineProfileSource::matchVolumeRequest(discovery, spelling, label).has_value(),
                 qPrintable(spelling));
    }
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
    // ProfileList unreadable: fail closed. Nothing proves the folder is a local account, so it
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("alice"), QStringLiteral("S-1-5-21-1-2-3-1001"), machine, false)
                .contains(QStringLiteral("could not be read")));
    // Hive read, folder with no account record: skipped, not guessed.
    QVERIFY(
        OfflineProfileSource::profileSkipReason(QStringLiteral("ghost"), QString(), machine, true)
            .contains(QStringLiteral("No single account record")));
    // Hive read, ProfileList fine but SAM unreadable: cannot PROVE local, so skipped.
    QVERIFY(OfflineProfileSource::profileSkipReason(
                QStringLiteral("alice"), QStringLiteral("S-1-5-21-1-2-3-1001"), QString(), true)
                .contains(QStringLiteral("SAM")));
    // Domain profile: skipped with its reason.
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

void TestOfflineProfileSource::bitLockerUnlockScript_carriesPlaceholderNotKey() {
    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    auto script = OfflineProfileSource::bitLockerUnlockScript(volume);
    QVERIFY(script.has_value());
    QVERIFY(script->contains(QStringLiteral("Unlock-BitLocker")));
    QVERIFY(
        script->contains(QStringLiteral("-MountPoint '\\\\?\\Volume{12345678-90ab-cdef-0123-"
                                        "456789abcdef}\\'")));
    // The key file is named only by the placeholder StagedScriptCredentials replaces.
    QVERIFY(script->contains(OfflineProfileSource::recoveryKeyFilePlaceholder()));
    QVERIFY(script->contains(QStringLiteral("SAK-OFFLINE-UNLOCK-OK")));
    // The script never takes the key at all. A volume GUID legitimately holds long digit runs,
    // so the check is for the RECOVERY-KEY shape (six digits, dash, six digits).
    QVERIFY(!script->contains(QRegularExpression(QStringLiteral("\\d{6}-\\d{6}"))));
}

void TestOfflineProfileSource::bitLockerUnlockScript_rejectsBadInputs() {
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(QStringLiteral("E:")).has_value());
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(QStringLiteral("\\\\?\\Volume{bad-guid}"))
                 .has_value());
    QVERIFY(!OfflineProfileSource::bitLockerUnlockScript(QString()).has_value());
}

namespace {

/// @brief Runs @p script in Windows PowerShell from standard input (it stays in memory) and
///        returns its standard output, or the failure. Used as the elevated-script runner so
///        a generated script's own text is what runs, not a copy of it.
std::expected<QString, QString> runPowerShellScript(const QString& script) {
    // One encoded command runs as one script, the way the elevation helper runs it: a
    // terminating error ends it with a failure. Read from standard input, each statement
    // would run on its own and the next one would still run after a Stop.
    const QString encoded =
        QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(script.utf16()),
                                       static_cast<qsizetype>(script.size() * sizeof(char16_t)))
                                .toBase64());
    QProcess shell;
    shell.start(QDir(qEnvironmentVariable("SystemRoot"))
                    .filePath(QStringLiteral("System32/WindowsPowerShell/v1.0/powershell.exe")),
                {QStringLiteral("-NoProfile"),
                 QStringLiteral("-NonInteractive"),
                 QStringLiteral("-ExecutionPolicy"),
                 QStringLiteral("Bypass"),
                 QStringLiteral("-EncodedCommand"),
                 encoded});
    if (!shell.waitForStarted(10'000)) {
        return std::unexpected(QStringLiteral("could not start powershell.exe"));
    }
    shell.closeWriteChannel();
    if (!shell.waitForFinished(180'000)) {
        shell.kill();
        shell.waitForFinished(5000);
        return std::unexpected(QStringLiteral("powershell.exe timed out"));
    }
    const QString output = QString::fromLocal8Bit(shell.readAllStandardOutput());
    if (shell.exitStatus() != QProcess::NormalExit || shell.exitCode() != 0) {
        return std::unexpected(
            QStringLiteral("powershell.exe exit %1: %2 %3")
                .arg(shell.exitCode())
                .arg(output, QString::fromLocal8Bit(shell.readAllStandardError())));
    }
    return output;
}

/// @brief The staged key file's path, read out of the script the way the script reads it.
QString stagedKeyPathIn(const QString& script) {
    const QString anchor = QStringLiteral("ReadAllText('");
    const int start = script.indexOf(anchor);
    const int end = start < 0 ? -1 : script.indexOf(QLatin1Char('\''), start + anchor.size());
    if (start < 0 || end <= start) {
        return {};
    }
    return script.mid(start + anchor.size(), end - start - anchor.size())
        .replace(QStringLiteral("''"), QStringLiteral("'"));
}

/// @brief A stand-in Unlock-BitLocker that reports what it was given, so the generated
///        unlock script runs end to end without touching a real volume.
const char* const kUnlockBitLockerStandIn =
    "function Unlock-BitLocker { param([string]$MountPoint, [string]$RecoveryPassword) "
    "[Console]::Out.WriteLine('STANDIN-MOUNT=' + $MountPoint); "
    "[Console]::Out.WriteLine('STANDIN-KEY=' + $RecoveryPassword); "
    "$global:SakStandInUnlocked = $true }\n"
    "function Get-BitLockerVolume { param([string]$MountPoint) "
    "[pscustomobject]@{ LockStatus = $(if ($global:SakStandInUnlocked) { 'Unlocked' } "
    "else { 'Locked' }) } }\n";

/// @brief What Windows Server 2022 did with a wrong recovery key: Unlock-BitLocker returned
///        normally and the volume stayed locked.
const char* const kUnlockBitLockerIgnoringStandIn =
    "function Unlock-BitLocker { param([string]$MountPoint, [string]$RecoveryPassword) }\n"
    "function Get-BitLockerVolume { param([string]$MountPoint) "
    "[pscustomobject]@{ LockStatus = 'Locked' } }\n";

}  // namespace

void TestOfflineProfileSource::unlockEncryptedVolume_stagesKeyAndShredsIt() {
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    const QString session = sak::app_paths::sessionTempDirectory();
    QVERIFY(!session.isEmpty());

    // A QVERIFY inside a lambda that returns std::expected is a compile error, so the runner
    // records what it saw and the slot asserts on it after the call. The runner RUNS the staged
    // script (with a stand-in Unlock-BitLocker), so the key file it reads is the real one.
    QString captured_script;
    QString staged_path;
    QString script_output;
    OfflineProfileSource::ElevatedScriptRunner runner =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        captured_script = script;
        staged_path = stagedKeyPathIn(script);
        auto output = runPowerShellScript(QLatin1String(kUnlockBitLockerStandIn) + script);
        script_output = output ? *output : output.error();
        return output;
    };

    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    const QString key = QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888");
    // Typed the way people type it; the staged file holds the normalized form.
    const auto result = OfflineProfileSource::unlockEncryptedVolume(
        volume,
        QStringLiteral(" 111111 222222 333333 444444 555555 666666 777777 888888 "),
        runner);
    QVERIFY2(result.has_value(), qPrintable(result ? QString() : result.error()));
    QVERIFY2(script_output.contains(QStringLiteral("STANDIN-KEY=") + key),
             qPrintable(script_output));
    QVERIFY2(script_output.contains(QStringLiteral("STANDIN-MOUNT=") + volume + QLatin1Char('\\')),
             qPrintable(script_output));
    QVERIFY(!staged_path.isEmpty());
    QVERIFY(QDir::fromNativeSeparators(staged_path)
                .startsWith(QDir::fromNativeSeparators(session), Qt::CaseInsensitive));
    // The digits never entered the script text; only the held file's path did.
    QVERIFY(!captured_script.contains(QStringLiteral("111111")));
    QVERIFY(!captured_script.contains(OfflineProfileSource::recoveryKeyFilePlaceholder()));
    // After the run the staged key file is gone (zeroed and removed).
    QVERIFY(!QFileInfo::exists(staged_path));
}

void TestOfflineProfileSource::unlockEncryptedVolume_runnerFailureStillShreds() {
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    QString staged_path;
    bool held_while_running = false;
    OfflineProfileSource::ElevatedScriptRunner failing =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        staged_path = stagedKeyPathIn(script);
        held_while_running = QFileInfo::exists(staged_path);
        return std::unexpected(QStringLiteral("helper refused"));
    };
    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    const auto result = OfflineProfileSource::unlockEncryptedVolume(
        volume, QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"), failing);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), QStringLiteral("helper refused"));
    QVERIFY(!staged_path.isEmpty());
    QVERIFY(held_while_running);
    QVERIFY(!QFileInfo::exists(staged_path));
}

void TestOfflineProfileSource::unlockEncryptedVolume_keyThatLeavesItLockedIsRefused() {
    // The generated script runs against a stand-in that, like the real cmdlet given a wrong
    // key on Windows Server 2022, returns normally and leaves the volume locked.
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    QString staged_path;
    OfflineProfileSource::ElevatedScriptRunner runner =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        staged_path = stagedKeyPathIn(script);
        return runPowerShellScript(QLatin1String(kUnlockBitLockerIgnoringStandIn) + script);
    };
    const QString volume = QStringLiteral("\\\\?\\Volume{12345678-90ab-cdef-0123-456789abcdef}");
    const auto result = OfflineProfileSource::unlockEncryptedVolume(
        volume, QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888"), runner);
    QVERIFY(!result.has_value());
    QVERIFY2(result.error().contains(QStringLiteral("did not unlock")), qPrintable(result.error()));
    QVERIFY(!result.error().contains(QStringLiteral("111111")));
    QVERIFY(!staged_path.isEmpty());
    QVERIFY(!QFileInfo::exists(staged_path));
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

void TestOfflineProfileSource::scanUsersDirectory_unreadableHivesSkipEveryFolder() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QDir root(dir.path());
    QVERIFY(root.mkpath(QStringLiteral("Users\\alice")));

    // Nothing readable: fail closed. A folder name alone proves no local account, so the
    // folder is listed with its reason and never offered for rescue.
    OfflineHiveFacts facts;
    facts.errors.append(QStringLiteral("SOFTWARE could not be loaded"));
    QString error;
    const auto entries = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(root.filePath(QStringLiteral("Users"))) + '\\', facts, error);
    QVERIFY(error.isEmpty());
    QCOMPARE(entries.size(), 1);
    QVERIFY(!entries.at(0).included);
    QVERIFY(entries.at(0).sid.isEmpty());
    QVERIFY(entries.at(0).skip_reason.contains(QStringLiteral("could not be read")));

    // ProfileList read but SAM not: the matching SID still cannot be proven local.
    OfflineHiveFacts no_sam = makeFacts(QString(), {});
    OfflineHiveProfileRow row;
    row.sid = QStringLiteral("S-1-5-21-1-2-3-1001");
    row.profile_image_path = QStringLiteral("\\Users\\alice");
    no_sam.profile_list = {row};
    const auto unproven = OfflineProfileSource::scanUsersDirectory(
        QDir::toNativeSeparators(root.filePath(QStringLiteral("Users"))) + '\\', no_sam, error);
    QCOMPARE(unproven.size(), 1);
    QVERIFY(!unproven.at(0).included);
    QVERIFY(unproven.at(0).skip_reason.contains(QStringLiteral("SAM")));
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

/// @brief This machine's account-domain SID from the LSA, the authority the SAM saved from
///        it must name. Empty when the query fails.
QString liveAccountDomainSid() {
    LSA_OBJECT_ATTRIBUTES attributes{};
    LSA_HANDLE policy = nullptr;
    if (LsaOpenPolicy(nullptr, &attributes, POLICY_VIEW_LOCAL_INFORMATION, &policy) != 0) {
        return {};
    }
    PPOLICY_ACCOUNT_DOMAIN_INFO info = nullptr;
    const NTSTATUS status = LsaQueryInformationPolicy(policy,
                                                      PolicyAccountDomainInformation,
                                                      reinterpret_cast<PVOID*>(&info));
    LsaClose(policy);
    QString sid;
    if (status == 0 && info != nullptr && info->DomainSid != nullptr) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(info->DomainSid, &text) != 0) {
            sid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    if (info != nullptr) {
        LsaFreeMemory(info);
    }
    return sid;
}

/// @brief The SID of the account running the test, from its token.
QString currentUserSid() {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == 0) {
        return {};
    }
    DWORD size = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    QByteArray buffer(static_cast<int>(size), '\0');
    QString sid;
    if (size > 0 && GetTokenInformation(token, TokenUser, buffer.data(), size, &size) != 0) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &text) !=
            0) {
            sid = QString::fromWCharArray(text);
            LocalFree(text);
        }
    }
    CloseHandle(token);
    return sid;
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
    // The real SAM, saved under backup privilege (reg.exe enables SeBackupPrivilege): its
    // Domains\Account key opens only with backup semantics, and its machine SID sits at the end
    // of "V". A synthesized hive proved only the synthesis.
    const QString sam_error = saveLiveTreeAsHive(QStringLiteral("SAM"),
                                                 config + QStringLiteral("SAM"));
    QVERIFY2(sam_error.isEmpty(), qPrintable(sam_error));

    const OfflineHiveFacts facts = OfflineProfileSource::readOfflineHives(
        QDir::toNativeSeparators(dir.path()) + QLatin1Char('\\'));
    QVERIFY2(facts.errors.isEmpty(), qPrintable(facts.errors.join(QStringLiteral("; "))));
    QVERIFY(facts.profile_list_ok);
    const QString live_sid = liveAccountDomainSid();
    QVERIFY2(!live_sid.isEmpty(), "the LSA did not name this machine's account domain");
    QCOMPARE(facts.machine_sid, live_sid);
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
            // A local account's profile classifies as rescuable against the saved SAM's SID.
            if (OfflineProfileSource::sidBelongsToMachine(currentUserSid(), live_sid)) {
                QVERIFY(OfflineProfileSource::sidBelongsToMachine(row.sid, facts.machine_sid));
            }
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

namespace {

/// @brief A script that makes a 64 MB VHDX at @p vhdx with one NTFS volume and no drive
///        letter, attached, and prints the volume's GUID path and (empty) letter.
QString letterlessVhdxScript(const QString& vhdx,
                             const QString& work_dir,
                             const QString& quoted_vhdx) {
    // diskpart makes and partitions the disk; Format-Volume formats it, because diskpart's
    // format answers "Incorrect function" when started from this test binary.
    return QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "$script = \"create vdisk file=`\"%1`\" maximum=64 type=expandable`r`n"
               "select vdisk file=`\"%1`\"`r`nattach vdisk`r`ncreate partition primary`r`n"
               "remove all noerr`r`n\"\n"
               "$file = Join-Path '%2' 'create.txt'\n"
               "[IO.File]::WriteAllText($file, $script)\n"
               "$said = & diskpart.exe /s $file | Out-String\n"
               "if ($LASTEXITCODE -ne 0) { Write-Error ('diskpart exit ' + $LASTEXITCODE + ': ' + "
               "$said); exit 1 }\n"
               "$partition = Get-DiskImage -ImagePath '%3' | Get-Disk | Get-Partition | "
               "Where-Object { $_.Type -ne 'Reserved' }\n"
               "$volume = $partition | Format-Volume -FileSystem NTFS -NewFileSystemLabel "
               "SAKOFFLINE -Confirm:$false\n"
               "[Console]::Out.WriteLine('PATH=' + $volume.Path)\n"
               "[Console]::Out.WriteLine('LETTER=' + $volume.DriveLetter)\n")
        .arg(vhdx, work_dir, quoted_vhdx);
}

constexpr DWORD kCanaryReadBytes = 32;

/// @brief The first bytes of @p path read with plain Win32 calls, or the Windows error code.
std::expected<QByteArray, DWORD> readSmallFile(const std::wstring& path) {
    HANDLE handle = CreateFileW(path.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(GetLastError());
    }
    char buffer[kCanaryReadBytes] = {};
    DWORD read = 0;
    const BOOL read_ok = ReadFile(handle, buffer, kCanaryReadBytes, &read, nullptr);
    const DWORD error = read_ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(handle);
    if (!read_ok) {
        return std::unexpected(error);
    }
    return QByteArray(buffer, static_cast<int>(read));
}

}  // namespace

void TestOfflineProfileSource::mountAndUnmount_realVolumeWithoutLetter() {
    // The real flow on a real volume: a VHDX with one NTFS partition and no drive letter is
    // mounted through its \\?\Volume{GUID}\ path by the product's own mount script, written
    // and read through the mount directory, then unmounted with the product's script. The
    // unmount removes only the mount point (mountvol /d), so the volume must still answer by
    // its GUID path afterwards; mountvol /p would have taken it offline.
    if (!processIsElevated()) {
        QSKIP(
            "Attaching a virtual disk and mounting its volume needs an elevated token; an "
            "elevated run proves it");
    }
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString vhdx = QDir::toNativeSeparators(dir.filePath(QStringLiteral("offline.vhdx")));
    const QString quoted_vhdx = QString(vhdx).replace(QStringLiteral("'"), QStringLiteral("''"));

    const auto created = runPowerShellScript(
        letterlessVhdxScript(vhdx, QDir::toNativeSeparators(dir.path()), quoted_vhdx));
    const auto detach = qScopeGuard([&] {
        (void)runPowerShellScript(
            QStringLiteral("Dismount-DiskImage -ImagePath '%1' | Out-Null\n").arg(quoted_vhdx));
    });
    QVERIFY2(created.has_value(), qPrintable(created ? QString() : created.error()));
    const QRegularExpressionMatch path_match =
        QRegularExpression(QStringLiteral("PATH=(\\S+)")).match(*created);
    QVERIFY2(path_match.hasMatch(), qPrintable(*created));
    QString volume_name = path_match.captured(1);
    QVERIFY(volume_name.endsWith(QLatin1Char('\\')));
    volume_name.chop(1);
    QVERIFY2(OfflineProfileSource::isVolumeGuidPath(volume_name), qPrintable(volume_name));
    QVERIFY2(QRegularExpression(QStringLiteral("LETTER=\\s*$"), QRegularExpression::MultilineOption)
                 .match(*created)
                 .hasMatch(),
             qPrintable(*created));  // no drive letter: the GUID path is the only way in

    OfflineProfileSource::ElevatedScriptRunner runner = [](const QString& script, const QString&) {
        return runPowerShellScript(script);
    };
    const auto mounted = OfflineProfileSource::mountVolumeToRunDirectory(volume_name, runner);
    QVERIFY2(mounted.has_value(), qPrintable(mounted ? QString() : mounted.error()));
    QVERIFY(OfflineProfileSource::isRunMountDirectory(*mounted));
    // Discovery reads the mount points by the stored name (no trailing backslash), which
    // Windows refuses unless the name is given its backslash.
    const OfflineVolumeSource seen = OfflineProfileSource::discoverVolume(volume_name, true);
    QVERIFY2(seen.root_path.compare(OfflineProfileSource::normalizeVolumeRoot(*mounted),
                                    Qt::CaseInsensitive) == 0,
             qPrintable(seen.root_path + QStringLiteral(" / ") + seen.unread_reason));
    {
        QFile canary(QDir(*mounted).filePath(QStringLiteral("canary.txt")));
        QVERIFY(canary.open(QIODevice::WriteOnly));
        QCOMPARE(canary.write("sak offline canary"), qint64(18));
    }

    const auto unmounted = OfflineProfileSource::unmountRunDirectory(*mounted, runner);
    QVERIFY2(unmounted.has_value(), qPrintable(unmounted ? QString() : unmounted.error()));
    QVERIFY(!QFileInfo::exists(*mounted));

    // Still online: the file written through the mount point reads back by the GUID path.
    const std::wstring canary_path = (volume_name + QStringLiteral("\\canary.txt")).toStdWString();
    const auto read_back = readSmallFile(canary_path);
    QVERIFY2(read_back.has_value(),
             qPrintable(QStringLiteral("volume offline after unmount: error %1")
                            .arg(read_back ? 0UL : read_back.error())));
    QCOMPARE(*read_back, QByteArray("sak offline canary"));
}

QTEST_GUILESS_MAIN(TestOfflineProfileSource)
#include "test_offline_profile_source.moc"
