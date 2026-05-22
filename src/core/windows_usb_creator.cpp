// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file windows_usb_creator.cpp
/// @brief Implements Windows USB installation media creation

#include "sak/windows_usb_creator.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryFile>
#include <QThread>

WindowsUSBCreator::WindowsUSBCreator(QObject* parent) : QObject(parent) {}

WindowsUSBCreator::~WindowsUSBCreator() {}

bool WindowsUSBCreator::createBootableUSB(const QString& isoPath, const QString& diskNumber) {
    Q_ASSERT(!isoPath.isEmpty());
    Q_ASSERT(!diskNumber.isEmpty());
    m_cancelled = false;
    setError({});
    m_diskNumber = diskNumber;

    if (!validateUSBInputs(isoPath, diskNumber)) {
        return false;
    }

    // ==================== STEP 1: FORMAT ====================
    QString driveLetter = formatAndVerifyDrive(diskNumber);
    if (driveLetter.isEmpty()) {
        return false;
    }

    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return false;
    }

    // ==================== STEP 2: EXTRACT ====================
    if (!extractAndVerifyFiles(isoPath, driveLetter)) {
        return false;
    }

    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return false;
    }

    // ==================== STEPS 3-4: BOOT CONFIGURATION ====================
    if (!configureBootAndVerify(diskNumber, driveLetter)) {
        return false;
    }

    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return false;
    }

    // ==================== STEP 5: FINAL VERIFICATION ====================
    Q_EMIT statusChanged("Step 5/5: Running final comprehensive verification...");
    sak::logInfo("STEP 5: Final comprehensive verification...");

    if (!finalVerification(driveLetter)) {
        sak::logError("STEP 5 VERIFICATION FAILED");
        Q_EMIT failed(lastError());
        return false;
    }

    // SUCCESS - finalVerification() emits completed() signal
    sak::logInfo("========================================");
    sak::logInfo("ALL STEPS COMPLETED AND VERIFIED");
    sak::logInfo("========================================");
    return true;
}

