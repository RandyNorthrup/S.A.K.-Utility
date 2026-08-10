// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_bitlocker_keys_action.cpp
/// @brief Implements BitLocker recovery key backup to a specified location

#include "sak/actions/backup_bitlocker_keys_action.h"

#include "sak/action_constants.h"
#include "sak/app_action_guards.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSysInfo>
#include <QTextStream>

#include <algorithm>
#include <array>
#include <atomic>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

// ============================================================================
// Construction
// ============================================================================

BackupBitlockerKeysAction::BackupBitlockerKeysAction(const QString& backup_location,
                                                     QObject* parent)
    : QuickAction(parent), m_backup_location(backup_location) {}

// ============================================================================
// Static Helpers -- WMI Enum Formatting
// ============================================================================

namespace {

struct CodeDescriptionEntry {
    int code;
    const char* description;
};

static constexpr CodeDescriptionEntry kEncryptionMethods[] = {
    {0, "None"},
    {1, "AES-128 with Diffuser"},
    {2, "AES-256 with Diffuser"},
    {3, "AES-128"},
    {4, "AES-256"},
    {5, "Hardware Encryption"},
    {6, "XTS-AES-128"},
    {7, "XTS-AES-256"},
};

static constexpr CodeDescriptionEntry kProtectorTypes[] = {
    {0, "Unknown or Other"},
    {1, "TPM"},
    {2, "External Key (USB)"},
    {3, "Numerical Password (Recovery Password)"},
    {4, "TPM + PIN"},
    {5, "TPM + Startup Key"},
    {6, "TPM + PIN + Startup Key"},
    {7, "Public Key (Certificate)"},
    {8, "Passphrase"},
    {9, "TPM + Certificate"},
    {10, "Clear Key (Unprotected)"},
};

constexpr int kVolumeTypeOperatingSystem = 0;
constexpr int kVolumeTypeFixedData = 1;
constexpr int kVolumeTypeRemovableData = 2;
constexpr int kEstimatedDurationPerVolumeMs = kTimeoutProcessShortMs;
constexpr int kKeyRetrievalProgressSpan = 40;
constexpr qsizetype kBitlockerSummaryLineReserve = 14;
constexpr int kVolumeSizeDisplayPrecision = 2;

/// Length of a bare drive-letter root in native form ("X:\\"): drive letter, ':' and separator.
constexpr int kDriveRootLength = 3;

/// GetDriveTypeW() value for a mapped network drive. Named so the classification
/// exists on every platform; the value is checked against the Win32 macro below.
constexpr unsigned int kWin32DriveRemote = 4;
#ifdef Q_OS_WIN
static_assert(kWin32DriveRemote == DRIVE_REMOTE, "DRIVE_REMOTE value drifted");
#endif

/// @brief Win32 drive type of @p path's "X:\" root, or 0 (DRIVE_UNKNOWN) when the path
///        has no drive-letter root or the platform has no such concept. GetDriveTypeW
///        answers from the local mount table, so it does not itself touch the share.
[[nodiscard]] unsigned int rootDriveType(const QString& path) {
#ifdef Q_OS_WIN
    const QString root =
        QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).left(kDriveRootLength);
    if (root.size() < kDriveRootLength || root[1] != QLatin1Char(':')) {
        return 0;
    }
    return GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16()));
#else
    Q_UNUSED(path)
    return 0;
#endif
}

static constexpr auto kDetectEncryptedVolumesScript = R"PS(
try {
    $vols = Get-WmiObject -Namespace "Root\CIMv2\Security\MicrosoftVolumeEncryption" `
        -Class Win32_EncryptableVolume -ErrorAction Stop

    $results = @()
    foreach ($vol in $vols) {
        $status = $vol.GetProtectionStatus()
        $encMethod = $vol.GetEncryptionMethod()
        $convStatus = $vol.GetConversionStatus()
        $lockStatus = $vol.GetLockStatus()

        $driveInfo = Get-Volume -DriveLetter ($vol.DriveLetter -replace ':',
            '') -ErrorAction SilentlyContinue

        $obj = @{
            DriveLetter      = $vol.DriveLetter
            DeviceID         = $vol.DeviceID
            VolumeLabel      = if ($driveInfo) { $driveInfo.FileSystemLabel } else { "" }
            VolumeType       = $vol.VolumeType
            ProtectionStatus = $status.ProtectionStatus
            EncryptionMethod = $encMethod.EncryptionMethod
            EncryptionPct    = $convStatus.EncryptionPercentage
            LockStatus       = $lockStatus.LockStatus
            SizeBytes        = if ($driveInfo) { $driveInfo.Size } else { 0 }
        }
        $results += $obj
    }
    $results | ConvertTo-Json -Depth 3
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
)PS";

static constexpr auto kRestrictPermissionsScriptTemplate = R"PS(
try {
    # Fail closed on ANY non-terminating error too: without this, a failed Set-Acl /
    # Get-ChildItem / Get-Acl would leave a permissive ACL in place yet still fall through
    # to Write-Output "SUCCESS", reporting plaintext recovery keys as secured when they are
    # not. With Stop, any such failure throws into catch -> exit 1 -> fail closed.
    $ErrorActionPreference = 'Stop'
    $path = '%1'

    # Disable inheritance and remove inherited ACEs.
    # -LiteralPath (not -Path): the backup path may contain [ or ] which -Path
    # treats as wildcards, so -Path could target the wrong file or nothing.
    $acl = Get-Acl -LiteralPath $path
    $acl.SetAccessRuleProtection($true, $false)

    # Clear all existing rules
    $acl.Access | ForEach-Object { $acl.RemoveAccessRule($_) | Out-Null }

    # Grant current user Full Control
    $currentUser = [System.Security.Principal.WindowsIdentity]::GetCurrent().Name
    $userRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        $currentUser, 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($userRule)

    # Grant Administrators Full Control
    $adminRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        'BUILTIN\Administrators', 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($adminRule)

    # Grant SYSTEM Full Control (needed for Windows services)
    $systemRule = New-Object System.Security.AccessControl.FileSystemAccessRule(
        'NT AUTHORITY\SYSTEM', 'FullControl', 'ContainerInherit,ObjectInherit', 'None', 'Allow')
    $acl.AddAccessRule($systemRule)

    Set-Acl -LiteralPath $path -AclObject $acl

    # Apply same ACL to all child items
    Get-ChildItem -LiteralPath $path -Recurse -Force | ForEach-Object {
        Set-Acl -LiteralPath $_.FullName -AclObject $acl
    }

    Write-Output "SUCCESS"
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
)PS";

static constexpr auto kKeyProtectorScriptTemplate = R"PS(
try {
    $vol = Get-WmiObject -Namespace "Root\CIMv2\Security\MicrosoftVolumeEncryption" `
        -Class Win32_EncryptableVolume -Filter "DriveLetter='%1'" -ErrorAction Stop

    if (-not $vol) {
        Write-Error "Volume %1 not found"
        exit 1
    }

    $kpResult = $vol.GetKeyProtectors(0)
    if ($kpResult.ReturnValue -ne 0) {
        # A failed enumeration must NOT be coerced to "no protectors" (which would silently
        # drop this volume's recovery key from the backup); fail the query so C++ fails closed.
        Write-Error "GetKeyProtectors failed for %1 (rc=$($kpResult.ReturnValue))"
        exit 1
    }
    $protectorIds = $kpResult.VolumeKeyProtectorID
    if (-not $protectorIds) {
        @() | ConvertTo-Json
        exit 0
    }

    $results = @()
    foreach ($id in $protectorIds) {
        $typeResult = $vol.GetKeyProtectorType($id)
        if ($typeResult.ReturnValue -ne 0) {
            Write-Error "GetKeyProtectorType failed for protector $id"
            exit 1
        }
        $type = $typeResult.KeyProtectorType

        $recoveryPassword = ""
        if ($type -eq 3) {
            $pwResult = $vol.GetKeyProtectorNumericalPassword($id)
            if ($pwResult.ReturnValue -ne 0) {
                # A recovery-password protector we cannot read must fail the whole query
                # rather than surface an empty password that the backup would treat as
                # "this volume has no recovery key".
                Write-Error "GetKeyProtectorNumericalPassword failed for protector $id"
                exit 1
            }
            $recoveryPassword = $pwResult.NumericalPassword
        }

        $keyFileName = ""
        if ($type -eq 2) {
            $fnResult = $vol.GetKeyProtectorFileName($id)
            if ($fnResult.ReturnValue -eq 0) {
                $keyFileName = $fnResult.FileName
            }
        }

        $obj = @{
            ProtectorID      = $id
            ProtectorType    = $type
            RecoveryPassword = $recoveryPassword
            KeyFileName      = $keyFileName
        }
        $results += $obj
    }
    $results | ConvertTo-Json -Depth 3
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
)PS";

