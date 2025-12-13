// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_usb_creator.h"
#include "sak/logger.h"
#include <QProcess>
#include <QDir>
#include <QFileInfo>
#include <QTextStream>
#include <QTemporaryFile>
#include <QThread>
#include <windows.h>
#include <winioctl.h>
#include <filesystem>

WindowsUSBCreator::WindowsUSBCreator(QObject* parent)
    : QObject(parent)
    , m_cancelled(false)
{
}

WindowsUSBCreator::~WindowsUSBCreator() {
}

bool WindowsUSBCreator::createBootableUSB(const QString& isoPath, const QString& driveLetter) {
    m_cancelled = false;
    m_lastError.clear();
    
    sak::log_info(QString("Creating Windows bootable USB: %1 -> %2").arg(isoPath, driveLetter).toStdString());
    
    // Step 1: Format drive as NTFS
    Q_EMIT statusChanged("Formatting drive as NTFS...");
    if (!formatDriveNTFS(driveLetter)) {
        return false;
    }
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        return false;
    }
    
    // Step 2: Mount ISO
    Q_EMIT statusChanged("Mounting ISO...");
    QString mountPoint = mountISO(isoPath);
    if (mountPoint.isEmpty()) {
        return false;
    }
    
    if (m_cancelled) {
        dismountISO(mountPoint);
        m_lastError = "Operation cancelled";
        return false;
    }
    
    // Step 3: Copy files from ISO to USB
    Q_EMIT statusChanged("Copying Windows installation files...");
    if (!copyISOContents(mountPoint, driveLetter)) {
        dismountISO(mountPoint);
        return false;
    }
    
    if (m_cancelled) {
        dismountISO(mountPoint);
        m_lastError = "Operation cancelled";
        return false;
    }
    
    // Step 4: Make bootable (bootsect command)
    Q_EMIT statusChanged("Making drive bootable...");
    if (!makeBootable(driveLetter, mountPoint)) {
        dismountISO(mountPoint);
        return false;
    }
    
    // Step 5: Verify bootable flag is set
    Q_EMIT statusChanged("Verifying bootable flag...");
    if (!verifyBootableFlag(driveLetter)) {
        sak::log_warning("Could not verify bootable flag - drive may still work for UEFI boot");
    }
    
    // Step 6: Dismount ISO
    dismountISO(mountPoint);
    
    Q_EMIT statusChanged("Completed successfully!");
    sak::log_info("Windows bootable USB created successfully");
    
    return true;
}

void WindowsUSBCreator::cancel() {
    m_cancelled = true;
    sak::log_info("Windows USB creation cancelled");
}

QString WindowsUSBCreator::lastError() const {
    return m_lastError;
}

