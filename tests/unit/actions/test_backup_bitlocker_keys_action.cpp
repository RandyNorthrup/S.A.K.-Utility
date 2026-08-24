// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_backup_bitlocker_keys_action.cpp
/// @brief Unit tests for the fail-closed seams of BackupBitlockerKeysAction
///        (Codex remediation batch B6-01..06). Verifies the recovery-key gate,
///        unique backup-directory naming, the key-protector parse failure
///        signal, and the recovery-password accounting helpers.

#include "sak/actions/backup_bitlocker_keys_action.h"

#include <QDateTime>
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
    QCOMPARE(b, QStringLiteral("BitLocker_Keys_20260730_141530_123_1"));
    // The counter is a process-wide monotonic std::atomic<unsigned>
    // (backup_bitlocker_keys_action.cpp :906-908) and is the ONLY tiebreaker when two backups start
    // in the same millisecond, so the FULL unsigned range must round-trip into the name. Pin a
    // multi-digit and a max-unsigned counter: a truncating/wrapping suffix (counter % 10, a
    // narrowing cast) would otherwise reopen an earlier name space, and createBackupDirectory's
    // mkdir at :922 then fails a valid backup.
    QCOMPARE(Action::uniqueBackupDirName(ts, 10u),
             QStringLiteral("BitLocker_Keys_20260730_141530_123_10"));
    QCOMPARE(Action::uniqueBackupDirName(ts, 4'294'967'295u),
             QStringLiteral("BitLocker_Keys_20260730_141530_123_4294967295"));
    QVERIFY(a != b);  // same-second, same timestamp, different counter -> distinct
    QVERIFY(a.startsWith(QStringLiteral("BitLocker_Keys_")));
}

