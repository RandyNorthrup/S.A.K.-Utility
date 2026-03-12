// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file backup_bitlocker_keys_action.cpp
/// @brief Implements BitLocker recovery key backup to a specified location

#include "sak/actions/backup_bitlocker_keys_action.h"

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
#include <QSysInfo>
#include <QTextStream>

#include <array>

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

template <std::size_t N>
QString lookupCodeDescription(const CodeDescriptionEntry (&table)[N], int code) {
    for (const auto& entry : table) {
        if (entry.code == code) {
            return QString::fromLatin1(entry.description);
        }
    }
    return QString("Unknown (%1)").arg(code);
}

}  // namespace

QString BackupBitlockerKeysAction::formatEncryptionMethod(int method_code) {
    Q_ASSERT(method_code >= 0);
    return lookupCodeDescription(kEncryptionMethods, method_code);
}

QString BackupBitlockerKeysAction::formatProtectorType(int type_code) {
    Q_ASSERT(type_code >= 0);
    return lookupCodeDescription(kProtectorTypes, type_code);
}

QString BackupBitlockerKeysAction::formatVolumeType(int type_code) {
    switch (type_code) {
    case 0:
        return "Operating System";
    case 1:
        return "Fixed Data";
    case 2:
        return "Removable Data";
    default:
        return QString("Unknown (%1)").arg(type_code);
    }
}

QString BackupBitlockerKeysAction::backupTimestamp() {
    return QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
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
    const QString& output) {
    QVector<VolumeInfo> volumes;

    if (output.isEmpty()) {
        return volumes;
    }

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(output.toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError) {
        Q_EMIT logMessage("Failed to parse BitLocker volume data: " + parse_error.errorString());
        return volumes;
    }

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
        vi.protection_status = protectionStatusLabel(obj["ProtectionStatus"].toInt(-1));
        vi.encryption_method = formatEncryptionMethod(obj["EncryptionMethod"].toInt());

        int enc_pct = obj["EncryptionPct"].toInt(-1);
        vi.encryption_percentage = (enc_pct >= 0) ? QString("%1%").arg(enc_pct) : "N/A";
        vi.lock_status = lockStatusLabel(obj["LockStatus"].toInt(-1));

        volumes.append(vi);
    }

    return volumes;
}

QVector<BackupBitlockerKeysAction::VolumeInfo> BackupBitlockerKeysAction::detectEncryptedVolumes() {
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
        return {};
    }

    return parseDetectedVolumes(proc.std_out.trimmed());
}

// ============================================================================
// Key Protector Retrieval
// ============================================================================