bool WindowsUSBCreator::validateUSBInputs(const QString& isoPath, const QString& diskNumber) {
    Q_ASSERT(!isoPath.isEmpty());
    Q_ASSERT(!diskNumber.isEmpty());
    // Validate diskNumber is a pure integer to prevent command injection.
    static const QRegularExpression diskNumRegex(QStringLiteral("^\\d{1,3}$"));
    if (!diskNumRegex.match(diskNumber).hasMatch()) {
        setError(QString("Invalid disk number format: %1").arg(diskNumber));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo(QString("========================================").toStdString());
    sak::logInfo(QString("Creating Windows bootable USB: %1 -> Disk %2")
                     .arg(isoPath, diskNumber)
                     .toStdString());
    sak::logInfo(QString("========================================").toStdString());

    // Verify ISO file exists
    if (!QFile::exists(isoPath)) {
        setError(QString("ISO file not found: %1").arg(isoPath));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    return true;
}

QString WindowsUSBCreator::formatAndVerifyDrive(const QString& diskNumber) {
    Q_ASSERT(!diskNumber.isEmpty());
    Q_EMIT progressUpdated(0);
    Q_EMIT statusChanged("Step 1/5: Formatting drive as NTFS...");
    sak::logInfo("STEP 1: Formatting disk...");

    if (!formatDriveNTFS(diskNumber)) {
        sak::logError("STEP 1 FAILED: Format failed");
        Q_EMIT failed(lastError());
        return {};
    }

    // VERIFY Step 1: Wait for partition and get drive letter
    Q_EMIT progressUpdated(5);
    Q_EMIT statusChanged("Waiting for partition to be recognized...");
    sak::logInfo("STEP 1: Verifying format and getting drive letter...");

    // Wait with progress updates
    for (int i = 0; i < 30; ++i) {
        QThread::msleep(sak::kTimerPollingFastMs);
        Q_EMIT progressUpdated(5 + (i * 5 / 30));  // 5% to 10%
        if (m_cancelled) {
            break;
        }
    }

    QString driveLetter = getDriveLetterFromDiskNumber();
    if (driveLetter.isEmpty()) {
        setError(QString("STEP 1 VERIFICATION FAILED: No drive letter found for disk %1 after "
                         "format")
                     .arg(diskNumber));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return {};
    }

    Q_EMIT progressUpdated(10);
    Q_EMIT statusChanged("Verifying NTFS filesystem...");

    if (!verifyNtfsFilesystem(driveLetter)) {
        return {};
    }

    Q_EMIT progressUpdated(13);
    Q_EMIT statusChanged("Format verified, preparing extraction...");

    return driveLetter;
}

bool WindowsUSBCreator::verifyNtfsFilesystem(const QString& driveLetter) {
    Q_ASSERT(!driveLetter.isEmpty());
    Q_ASSERT(driveLetter.length() == 1);

    QString checkCmd = QString("(Get-Volume -DriveLetter %1).FileSystem").arg(driveLetter);
    const auto check_result =
        sak::runPowerShell(checkCmd, sak::kTimeoutProcessShortMs, true, false, [this]() {
            return m_cancelled.load();
        });
    if (check_result.timed_out || check_result.cancelled) {
        setError(QString("STEP 1 VERIFICATION FAILED: "
                         "Filesystem check timed out for drive %1")
                     .arg(driveLetter));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    QString fs = check_result.std_out.trimmed();
    if (fs != "NTFS") {
        setError(QString("STEP 1 VERIFICATION FAILED: "
                         "Drive is %1, expected NTFS")
                     .arg(fs));
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }
    sak::logInfo(QString("[x] STEP 1 VERIFIED: "
                         "Drive %1: formatted as NTFS")
                     .arg(driveLetter)
                     .toStdString());
    return true;
}

bool WindowsUSBCreator::extractAndVerifyFiles(const QString& isoPath, const QString& driveLetter) {
    Q_ASSERT(!isoPath.isEmpty());
    Q_ASSERT(!driveLetter.isEmpty());
    // ==================== STEP 2: EXTRACT ====================
    Q_EMIT statusChanged("Step 2/5: Extracting Windows installation files...");
    sak::logInfo("STEP 2: Extracting ISO contents...");

    if (!copyISOContents(isoPath, driveLetter)) {
        sak::logError("STEP 2 FAILED: Extraction failed");
        Q_EMIT failed(lastError());
        return false;
    }

    // VERIFY Step 2: Check critical files exist
    sak::logInfo("STEP 2: Verifying extraction...");
    Q_EMIT statusChanged("Step 2/5: Verifying extracted files...");

    QString basePath = driveLetter + ":\\";
    QStringList criticalFiles = {"setup.exe", "sources\\boot.wim", "bootmgr"};

    // cppcheck-suppress useStlAlgorithm ; loop has side effects (per-file error + logging)
    for (const QString& file : criticalFiles) {
        QString fullPath = basePath + file;
        if (!QFile::exists(fullPath)) {
            setError(QString("STEP 2 VERIFICATION FAILED: Missing critical file: %1").arg(file));
            sak::logError(lastError().toStdString());
            Q_EMIT failed(lastError());
            return false;
        }
        QFileInfo info(fullPath);
        sak::logInfo(QString("  [x] %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
    }

    // Check for install image
    bool hasInstall = QFile::exists(basePath + "sources\\install.wim") ||
                      QFile::exists(basePath + "sources\\install.esd");
    if (!hasInstall) {
        setError("STEP 2 VERIFICATION FAILED: No install.wim or install.esd found");
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 2 VERIFIED: All critical files extracted");
    Q_EMIT progressUpdated(60);

    return true;
}

bool WindowsUSBCreator::setAndVerifyBootFlag(const QString& diskNumber,
                                             const QString& driveLetter) {
    Q_EMIT statusChanged("Step 4/5: Setting bootable flag...");
    sak::logInfo("STEP 4: Setting bootable flag...");

    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        setError("STEP 4 FAILED: Could not create diskpart script");
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    QString diskpartScript = QString(
                                 "select disk %1\\n"
                                 "select partition 1\\n"
                                 "active\\n")
                                 .arg(diskNumber);

    const QByteArray script_bytes = diskpartScript.toLocal8Bit();
    if (scriptFile.write(script_bytes) != script_bytes.size()) {
        setError("STEP 4 FAILED: Could not write diskpart script");
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }
    scriptFile.flush();

    const auto diskpart_result = sak::runProcess(QStringLiteral("cmd.exe"),
                                                 {QStringLiteral("/c"),
                                                  QStringLiteral("diskpart"),
                                                  QStringLiteral("/s"),
                                                  scriptFile.fileName()},
                                                 sak::kTimeoutProcessLongMs,
                                                 [this]() { return m_cancelled.load(); });
    if (!diskpart_result.succeeded()) {
        setError("STEP 4 FAILED: Diskpart failed to set active flag");
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("STEP 4: Verifying bootable flag...");
    Q_EMIT statusChanged("Step 4/5: Verifying bootable flag...");

    if (!verifyBootableFlag(driveLetter)) {
        setError("STEP 4 VERIFICATION FAILED: " + lastError());
        sak::logError(lastError().toStdString());
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 4 VERIFIED: Bootable flag is set (Active)");
    Q_EMIT progressUpdated(85);
    return true;
}

bool WindowsUSBCreator::configureBootAndVerify(const QString& diskNumber,
                                               const QString& driveLetter) {
    // ==================== STEP 3: MAKE BOOTABLE ====================
    Q_EMIT progressUpdated(62);
    Q_EMIT statusChanged("Step 3/5: Making drive bootable...");
    sak::logInfo("STEP 3: Making drive bootable...");

    if (!makeBootable(driveLetter)) {
        sak::logError("STEP 3 FAILED: Could not configure boot files");
        Q_EMIT failed(lastError());
        return false;
    }

    sak::logInfo("[x] STEP 3 COMPLETED: Boot configuration done");
    Q_EMIT progressUpdated(70);

    if (m_cancelled) {
        setError("Operation cancelled");
        Q_EMIT failed(lastError());
        return false;
    }

    // ==================== STEP 4: SET BOOT FLAG ====================
    return setAndVerifyBootFlag(diskNumber, driveLetter);
}

void WindowsUSBCreator::cancel() {
    m_cancelled = true;
    sak::logInfo("Windows USB creation cancelled");
}

QString WindowsUSBCreator::lastError() const {
    QMutexLocker locker(&m_errorMutex);
    return m_lastError;
}

bool WindowsUSBCreator::cleanAndPartitionDisk(const QString& diskNumber) {
    Q_ASSERT(!diskNumber.isEmpty());
    QString diskpartScript = QString(
                                 "select disk %1\n"
                                 "clean\n"
                                 "create partition primary\n"
                                 "select partition 1\n"
                                 "active\n"
                                 "exit\n")
                                 .arg(diskNumber);

    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        setError("Failed to create temporary diskpart script");
        sak::logError(lastError().toStdString());
        return false;
    }

    const QByteArray script_bytes = diskpartScript.toLocal8Bit();
    if (scriptFile.write(script_bytes) != script_bytes.size()) {
        setError("Failed to write diskpart script");
        sak::logError(lastError().toStdString());
        return false;
    }
    scriptFile.flush();

    sak::logInfo(QString("Running diskpart script:\n%1").arg(diskpartScript).toStdString());

    // Diskpart requires Administrator privileges
    const auto diskpart_result = sak::runProcess(QStringLiteral("cmd.exe"),
                                                 {QStringLiteral("/c"),
                                                  QStringLiteral("diskpart"),
                                                  QStringLiteral("/s"),
                                                  scriptFile.fileName()},
                                                 sak::kTimeoutProcessLongMs,
                                                 [this]() { return m_cancelled.load(); });

    if (diskpart_result.timed_out || diskpart_result.cancelled) {
        setError("Diskpart timed out");
        sak::logError(lastError().toStdString());
        return false;
    }

    QString output = diskpart_result.std_out;
    QString errors = diskpart_result.std_err;
    sak::logInfo(QString("Diskpart output:\n%1").arg(output).toStdString());
    if (!errors.isEmpty()) {
        sak::logError(QString("Diskpart errors:\n%1").arg(errors).toStdString());
    }

    if (diskpart_result.exit_code != 0) {
        setError(QString("Diskpart failed with exit code %1. Ensure you are running as "
                         "Administrator.")
                     .arg(diskpart_result.exit_code));
        sak::logError(lastError().toStdString());
        return false;
    }

    return true;
}

bool WindowsUSBCreator::formatPartitionNTFS(const QString& diskNumber) {
    Q_ASSERT(!diskNumber.isEmpty());
    sak::logInfo("Waiting for Windows to recognize partition...");
    Q_EMIT statusChanged("Formatting partition as NTFS...");
    QThread::msleep(sak::kTimerStatusMessageMs);

    QString formatCmd = QString("format FS=NTFS QUICK label=\"BOOT\"");
    QString formatScript = QString(
                               "select disk %1\n"
                               "select partition 1\n"
                               "%2\n"
                               "exit\n")
                               .arg(diskNumber, formatCmd);

    QTemporaryFile formatScriptFile;
    if (!formatScriptFile.open()) {
        setError("Failed to create format script");
        sak::logError(lastError().toStdString());
        return false;
    }

    const QByteArray format_bytes = formatScript.toLocal8Bit();
    if (formatScriptFile.write(format_bytes) != format_bytes.size()) {
        setError("Failed to write format script");
        sak::logError(lastError().toStdString());
        return false;
    }
    formatScriptFile.flush();

    sak::logInfo(QString("Running format script:\n%1").arg(formatScript).toStdString());

    const auto format_result = sak::runProcess(QStringLiteral("cmd.exe"),
                                               {QStringLiteral("/c"),
                                                QStringLiteral("diskpart"),
                                                QStringLiteral("/s"),
                                                formatScriptFile.fileName()},
                                               sak::kTimeoutProcessVeryLongMs,
                                               [this]() { return m_cancelled.load(); });

    if (format_result.timed_out || format_result.cancelled) {
        setError("Format timed out");
        sak::logError(lastError().toStdString());
        return false;
    }

    QString formatOutput = format_result.std_out;
    QString formatErrors = format_result.std_err;
    sak::logInfo(QString("Format output:\n%1").arg(formatOutput).toStdString());
    if (!formatErrors.isEmpty()) {
        sak::logError(QString("Format errors:\n%1").arg(formatErrors).toStdString());
    }

    if (format_result.exit_code != 0) {
        setError(QString("Format failed with exit code %1").arg(format_result.exit_code));
        sak::logError(lastError().toStdString());
        return false;
    }

    // Wait for format to complete
    sak::logInfo("Waiting for format to settle...");
    QThread::msleep(sak::kTimerStatusMessageMs);

    return true;
}

bool WindowsUSBCreator::formatDriveNTFS(const QString& diskNumber) {
    Q_ASSERT(!diskNumber.isEmpty());
    // Use the official Microsoft method: diskpart clean, then format command
    // Based on Windows Server USB creation documentation and Rufus implementation

    sak::logInfo(QString("Formatting disk %1 as NTFS").arg(diskNumber).toStdString());
    Q_EMIT statusChanged("Preparing USB drive...");

    // Step 1: Clean the disk and create MBR partition using diskpart
    if (!cleanAndPartitionDisk(diskNumber)) {
        return false;
    }

    // Step 2: Wait for partition and format with NTFS
    if (!formatPartitionNTFS(diskNumber)) {
        return false;
    }

    sak::logInfo(QString("Successfully formatted disk %1 as NTFS").arg(diskNumber).toStdString());
    return true;
}

QString WindowsUSBCreator::getDriveLetterFromDiskNumber() {
    Q_ASSERT(!m_diskNumber.isEmpty());
    if (m_diskNumber.isEmpty()) {
        setError("Cannot query drive letter: No disk number set");
        sak::logError(lastError().toStdString());
        return QString();
    }

    // Validate disk number is numeric
    bool ok = false;
    int diskNum = m_diskNumber.toInt(&ok);
    if (!ok || diskNum < 0) {
        setError(QString("Invalid disk number format: '%1'").arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return QString();
    }

    sak::logInfo(QString("Querying drive letter for disk %1").arg(m_diskNumber).toStdString());

    QString cmd = QString(
                      "(Get-Partition -DiskNumber %1 | Get-Volume | Where-Object "
                      "{$_.DriveLetter -ne $null} | Select-Object -First 1).DriveLetter")
                      .arg(m_diskNumber);
    const auto drive_result = sak::runPowerShell(
        cmd, sak::kTimeoutProcessMediumMs, true, false, [this]() { return m_cancelled.load(); });

    if (drive_result.timed_out || drive_result.cancelled) {
        setError(QString("Timeout querying drive letter for disk %1").arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return QString();
    }

    if (drive_result.exit_code != 0) {
        QString errors = drive_result.std_err.trimmed();
        setError(QString("PowerShell query failed for disk %1: %2").arg(m_diskNumber, errors));
        sak::logError(lastError().toStdString());
        return QString();
    }

    QString driveLetter = validateDriveLetter(drive_result.std_out.trimmed());
    if (driveLetter.isEmpty()) {
        return {};
    }

    sak::logInfo(QString("[x] Successfully mapped disk %1 "
                         "to drive letter %2")
                     .arg(m_diskNumber, driveLetter)
                     .toStdString());
    return driveLetter;
}

QString WindowsUSBCreator::validateDriveLetter(const QString& rawLetter) {
    Q_ASSERT(!rawLetter.isEmpty());
    Q_ASSERT(!m_diskNumber.isEmpty());

    if (rawLetter.isEmpty()) {
        setError(QString("No drive letter assigned to disk %1. "
                         "Drive may not be formatted or "
                         "partition not recognized.")
                     .arg(m_diskNumber));
        sak::logError(lastError().toStdString());
        return {};
    }

    if (rawLetter.length() != 1 || !rawLetter[0].isLetter()) {
        setError(QString("Invalid drive letter from PowerShell: "
                         "'%1' (expected single A-Z character)")
                     .arg(rawLetter));
        sak::logError(lastError().toStdString());
        return {};
    }

    return rawLetter.toUpper();
}
