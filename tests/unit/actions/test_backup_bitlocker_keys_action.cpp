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
    // The sentinel-vs-index-0 boundaries, a real-sized volume, and the per-ELEMENT
    // non-object guard that every all-objects fixture leaves unreached.
    void parseDetectedVolumes_pinsBoundariesAndPerElementGuard();
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

    // A BARE OBJECT -- and this is the ORDINARY production shape, not an edge case. Every payload
    // this file feeds the parser is either an array or a non-array scalar, yet ConvertTo-Json
    // emits a bare object rather than a one-element array whenever a volume has exactly ONE key
    // protector, which is the common case for a TPM-only or single-recovery-password volume. So
    // the object arm is the path most real machines take and it was entirely unobserved: delete
    // it and a single-protector volume parses as EMPTY, silently backing up no key at all.
    parse_ok = false;
    result = action.parseKeyProtectorResponse(kValidFirst, parse_ok);
    QVERIFY2(parse_ok, "a bare protector OBJECT is the single-protector production shape");
    QCOMPARE(result.size(), 1);
    QCOMPARE(result[0].protector_id, QStringLiteral("{guid-1}"));
    QCOMPARE(result[0].recovery_password, QStringLiteral("111111-222222-333333"));

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
        QStringLiteral("[{\"DriveLetter\":\"C:\","
                       "\"DeviceID\":\"Volume{11111111-2222-3333-4444-555555555555}\","
                       "\"VolumeLabel\":\"OS-Disk\",\"ProtectionStatus\":1,\"LockStatus\":0,"
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
    // Two of VolumeInfo's nine mapped fields were asserted NOWHERE in the repository, though both
    // reach the technician: device_id prints as "Device ID:" in the recovery document and as
    // "device_id" in bitlocker_keys.json, and volume_label appears in the volume header, the scan
    // summary and every per-volume key file. They are how a technician matches a recovery key to
    // a physical disk, so a dropped or cross-wired assignment is a real recovery failure.
    QCOMPARE(many[0].device_id, QStringLiteral("Volume{11111111-2222-3333-4444-555555555555}"));
    QCOMPARE(many[0].volume_label, QStringLiteral("OS-Disk"));
    QCOMPARE(many[1].drive_letter, QStringLiteral("D:"));
    QCOMPARE(many[1].protection_status, QStringLiteral("Off"));
    // Control: element 1 supplies neither key, so both stay empty -- proving the values above came
    // from element 0's OWN keys rather than a constant or a carry-over between records.
    QVERIFY2(many[1].device_id.isEmpty(), qPrintable(many[1].device_id));
    QVERIFY2(many[1].volume_label.isEmpty(), qPrintable(many[1].volume_label));
}