QVector<BackupBitlockerKeysAction::KeyProtectorInfo> BackupBitlockerKeysAction::getKeyProtectors(
    const QString& drive_letter) {
    QVector<KeyProtectorInfo> protectors;

    const QString script = buildKeyProtectorScript(drive_letter);

    ProcessResult proc = runPowerShell(script, sak::kTimeoutProcessLongMs);

    if (!proc.succeeded()) {
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

    return parseKeyProtectorResponse(output);
}

QString BackupBitlockerKeysAction::buildKeyProtectorScript(const QString& drive_letter) const {
    Q_ASSERT(!drive_letter.isEmpty());
    return QString(R"PS(
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
)PS")
        .arg(drive_letter);
}

QVector<BackupBitlockerKeysAction::KeyProtectorInfo>
BackupBitlockerKeysAction::parseKeyProtectorResponse(const QString& output) {
    Q_ASSERT(!output.isEmpty());
    QVector<KeyProtectorInfo> protectors;

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

void BackupBitlockerKeysAction::scan() {
    setStatus(ActionStatus::Scanning);
    Q_ASSERT(status() == ActionStatus::Scanning);
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
        result.estimated_duration_ms = m_volumes.size() * 5000;  // ~5s per volume
        result.warning = "Recovery keys are sensitive -- store the backup securely";
    }

    Q_ASSERT(!result.summary.isEmpty());

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
    Q_ASSERT(status() == ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    Q_ASSERT(start_time.isValid());

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
    Q_EMIT executionProgress("Detecting BitLocker volumes...", 5);

    if (m_volumes.isEmpty()) {
        m_volumes = detectEncryptedVolumes();
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
    Q_EMIT executionProgress("Retrieving recovery keys...", 15);

    for (int i = 0; i < m_volumes.size(); ++i) {
        if (isCancelled()) {
            emitCancelledResult("BitLocker key backup cancelled", start_time);
            return false;
        }

        auto& vol = m_volumes[i];
        int progress = 15 + static_cast<int>((static_cast<double>(i) / m_volumes.size()) * 40);
        Q_EMIT executionProgress(QString("Retrieving keys for %1 (%2/%3)...")
                                     .arg(vol.drive_letter)
                                     .arg(i + 1)
                                     .arg(m_volumes.size()),
                                 progress);

        vol.key_protectors = getKeyProtectors(vol.drive_letter);
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

    if (total_keys_found == 0) {
        emitFailedResult("No key protectors found on any volume",
                         "BitLocker volumes were detected but no key protectors could be read.\n"
                         "Ensure the application has administrator privileges.",
                         start_time);
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }
    return true;
}

bool BackupBitlockerKeysAction::executeSaveKeyFiles(const QDateTime& start_time,
                                                    QString& backup_dir_path,
                                                    int& key_files_written,
                                                    bool& permissions_set) {
    Q_EMIT executionProgress("Creating backup directory...", 60);

    QString timestamp = backupTimestamp();
    QString backup_dir_name = QString("BitLocker_Keys_%1").arg(timestamp);
    backup_dir_path = QDir(m_backup_location).filePath(backup_dir_name);

    QDir backup_dir(backup_dir_path);
    if (!backup_dir.mkpath(".")) {
        emitFailedResult("Failed to create backup directory",
                         "Could not create: " + backup_dir_path,
                         start_time);
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }

    Q_EMIT executionProgress("Writing recovery key document...", 70);

    bool doc_written = writeRecoveryDocument(backup_dir_path);
    if (!doc_written) {
        emitFailedResult("Failed to write recovery key document", QString(), start_time);
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }

    Q_EMIT executionProgress("Writing per-volume key files...", 80);

    key_files_written = writePerVolumeKeyFiles(backup_dir_path);

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }

    Q_EMIT executionProgress("Writing JSON backup...", 85);

    if (!writeJsonBackup(backup_dir_path)) {
        emitFailedResult("Failed to write BitLocker key backup file", QString(), start_time);
        return false;
    }

    if (isCancelled()) {
        emitCancelledResult("BitLocker key backup cancelled", start_time);
        return false;
    }

    Q_EMIT executionProgress("Securing backup files...", 90);

    permissions_set = restrictFilePermissions(backup_dir_path);
    if (!permissions_set) {
        Q_EMIT logMessage("Warning: Could not restrict backup directory permissions");
    }

    return true;
}

bool BackupBitlockerKeysAction::writeJsonBackup(const QString& backup_dir_path) {
    Q_ASSERT(!backup_dir_path.isEmpty());
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
    QFile json_file(json_path);
    if (json_file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        const QByteArray data = QJsonDocument(json_backup).toJson(QJsonDocument::Indented);
        if (json_file.write(data) != data.size()) {
            logError("Incomplete write of BitLocker key backup JSON");
            return false;
        }
        json_file.close();
        return true;
    }

    logError("Failed to write BitLocker key backup JSON: {}",
             json_file.errorString().toStdString());
    return false;
}

void BackupBitlockerKeysAction::executeBuildReport(const QDateTime& start_time,
                                                   const BitlockerReportData& data) {
    Q_EMIT executionProgress("Finalizing backup...", 95);

    qint64 total_bytes = 0;
    int total_files = 0;
    QDirIterator dir_it(data.backup_dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (dir_it.hasNext()) {
        dir_it.next();
        total_bytes += dir_it.fileInfo().size();
        total_files++;
    }

    Q_EMIT executionProgress("Backup complete", 100);

    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    ExecutionResult result;
    Q_ASSERT(!result.success);  // verify default init
    result.success = true;
    result.bytes_processed = total_bytes;
    result.files_processed = total_files;
    result.duration_ms = duration_ms;
    result.output_path = data.backup_dir_path;

    result.message = QString("Backed up %1 recovery key(s) from %2 volume(s)")
                         .arg(data.total_recovery_passwords)
                         .arg(m_volumes.size());

    QStringList log_lines;
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

    Q_ASSERT(result.duration_ms >= 0);

    finishWithResult(result, ActionStatus::Success);
}

// ============================================================================
// File Output -- Master Recovery Document
// ============================================================================

bool BackupBitlockerKeysAction::writeRecoveryDocument(const QString& backup_dir) {
    QString doc_path = QDir(backup_dir).filePath("BitLocker_Recovery_Keys.txt");

    QFile file(doc_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        Q_EMIT logMessage("Failed to open recovery document for writing: " + doc_path);
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    writeRecoveryDocumentHeader(out);
    writeRecoveryDocumentVolumes(out);
    writeRecoveryDocumentFooter(out);

    file.close();
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
    Q_ASSERT(!m_volumes.empty());
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
            out << "  Volume Size:          " << QString::number(size_gb, 'f', 2) << " GB\n";
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

void BackupBitlockerKeysAction::writeRecoveryDocumentFooter(QTextStream& out) const {
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

int BackupBitlockerKeysAction::writePerVolumeKeyFiles(const QString& backup_dir) {
    Q_ASSERT(!backup_dir.isEmpty());
    int files_written = 0;

    for (const auto& vol : m_volumes) {
        // Only write files for volumes that have recovery passwords
        if (!volumeHasRecoveryPassword(vol.key_protectors)) {
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

        writeVolumeKeyEntries(out, vol.key_protectors);

        out << "Drive:        " << vol.drive_letter;
        if (!vol.volume_label.isEmpty()) {
            out << " (" << vol.volume_label << ")";
        }
        out << "\n";
        out << "Computer:     " << QSysInfo::machineHostName() << "\n";
        out << "Date:         " << QDateTime::currentDateTime().toString("yyyy-MM-dd hh:mm:ss")
            << "\n";
        out << "\n";
        out << "If the above identifier matches the one shown on your PC, you can use\n";
        out << "the corresponding recovery key to unlock the drive.\n";

        file.close();
        files_written++;
    }

    return files_written;
}

// ============================================================================
// Security -- Restrict File Permissions
// ============================================================================

bool BackupBitlockerKeysAction::restrictFilePermissions(const QString& path) {
    Q_ASSERT(!path.isEmpty());
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
)PS")
                               .arg(QString(path).replace("'", "''"));

    ProcessResult proc = runPowerShell(script, sak::kTimeoutChocoListMs);
    return proc.succeeded() && proc.std_out.trimmed().contains("SUCCESS");
}

// ============================================================================
// Extracted Helpers -- Nesting Reduction
// ============================================================================

int BackupBitlockerKeysAction::countRecoveryPasswords(const QVector<KeyProtectorInfo>& protectors) {
    int count = 0;
    for (const auto& kp : protectors) {
        if (!kp.recovery_password.isEmpty()) {
            count++;
        }
    }
    return count;
}

bool BackupBitlockerKeysAction::volumeHasRecoveryPassword(
    const QVector<KeyProtectorInfo>& protectors) {
    for (const auto& kp : protectors) {
        if (!kp.recovery_password.isEmpty()) {
            return true;
        }
    }
    return false;
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

void BackupBitlockerKeysAction::writeVolumeKeyEntries(
    QTextStream& out, const QVector<KeyProtectorInfo>& protectors) const {
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