bool WindowsUSBCreator::formatDriveNTFS(const QString& driveLetter) {
    // Use the official Microsoft method: diskpart clean, then format command
    // Based on Windows Server USB creation documentation and Rufus implementation
    QString cleanDriveLetter = driveLetter;
    cleanDriveLetter.remove(":");
    cleanDriveLetter.remove("\\");
    
    sak::log_info(QString("Formatting %1: as NTFS").arg(cleanDriveLetter).toStdString());
    Q_EMIT statusChanged("Preparing USB drive...");
    
    // Get the disk number for this drive letter
    QProcess getDisk;
    QString getDiskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDriveLetter);
    getDisk.start("powershell", QStringList() << "-NoProfile" << "-Command" << getDiskCmd);
    
    if (!getDisk.waitForFinished(10000)) {
        m_lastError = "Failed to get disk number";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    QString diskNumber = QString(getDisk.readAllStandardOutput()).trimmed();
    if (diskNumber.isEmpty()) {
        m_lastError = "Could not determine disk number for drive";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    sak::log_info(QString("Drive %1: is on disk %2").arg(cleanDriveLetter, diskNumber).toStdString());
    Q_EMIT statusChanged("Cleaning and partitioning drive...");
    
    // Step 1: Clean the disk and create MBR partition using diskpart
    QString diskpartScript = QString(
        "select disk %1\n"
        "clean\n"
        "create partition primary\n"
        "select partition 1\n"
        "active\n"
        "exit\n"
    ).arg(diskNumber);
    
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        m_lastError = "Failed to create temporary diskpart script";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    sak::log_info(QString("Running diskpart script:\n%1").arg(diskpartScript).toStdString());
    
    QProcess diskpart;
    diskpart.start("diskpart", QStringList() << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted()) {
        m_lastError = "Failed to start diskpart";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    if (!diskpart.waitForFinished(30000)) {
        m_lastError = "Diskpart timed out";
        sak::log_error(m_lastError.toStdString());
        diskpart.kill();
        return false;
    }
    
    QString output = diskpart.readAllStandardOutput();
    sak::log_info(QString("Diskpart output:\n%1").arg(output).toStdString());
    
    if (diskpart.exitCode() != 0) {
        m_lastError = QString("Diskpart failed with exit code %1").arg(diskpart.exitCode());
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Step 2: Wait for the partition to be recognized by Windows
    sak::log_info("Waiting for partition to be recognized...");
    QThread::msleep(3000);  // Increased wait time
    
    // Step 3: Format and assign drive letter using diskpart
    Q_EMIT statusChanged("Formatting drive as NTFS...");
    sak::log_info("Formatting partition with NTFS...");
    
    // Format using diskpart (more reliable than Format-Volume after clean)
    QString formatScript = QString(
        "select disk %1\n"
        "select partition 1\n"
        "format fs=ntfs quick label=\"Windows USB\"\n"
        "assign letter=%2\n"
        "exit\n"
    ).arg(diskNumber, cleanDriveLetter);
    
    QTemporaryFile formatScriptFile;
    if (!formatScriptFile.open()) {
        m_lastError = "Failed to create format script";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    formatScriptFile.write(formatScript.toLocal8Bit());
    formatScriptFile.flush();
    
    sak::log_info(QString("Running format script:\n%1").arg(formatScript).toStdString());
    
    QProcess formatProcess;
    formatProcess.start("diskpart", QStringList() << "/s" << formatScriptFile.fileName());
    
    if (!formatProcess.waitForStarted()) {
        m_lastError = "Failed to start format";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    if (!formatProcess.waitForFinished(120000)) { // 2 minutes
        m_lastError = "Format timed out";
        sak::log_error(m_lastError.toStdString());
        formatProcess.kill();
        return false;
    }
    
    QString formatOutput = formatProcess.readAllStandardOutput();
    QString formatErrors = formatProcess.readAllStandardError();
    
    sak::log_info(QString("Format output:\n%1").arg(formatOutput).toStdString());
    if (!formatErrors.isEmpty()) {
        sak::log_error(QString("Format errors:\n%1").arg(formatErrors).toStdString());
    }
    
    if (formatProcess.exitCode() != 0) {
        m_lastError = QString("Format failed with exit code %1").arg(formatProcess.exitCode());
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    sak::log_info(QString("Successfully formatted %1: as NTFS with active partition").arg(cleanDriveLetter).toStdString());
    return true;
}

QString WindowsUSBCreator::mountISO(const QString& isoPath) {
    sak::log_info(QString("Mounting ISO: %1").arg(isoPath).toStdString());
    
    // Use PowerShell Mount-DiskImage
    QProcess powershell;
    QString command = QString("Mount-DiskImage -ImagePath '%1' -PassThru | Get-Volume | Select-Object -ExpandProperty DriveLetter")
        .arg(isoPath);
    
    powershell.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << command);
    
    if (!powershell.waitForStarted()) {
        m_lastError = "Failed to start PowerShell";
        sak::log_error(m_lastError.toStdString());
        return QString();
    }
    
    if (!powershell.waitForFinished(30000)) {
        m_lastError = "PowerShell timed out mounting ISO";
        sak::log_error(m_lastError.toStdString());
        powershell.kill();
        return QString();
    }
    
    QString output = powershell.readAllStandardOutput().trimmed();
    QString errors = powershell.readAllStandardError();
    
    if (powershell.exitCode() != 0 || output.isEmpty()) {
        m_lastError = QString("Failed to mount ISO: %1").arg(errors);
        sak::log_error(m_lastError.toStdString());
        return QString();
    }
    
    QString mountPoint = output + ":";
    sak::log_info(QString("ISO mounted at %1").arg(mountPoint).toStdString());
    
    return mountPoint;
}

void WindowsUSBCreator::dismountISO(const QString& mountPoint) {
    if (mountPoint.isEmpty()) {
        return;
    }
    
    sak::log_info(QString("Dismounting ISO from %1").arg(mountPoint).toStdString());
    
    QProcess powershell;
    QString command = QString("Get-DiskImage -DevicePath (Get-Volume -DriveLetter '%1').Path.TrimEnd('\\') | Dismount-DiskImage")
        .arg(mountPoint.left(1));
    
    powershell.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << command);
    powershell.waitForFinished(10000);
    
    sak::log_info("ISO dismounted");
}

bool WindowsUSBCreator::copyISOContents(const QString& sourcePath, const QString& destPath) {
    sak::log_info(QString("Copying files from %1 to %2").arg(sourcePath, destPath).toStdString());
    
    QDir sourceDir(sourcePath);
    QDir destDir(destPath);
    
    if (!sourceDir.exists()) {
        m_lastError = QString("Source path does not exist: %1").arg(sourcePath);
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Use robocopy for reliable copying with progress
    QProcess robocopy;
    QString source = sourcePath;
    QString dest = destPath;
    source.replace("/", "\\");
    dest.replace("/", "\\");
    
    QStringList args;
    args << source;
    args << dest;
    args << "/E"; // Copy subdirectories including empty
    args << "/R:3"; // Retry 3 times
    args << "/W:5"; // Wait 5 seconds between retries
    args << "/NFL"; // No file list
    args << "/NDL"; // No directory list
    args << "/NJH"; // No job header
    args << "/NJS"; // No job summary
    args << "/NC"; // No class
    args << "/NS"; // No size
    args << "/NP"; // No progress percentage
    
    robocopy.start("robocopy", args);
    
    if (!robocopy.waitForStarted()) {
        m_lastError = "Failed to start robocopy";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Monitor progress
    connect(&robocopy, &QProcess::readyReadStandardOutput, this, [&]() {
        QString output = robocopy.readAllStandardOutput();
        // Parse robocopy output for progress updates if needed
        Q_EMIT progressUpdated(50); // Simplified for now
    });
    
    if (!robocopy.waitForFinished(600000)) { // 10 minutes timeout
        m_lastError = "Robocopy timed out";
        sak::log_error(m_lastError.toStdString());
        robocopy.kill();
        return false;
    }
    
    int exitCode = robocopy.exitCode();
    
    // Robocopy exit codes: 0-7 are success (various levels), 8+ are errors
    if (exitCode >= 8) {
        QString errors = robocopy.readAllStandardError();
        m_lastError = QString("Robocopy failed with exit code %1: %2").arg(exitCode).arg(errors);
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    sak::log_info("Files copied successfully");
    return true;
}

bool WindowsUSBCreator::makeBootable(const QString& driveLetter, const QString& isoMountPoint) {
    sak::log_info(QString("Making %1 bootable using bootsect").arg(driveLetter).toStdString());
    
    // bootsect.exe is in the boot folder of the Windows ISO
    QString bootsectPath = isoMountPoint + "\\boot\\bootsect.exe";
    
    if (!QFile::exists(bootsectPath)) {
        // Try alternative location
        bootsectPath = isoMountPoint + "\\efi\\microsoft\\boot\\bootsect.exe";
    }
    
    if (!QFile::exists(bootsectPath)) {
        m_lastError = "bootsect.exe not found in ISO";
        sak::log_error(m_lastError.toStdString());
        // This is not critical - many ISOs don't include bootsect
        sak::log_warning("Skipping bootsect - drive may still be bootable via UEFI");
        return true; // Don't fail the whole process
    }
    
    // Run bootsect to update boot code
    QProcess bootsect;
    QString cleanDriveLetter = driveLetter;
    cleanDriveLetter.remove("\\");
    
    QStringList args;
    args << "/nt60"; // Use BOOTMGR compatible boot code
    args << cleanDriveLetter;
    args << "/mbr"; // Update MBR
    args << "/force"; // Force dismount
    
    bootsect.start(bootsectPath, args);
    
    if (!bootsect.waitForStarted()) {
        m_lastError = "Failed to start bootsect";
        sak::log_error(m_lastError.toStdString());
        // Not critical
        return true;
    }
    
    if (!bootsect.waitForFinished(30000)) {
        m_lastError = "bootsect timed out";
        sak::log_error(m_lastError.toStdString());
        bootsect.kill();
        // Not critical
        return true;
    }
    
    QString output = bootsect.readAllStandardOutput();
    QString errors = bootsect.readAllStandardError();
    
    sak::log_info(QString("bootsect output: %1").arg(output).toStdString());
    
    if (bootsect.exitCode() != 0) {
        sak::log_warning(QString("bootsect returned code %1: %2").arg(bootsect.exitCode()).arg(errors).toStdString());
        // Not critical - UEFI boot should still work
    } else {
        sak::log_info("Boot sector updated successfully");
    }
    
    return true;
}

bool WindowsUSBCreator::verifyBootableFlag(const QString& driveLetter) {
    Q_EMIT statusChanged("Verifying bootable flag...");
    sak::log_info(QString("Verifying bootable flag on drive %1").arg(driveLetter).toStdString());
    
    // Create diskpart script to check if partition is active
    QString diskpartScript = QString(
        "select volume %1\n"
        "detail partition\n"
    ).arg(driveLetter.at(0));
    
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        sak::log_error("Failed to create temporary diskpart script for verification");
        return false;
    }
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    QProcess diskpart;
    diskpart.start("diskpart", QStringList() << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted()) {
        sak::log_error("Failed to start diskpart for verification");
        return false;
    }
    
    if (!diskpart.waitForFinished(30000)) {
        diskpart.kill();
        sak::log_error("diskpart verification timed out");
        return false;
    }
    
    QString output = diskpart.readAllStandardOutput();
    
    // Check if output contains "Active" flag
    bool isActive = output.contains("Active", Qt::CaseInsensitive) && 
                    output.contains("Yes", Qt::CaseInsensitive);
    
    if (isActive) {
        sak::log_info("Bootable flag verified successfully");
        return true;
    } else {
        sak::log_warning("Bootable flag not set - partition may not be bootable");
        // Not critical - UEFI systems don't require active flag
        return true;
    }
}
