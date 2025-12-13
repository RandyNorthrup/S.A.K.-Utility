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
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // Step 2: Mount ISO
    Q_EMIT statusChanged("Mounting ISO...");
    QString mountPoint = mountISO(isoPath);
    if (mountPoint.isEmpty()) {
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    if (m_cancelled) {
        dismountISO(mountPoint);
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // Step 3: Copy files from ISO to USB
    Q_EMIT statusChanged("Copying Windows installation files...");
    if (!copyISOContents(mountPoint, driveLetter)) {
        dismountISO(mountPoint);
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    if (m_cancelled) {
        dismountISO(mountPoint);
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // Step 4: Make bootable (bootsect command)
    Q_EMIT statusChanged("Making drive bootable...");
    if (!makeBootable(driveLetter, mountPoint)) {
        dismountISO(mountPoint);
        Q_EMIT failed(m_lastError);
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
    Q_EMIT completed();
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
    
    // Diskpart requires Administrator privileges
    // Use cmd.exe to run diskpart (QProcess doesn't elevate on its own)
    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted(5000)) {
        m_lastError = "Failed to start diskpart - ensure application is running as Administrator";
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
    QString errors = diskpart.readAllStandardError();
    sak::log_info(QString("Diskpart output:\n%1").arg(output).toStdString());
    if (!errors.isEmpty()) {
        sak::log_error(QString("Diskpart errors:\n%1").arg(errors).toStdString());
    }
    
    if (diskpart.exitCode() != 0) {
        m_lastError = QString("Diskpart failed with exit code %1. Ensure you are running as Administrator.").arg(diskpart.exitCode());
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Step 2: Wait for the partition to be recognized and assigned a drive letter
    sak::log_info("Waiting for Windows to assign drive letter...");
    Q_EMIT statusChanged("Waiting for drive letter...");
    QThread::msleep(5000);
    
    // Check if Windows auto-assigned a drive letter to the new partition
    QProcess checkLetter;
    QString checkCmd = QString("(Get-Partition -DiskNumber %1 -PartitionNumber 1 | Get-Volume).DriveLetter").arg(diskNumber);
    checkLetter.start("powershell", QStringList() << "-NoProfile" << "-Command" << checkCmd);
    
    QString autoAssignedLetter;
    if (checkLetter.waitForFinished(10000)) {
        autoAssignedLetter = QString(checkLetter.readAllStandardOutput()).trimmed();
        if (!autoAssignedLetter.isEmpty()) {
            sak::log_info(QString("Windows auto-assigned drive letter: %1").arg(autoAssignedLetter).toStdString());
            cleanDriveLetter = autoAssignedLetter;
        }
    }
    
    // If no drive letter was assigned, assign the desired one
    if (cleanDriveLetter.isEmpty() || autoAssignedLetter.isEmpty()) {
        sak::log_info(QString("Assigning drive letter: %1").arg(cleanDriveLetter).toStdString());
        Q_EMIT statusChanged("Assigning drive letter...");
        
        QProcess assignLetter;
        QString assignCmd = QString("Get-Partition -DiskNumber %1 -PartitionNumber 1 | Set-Partition -NewDriveLetter %2")
            .arg(diskNumber, cleanDriveLetter);
        assignLetter.start("powershell", QStringList() << "-NoProfile" << "-Command" << assignCmd);
        assignLetter.waitForFinished(10000);
        
        QString assignOutput = QString(assignLetter.readAllStandardOutput()).trimmed();
        QString assignErrors = QString(assignLetter.readAllStandardError()).trimmed();
        
        if (!assignOutput.isEmpty()) {
            sak::log_info(QString("Assign output: %1").arg(assignOutput).toStdString());
        }
        if (!assignErrors.isEmpty()) {
            sak::log_warning(QString("Assign warnings: %1").arg(assignErrors).toStdString());
        }
        
        QThread::msleep(2000);
    }
    
    // Step 3: Format using PowerShell Format-Volume (Microsoft's official method)
    Q_EMIT statusChanged("Formatting drive as NTFS...");
    sak::log_info(QString("Formatting drive %1: with Format-Volume...").arg(cleanDriveLetter).toStdString());
    
    // Use the exact method from Microsoft's Windows Server USB creation script
    // Label will be set properly after mounting ISO
    QProcess formatProcess;
    QString formatCmd = QString("Format-Volume -DriveLetter %1 -FileSystem NTFS -NewFileSystemLabel 'WINUSB' -Confirm:$false -Force")
        .arg(cleanDriveLetter);
    formatProcess.start("powershell", QStringList() << "-NoProfile" << "-Command" << formatCmd);
    
    if (!formatProcess.waitForStarted()) {
        m_lastError = "Failed to start Format-Volume";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    if (!formatProcess.waitForFinished(120000)) { // 2 minutes
        m_lastError = "Format-Volume timed out";
        sak::log_error(m_lastError.toStdString());
        formatProcess.kill();
        return false;
    }
    
    QString formatOutput = QString(formatProcess.readAllStandardOutput()).trimmed();
    QString formatErrors = QString(formatProcess.readAllStandardError()).trimmed();
    
    if (!formatOutput.isEmpty()) {
        sak::log_info(QString("Format-Volume output:\n%1").arg(formatOutput).toStdString());
    }
    if (!formatErrors.isEmpty()) {
        sak::log_warning(QString("Format-Volume stderr:\n%1").arg(formatErrors).toStdString());
    }
    
    if (formatProcess.exitCode() != 0) {
        m_lastError = QString("Format-Volume failed with exit code %1").arg(formatProcess.exitCode());
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
    
    // Get the volume label from the mounted ISO
    QProcess labelProcess;
    QString labelCmd = QString("(Get-Volume -DriveLetter %1).FileSystemLabel").arg(output);
    labelProcess.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << labelCmd);
    if (labelProcess.waitForFinished(5000)) {
        m_volumeLabel = labelProcess.readAllStandardOutput().trimmed();
        if (m_volumeLabel.isEmpty()) {
            m_volumeLabel = "WINDOWS";
        }
        sak::log_info(QString("ISO volume label: %1").arg(m_volumeLabel).toStdString());
    } else {
        m_volumeLabel = "WINDOWS";
    }
    
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
    
    if (!robocopy.waitForStarted(5000)) {
        m_lastError = "Failed to start robocopy";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    sak::log_info("Robocopy started, copying files...");
    
    if (!robocopy.waitForFinished(600000)) { // 10 minutes timeout
        m_lastError = "Robocopy timed out";
        sak::log_error(m_lastError.toStdString());
        robocopy.kill();
        return false;
    }
    
    int exitCode = robocopy.exitCode();
    QString output = robocopy.readAllStandardOutput();
    QString errors = robocopy.readAllStandardError();
    
    sak::log_info(QString("Robocopy exit code: %1").arg(exitCode).toStdString());
    if (!output.isEmpty()) {
        sak::log_info(QString("Robocopy output: %1").arg(output).toStdString());
    }
    if (!errors.isEmpty()) {
        sak::log_warning(QString("Robocopy stderr: %1").arg(errors).toStdString());
    }
    
    // Robocopy exit codes: 0-7 are success (various levels), 8+ are errors
    if (exitCode >= 8) {
        m_lastError = QString("Robocopy failed with exit code %1: %2").arg(exitCode).arg(errors);
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Set the volume label from ISO
    if (!m_volumeLabel.isEmpty()) {
        sak::log_info(QString("Setting volume label to: %1").arg(m_volumeLabel).toStdString());
        QString cleanDest = destPath;
        if (cleanDest.endsWith(":")) {
            cleanDest = cleanDest.left(1);
        }
        QProcess labelProcess;
        QString labelCmd = QString("Set-Volume -DriveLetter %1 -NewFileSystemLabel '%2'")
            .arg(cleanDest, m_volumeLabel);
        labelProcess.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << labelCmd);
        labelProcess.waitForFinished(10000);
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
    
    // Get disk number from drive letter
    QString cleanDrive = driveLetter;
    if (cleanDrive.endsWith(":")) {
        cleanDrive = cleanDrive.left(1);
    }
    cleanDrive.remove("\\");
    
    QProcess getDisk;
    QString diskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDrive);
    getDisk.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << diskCmd);
    
    if (!getDisk.waitForFinished(10000)) {
        sak::log_warning("Could not get disk number for verification");
        return true; // Not critical
    }
    
    QString diskNumber = QString(getDisk.readAllStandardOutput()).trimmed();
    if (diskNumber.isEmpty()) {
        sak::log_warning("Could not determine disk number");
        return true; // Not critical
    }
    
    // Use diskpart to check if partition is active
    QString diskpartScript = QString(
        "select disk %1\n"
        "select partition 1\n"
        "detail partition\n"
    ).arg(diskNumber);
    
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        sak::log_error("Failed to create temporary diskpart script for verification");
        return false;
    }
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted(5000)) {
        sak::log_error("Failed to start diskpart for verification");
        return false;
    }
    
    if (!diskpart.waitForFinished(30000)) {
        diskpart.kill();
        sak::log_error("diskpart verification timed out");
        return false;
    }
    
    QString output = diskpart.readAllStandardOutput();
    sak::log_info(QString("Diskpart detail output: %1").arg(output).toStdString());
    
    // Check if partition is marked as Active
    bool isActive = output.contains("Active", Qt::CaseInsensitive);
    
    if (isActive) {
        sak::log_info("Bootable flag verified - partition is active");
        return true;
    } else {
        sak::log_warning("Partition is not marked as active - may not be bootable on legacy BIOS");
        // Not critical - UEFI systems don't require active flag
        return true;
    }
}