template <std::size_t N>
QString lookupCodeDescription(const CodeDescriptionEntry (&table)[N], int code) {
    auto it = std::find_if(std::begin(table), std::end(table), [code](const auto& e) {
        return e.code == code;
    });
    if (it != std::end(table)) {
        return QString::fromLatin1(it->description);
    }
    return QString("Unknown (%1)").arg(code);
}

// A BitLocker drive letter from Win32_EncryptableVolume is a single ASCII
// letter, optionally with a trailing colon (e.g. "C:"). Anything else is
// rejected so a malformed value can never break out of the single-quoted
// PowerShell filter or escape the backup directory when used in a filename.
bool isValidDriveLetter(const QString& drive_letter) {
    static const QRegularExpression re(QStringLiteral("^[A-Za-z]:?$"));
    return re.match(drive_letter).hasMatch();
}

}  // namespace

QString BackupBitlockerKeysAction::formatEncryptionMethod(int method_code) {
    if (method_code < 0) {
        return QString("Unknown (%1)").arg(method_code);
    }
    return lookupCodeDescription(kEncryptionMethods, method_code);
}

QString BackupBitlockerKeysAction::formatProtectorType(int type_code) {
    if (type_code < 0) {
        return QString("Unknown (%1)").arg(type_code);
    }
    return lookupCodeDescription(kProtectorTypes, type_code);
}

QString BackupBitlockerKeysAction::formatVolumeType(int type_code) {
    switch (type_code) {
    case kVolumeTypeOperatingSystem:
        return "Operating System";
    case kVolumeTypeFixedData:
        return "Fixed Data";
    case kVolumeTypeRemovableData:
        return "Removable Data";
    default:
        return QString("Unknown (%1)").arg(type_code);
    }
}

QString BackupBitlockerKeysAction::backupTimestamp() {
    // Millisecond resolution: two backups started in the same second would
    // otherwise produce the same directory name and clobber each other.
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss_zzz");
}

QString BackupBitlockerKeysAction::uniqueBackupDirName(const QString& timestamp, unsigned counter) {
    return QStringLiteral("BitLocker_Keys_%1_%2").arg(timestamp).arg(counter);
}

BackupBitlockerKeysAction::KeyGate BackupBitlockerKeysAction::evaluateKeyGate(
    int total_keys_found, int total_recovery_passwords) {
    if (total_keys_found <= 0) {
        return KeyGate::NoProtectors;
    }
    if (total_recovery_passwords <= 0) {
        return KeyGate::NoRecoveryPasswords;
    }
    return KeyGate::Ok;
}

// ============================================================================
// Volume Detection -- WMI Queries via PowerShell
// ============================================================================

namespace {

constexpr std::array<const char*, 3> kProtectionStatusLabels = {"Off", "On", "Unknown"};
constexpr std::array<const char*, 2> kLockStatusLabels = {"Unlocked", "Locked"};

QString protectionStatusLabel(int status) {
    if (status >= 0 && status < static_cast<int>(kProtectionStatusLabels.size())) {
        return QString::fromLatin1(kProtectionStatusLabels[status]);
    }
    return QStringLiteral("Unknown");
}

QString lockStatusLabel(int status) {
    if (status >= 0 && status < static_cast<int>(kLockStatusLabels.size())) {
        return QString::fromLatin1(kLockStatusLabels[status]);
    }
    return QStringLiteral("Unknown");
}

}  // namespace

