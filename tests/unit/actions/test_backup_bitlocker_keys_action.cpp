// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_backup_bitlocker_keys_action.cpp
/// @brief Unit tests for the fail-closed seams of BackupBitlockerKeysAction
///        (Codex remediation batch B6-01..06). Verifies the recovery-key gate,
///        unique backup-directory naming, the key-protector parse failure
///        signal, and the recovery-password accounting helpers.

#include "sak/actions/backup_bitlocker_keys_action.h"

#include <QRegularExpression>
#include <QtTest/QtTest>

using sak::BackupBitlockerKeysAction;

/// Friend of BackupBitlockerKeysAction: reaches its private fail-closed seams
/// without widening the production class's public surface.
class BackupBitlockerKeysActionTests : public QObject {
    Q_OBJECT

    using Action = BackupBitlockerKeysAction;
    using KeyGate = BackupBitlockerKeysAction::KeyGate;
    using KeyProtectorInfo = BackupBitlockerKeysAction::KeyProtectorInfo;

private Q_SLOTS:
    // B6-05: gate requires a recovery password, not merely any protector.
    void evaluateKeyGate_requiresRecoveryPassword();
    // B6-03: backup dir names are unique and well-formed.
    void uniqueBackupDirName_distinctPerCounter();
    void backupTimestamp_hasMillisecondResolution();
    // B6-04: a malformed protector response is signalled, not treated as empty.
    void parseKeyProtectorResponse_signalsParseFailure();
    void parseKeyProtectorResponse_parsesValidPayload();
    // WaveD-03: a well-formed JSON array with a non-object element (or a bare
    // scalar) must fail closed, not silently drop a protector.
    void parseKeyProtectorResponse_rejectsMalformedElements();
    void parseDetectedVolumes_signalsParseState();
    // WaveD-04: drive letters are validated before entering the PowerShell filter
    // / key filename, so a malformed value cannot inject or escape the directory.
    void buildKeyProtectorScript_rejectsInvalidDriveLetters();
    // R5 p8_appaction-2: the backup destination crosses the elevation boundary and
    // receives plaintext recovery keys, so it is canonicalized + policy-screened.
    void screenBackupLocation_refusesUnsafeDestinations();
    void screenBackupLocation_canonicalizesAcceptedLocation();
    // Recovery-password accounting used by the gate and per-volume file writer.
    void recoveryPasswordHelpers_countAndDetect();
    // Enum formatters the recovery document depends on.
    void formatters_mapKnownCodes();
};

// ============================================================================
// B6-05 -- recovery-key gate
// ============================================================================

void BackupBitlockerKeysActionTests::evaluateKeyGate_requiresRecoveryPassword() {
    // No protectors read at all -> needs admin, fail closed.
    QCOMPARE(Action::evaluateKeyGate(0, 0), KeyGate::NoProtectors);
    // Protectors exist (e.g. TPM-only) but none is a recovery password ->
    // there is nothing usable to back up; must not report success.
    QCOMPARE(Action::evaluateKeyGate(3, 0), KeyGate::NoRecoveryPasswords);
    // At least one recovery password -> proceed.
    QCOMPARE(Action::evaluateKeyGate(3, 1), KeyGate::Ok);
    QCOMPARE(Action::evaluateKeyGate(1, 1), KeyGate::Ok);
}

// ============================================================================
// B6-03 -- unique backup directory naming
// ============================================================================

void BackupBitlockerKeysActionTests::uniqueBackupDirName_distinctPerCounter() {
    const QString ts = QStringLiteral("20260730_141530_123");
    const QString a = Action::uniqueBackupDirName(ts, 0);
    const QString b = Action::uniqueBackupDirName(ts, 1);

    QCOMPARE(a, QStringLiteral("BitLocker_Keys_20260730_141530_123_0"));
    QVERIFY(a != b);  // same-second, same timestamp, different counter -> distinct
    QVERIFY(a.startsWith(QStringLiteral("BitLocker_Keys_")));
}

void BackupBitlockerKeysActionTests::backupTimestamp_hasMillisecondResolution() {
    // yyyyMMdd_HHmmss_zzz -- the trailing _zzz is what defeats same-second
    // collisions; assert the shape rather than an exact (time-dependent) value.
    const QString ts = Action::backupTimestamp();
    const QRegularExpression shape(QStringLiteral("^\\d{8}_\\d{6}_\\d{3}$"));
    QVERIFY2(shape.match(ts).hasMatch(), qPrintable(ts));
}