void BackupBitlockerKeysActionTests::backupTimestamp_hasMillisecondResolution() {
    // yyyyMMdd_HHmmss_zzz -- the trailing _zzz is what defeats same-second
    // collisions; assert the shape rather than an exact (time-dependent) value.
    const QString ts = Action::backupTimestamp();
    const QRegularExpression shape(QStringLiteral("^\\d{8}_\\d{6}_\\d{3}$"));
    QVERIFY2(shape.match(ts).hasMatch(), qPrintable(ts));

    // The digits are the LIVE wall clock in that exact field order, not a canned string: the
    // value must round-trip back through the documented format to a datetime within seconds
    // of now.
    const QDateTime parsed = QDateTime::fromString(ts, QStringLiteral("yyyyMMdd_HHmmss_zzz"));
    QVERIFY2(parsed.isValid(), qPrintable(ts));
    QVERIFY2(qAbs(parsed.msecsTo(QDateTime::currentDateTime())) < 5000, qPrintable(ts));

    // The trailing field is a live MILLISECOND value, not a constant. That is the whole point
    // of the field: a frozen sub-second field (e.g. a literal "000" in the format string, which
    // Qt emits verbatim and which still matches the shape regex above) makes two backups started
    // in the same second collide on the directory name -- and since the dedup counter is
    // process-local, the second backup then fails closed on mkdir.
    const QString first_ms = ts.right(3);
    bool ms_varied = false;
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + 2000;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (Action::backupTimestamp().right(3) != first_ms) {
            ms_varied = true;
            break;
        }
    }
    QVERIFY2(ms_varied,
             "backupTimestamp() sub-second field never changed over 2s: same-second "
             "backups would produce the same directory name");
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

    // Record 1 is an External Key (USB) protector: it carries NO recovery password but DOES
    // carry a key file name, so every field is populated from a distinct JSON key with a
    // distinguishable value.
    const QString json = QStringLiteral(
        "[{\"ProtectorID\":\"{guid-1}\",\"ProtectorType\":3,"
        "\"RecoveryPassword\":\"111111-222222-333333\",\"KeyFileName\":\"\"},"
        "{\"ProtectorID\":\"{guid-2}\",\"ProtectorType\":2,"
        "\"RecoveryPassword\":\"\",\"KeyFileName\":\"0A1B2C3D-EXT.BEK\"}]");

    bool parse_ok = false;
    const QVector<KeyProtectorInfo> result = action.parseKeyProtectorResponse(json, parse_ok);

    QVERIFY(parse_ok);
    QCOMPARE(result.size(), 2);

    // Record 0 -- all four fields, each read back from its own source key.
    QCOMPARE(result[0].protector_id, QStringLiteral("{guid-1}"));
    QCOMPARE(result[0].protector_type, Action::formatProtectorType(3));
    QCOMPARE(result[0].recovery_password, QStringLiteral("111111-222222-333333"));
    // Not cross-wired from RecoveryPassword: the 48-digit secret must never land in
    // key_file_name, which the recovery document prints as "Key File:".
    QVERIFY2(result[0].key_file_name.isEmpty(), qPrintable(result[0].key_file_name));

    // Record 1 -- identity and type pinned too, so a positional mix-up is caught.
    QCOMPARE(result[1].protector_id, QStringLiteral("{guid-2}"));
    QCOMPARE(result[1].protector_type, QStringLiteral("External Key (USB)"));
    QVERIFY(result[1].recovery_password.isEmpty());
    QCOMPARE(result[1].key_file_name, QStringLiteral("0A1B2C3D-EXT.BEK"));
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

    // Non-degeneracy control: the leading object below IS well-formed on its own, so a discarded
    // result cannot be blamed on an unparseable first element.
    const QString kValidFirst = QStringLiteral(
        "{\"ProtectorID\":\"{guid-1}\",\"ProtectorType\":3,"
        "\"RecoveryPassword\":\"111111-222222-333333\",\"KeyFileName\":\"\"}");
    parse_ok = false;
    result = action.parseKeyProtectorResponse(QStringLiteral("[%1]").arg(kValidFirst), parse_ok);
    QVERIFY2(parse_ok, "control: a lone well-formed protector must parse");
    QCOMPARE(result.size(), 1);

    // The guard rejects the WHOLE response, it does not return what it has so far: a malformed
    // entry AFTER a valid one must discard the partially-parsed list, so no half-read protector
    // set can escape alongside parse_ok == false. Every element the existing cases reject is in
    // FIRST position, where "rejected" and "never accumulated anything" are indistinguishable.
    parse_ok = true;
    result = action.parseKeyProtectorResponse(QStringLiteral("[%1, 123]").arg(kValidFirst),
                                              parse_ok);
    QVERIFY2(!parse_ok, "a trailing non-object element must report parse failure");
    QVERIFY2(result.isEmpty(),
             "a failed parse must discard already-accumulated protectors, not return them");
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
    // The success path must MAP fields, not merely count records: drive_letter is what
    // routes getKeyProtectors/buildKeyProtectorScript per volume.
    QCOMPARE(one[0].drive_letter, QStringLiteral("C:"));
    // Omitted status fields must reach the -1 sentinels and read "Unknown"/"N/A",
    // never index 0 ("Off"/"Unlocked"/"0%") -- an encrypted volume must not be
    // documented as unprotected just because the field was absent.
    QCOMPARE(one[0].protection_status, QStringLiteral("Unknown"));
    QCOMPARE(one[0].lock_status, QStringLiteral("Unknown"));
    QCOMPARE(one[0].encryption_percentage, QStringLiteral("N/A"));

    // A JSON ARRAY is the shape ConvertTo-Json emits on a multi-volume machine;
    // that arm must yield one record per element, fully mapped.
    parse_ok = false;
    const QVector<BackupBitlockerKeysAction::VolumeInfo> many = action.parseDetectedVolumes(
        QStringLiteral("[{\"DriveLetter\":\"C:\",\"ProtectionStatus\":1,\"LockStatus\":0,"
                       "\"EncryptionPct\":100,\"EncryptionMethod\":7,\"VolumeType\":0,"
                       "\"SizeBytes\":1024},{\"DriveLetter\":\"D:\",\"ProtectionStatus\":0}]"),
        parse_ok);
    QVERIFY2(parse_ok, "a JSON array of objects is the multi-volume success shape");
    QCOMPARE(many.size(), 2);
    QCOMPARE(many[0].drive_letter, QStringLiteral("C:"));
    QCOMPARE(many[0].protection_status, QStringLiteral("On"));
    QCOMPARE(many[0].lock_status, QStringLiteral("Unlocked"));
    QCOMPARE(many[0].encryption_percentage, QStringLiteral("100%"));
    QCOMPARE(many[0].encryption_method, QStringLiteral("XTS-AES-256"));
    QCOMPARE(many[0].volume_type, QStringLiteral("Operating System"));
    QCOMPARE(many[0].volume_size_bytes, Q_INT64_C(1024));
    QCOMPARE(many[1].drive_letter, QStringLiteral("D:"));
    QCOMPARE(many[1].protection_status, QStringLiteral("Off"));
}