void BackupBitlockerKeysActionTests::parseDetectedVolumes_pinsBoundariesAndPerElementGuard() {
    BackupBitlockerKeysAction action(QStringLiteral("C:/temp/does-not-matter"));

    // Both sentinel-vs-index-0 boundaries, fed from the OTHER side. The slot above states the
    // contract as "omitted fields must reach the -1 sentinels ... never index 0", but only the
    // ABSENT direction was ever supplied: EncryptionPct was never 0, so `enc_pct >= 0` agreed
    // with `> 0` on every input it saw, and LockStatus was never 1, so kLockStatusLabels[1]
    // ("Locked") was never rendered by any test. A volume genuinely reporting 0% converted and
    // LOCKED must say so -- "N/A"/"Unknown" mean "we could not read this field", which is a very
    // different thing to tell someone holding a recovery key.
    bool parse_ok = false;
    const QVector<BackupBitlockerKeysAction::VolumeInfo> zero_locked = action.parseDetectedVolumes(
        QStringLiteral("{\"DriveLetter\":\"E:\",\"EncryptionPct\":0,\"LockStatus\":1}"), parse_ok);
    QVERIFY2(parse_ok, "a well-formed object is a parse success");
    QCOMPARE(zero_locked.size(), 1);
    QCOMPARE(zero_locked[0].encryption_percentage, QStringLiteral("0%"));
    QCOMPARE(zero_locked[0].lock_status, QStringLiteral("Locked"));

    // A REAL volume size. 1024 was the only size the suite ever asserted, and it fits in an int --
    // at that value the double and int accessors are indistinguishable, so the very reason the
    // production read is `static_cast<qint64>(obj["SizeBytes"].toDouble())` went untested. A real
    // BitLocker volume is hundreds of gigabytes: 500107862016 is past INT_MAX, so an int accessor
    // truncates it and the recovery document reports a fraction of the disk.
    parse_ok = false;
    const QVector<BackupBitlockerKeysAction::VolumeInfo> large = action.parseDetectedVolumes(
        QStringLiteral("{\"DriveLetter\":\"F:\",\"SizeBytes\":500107862016}"), parse_ok);
    QVERIFY2(parse_ok, "a well-formed object is a parse success");
    QCOMPARE(large.size(), 1);
    QCOMPARE(large[0].volume_size_bytes, Q_INT64_C(500'107'862'016));

    // The per-ELEMENT non-object guard, which every all-objects fixture leaves unreached.
    // parseDetectedVolumes fails closed through three independent arms -- the JSON parse error,
    // the whole-document shape guard, and this one -- and only the first two were exercised. The
    // production comment says this arm is meant to behave "exactly like parseKeyProtectorResponse",
    // an equivalence nothing tested.
    parse_ok = true;
    QVERIFY(action.parseDetectedVolumes(QStringLiteral("[123]"), parse_ok).isEmpty());
    QVERIFY2(!parse_ok, "a non-object array element must fail closed");

    // ... and in TRAILING position, where "rejected" and "never accumulated anything" are
    // distinguishable: the whole response is discarded, not returned half-read. Same shape the
    // sibling parser already pins.
    parse_ok = true;
    const QVector<BackupBitlockerKeysAction::VolumeInfo> partial =
        action.parseDetectedVolumes(QStringLiteral("[{\"DriveLetter\":\"C:\"}, 123]"), parse_ok);
    QVERIFY2(!parse_ok, "a trailing non-object element must fail closed");
    QVERIFY2(partial.isEmpty(),
             "a failed parse must discard already-accumulated volumes, not return them");
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

    // A lowercase letter is accepted too: the validator is [A-Za-z], and PowerShell drive letters
    // arrive in either case, so pinning only uppercase would let the class narrow to [A-Z] and
    // fail-closed every lowercase volume.
    const QString lower_form = action.buildKeyProtectorScript(QStringLiteral("e"));
    QVERIFY2(lower_form.contains(QStringLiteral(R"(-Filter "DriveLetter='e'")")),
             qPrintable(lower_form));

    // Anything else yields an empty script (fail closed): empty, injection
    // attempts, and path-escape sequences must all be refused.
    QVERIFY(action.buildKeyProtectorScript(QString()).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("C:' ; Remove-Item C:\\ #")).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("C:\\..\\..\\evil")).isEmpty());
    QVERIFY(action.buildKeyProtectorScript(QStringLiteral("CC")).isEmpty());

    // NEAR MISSES of the accepted forms. Every rejection above is disqualified by something
    // coarse -- a second letter, a backslash, or a quote plus spaces -- so none of them constrains
    // the validator's SHAPE (`^[A-Za-z]:?$`, anchored at both ends, with at most one colon).
    // Notably the comment above names DriveLetter='C::' as the malformed filter being guarded
    // against, and then nothing ever fed "C::". The value is interpolated into a DOUBLE-quoted
    // PowerShell argument, where $( ) still expands, so a quote-only blacklist is not equivalent
    // to the regex -- which is exactly what these pin.
    const QStringList near_misses{
        QStringLiteral("C::"),          // the doubled colon the comment above names
        QStringLiteral("C:\\"),         // a trailing separator
        QStringLiteral(" C:"),          // unanchored at the front
        QStringLiteral("C: "),          // unanchored at the end
        QStringLiteral("C:$(whoami)"),  // subexpression: expands inside a double-quoted argument
        QStringLiteral("C:`n"),         // an escape sequence, no quote involved
        QStringLiteral(":"),            // a colon with no letter
        QStringLiteral("1:"),           // a digit, not [A-Za-z]
        QStringLiteral("C:\nD:"),       // a newline-separated second value
    };
    for (const QString& candidate : near_misses) {
        QVERIFY2(action.buildKeyProtectorScript(candidate).isEmpty(), qPrintable(candidate));
    }
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

    // The "no recovery password" fixtures above are DEFAULT-CONSTRUCTED, i.e. null, QStrings --
    // but what production actually holds comes from obj["RecoveryPassword"].toString() applied to
    // "RecoveryPassword":"", which is empty YET NON-NULL. Both helpers test isEmpty(), and no
    // fixture anywhere made isEmpty() and isNull() disagree, so nothing in the suite decided which
    // predicate they use: switch either to !isNull() and every assertion above stays green while a
    // USB-only volume is counted as HAVING a recovery password -- passing the key gate and writing
    // a key file with nothing in it. Parse the value the way production does rather than
    // hand-building it, so the fixture cannot drift from the real shape.
    BackupBitlockerKeysAction parser(QStringLiteral("C:/temp/does-not-matter"));
    bool parse_ok = false;
    const QVector<KeyProtectorInfo> from_wmi = parser.parseKeyProtectorResponse(
        QStringLiteral("[{\"ProtectorID\":\"{guid-2}\",\"ProtectorType\":2,"
                       "\"RecoveryPassword\":\"\",\"KeyFileName\":\"0A1B2C3D-EXT.BEK\"}]"),
        parse_ok);
    QVERIFY(parse_ok);
    QCOMPARE(from_wmi.size(), 1);
    QVERIFY2(from_wmi[0].recovery_password.isEmpty(), "control: the External Key record has none");
    QVERIFY2(!from_wmi[0].recovery_password.isNull(),
             "control: a JSON \"\" must arrive EMPTY BUT NON-NULL, or this fixture cannot tell "
             "isEmpty() from isNull() any better than the hand-built ones above");
    QCOMPARE(Action::countRecoveryPasswords(from_wmi), 0);
    QVERIFY(!Action::volumeHasRecoveryPassword(from_wmi));
}