QVector<BackupBitlockerKeysAction::VolumeInfo> BackupBitlockerKeysAction::parseDetectedVolumes(
    const QString& output, bool& parse_ok) {
    parse_ok = false;
    QVector<VolumeInfo> volumes;

    if (output.isEmpty()) {
        parse_ok = true;  // The query ran and the system genuinely reports no BitLocker volumes.
        return volumes;
    }

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        Q_EMIT logMessage("Failed to parse BitLocker volume data: " + parse_error.errorString());
        return volumes;  // parse_ok stays false: a parse failure is not "no volumes".
    }
    if (!doc.isArray() && !doc.isObject()) {
        Q_EMIT logMessage(
            "BitLocker volume data was neither an array nor an object; failing closed");
        return volumes;  // parse_ok stays false.
    }

    QJsonArray volume_array;
    if (doc.isArray()) {
        volume_array = doc.array();
    } else if (doc.isObject()) {
        volume_array.append(doc.object());
    }

    for (const QJsonValue& val : volume_array) {
        if (!val.isObject()) {
            // A non-object entry must not be coerced to an empty volume record (blank drive
            // letter, zeroed fields); fail closed exactly like parseKeyProtectorResponse so a
            // malformed response is never read as a valid volume set.
            Q_EMIT logMessage(
                "Malformed BitLocker volume entry (not a JSON object); failing closed");
            return {};  // parse_ok stays false
        }
        QJsonObject obj = val.toObject();

        VolumeInfo vi;
        vi.drive_letter = obj["DriveLetter"].toString();
        vi.device_id = obj["DeviceID"].toString();
        vi.volume_label = obj["VolumeLabel"].toString();
        vi.volume_type = formatVolumeType(obj["VolumeType"].toInt());
        vi.volume_size_bytes = static_cast<qint64>(obj["SizeBytes"].toDouble());
        vi.protection_status = protectionStatusLabel(obj["ProtectionStatus"].toInt(-1));
        vi.encryption_method = formatEncryptionMethod(obj["EncryptionMethod"].toInt());

        int enc_pct = obj["EncryptionPct"].toInt(-1);
        vi.encryption_percentage = (enc_pct >= 0) ? QString("%1%").arg(enc_pct) : "N/A";
        vi.lock_status = lockStatusLabel(obj["LockStatus"].toInt(-1));

        volumes.append(vi);
    }

    parse_ok = true;
    return volumes;
}

QVector<BackupBitlockerKeysAction::VolumeInfo> BackupBitlockerKeysAction::detectEncryptedVolumes(
    bool& query_ok) {
    query_ok = false;
    const QString script = QString::fromUtf8(kDetectEncryptedVolumesScript);

    Q_EMIT logMessage("Querying BitLocker volume encryption status...");

    ProcessResult proc = runPowerShell(script, sak::kTimeoutProcessLongMs);

    if (!proc.succeeded()) {
        QString error = proc.std_err.trimmed();
        if (error.contains("Access is denied", Qt::CaseInsensitive) ||
            error.contains("not recognized", Qt::CaseInsensitive)) {
            Q_EMIT logMessage("BitLocker WMI query requires administrator privileges");
        } else if (!error.isEmpty()) {
            Q_EMIT logMessage("BitLocker detection error: " + error);
        }
        return {};  // query_ok stays false: the volume set is unknown, not empty.
    }

    return parseDetectedVolumes(proc.std_out.trimmed(), query_ok);
}

// ============================================================================
// Key Protector Retrieval
// ============================================================================

QVector<BackupBitlockerKeysAction::KeyProtectorInfo> BackupBitlockerKeysAction::getKeyProtectors(
    const QString& drive_letter, bool& query_ok) {
    query_ok = false;
    QVector<KeyProtectorInfo> protectors;

    const QString script = buildKeyProtectorScript(drive_letter);
    if (script.isEmpty()) {
        // An empty/malformed drive letter yields no script; treat it as a query
        // failure (query_ok stays false) so the caller fails closed rather than
        // silently omitting this volume's recovery key from the backup.
        Q_EMIT logMessage(QString("Refusing to query key protectors for invalid drive letter: %1")
                              .arg(drive_letter));
        return protectors;
    }

    ProcessResult proc = runPowerShell(script, sak::kTimeoutProcessLongMs);

    if (!proc.succeeded()) {
        // query_ok stays false: the caller must fail closed rather than treat a
        // failed query as "this volume simply has no protectors".
        if (!proc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage(QString("Key protector query failed for %1: %2")
                                  .arg(drive_letter, proc.std_err.trimmed()));
        }
        return protectors;
    }

    QString output = proc.std_out.trimmed();
    if (output.isEmpty()) {
        query_ok = true;  // ran successfully; this volume genuinely has none
        return protectors;
    }

    return parseKeyProtectorResponse(output, query_ok);
}

QString BackupBitlockerKeysAction::buildKeyProtectorScript(const QString& drive_letter) const {
    // Reject anything that is not a bare drive letter (optionally with a colon)
    // before it is interpolated into the DriveLetter='%1' filter -- defense in
    // depth against injection even though the value comes from a system source.
    if (!isValidDriveLetter(drive_letter)) {
        return {};
    }
    return QString::fromUtf8(kKeyProtectorScriptTemplate).arg(drive_letter);
}

QVector<BackupBitlockerKeysAction::KeyProtectorInfo>
BackupBitlockerKeysAction::parseKeyProtectorResponse(const QString& output, bool& parse_ok) {
    parse_ok = false;
    QVector<KeyProtectorInfo> protectors;

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        Q_EMIT logMessage("Failed to parse key protector data: " + parse_error.errorString());
        return protectors;  // parse_ok stays false -> caller fails closed
    }

    if (!doc.isArray() && !doc.isObject()) {
        // Valid JSON but not the expected array/object shape: do not coerce it to
        // "no protectors". Fail closed so the caller does not omit a volume's key.
        Q_EMIT logMessage("Key protector data was neither an array nor an object; failing closed");
        return protectors;  // parse_ok stays false
    }

    QJsonArray protector_array;
    if (doc.isArray()) {
        protector_array = doc.array();
    } else {
        protector_array.append(doc.object());
    }

    for (const QJsonValue& val : protector_array) {
        if (!val.isObject()) {
            // A malformed (non-object) entry must not be silently dropped -- that
            // could omit a real recovery protector. Reject the whole response.
            Q_EMIT logMessage("Malformed key protector entry (not a JSON object); failing closed");
            return {};  // parse_ok stays false -> caller fails closed
        }
        QJsonObject obj = val.toObject();

        KeyProtectorInfo kpi;
        kpi.protector_id = obj["ProtectorID"].toString();
        kpi.protector_type = formatProtectorType(obj["ProtectorType"].toInt());
        kpi.recovery_password = obj["RecoveryPassword"].toString();
        kpi.key_file_name = obj["KeyFileName"].toString();

        protectors.append(kpi);
    }

    parse_ok = true;
    return protectors;
}

