// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/backup_bitlocker_keys_action.h"
#include "sak/process_runner.h"
#include "sak/logger.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>
#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QDirIterator>
#include <QSysInfo>

namespace sak {

// ============================================================================
// Construction
// ============================================================================

BackupBitlockerKeysAction::BackupBitlockerKeysAction(const QString& backup_location, QObject* parent)
    : QuickAction(parent)
    , m_backup_location(backup_location)
{
}

// ============================================================================
// Static Helpers — WMI Enum Formatting
// ============================================================================

QString BackupBitlockerKeysAction::formatEncryptionMethod(int method_code)
{
    // Win32_EncryptableVolume.EncryptionMethod enum values
    switch (method_code) {
        case 0: return "None";
        case 1: return "AES-128 with Diffuser";
        case 2: return "AES-256 with Diffuser";
        case 3: return "AES-128";
        case 4: return "AES-256";
        case 5: return "Hardware Encryption";
        case 6: return "XTS-AES-128";
        case 7: return "XTS-AES-256";
        default: return QString("Unknown (%1)").arg(method_code);
    }
}

QString BackupBitlockerKeysAction::formatProtectorType(int type_code)
{
    // Win32_EncryptableVolume.KeyProtectorType enum values
    switch (type_code) {
        case 0: return "Unknown or Other";
        case 1: return "TPM";
        case 2: return "External Key (USB)";
        case 3: return "Numerical Password (Recovery Password)";
        case 4: return "TPM + PIN";
        case 5: return "TPM + Startup Key";
        case 6: return "TPM + PIN + Startup Key";
        case 7: return "Public Key (Certificate)";
        case 8: return "Passphrase";
        case 9: return "TPM + Certificate";
        case 10: return "Clear Key (Unprotected)";
        default: return QString("Unknown (%1)").arg(type_code);
    }
}

QString BackupBitlockerKeysAction::formatVolumeType(int type_code)
{
    switch (type_code) {
        case 0: return "Operating System";
        case 1: return "Fixed Data";
        case 2: return "Removable Data";
        default: return QString("Unknown (%1)").arg(type_code);
    }
}

QString BackupBitlockerKeysAction::backupTimestamp()
{
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
}

// ============================================================================
// Volume Detection — WMI Queries via PowerShell
// ============================================================================

QVector<BackupBitlockerKeysAction::VolumeInfo> BackupBitlockerKeysAction::detectEncryptedVolumes()
{
    QVector<VolumeInfo> volumes;

    // PowerShell script to query BitLocker volumes via WMI
    // Returns JSON array of volume objects with protection details
    const QString script = R"PS(
try {
    $vols = Get-WmiObject -Namespace "Root\CIMv2\Security\MicrosoftVolumeEncryption" `
        -Class Win32_EncryptableVolume -ErrorAction Stop

    $results = @()
    foreach ($vol in $vols) {
        $status = $vol.GetProtectionStatus()
        $encMethod = $vol.GetEncryptionMethod()
        $convStatus = $vol.GetConversionStatus()
        $lockStatus = $vol.GetLockStatus()

        $driveInfo = Get-Volume -DriveLetter ($vol.DriveLetter -replace ':', '') -ErrorAction SilentlyContinue

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

    Q_EMIT logMessage("Querying BitLocker volume encryption status...");

    ProcessResult proc = runPowerShell(script, 30000);

    if (proc.exit_code != 0 || proc.timed_out) {
        QString error = proc.std_err.trimmed();
        if (error.contains("Access is denied", Qt::CaseInsensitive) ||
            error.contains("not recognized", Qt::CaseInsensitive)) {
            Q_EMIT logMessage("BitLocker WMI query requires administrator privileges");
        } else if (!error.isEmpty()) {
            Q_EMIT logMessage("BitLocker detection error: " + error);
        }
        return volumes;
    }

    QString output = proc.std_out.trimmed();
    if (output.isEmpty()) {
        return volumes;
    }

    // Parse JSON output — PowerShell returns a single object if only one volume
    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        Q_EMIT logMessage("Failed to parse BitLocker volume data: " + parse_error.errorString());
        return volumes;
    }

    // Normalize to array (PowerShell returns bare object for single-element results)
    QJsonArray volume_array;
    if (doc.isArray()) {
        volume_array = doc.array();
    } else if (doc.isObject()) {
        volume_array.append(doc.object());
    }

    for (const QJsonValue& val : volume_array) {
        QJsonObject obj = val.toObject();

        VolumeInfo vi;
        vi.drive_letter = obj["DriveLetter"].toString();
        vi.device_id = obj["DeviceID"].toString();
        vi.volume_label = obj["VolumeLabel"].toString();
        vi.volume_type = formatVolumeType(obj["VolumeType"].toInt());
        vi.volume_size_bytes = static_cast<qint64>(obj["SizeBytes"].toDouble());

        int protection_status = obj["ProtectionStatus"].toInt(-1);
        switch (protection_status) {
            case 0:  vi.protection_status = "Off"; break;
            case 1:  vi.protection_status = "On"; break;
            case 2:  vi.protection_status = "Unknown"; break;
            default: vi.protection_status = "Unknown"; break;
        }

        vi.encryption_method = formatEncryptionMethod(obj["EncryptionMethod"].toInt());

        int enc_pct = obj["EncryptionPct"].toInt(-1);
        vi.encryption_percentage = (enc_pct >= 0) ? QString("%1%").arg(enc_pct) : "N/A";

        int lock_status = obj["LockStatus"].toInt(-1);
        switch (lock_status) {
            case 0:  vi.lock_status = "Unlocked"; break;
            case 1:  vi.lock_status = "Locked"; break;
            default: vi.lock_status = "Unknown"; break;
        }

        volumes.append(vi);
    }

    return volumes;
}

// ============================================================================
// Key Protector Retrieval
// ============================================================================

QVector<BackupBitlockerKeysAction::KeyProtectorInfo>
BackupBitlockerKeysAction::getKeyProtectors(const QString& drive_letter)
{
    QVector<KeyProtectorInfo> protectors;

    // Query all key protectors for the specified volume
    // This enumerates protector IDs, types, and recovery passwords
    const QString script = QString(R"PS(
try {
    $vol = Get-WmiObject -Namespace "Root\CIMv2\Security\MicrosoftVolumeEncryption" `
        -Class Win32_EncryptableVolume -Filter "DriveLetter='%1'" -ErrorAction Stop

    if (-not $vol) {
        Write-Error "Volume %1 not found"
        exit 1
    }

    $protectorIds = $vol.GetKeyProtectors(0).VolumeKeyProtectorID
    if (-not $protectorIds) {
        @() | ConvertTo-Json
        exit 0
    }

    $results = @()
    foreach ($id in $protectorIds) {
        $typeResult = $vol.GetKeyProtectorType($id)
        $type = $typeResult.KeyProtectorType

        $recoveryPassword = ""
        if ($type -eq 3) {
            $pwResult = $vol.GetKeyProtectorNumericalPassword($id)
            if ($pwResult.ReturnValue -eq 0) {
                $recoveryPassword = $pwResult.NumericalPassword
            }
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
)PS").arg(drive_letter);

    ProcessResult proc = runPowerShell(script, 30000);

    if (proc.exit_code != 0 || proc.timed_out) {
        if (!proc.std_err.trimmed().isEmpty()) {
            Q_EMIT logMessage(QString("Key protector query failed for %1: %2")
                             .arg(drive_letter, proc.std_err.trimmed()));
        }
        return protectors;
    }

    QString output = proc.std_out.trimmed();
    if (output.isEmpty()) {
        return protectors;
    }

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        Q_EMIT logMessage("Failed to parse key protector data: " + parse_error.errorString());
        return protectors;
    }

    QJsonArray protector_array;
    if (doc.isArray()) {
        protector_array = doc.array();
    } else if (doc.isObject()) {
        protector_array.append(doc.object());
    }

    for (const QJsonValue& val : protector_array) {
        QJsonObject obj = val.toObject();

        KeyProtectorInfo kpi;
        kpi.protector_id = obj["ProtectorID"].toString();
        kpi.protector_type = formatProtectorType(obj["ProtectorType"].toInt());
        kpi.recovery_password = obj["RecoveryPassword"].toString();
        kpi.key_file_name = obj["KeyFileName"].toString();

        protectors.append(kpi);
    }

    return protectors;
}

// ============================================================================
// Pre-Execution Scan
// ============================================================================

void BackupBitlockerKeysAction::scan()
{
    setStatus(ActionStatus::Scanning);
    Q_EMIT scanProgress("Detecting BitLocker-encrypted volumes...");

    m_volumes = detectEncryptedVolumes();

    // Filter to volumes that actually have protection
    int protected_count = 0;
    for (const auto& vol : m_volumes) {
        if (vol.protection_status == "On" || vol.protection_status == "Off") {
            // "Off" means BitLocker was configured but protection is suspended
            // Still want to back up any existing keys
            protected_count++;
        }
    }

    // Count total key protectors across all volumes
    // We haven't retrieved keys yet during scan (that requires admin)
    // Just report volume count

    ScanResult result;
    if (m_volumes.isEmpty()) {
        result.applicable = false;
        result.summary = "No BitLocker-encrypted volumes detected";
        result.details = "BitLocker is not configured on any volumes, "
                        "or administrator privileges are required to detect them.";
    } else {
        result.applicable = true;
        result.summary = QString("Found %1 BitLocker volume(s)").arg(m_volumes.size());

        QStringList volume_details;
        for (const auto& vol : m_volumes) {
            QString detail = QString("%1 (%2) — Protection: %3, Encryption: %4")
                .arg(vol.drive_letter)
                .arg(vol.volume_label.isEmpty() ? "No Label" : vol.volume_label)
                .arg(vol.protection_status)
                .arg(vol.encryption_method);
            volume_details.append(detail);
        }
        result.details = volume_details.join("\n");
        result.files_count = m_volumes.size();
        result.estimated_duration_ms = m_volumes.size() * 5000; // ~5s per volume
        result.warning = "Recovery keys are sensitive — store the backup securely";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

// ============================================================================
// Execution — Full Key Backup
// ============================================================================

void BackupBitlockerKeysAction::execute()
{
    if (isCancelled()) {
        ExecutionResult result;
        result.success = false;
        result.message = "BitLocker key backup cancelled";
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    auto finishCancelled = [this, &start_time]() {
        ExecutionResult result;
        result.success = false;
        result.message = "BitLocker key backup cancelled";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
    };

    // Step 1: Re-detect volumes if needed (in case scan was stale)
    Q_EMIT executionProgress("Detecting BitLocker volumes...", 5);

    if (m_volumes.isEmpty()) {
        m_volumes = detectEncryptedVolumes();
    }

    if (m_volumes.isEmpty()) {
        ExecutionResult result;
        result.success = false;
        result.message = "No BitLocker-encrypted volumes found";
        result.log = "Ensure BitLocker is enabled on at least one volume and "
                    "the application is running with administrator privileges.";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }

    if (isCancelled()) { finishCancelled(); return; }

    // Step 2: Retrieve key protectors for each volume
    Q_EMIT executionProgress("Retrieving recovery keys...", 15);

    int total_keys_found = 0;
    int total_recovery_passwords = 0;

    for (int i = 0; i < m_volumes.size(); ++i) {
        if (isCancelled()) { finishCancelled(); return; }

        auto& vol = m_volumes[i];
        int progress = 15 + static_cast<int>((static_cast<double>(i) / m_volumes.size()) * 40);
        Q_EMIT executionProgress(
            QString("Retrieving keys for %1 (%2/%3)...")
                .arg(vol.drive_letter).arg(i + 1).arg(m_volumes.size()),
            progress);

        vol.key_protectors = getKeyProtectors(vol.drive_letter);
        total_keys_found += vol.key_protectors.size();

        for (const auto& kp : vol.key_protectors) {
            if (!kp.recovery_password.isEmpty()) {
                total_recovery_passwords++;
            }
        }

        // Log protector IDs only (never log the actual recovery passwords)
        for (const auto& kp : vol.key_protectors) {
            logInfo("  Volume {}: Key Protector {} ({})",
                   vol.drive_letter.toStdString(),
                   kp.protector_id.toStdString(),
                   kp.protector_type.toStdString());
        }
    }

    if (total_keys_found == 0) {
        ExecutionResult result;
        result.success = false;
        result.message = "No key protectors found on any volume";
        result.log = "BitLocker volumes were detected but no key protectors could be read.\n"
                    "Ensure the application has administrator privileges.";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }

    if (isCancelled()) { finishCancelled(); return; }

    // Step 3: Create backup directory
    Q_EMIT executionProgress("Creating backup directory...", 60);

    QString timestamp = backupTimestamp();
    QString backup_dir_name = QString("BitLocker_Keys_%1").arg(timestamp);
    QString backup_dir_path = QDir(m_backup_location).filePath(backup_dir_name);

    QDir backup_dir(backup_dir_path);
    if (!backup_dir.mkpath(".")) {
        ExecutionResult result;
        result.success = false;
        result.message = "Failed to create backup directory";
        result.log = "Could not create: " + backup_dir_path;
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }

    if (isCancelled()) { finishCancelled(); return; }

    // Step 4: Write master recovery document
    Q_EMIT executionProgress("Writing recovery key document...", 70);

    bool doc_written = writeRecoveryDocument(backup_dir_path);
    if (!doc_written) {
        ExecutionResult result;
        result.success = false;
        result.message = "Failed to write recovery key document";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }

    if (isCancelled()) { finishCancelled(); return; }

    // Step 5: Write individual per-volume key files
    Q_EMIT executionProgress("Writing per-volume key files...", 80);

    int key_files_written = writePerVolumeKeyFiles(backup_dir_path);

    if (isCancelled()) { finishCancelled(); return; }

    // Step 6: Write machine-readable JSON backup
    Q_EMIT executionProgress("Writing JSON backup...", 85);

    QJsonObject json_backup;
    json_backup["backup_version"] = "1.0";
    json_backup["created"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    json_backup["computer_name"] = QSysInfo::machineHostName();
    json_backup["os_version"] = QSysInfo::prettyProductName();

    QJsonArray volumes_json;
    for (const auto& vol : m_volumes) {
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
        volumes_json.append(vol_obj);
    }
    json_backup["volumes"] = volumes_json;

    QString json_path = QDir(backup_dir_path).filePath("bitlocker_keys.json");
    QFile json_file(json_path);
    if (json_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        json_file.write(QJsonDocument(json_backup).toJson(QJsonDocument::Indented));
        json_file.close();
    }

    if (isCancelled()) { finishCancelled(); return; }

    // Step 7: Restrict file permissions on backup directory
    Q_EMIT executionProgress("Securing backup files...", 90);

    bool permissions_set = restrictFilePermissions(backup_dir_path);
    if (!permissions_set) {
        Q_EMIT logMessage("Warning: Could not restrict backup directory permissions");
    }

    // Step 8: Calculate total backup size
    Q_EMIT executionProgress("Finalizing backup...", 95);

    qint64 total_bytes = 0;
    int total_files = 0;
    QDirIterator dir_it(backup_dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (dir_it.hasNext()) {
        dir_it.next();
        total_bytes += dir_it.fileInfo().size();
        total_files++;
    }

    Q_EMIT executionProgress("Backup complete", 100);

    // Build final result
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    result.success = true;
    result.bytes_processed = total_bytes;
    result.files_processed = total_files;
    result.duration_ms = duration_ms;
    result.output_path = backup_dir_path;

    result.message = QString("Backed up %1 recovery key(s) from %2 volume(s)")
        .arg(total_recovery_passwords).arg(m_volumes.size());

    QStringList log_lines;
    log_lines.append("=== BitLocker Recovery Key Backup Summary ===");
    log_lines.append(QString("Computer: %1").arg(QSysInfo::machineHostName()));
    log_lines.append(QString("Date: %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate)));
    log_lines.append(QString("Volumes: %1").arg(m_volumes.size()));
    log_lines.append(QString("Total key protectors: %1").arg(total_keys_found));
    log_lines.append(QString("Recovery passwords: %1").arg(total_recovery_passwords));
    log_lines.append(QString("Key files written: %1").arg(key_files_written));
    log_lines.append(QString("Backup location: %1").arg(backup_dir_path));
    log_lines.append(QString("Backup size: %1 bytes (%2 files)")
                    .arg(total_bytes).arg(total_files));
    if (permissions_set) {
        log_lines.append("File permissions: Restricted to current user + Administrators");
    }
    log_lines.append("");
    log_lines.append("IMPORTANT: Store this backup in a secure location.");
    log_lines.append("Recovery keys can unlock BitLocker-encrypted volumes.");
    result.log = log_lines.join("\n");

    setExecutionResult(result);
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

// ============================================================================
// File Output — Master Recovery Document
// ============================================================================

bool BackupBitlockerKeysAction::writeRecoveryDocument(const QString& backup_dir)
{
    QString doc_path = QDir(backup_dir).filePath("BitLocker_Recovery_Keys.txt");

    QFile file(doc_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to open recovery document for writing: " + doc_path);
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // Header
    out << "===============================================================================\n";
    out << "                    BITLOCKER RECOVERY KEY BACKUP\n";
    out << "===============================================================================\n";
    out << "\n";
    out << "  Computer Name:  " << QSysInfo::machineHostName() << "\n";
    out << "  Backup Date:    " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss") << "\n";
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

    // Per-volume sections
    for (int v = 0; v < m_volumes.size(); ++v) {
        const auto& vol = m_volumes[v];

        out << "-------------------------------------------------------------------------------\n";
        out << "  VOLUME " << (v + 1) << ": " << vol.drive_letter;
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
            double size_gb = static_cast<double>(vol.volume_size_bytes) / (1024.0 * 1024.0 * 1024.0);
            out << "  Volume Size:          " << QString::number(size_gb, 'f', 2) << " GB\n";
        }

        out << "\n";
        out << "  Key Protectors (" << vol.key_protectors.size() << "):\n";
        out << "\n";

        if (vol.key_protectors.isEmpty()) {
            out << "    (No key protectors found — administrator privileges may be required)\n";
        }

        for (int k = 0; k < vol.key_protectors.size(); ++k) {
            const auto& kp = vol.key_protectors[k];

            out << "    Protector " << (k + 1) << ":\n";
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

        out << "\n";
    }

    // Footer with restore instructions
    out << "===============================================================================\n";
    out << "                         RECOVERY INSTRUCTIONS\n";
    out << "===============================================================================\n";
    out << "\n";
    out << "  To unlock a BitLocker-encrypted volume using a recovery password:\n";
    out << "\n";
    out << "  Method 1 — BitLocker Recovery Screen (during boot):\n";
    out << "    1. When prompted, select 'Enter recovery key'\n";
    out << "    2. Type the 48-digit numerical recovery password\n";
    out << "    3. Press Enter to unlock\n";
    out << "\n";
    out << "  Method 2 — Command Line (from recovery environment):\n";
    out << "    manage-bde -unlock C: -RecoveryPassword YOUR-RECOVERY-KEY\n";
    out << "\n";
    out << "  Method 3 — PowerShell (elevated):\n";
    out << "    Unlock-BitLocker -MountPoint 'C:' -RecoveryPassword 'YOUR-KEY'\n";
    out << "\n";
    out << "  To identify which key to use, match the Key Protector ID shown\n";
    out << "  on the BitLocker recovery screen with the Protector ID above.\n";
    out << "\n";
    out << "===============================================================================\n";
    out << "  End of BitLocker Recovery Key Backup\n";
    out << "===============================================================================\n";

    file.close();
    return true;
}

// ============================================================================
// File Output — Per-Volume Key Files
// ============================================================================

int BackupBitlockerKeysAction::writePerVolumeKeyFiles(const QString& backup_dir)
{
    int files_written = 0;

    for (const auto& vol : m_volumes) {
        // Only write files for volumes that have recovery passwords
        bool has_recovery_password = false;
        for (const auto& kp : vol.key_protectors) {
            if (!kp.recovery_password.isEmpty()) {
                has_recovery_password = true;
                break;
            }
        }

        if (!has_recovery_password) {
            continue;
        }

        // Create a file named like "BitLocker Recovery Key C.txt"
        QString safe_drive = vol.drive_letter;
        safe_drive.remove(':');
        QString key_file_name = QString("BitLocker Recovery Key %1.txt").arg(safe_drive);
        QString key_file_path = QDir(backup_dir).filePath(key_file_name);

        QFile file(key_file_path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            Q_EMIT logMessage("Failed to write key file: " + key_file_path);
            continue;
        }

        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);

        out << "BitLocker Drive Encryption Recovery Key\n";
        out << "\n";
        out << "To verify that this is the correct recovery key, compare the start of\n";
        out << "the following identifier with the identifier value displayed on your PC.\n";
        out << "\n";

        for (const auto& kp : vol.key_protectors) {
            if (kp.recovery_password.isEmpty()) {
                continue;
            }

            out << "Identifier:   " << kp.protector_id << "\n";
            out << "Recovery Key: " << kp.recovery_password << "\n";
            out << "\n";
        }

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

        file.close();
        files_written++;
    }

    return files_written;
}

// ============================================================================
// Security — Restrict File Permissions
// ============================================================================

bool BackupBitlockerKeysAction::restrictFilePermissions(const QString& path)
{
    // Use icacls to restrict the backup directory to current user + Administrators only
    // Remove inherited permissions, then grant explicit access
    const QString script = QString(R"PS(
try {
    $path = '%1'

    # Disable inheritance and remove inherited ACEs
    $acl = Get-Acl -Path $path
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

    Set-Acl -Path $path -AclObject $acl

    # Apply same ACL to all child items
    Get-ChildItem -Path $path -Recurse -Force | ForEach-Object {
        Set-Acl -Path $_.FullName -AclObject $acl
    }

    Write-Output "SUCCESS"
} catch {
    Write-Error $_.Exception.Message
    exit 1
}
)PS").arg(QString(path).replace("'", "''"));

    ProcessResult proc = runPowerShell(script, 15000);
    return proc.exit_code == 0 && proc.std_out.trimmed().contains("SUCCESS");
}

} // namespace sak