// ============================================================================
// B6-04 -- key-protector query failure is surfaced
// ============================================================================

void BackupBitlockerKeysActionTests::parseKeyProtectorResponse_signalsParseFailure() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    bool parse_ok = true;  // must be flipped to false
    const QVector<KeyProtectorInfo> result =
        action.parseKeyProtectorResponse(QStringLiteral("{ this is not json"), parse_ok);

    QVERIFY2(!parse_ok, "malformed JSON must report parse failure, not empty success");
    QVERIFY(result.isEmpty());
}

void BackupBitlockerKeysActionTests::parseKeyProtectorResponse_parsesValidPayload() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    const QString json = QStringLiteral(
        "[{\"ProtectorID\":\"{guid-1}\",\"ProtectorType\":3,"
        "\"RecoveryPassword\":\"111111-222222-333333\",\"KeyFileName\":\"\"},"
        "{\"ProtectorID\":\"{guid-2}\",\"ProtectorType\":1,"
        "\"RecoveryPassword\":\"\",\"KeyFileName\":\"\"}]");

    bool parse_ok = false;
    const QVector<KeyProtectorInfo> result = action.parseKeyProtectorResponse(json, parse_ok);

    QVERIFY(parse_ok);
    QCOMPARE(result.size(), 2);
    QCOMPARE(result[0].protector_id, QStringLiteral("{guid-1}"));
    QCOMPARE(result[0].recovery_password, QStringLiteral("111111-222222-333333"));
    QCOMPARE(result[0].protector_type, Action::formatProtectorType(3));
    QVERIFY(result[1].recovery_password.isEmpty());
}

void BackupBitlockerKeysActionTests::parseKeyProtectorResponse_rejectsMalformedElements() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    // A JSON array whose element is not an object must not be coerced to "no
    // protectors" -- it must fail closed so the caller does not omit a key.
    bool parse_ok = true;
    QVector<KeyProtectorInfo> result =
        action.parseKeyProtectorResponse(QStringLiteral("[123, \"nope\"]"), parse_ok);
    QVERIFY2(!parse_ok, "a non-object array element must report parse failure");
    QVERIFY(result.isEmpty());

    // A valid-JSON bare scalar (neither array nor object) is likewise rejected.
    parse_ok = true;
    result = action.parseKeyProtectorResponse(QStringLiteral("\"unexpected\""), parse_ok);
    QVERIFY2(!parse_ok, "a bare JSON scalar must report parse failure");
    QVERIFY(result.isEmpty());
}

void BackupBitlockerKeysActionTests::parseDetectedVolumes_signalsParseState() {
    // CODEX_REVIEW_4 M-B3-20: a failed/garbled detection must not be indistinguishable from a
    // genuine "no BitLocker volumes" result (empty). parse_ok separates the two.
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    bool parse_ok = false;
    QVERIFY(action.parseDetectedVolumes(QString(), parse_ok).isEmpty());
    QVERIFY2(parse_ok, "empty output is a genuine 'no volumes' success");

    parse_ok = true;
    QVERIFY(action.parseDetectedVolumes(QStringLiteral("{ not json"), parse_ok).isEmpty());
    QVERIFY2(!parse_ok, "malformed JSON must fail closed, not read as 'no volumes'");

    parse_ok = true;
    QVERIFY(action.parseDetectedVolumes(QStringLiteral("123"), parse_ok).isEmpty());
    QVERIFY2(!parse_ok, "a JSON scalar (neither array nor object) must fail closed");

    parse_ok = false;
    const QVector<BackupBitlockerKeysAction::VolumeInfo> one =
        action.parseDetectedVolumes(QStringLiteral("{\"DriveLetter\":\"C:\"}"), parse_ok);
    QVERIFY2(parse_ok, "a well-formed object is a parse success");
    QCOMPARE(one.size(), 1);
}

void BackupBitlockerKeysActionTests::buildKeyProtectorScript_rejectsInvalidDriveLetters() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    // Valid drive letters (with or without a colon) produce a real script.
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("C:")).contains(QStringLiteral("C:")));
    QVERIFY(!action.buildKeyProtectorScript(QStringLiteral("D")).isEmpty());

    // Anything else yields an empty script (fail closed): empty, injection
    // attempts, and path-escape sequences must all be refused.
    QVERIFY(action.buildKeyProtectorScript(QString()).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("C:' ; Remove-Item C:\\ #")).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("C:\\..\\..\\evil")).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("CC")).isEmpty());
}

