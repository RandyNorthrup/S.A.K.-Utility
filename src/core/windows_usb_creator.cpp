// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_usb_creator.h"
#include "sak/logger.h"
#include <QProcess>
#include <QDir>
#include <QFileInfo>
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
    // Use Windows format command via diskpart for reliability
    QString cleanDriveLetter = driveLetter;
    cleanDriveLetter.remove(":");
    cleanDriveLetter.remove("\\");
    
    sak::log_info(QString("Formatting %1: as NTFS").arg(cleanDriveLetter).toStdString());
    
    // Create diskpart script
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        m_lastError = "Failed to create diskpart script";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Write diskpart commands
    QTextStream script(&scriptFile);
    script << "list volume\n";
    script << QString("select volume %1\n").arg(cleanDriveLetter);
    script << "clean\n";
    script << "create partition primary\n";
    script << "format fs=ntfs quick label=\"Windows USB\"\n";
    script << "assign\n";
    script << "active\n";
    script.flush();
    scriptFile.close();
    
    // Run diskpart
    QProcess diskpart;
    diskpart.start("diskpart", QStringList() << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted()) {
        m_lastError = "Failed to start diskpart";
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    // Wait for completion (format can take a while)
    if (!diskpart.waitForFinished(300000)) { // 5 minutes timeout
        m_lastError = "Diskpart timed out";
        sak::log_error(m_lastError.toStdString());
        diskpart.kill();
        return false;
    }
    
    int exitCode = diskpart.exitCode();
    QString output = diskpart.readAllStandardOutput();
    QString errors = diskpart.readAllStandardError();
    
    sak::log_info(QString("Diskpart output: %1").arg(output).toStdString());
    
    if (exitCode != 0) {
        m_lastError = QString("Diskpart failed with exit code %1: %2").arg(exitCode).arg(errors);
        sak::log_error(m_lastError.toStdString());
        return false;
    }
    
    sak::log_info(QString("Successfully formatted %1: as NTFS").arg(cleanDriveLetter).toStdString());
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
    QStringList args;
    args << sourcePath.replace("/", "\\");
    args << destPath.replace("/", "\\");
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