// ============================================================================
// Pre-Execution Scan
// ============================================================================

void BackupBitlockerKeysAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_EMIT scanProgress("Detecting BitLocker-encrypted volumes...");

    bool query_ok = false;
    m_volumes = detectEncryptedVolumes(query_ok);

    // A failed/denied query must not be reported as "no BitLocker" (fail closed): the volume set
    // is UNKNOWN, so surface that and tell the technician to retry elevated.
    if (!query_ok) {
        ScanResult failure;
        failure.applicable = false;
        failure.summary = "BitLocker detection failed";
        failure.details =
            "Could not query BitLocker volume status. Administrator privileges are required, or "
            "the query failed. The volume set is unknown -- run elevated and retry.";
        setScanResult(failure);
        setStatus(ActionStatus::Ready);
        Q_EMIT scanComplete(failure);
        return;
    }

    // Count total key protectors across all volumes
    // We haven't retrieved keys yet during scan (that requires admin)
    // Just report volume count

    ScanResult result;
    if (m_volumes.isEmpty()) {
        result.applicable = false;
        result.summary = "No BitLocker-encrypted volumes detected";
        result.details =
            "BitLocker is not configured on any volumes, "
            "or administrator privileges are required to detect them.";
    } else {
        result.applicable = true;
        result.summary = QString("Found %1 BitLocker volume(s)").arg(m_volumes.size());

        QStringList volume_details;
        for (const auto& vol : m_volumes) {
            QString detail = QString("%1 (%2) -- Protection: %3, Encryption: %4")
                                 .arg(vol.drive_letter)
                                 .arg(vol.volume_label.isEmpty() ? "No Label" : vol.volume_label)
                                 .arg(vol.protection_status)
                                 .arg(vol.encryption_method);
            volume_details.append(detail);
        }
        result.details = volume_details.join("\n");
        result.files_count = m_volumes.size();
        result.estimated_duration_ms = m_volumes.size() * kEstimatedDurationPerVolumeMs;
        result.warning = "Recovery keys are sensitive -- store the backup securely";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

// ============================================================================
// Execution -- Full Key Backup
// ============================================================================

void BackupBitlockerKeysAction::execute() {
    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled");
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    int total_keys_found = 0;
    int total_recovery_passwords = 0;
    if (!executeDiscoverVolumes(start_time)) {
        return;
    }
    if (!executeExtractKeys(start_time, total_keys_found, total_recovery_passwords)) {
        return;
    }

    QString backup_dir_path;
    int key_files_written = 0;
    bool permissions_set = false;
    if (!executeSaveKeyFiles(start_time, backup_dir_path, key_files_written, permissions_set)) {
        return;
    }

    BitlockerReportData report_data;
    report_data.total_keys_found = total_keys_found;
    report_data.total_recovery_passwords = total_recovery_passwords;
    report_data.backup_dir_path = backup_dir_path;
    report_data.key_files_written = key_files_written;
    report_data.permissions_set = permissions_set;

    executeBuildReport(start_time, report_data);
}

bool BackupBitlockerKeysAction::executeDiscoverVolumes(const QDateTime& start_time) {
    Q_EMIT executionProgress("Detecting BitLocker volumes...", progress::kStep5);

    // Always re-detect at execution time rather than trusting the pre-scan
    // cache: a volume added, removed, or reconfigured between scan and execute
    // must be reflected so the backup never certifies a stale volume set.
    bool query_ok = false;
    m_volumes = detectEncryptedVolumes(query_ok);

    // Fail closed on a failed/denied query: an unknown volume set must not be reported as an
    // empty one (which would read as "nothing to back up" and silently omit real keys).
    if (!query_ok) {
        emitFailedResult("BitLocker detection failed",
                         "Could not query BitLocker volume status; the volume set is unknown. "
                         "Run with administrator privileges and retry. No keys were backed up, "
                         "so none were silently omitted.",
                         start_time);
        return false;
    }

    if (m_volumes.isEmpty()) {
        emitFailedResult("No BitLocker-encrypted volumes found",
                         "Ensure BitLocker is enabled on at least one volume and "
                         "the application is running with administrator privileges.",
                         start_time);
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }
    return true;
}

bool BackupBitlockerKeysAction::executeExtractKeys(const QDateTime& start_time,
                                                   int& total_keys_found,
                                                   int& total_recovery_passwords) {
    Q_EMIT executionProgress("Retrieving recovery keys...", progress::kStep15);

    for (qsizetype i = 0; i < m_volumes.size(); ++i) {
        if (isCancelled()) {
            emitCancelledResult("BitLocker key backup cancelled", start_time);
            return false;
        }

        auto& vol = m_volumes[i];
        const int progress_percent = progress::kStep15 +
                                     static_cast<int>((static_cast<double>(i) / m_volumes.size()) *
                                                      kKeyRetrievalProgressSpan);
        Q_EMIT executionProgress(QString("Retrieving keys for %1 (%2/%3)...")
                                     .arg(vol.drive_letter)
                                     .arg(i + 1)
                                     .arg(m_volumes.size()),
                                 progress_percent);

        bool query_ok = false;
        vol.key_protectors = getKeyProtectors(vol.drive_letter, query_ok);
        if (!query_ok) {
            // A failed query returns an empty list; counting it as "no keys" would
            // silently drop this volume's recovery key from the backup, then report
            // success. Fail closed instead.
            emitFailedResult("Failed to read BitLocker key protectors",
                             QString("Key protectors for %1 could not be read, so the backup "
                                     "would silently omit its recovery key. Ensure the "
                                     "application is running with administrator privileges.")
                                 .arg(vol.drive_letter),
                             start_time);
            return false;
        }
        total_keys_found += vol.key_protectors.size();
        total_recovery_passwords += countRecoveryPasswords(vol.key_protectors);

        // Log protector IDs only (never log the actual recovery passwords)
        for (const auto& kp : vol.key_protectors) {
            logInfo("  Volume {}: Key Protector {} ({})",
                    vol.drive_letter.toStdString(),
                    kp.protector_id.toStdString(),
                    kp.protector_type.toStdString());
        }
    }

    if (!extractKeysGatePassed(start_time, total_keys_found, total_recovery_passwords)) {
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }
    return true;
}

bool BackupBitlockerKeysAction::extractKeysGatePassed(const QDateTime& start_time,
                                                      int total_keys_found,
                                                      int total_recovery_passwords) {
    switch (evaluateKeyGate(total_keys_found, total_recovery_passwords)) {
    case KeyGate::Ok:
        return true;
    case KeyGate::NoProtectors:
        emitFailedResult("No key protectors found on any volume",
                         "BitLocker volumes were detected but no key protectors could be read.\n"
                         "Ensure the application has administrator privileges.",
                         start_time);
        return false;
    case KeyGate::NoRecoveryPasswords:
        emitFailedResult("No recovery passwords found on any volume",
                         "BitLocker key protectors were read, but none is a numerical recovery "
                         "password. There is no recovery key to back up.",
                         start_time);
        return false;
    }
    return false;
}

bool BackupBitlockerKeysAction::executeSaveKeyFiles(const QDateTime& start_time,
                                                    QString& backup_dir_path,
                                                    int& key_files_written,
                                                    bool& permissions_set) {
    if (!createBackupDirectory(start_time, backup_dir_path)) {
        return false;
    }

    // A cancellation between writes must not leave plaintext recovery keys on
    // disk: remove the partial backup directory before reporting the cancel. If the
    // removal itself fails, plaintext keys may remain, so that is a failure -- not a
    // clean cancel.
    auto cancelWithCleanup = [&]() {
        if (!QDir(backup_dir_path).removeRecursively()) {
            emitFailedResult(
                "BitLocker key backup cancelled, but the partial backup directory "
                "could not be removed; plaintext recovery keys may remain at " +
                    backup_dir_path,
                QString(),
                start_time);
            return;
        }
        emitCancelledResult("BitLocker key backup cancelled", start_time);
    };

    if (isCancelled()) {
        cancelWithCleanup();
        return false;
    }

    Q_EMIT executionProgress("Writing recovery key document...", progress::kStep70);
    if (!writeRecoveryDocument(backup_dir_path)) {
        emitFailedResult("Failed to write recovery key document", QString(), start_time);
        return false;
    }
    if (isCancelled()) {
        cancelWithCleanup();
        return false;
    }

    Q_EMIT executionProgress("Writing per-volume key files...", progress::kStep80);
    if (!writePerVolumeKeyFiles(backup_dir_path, key_files_written)) {
        emitFailedResult("Failed to write a per-volume recovery key file",
                         "A recovery key file could not be durably written; the incomplete "
                         "backup was not certified.",
                         start_time);
        return false;
    }
    if (isCancelled()) {
        cancelWithCleanup();
        return false;
    }

    Q_EMIT executionProgress("Writing JSON backup...", progress::kStep85);
    if (!writeJsonBackup(backup_dir_path)) {
        emitFailedResult("Failed to write BitLocker key backup file", QString(), start_time);
        return false;
    }
    if (isCancelled()) {
        cancelWithCleanup();
        return false;
    }

    return secureBackupDirectory(start_time, backup_dir_path, permissions_set);
}

QString BackupBitlockerKeysAction::screenBackupLocation(const QString& raw_location,
                                                        QString& refusal) {
    if (raw_location.trimmed().isEmpty()) {
        refusal = QStringLiteral("No backup location was supplied.");
        return {};
    }
    // A UNC/device root would push plaintext recovery keys to a remote host (and the first
    // stat alone triggers an SMB/NTLM handshake). Screened on the literal string BEFORE any
    // stat, exactly like every other model-/payload-supplied destination in the app.
    if (isNetworkOrDevicePath(raw_location)) {
        refusal = QStringLiteral("Refused a network/device backup location: ") + raw_location;
        return {};
    }
    if (QFileInfo(raw_location).isRelative()) {
        // A relative destination resolves against the ELEVATED helper's working directory,
        // which the requesting client does not control or see. Require an absolute path.
        refusal = QStringLiteral("Backup location must be an absolute path: ") + raw_location;
        return {};
    }
    const QString canonical = QDir::cleanPath(QFileInfo(raw_location).absoluteFilePath());
    if (isNetworkOrDevicePath(canonical)) {
        refusal = QStringLiteral("Refused a network/device backup location: ") + canonical;
        return {};
    }
    // A pre-planted symlink/junction on the leaf or ANY ancestor silently redirects the keys
    // elsewhere (possibly to a UNC target); refuse rather than follow it.
    if (pathReparseUnsafe(canonical)) {
        refusal = QStringLiteral("Refused a backup location behind a symlink/junction: ") +
                  canonical;
        return {};
    }
    // A mapped drive letter is a network destination that the literal-string screen above
    // cannot see. Removable media stays allowed -- a USB key is a legitimate BitLocker
    // recovery-key medium -- but a remote share is not.
    if (rootDriveType(canonical) == kWin32DriveRemote) {
        refusal = QStringLiteral("Refused a mapped network drive as the backup location: ") +
                  canonical;
        return {};
    }
    return canonical;
}

bool BackupBitlockerKeysAction::createBackupDirectory(const QDateTime& start_time,
                                                      QString& backup_dir_path) {
    Q_EMIT executionProgress("Creating backup directory...", progress::kStep60);

    // The backup location arrives in the caller's task payload, crosses the elevation
    // boundary, and is about to hold PLAINTEXT recovery keys -- so it is canonicalized and
    // policy-screened HERE, before any directory is created under it. A refused location
    // fails closed: nothing is created and nothing is written.
    QString refusal;
    const QString location = screenBackupLocation(m_backup_location, refusal);
    if (location.isEmpty()) {
        emitFailedResult("Refused the BitLocker backup location", refusal, start_time);
        return false;
    }

    static std::atomic<unsigned> s_dir_counter{0};
    const QString timestamp = backupTimestamp();
    const unsigned counter = s_dir_counter.fetch_add(1, std::memory_order_relaxed);
    const QString backup_dir_name = uniqueBackupDirName(timestamp, counter);

    QDir parent(location);
    if (!parent.mkpath(QStringLiteral("."))) {
        emitFailedResult("Failed to create backup directory",
                         "Could not create backup location: " + location,
                         start_time);
        return false;
    }

    backup_dir_path = parent.filePath(backup_dir_name);
    // mkdir (unlike mkpath) fails if the directory already exists, so a name
    // collision fails closed instead of reopening and truncating a prior backup.
    if (!parent.mkdir(backup_dir_name)) {
        emitFailedResult("Failed to create backup directory",
                         "A fresh backup directory could not be created: " + backup_dir_path,
                         start_time);
        return false;
    }
    // Harden the ACL BEFORE any plaintext recovery key is written into the
    // directory, so there is never a window where secrets sit under the default
    // inherited ACL. Child files created afterwards inherit this protected ACL.
    // If the restriction cannot be applied, remove the empty directory and fail
    // closed rather than proceeding to write plaintext under a permissive ACL.
    if (!restrictFilePermissions(backup_dir_path)) {
        if (!QDir(backup_dir_path).removeRecursively()) {
            Q_EMIT logMessage("Failed to remove unsecured backup directory: " + backup_dir_path);
        }
        emitFailedResult("Failed to secure backup directory",
                         "The backup directory permissions could not be restricted before "
                         "writing any recovery keys; the empty directory was removed.",
                         start_time);
        return false;
    }
    return true;
}

bool BackupBitlockerKeysAction::secureBackupDirectory(const QDateTime& start_time,
                                                      const QString& backup_dir_path,
                                                      bool& permissions_set) {
    Q_EMIT executionProgress("Securing backup files...", progress::kStep90);

    permissions_set = restrictFilePermissions(backup_dir_path);
    if (permissions_set) {
        return true;
    }

    // The final re-hardening of the backup ACL failed. Reporting success could
    // leave insufficiently protected secrets on disk, so delete the backup and
    // fail closed -- and only claim the keys were removed if removal succeeded.
    Q_EMIT logMessage("Could not restrict backup permissions; removing exposed key backup");
    if (QDir(backup_dir_path).removeRecursively()) {
        emitFailedResult(
            "Failed to secure the recovery key backup",
            "The recovery keys could not be protected with restricted permissions, "
            "so the exposed backup was removed. No keys were left unprotected on disk.",
            start_time);
    } else {
        emitFailedResult("Failed to secure the recovery key backup",
                         "The recovery keys could not be protected with restricted permissions "
                         "AND the exposed backup could not be removed. Manually delete: " +
                             backup_dir_path,
                         start_time);
    }
    return false;
}

bool BackupBitlockerKeysAction::writeJsonBackup(const QString& backup_dir_path) {
    if (backup_dir_path.isEmpty()) {
        return false;
    }
    QJsonObject json_backup;
    json_backup["backup_version"] = "1.0";
    json_backup["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    json_backup["computer_name"] = QSysInfo::machineHostName();
    json_backup["os_version"] = QSysInfo::prettyProductName();

    QJsonArray volumes_json;
    for (const auto& vol : m_volumes) {
        volumes_json.append(buildVolumeJson(vol));
    }
    json_backup["volumes"] = volumes_json;

    QString json_path = QDir(backup_dir_path).filePath("bitlocker_keys.json");
    QSaveFile json_file(json_path);
    if (!json_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("Failed to write BitLocker key backup JSON: {}",
                 json_file.errorString().toStdString());
        return false;
    }

    const QByteArray data = QJsonDocument(json_backup).toJson(QJsonDocument::Indented);
    if (json_file.write(data) != data.size()) {
        json_file.cancelWriting();  // discard the partial temp; no truncated JSON remains
        logError("Incomplete write of BitLocker key backup JSON");
        return false;
    }
    if (!json_file.commit()) {
        logError("Failed to commit BitLocker key backup JSON: {}",
                 json_file.errorString().toStdString());
        return false;
    }
    return true;
}

void BackupBitlockerKeysAction::executeBuildReport(const QDateTime& start_time,
                                                   const BitlockerReportData& data) {
    Q_EMIT executionProgress("Finalizing backup...", progress::kStep95);

    qint64 total_bytes = 0;
    int total_files = 0;
    QDirIterator dir_it(data.backup_dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (dir_it.hasNext()) {
        dir_it.next();
        total_bytes += dir_it.fileInfo().size();
        total_files++;
    }

    Q_EMIT executionProgress("Backup complete", progress::kComplete);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = true;
    result.bytes_processed = total_bytes;
    result.files_processed = total_files;
    result.duration_ms = duration_ms;
    result.output_path = data.backup_dir_path;

    result.message = QString("Backed up %1 recovery key(s) from %2 volume(s)")
                         .arg(data.total_recovery_passwords)
                         .arg(m_volumes.size());

    QStringList log_lines;
    log_lines.reserve(kBitlockerSummaryLineReserve);
    log_lines.append("=== BitLocker Recovery Key Backup Summary ===");
    log_lines.append(QString("Computer: %1").arg(QSysInfo::machineHostName()));
    log_lines.append(QString("Date: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    log_lines.append(QString("Volumes: %1").arg(m_volumes.size()));
    log_lines.append(QString("Total key protectors: %1").arg(data.total_keys_found));
    log_lines.append(QString("Recovery passwords: %1").arg(data.total_recovery_passwords));
    log_lines.append(QString("Key files written: %1").arg(data.key_files_written));
    log_lines.append(QString("Backup location: %1").arg(data.backup_dir_path));
    log_lines.append(QString("Backup size: %1 bytes (%2 files)").arg(total_bytes).arg(total_files));
    if (data.permissions_set) {
        log_lines.append("File permissions: Restricted to current user + Administrators");
    }
    log_lines.append("");
    log_lines.append("IMPORTANT: Store this backup in a secure location.");
    log_lines.append("Recovery keys can unlock BitLocker-encrypted volumes.");
    result.log = log_lines.join("\n");
    finishWithResult(result, ActionStatus::Success);
}

// ============================================================================
// File Output -- Master Recovery Document
// ============================================================================

bool BackupBitlockerKeysAction::writeRecoveryDocument(const QString& backup_dir) {
    const QString doc_path = QDir(backup_dir).filePath("BitLocker_Recovery_Keys.txt");

    QSaveFile file(doc_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to open recovery document for writing: " + doc_path);
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    writeRecoveryDocumentHeader(out);
    writeRecoveryDocumentVolumes(out);
    writeRecoveryDocumentFooter(out);
    out.flush();

    if (out.status() != QTextStream::Ok) {
        file.cancelWriting();
        Q_EMIT logMessage("Stream error writing recovery document: " + doc_path);
        return false;
    }
    if (!file.commit()) {
        Q_EMIT logMessage("Failed to commit recovery document: " + doc_path);
        return false;
    }
    return true;
}

void BackupBitlockerKeysAction::writeRecoveryDocumentHeader(QTextStream& out) const {
    // Header
    out << "===============================================================================\n";
    out << "                    BITLOCKER RECOVERY KEY BACKUP\n";
    out << "===============================================================================\n";
    out << "\n";
    out << "  Computer Name:  " << QSysInfo::machineHostName() << "\n";
    out << "  Backup Date:    " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
        << "\n";
    out << "  OS Version:     " << QSysInfo::prettyProductName() << "\n";
    out << "  Kernel:         " << QSysInfo::kernelVersion() << "\n";
    out << "  Generated By:   S.A.K. Utility\n";
    out << "\n";
    out << "  SECURITY WARNING: This file contains BitLocker recovery keys.\n";
    out << "  Store this document in a secure location (encrypted drive, safe,\n";
    out << "  or password manager). Anyone with these keys can unlock your\n";
    out << "  encrypted volumes.\n";
    out << "\n";
    out << "===============================================================================\n";
    out << "\n";
}

void BackupBitlockerKeysAction::writeRecoveryDocumentVolumes(QTextStream& out) const {
    // executeDiscoverVolumes fails the run closed when m_volumes is empty, and it runs before
    // executeSaveKeyFiles reaches the document writer.
    Q_ASSERT(!m_volumes.isEmpty());
    // Per-volume sections
    for (int volume_index = 0; volume_index < m_volumes.size(); ++volume_index) {
        const auto& vol = m_volumes[volume_index];

        out << "-------------------------------------------------------------------------------\n";
        out << "  VOLUME " << (volume_index + 1) << ": " << vol.drive_letter;
        if (!vol.volume_label.isEmpty()) {
            out << " (" << vol.volume_label << ")";
        }
        out << "\n";
        out << "-------------------------------------------------------------------------------\n";
        out << "\n";
        out << "  Device ID:            " << vol.device_id << "\n";
        out << "  Volume Type:          " << vol.volume_type << "\n";
        out << "  Protection Status:    " << vol.protection_status << "\n";
        out << "  Encryption Method:    " << vol.encryption_method << "\n";
        out << "  Encryption Progress:  " << vol.encryption_percentage << "\n";
        out << "  Lock Status:          " << vol.lock_status << "\n";

        if (vol.volume_size_bytes > 0) {
            double size_gb = static_cast<double>(vol.volume_size_bytes) / sak::kBytesPerGBf;
            out << "  Volume Size:          "
                << QString::number(size_gb, 'f', kVolumeSizeDisplayPrecision) << " GB\n";
        }

        out << "\n";
        out << "  Key Protectors (" << vol.key_protectors.size() << "):\n";
        out << "\n";

        if (vol.key_protectors.isEmpty()) {
            out << "    (No key protectors found -- administrator privileges may be required)\n";
        }

        for (int k = 0; k < vol.key_protectors.size(); ++k) {
            writeKeyProtectorEntry(out, vol.key_protectors[k], k);
        }

        out << "\n";
    }
}

void BackupBitlockerKeysAction::writeRecoveryDocumentFooter(QTextStream& out) {
    // Footer with restore instructions
    out << "===============================================================================\n";
    out << "                         RECOVERY INSTRUCTIONS\n";
    out << "===============================================================================\n";
    out << "\n";
    out << "  To unlock a BitLocker-encrypted volume using a recovery password:\n";
    out << "\n";
    out << "  Method 1 -- BitLocker Recovery Screen (during boot):\n";
    out << "    1. When prompted, select 'Enter recovery key'\n";
    out << "    2. Type the 48-digit numerical recovery password\n";
    out << "    3. Press Enter to unlock\n";
    out << "\n";
    out << "  Method 2 -- Command Line (from recovery environment):\n";
    out << "    manage-bde -unlock C: -RecoveryPassword YOUR-RECOVERY-KEY\n";
    out << "\n";
    out << "  Method 3 -- PowerShell (elevated):\n";
    out << "    Unlock-BitLocker -MountPoint 'C:' -RecoveryPassword 'YOUR-KEY'\n";
    out << "\n";
    out << "  To identify which key to use, match the Key Protector ID shown\n";
    out << "  on the BitLocker recovery screen with the Protector ID above.\n";
    out << "\n";
    out << "===============================================================================\n";
    out << "  End of BitLocker Recovery Key Backup\n";
    out << "===============================================================================\n";
}

// ============================================================================
// File Output -- Per-Volume Key Files
// ============================================================================

bool BackupBitlockerKeysAction::writePerVolumeKeyFiles(const QString& backup_dir,
                                                       int& files_written) {
    // backup_dir is the path createBackupDirectory produced; that runs first in
    // executeSaveKeyFiles and fails closed unless screenBackupLocation returned an absolute,
    // screened location whose directory was actually created.
    Q_ASSERT(!backup_dir.isEmpty());
    files_written = 0;

    for (const auto& vol : m_volumes) {
        // Only write files for volumes that have recovery passwords
        if (!volumeHasRecoveryPassword(vol.key_protectors)) {
            continue;
        }
        if (!writeOneVolumeKeyFile(backup_dir, vol)) {
            return false;  // fail closed: never leave a truncated key file behind
        }
        files_written++;
    }

    return true;
}

bool BackupBitlockerKeysAction::writeOneVolumeKeyFile(const QString& backup_dir,
                                                      const VolumeInfo& vol) {
    // Reject a malformed drive letter before it is used to compose the filename,
    // so a value like "C:\..\.." can never escape the backup directory.
    if (!isValidDriveLetter(vol.drive_letter)) {
        Q_EMIT logMessage("Refusing to write key file for invalid drive letter: " +
                          vol.drive_letter);
        return false;
    }
    // Create a file named like "BitLocker Recovery Key C.txt"
    QString safe_drive = vol.drive_letter;
    safe_drive.remove(':');
    const QString key_file_name = QString("BitLocker Recovery Key %1.txt").arg(safe_drive);
    const QString key_file_path = QDir(backup_dir).filePath(key_file_name);

    QSaveFile file(key_file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to write key file: " + key_file_path);
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    out << "BitLocker Drive Encryption Recovery Key\n";
    out << "\n";
    out << "To verify that this is the correct recovery key, compare the start of\n";
    out << "the following identifier with the identifier value displayed on your PC.\n";
    out << "\n";

    writeVolumeKeyEntries(out, vol.key_protectors);

    out << "Drive:        " << vol.drive_letter;
    if (!vol.volume_label.isEmpty()) {
        out << " (" << vol.volume_label << ")";
    }
    out << "\n";
    out << "Computer:     " << QSysInfo::machineHostName() << "\n";
    out << "Date:         " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << "\n";
    out << "\n";
    out << "If the above identifier matches the one shown on your PC, you can use\n";
    out << "the corresponding recovery key to unlock the drive.\n";
    out.flush();

    if (out.status() != QTextStream::Ok) {
        file.cancelWriting();
        Q_EMIT logMessage("Stream error writing key file: " + key_file_path);
        return false;
    }
    if (!file.commit()) {
        Q_EMIT logMessage("Failed to commit key file: " + key_file_path);
        return false;
    }
    return true;
}

// ============================================================================
// Security -- Restrict File Permissions
// ============================================================================

bool BackupBitlockerKeysAction::restrictFilePermissions(const QString& path) {
    // Both callers pass the backup_dir_path built by createBackupDirectory from the screened,
    // absolute location, so it is never empty.
    Q_ASSERT(!path.isEmpty());
    const QString script =
        QString::fromUtf8(kRestrictPermissionsScriptTemplate).arg(QString(path).replace("'", "''"));

    ProcessResult proc = runPowerShell(script, sak::kTimeoutChocoListMs);
    // The script emits exactly "SUCCESS" on its one success path and nothing else on stdout
    // (Set-Acl/Get-ChildItem produce no output), so require an exact match rather than a
    // substring -- a stray token accompanying an error must not read as success.
    return proc.succeeded() && proc.std_out.trimmed() == QStringLiteral("SUCCESS");
}

// ============================================================================
// Extracted Helpers -- Nesting Reduction
// ============================================================================

int BackupBitlockerKeysAction::countRecoveryPasswords(const QVector<KeyProtectorInfo>& protectors) {
    return static_cast<int>(std::count_if(protectors.begin(), protectors.end(), [](const auto& kp) {
        return !kp.recovery_password.isEmpty();
    }));
}

bool BackupBitlockerKeysAction::volumeHasRecoveryPassword(
    const QVector<KeyProtectorInfo>& protectors) {
    return std::any_of(protectors.begin(), protectors.end(), [](const auto& kp) {
        return !kp.recovery_password.isEmpty();
    });
}

void BackupBitlockerKeysAction::writeKeyProtectorEntry(QTextStream& out,
                                                       const KeyProtectorInfo& kp,
                                                       int index) const {
    out << "    Protector " << (index + 1) << ":\n";
    out << "      Type:           " << kp.protector_type << "\n";
    out << "      Protector ID:   " << kp.protector_id << "\n";

    if (!kp.recovery_password.isEmpty()) {
        out << "\n";
        out << "      *** RECOVERY PASSWORD ***\n";
        out << "      " << kp.recovery_password << "\n";
        out << "\n";
        out << "      (Enter this 48-digit password at the BitLocker recovery screen)\n";
    }

    if (!kp.key_file_name.isEmpty()) {
        out << "      Key File:       " << kp.key_file_name << "\n";
    }

    out << "\n";
}

void BackupBitlockerKeysAction::writeVolumeKeyEntries(QTextStream& out,
                                                      const QVector<KeyProtectorInfo>& protectors) {
    for (const auto& kp : protectors) {
        if (kp.recovery_password.isEmpty()) {
            continue;
        }
        out << "Identifier:   " << kp.protector_id << "\n";
        out << "Recovery Key: " << kp.recovery_password << "\n";
        out << "\n";
    }
}

QJsonObject BackupBitlockerKeysAction::buildVolumeJson(const VolumeInfo& vol) const {
    QJsonObject vol_obj;
    vol_obj["drive_letter"] = vol.drive_letter;
    vol_obj["volume_label"] = vol.volume_label;
    vol_obj["device_id"] = vol.device_id;
    vol_obj["protection_status"] = vol.protection_status;
    vol_obj["encryption_method"] = vol.encryption_method;
    vol_obj["encryption_percentage"] = vol.encryption_percentage;
    vol_obj["lock_status"] = vol.lock_status;
    vol_obj["volume_type"] = vol.volume_type;
    vol_obj["volume_size_bytes"] = vol.volume_size_bytes;

    QJsonArray protectors_json;
    for (const auto& kp : vol.key_protectors) {
        QJsonObject kp_obj;
        kp_obj["protector_id"] = kp.protector_id;
        kp_obj["protector_type"] = kp.protector_type;
        if (!kp.recovery_password.isEmpty()) {
            kp_obj["recovery_password"] = kp.recovery_password;
        }
        if (!kp.key_file_name.isEmpty()) {
            kp_obj["key_file_name"] = kp.key_file_name;
        }
        protectors_json.append(kp_obj);
    }
    vol_obj["key_protectors"] = protectors_json;
    return vol_obj;
}

}  // namespace sak