// ============================================================================
// R5 p8_appaction-2 -- backup destination screen
// ============================================================================

void BackupBitlockerKeysActionTests::screenBackupLocation_refusesUnsafeDestinations() {
    const auto refused = [](const QString& location) {
        QString reason;
        const QString result = Action::screenBackupLocation(location, reason);
        // Fail closed: an empty result AND a stated reason, never a silent default.
        return result.isEmpty() && !reason.isEmpty();
    };

    // Absent / blank destination: refused, never coerced to a built-in default.
    QVERIFY(refused(QString()));
    QVERIFY(refused(QStringLiteral("   ")));
    // UNC share: plaintext recovery keys must not be pushed to a remote host.
    QVERIFY(refused(QStringLiteral("\\\\attacker\\share\\keys")));
    QVERIFY(refused(QStringLiteral("//attacker/share/keys")));
    // Mixed separators still form a UNC/device root on Windows.
    QVERIFY(refused(QStringLiteral("\\/attacker/share")));
    // Device namespace.
    QVERIFY(refused(QStringLiteral("\\\\?\\C:\\SAK_Backups")));
    QVERIFY(refused(QStringLiteral("\\\\.\\PhysicalDrive0")));
    // Relative destination: it would resolve against the ELEVATED helper's working
    // directory, which the requesting client neither controls nor sees.
    QVERIFY(refused(QStringLiteral("SAK_Backups")));
    QVERIFY(refused(QStringLiteral("..\\..\\SAK_Backups")));
}

void BackupBitlockerKeysActionTests::screenBackupLocation_canonicalizesAcceptedLocation() {
    QString reason;
    // A plain absolute local path is accepted and returned lexically canonicalized
    // (cleaned, forward-separator form) so the caller writes to a resolved path.
    const QString accepted =
        Action::screenBackupLocation(QStringLiteral("C:\\SAK_Backups\\.\\keys\\"), reason);
    QVERIFY2(reason.isEmpty(), qPrintable(reason));
    QCOMPARE(accepted, QStringLiteral("C:/SAK_Backups/keys"));

    // A traversal-laden but still-local path is normalized rather than passed through raw.
    reason.clear();
    const QString collapsed =
        Action::screenBackupLocation(QStringLiteral("C:\\SAK_Backups\\sub\\..\\keys"), reason);
    QVERIFY2(reason.isEmpty(), qPrintable(reason));
    QCOMPARE(collapsed, QStringLiteral("C:/SAK_Backups/keys"));
}

// ============================================================================
// Recovery-password accounting
// ============================================================================

void BackupBitlockerKeysActionTests::recoveryPasswordHelpers_countAndDetect() {
    QVector<KeyProtectorInfo> protectors;
    KeyProtectorInfo tpm;
    tpm.recovery_password = QString();  // TPM protector, no password
    KeyProtectorInfo rec;
    rec.recovery_password = QStringLiteral("111111-222222-333333");

    protectors << tpm;
    QCOMPARE(Action::countRecoveryPasswords(protectors), 0);
    QVERIFY(!Action::volumeHasRecoveryPassword(protectors));

    protectors << rec;
    QCOMPARE(Action::countRecoveryPasswords(protectors), 1);
    QVERIFY(Action::volumeHasRecoveryPassword(protectors));
}

// ============================================================================
// Enum formatters
// ============================================================================

void BackupBitlockerKeysActionTests::formatters_mapKnownCodes() {
    QCOMPARE(Action::formatProtectorType(3),
             QStringLiteral("Numerical Password (Recovery Password)"));
    QCOMPARE(Action::formatEncryptionMethod(7), QStringLiteral("XTS-AES-256"));
    QCOMPARE(Action::formatVolumeType(0), QStringLiteral("Operating System"));
    // Out-of-range codes fall through to a labelled Unknown, never a crash.
    QVERIFY(Action::formatProtectorType(-1).startsWith(QStringLiteral("Unknown")));
    QVERIFY(Action::formatEncryptionMethod(999).startsWith(QStringLiteral("Unknown")));
}

QTEST_GUILESS_MAIN(BackupBitlockerKeysActionTests)
#include "test_backup_bitlocker_keys_action.moc"