void BackupBitlockerKeysActionTests::buildKeyProtectorScript_rejectsInvalidDriveLetters() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    // Valid drive letters (with or without a colon) produce a real script.
    // The drive letter is interpolated verbatim into the WMI filter clause, so pin the EXACT
    // clause for both accepted forms -- "C: appears somewhere in the script" would still pass
    // for a malformed filter such as DriveLetter='C::'.
    const QString colon_form = action.buildKeyProtectorScript(QStringLiteral("C:"));
    QVERIFY2(colon_form.contains(QStringLiteral(R"(-Filter "DriveLetter='C:'")")),
             qPrintable(colon_form));
    const QString bare_form = action.buildKeyProtectorScript(QStringLiteral("D"));
    QVERIFY2(bare_form.contains(QStringLiteral(R"(-Filter "DriveLetter='D'")")),
             qPrintable(bare_form));

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
    // Fail closed: an empty result AND the SPECIFIC diagnostic for the guard that fired. Three
    // guards refuse here (blank, UNC/device literal, relative) and the caller shows whichever
    // text came back to the technician, so the message is the contract -- not merely "somebody
    // said no". "A non-empty reason" alone is satisfied by any guard answering for any other.
    const auto refusalFor = [](const QString& location) {
        QString reason;
        const QString result = Action::screenBackupLocation(location, reason);
        // An accepted location can never match an expected refusal string below.
        return result.isEmpty() ? reason : QStringLiteral("ACCEPTED: ") + result;
    };

    // Absent / blank destination: refused as "no location supplied", never coerced to a built-in
    // default and never mislabelled as merely relative (a blank path is ALSO
    // QFileInfo::isRelative(), so this pins the dedicated blank guard).
    QCOMPARE(refusalFor(QString()), QStringLiteral("No backup location was supplied."));
    QCOMPARE(refusalFor(QStringLiteral("   ")), QStringLiteral("No backup location was supplied."));

    // UNC share: plaintext recovery keys must not be pushed to a remote host, and the technician
    // must be told it was the NETWORK screen that refused it.
    QCOMPARE(refusalFor(QStringLiteral("\\\\attacker\\share\\keys")),
             QStringLiteral("Refused a network/device backup location: \\\\attacker\\share\\keys"));
    QCOMPARE(refusalFor(QStringLiteral("//attacker/share/keys")),
             QStringLiteral("Refused a network/device backup location: //attacker/share/keys"));
    // Mixed separators still form a UNC/device root on Windows.
    QCOMPARE(refusalFor(QStringLiteral("\\/attacker/share")),
             QStringLiteral("Refused a network/device backup location: \\/attacker/share"));
    // Device namespace.
    QCOMPARE(refusalFor(QStringLiteral("\\\\?\\C:\\SAK_Backups")),
             QStringLiteral("Refused a network/device backup location: \\\\?\\C:\\SAK_Backups"));
    QCOMPARE(refusalFor(QStringLiteral("\\\\.\\PhysicalDrive0")),
             QStringLiteral("Refused a network/device backup location: \\\\.\\PhysicalDrive0"));

    // Relative destination: it would resolve against the ELEVATED helper's working directory,
    // which the requesting client neither controls nor sees.
    QCOMPARE(refusalFor(QStringLiteral("SAK_Backups")),
             QStringLiteral("Backup location must be an absolute path: SAK_Backups"));
    QCOMPARE(refusalFor(QStringLiteral("..\\..\\SAK_Backups")),
             QStringLiteral("Backup location must be an absolute path: ..\\..\\SAK_Backups"));
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

    // The count is a distinct quantity from the boolean: it accumulates into
    // total_recovery_passwords (:717) and is reported to the technician (:1039,
    // :1049), whereas the boolean only gates the per-volume key file (:1204).
    // Pin a second recovery password alongside a second passwordless protector
    // so the count must be 2 while the boolean is unchanged -- this kills both a
    // `volumeHasRecoveryPassword(protectors) ? 1 : 0` collapse and a
    // `protectors.size() - 1` miscount.
    KeyProtectorInfo rec2;
    rec2.recovery_password = QStringLiteral("444444-555555-666666");
    KeyProtectorInfo tpm2;
    tpm2.recovery_password = QString();
    protectors << rec2 << tpm2;
    QCOMPARE(Action::countRecoveryPasswords(protectors), 2);
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
    QCOMPARE(Action::formatVolumeType(1), QStringLiteral("Fixed Data"));
    QCOMPARE(Action::formatVolumeType(2), QStringLiteral("Removable Data"));
    // Out-of-range codes fall through to a labelled Unknown that ECHOES the code: the unmapped
    // WMI enum value is the whole diagnostic value in the recovery document, so pin the exact
    // rendering, not merely the "Unknown" prefix.
    QCOMPARE(Action::formatProtectorType(-1), QStringLiteral("Unknown (-1)"));
    QCOMPARE(Action::formatEncryptionMethod(999), QStringLiteral("Unknown (999)"));
    QCOMPARE(Action::formatVolumeType(42), QStringLiteral("Unknown (42)"));
}

QTEST_GUILESS_MAIN(BackupBitlockerKeysActionTests)
#include "test_backup_bitlocker_keys_action.moc"