// ============================================================================
// Enum formatters
// ============================================================================

void BackupBitlockerKeysActionTests::formatters_mapKnownCodes() {
    // Both tables in FULL. kProtectorTypes holds 11 entries and kEncryptionMethods 8, and the
    // suite pinned two protector codes and one encryption code between them. lookupCodeDescription
    // matches on the explicit m_code field, so a whole-table shift would have been caught -- but a
    // SINGLE mislabelled entry away from the sampled codes was invisible, and every one of these
    // strings is printed verbatim to the technician as "Type:" and "Encryption Method:" in the
    // document used to recover an encrypted disk. formatVolumeType below was already pinned
    // exhaustively, so the file disagreed with itself about how much of a table to prove.
    const QVector<QPair<int, QString>> protector_types{
        {0, QStringLiteral("Unknown or Other")},
        {1, QStringLiteral("TPM")},
        {2, QStringLiteral("External Key (USB)")},
        {3, QStringLiteral("Numerical Password (Recovery Password)")},
        {4, QStringLiteral("TPM + PIN")},
        {5, QStringLiteral("TPM + Startup Key")},
        {6, QStringLiteral("TPM + PIN + Startup Key")},
        {7, QStringLiteral("Public Key (Certificate)")},
        {8, QStringLiteral("Passphrase")},
        {9, QStringLiteral("TPM + Certificate")},
        {10, QStringLiteral("Clear Key (Unprotected)")},
    };
    for (const auto& [code, description] : protector_types) {
        QCOMPARE(Action::formatProtectorType(code), description);
    }

    const QVector<QPair<int, QString>> encryption_methods{
        {0, QStringLiteral("None")},
        {1, QStringLiteral("AES-128 with Diffuser")},
        {2, QStringLiteral("AES-256 with Diffuser")},
        {3, QStringLiteral("AES-128")},
        {4, QStringLiteral("AES-256")},
        {5, QStringLiteral("Hardware Encryption")},
        {6, QStringLiteral("XTS-AES-128")},
        {7, QStringLiteral("XTS-AES-256")},
    };
    for (const auto& [code, description] : encryption_methods) {
        QCOMPARE(Action::formatEncryptionMethod(code), description);
    }
    // One past each table's last mapped code, so the tables cannot silently GROW an entry either.
    QCOMPARE(Action::formatProtectorType(11), QStringLiteral("Unknown (11)"));
    QCOMPARE(Action::formatEncryptionMethod(8), QStringLiteral("Unknown (8)"));

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
