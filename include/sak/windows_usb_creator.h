// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>
#include <QTemporaryFile>

/**
 * @brief Creates bootable Windows USB drives from ISO files
 * 
 * This class properly extracts Windows ISO contents to a USB drive formatted
 * as NTFS, unlike raw disk imaging which would result in a UDF filesystem.
 * 
 * Process:
 * 1. Format USB drive as NTFS (using diskpart)
 * 2. Mount the Windows ISO (using PowerShell Mount-DiskImage)
 * 3. Copy all files from ISO to USB (using robocopy)
 * 4. Make bootable (using bootsect.exe from the ISO)
 * 5. Dismount ISO
 */
class WindowsUSBCreator : public QObject {
    Q_OBJECT
    
public:
    explicit WindowsUSBCreator(QObject* parent = nullptr);
    ~WindowsUSBCreator();
    
    /**
     * @brief Create a bootable Windows USB drive from an ISO
     * @param isoPath Path to the Windows ISO file
     * @param driveLetter Target USB drive letter (e.g., "E:" or "E")
     * @return true if successful, false otherwise
     */
    bool createBootableUSB(const QString& isoPath, const QString& driveLetter);
    
    /**
     * @brief Cancel the current operation
     */
    void cancel();
    
    /**
     * @brief Get the last error message
     * @return Error message string
     */
    QString lastError() const;
    
Q_SIGNALS:
    /**
     * @brief Emitted when status changes (e.g., "Formatting...", "Copying files...")
     * @param status Current status message
     */
    void statusChanged(const QString& status);
    
    /**
     * @brief Emitted to report progress
     * @param percentage Progress percentage (0-100)
     */
    void progressUpdated(int percentage);
    
    /**
     * @brief Emitted when operation completes successfully
     */
    void completed();
    
    /**
     * @brief Emitted when operation fails
     * @param error Error message
     */
    void failed(const QString& error);
    
private:
    /**
     * @brief Format drive as NTFS using diskpart
     * @param driveLetter Drive letter to format
     * @return true if successful
     */
    bool formatDriveNTFS(const QString& driveLetter);
    
    /**
     * @brief Mount ISO file using PowerShell
     * @param isoPath Path to ISO file
     * @return Mount point (drive letter) or empty string on failure
     */
    QString mountISO(const QString& isoPath);
    
    /**
     * @brief Dismount ISO file
     * @param mountPoint Drive letter where ISO is mounted
     */
    void dismountISO(const QString& mountPoint);
    
    /**
     * @brief Copy ISO contents to USB drive
     * @param sourcePath Source path (mounted ISO)
     * @param destPath Destination path (USB drive)
     * @return true if successful
     */
    bool copyISOContents(const QString& sourcePath, const QString& destPath);
    
    /**
     * @brief Make drive bootable using bootsect.exe
     * @param driveLetter Target drive letter
     * @param isoMountPoint Mounted ISO path (to find bootsect.exe)
     * @return true if successful
     */
    bool makeBootable(const QString& driveLetter, const QString& isoMountPoint);
    
    /**
     * @brief Verify that the bootable flag is set on the partition
     * @param driveLetter Drive letter to verify
     * @return true if bootable flag is confirmed
     */
    bool verifyBootableFlag(const QString& driveLetter);
    
    bool m_cancelled;
    QString m_lastError;
    QString m_volumeLabel;  // Volume label extracted from ISO
};
