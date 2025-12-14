// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QObject>
#include <QString>

/**
 * @brief Creates bootable Windows USB drives from ISO files
 * 
 * This class properly extracts Windows ISO contents to a USB drive formatted
 * as NTFS, unlike raw disk imaging which would result in a UDF filesystem.
 * 
 * Process:
 * 1. Format USB drive as NTFS (using diskpart)
 * 2. Extract ISO contents directly using 7z (no mounting required)
 * 3. Verify extraction integrity (file sizes and critical files)
 * 4. Configure boot files (using bcdboot if available)
 * 5. Set bootable flag (using diskpart active command)
 * 6. Final comprehensive verification
 */
class WindowsUSBCreator : public QObject {
    Q_OBJECT
    
public:
    explicit WindowsUSBCreator(QObject* parent = nullptr);
    ~WindowsUSBCreator();
    
    /**
     * @brief Create a bootable Windows USB drive from an ISO
     * @param isoPath Path to the Windows ISO file
     * @param diskNumber Target USB disk number (e.g., "1" for PhysicalDrive1)
     * @return true if successful, false otherwise
     */
    bool createBootableUSB(const QString& isoPath, const QString& diskNumber);
    
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
     * @brief Emitted when operation completes successfully after ALL verifications pass
     * This signal is ONLY emitted when:
     * - Format succeeded
     * - Extraction succeeded and verified against ISO
     * - All critical files verified present
     * - Bootable flag verified as Active
     */
    void completed();
    
    /**
     * @brief Emitted when operation fails at any step or verification fails
     * @param error Error message
     */
    void failed(const QString& error);
    
private:
    /**
     * @brief Format drive as NTFS using diskpart
     * @param diskNumber Disk number (hardware ID) to format
     * @return true if successful
     */
    bool formatDriveNTFS(const QString& diskNumber);
    
    /**
     * @brief Extract ISO contents directly to USB drive using 7z
     * @param sourcePath Path to ISO file
     * @param destPath Destination drive letter (single letter, e.g., "E")
     * @return true if successful
     */
    bool copyISOContents(const QString& sourcePath, const QString& destPath);
    
    /**
     * @brief Configure boot files using bcdboot (if available)
     * @param driveLetter Target drive letter (single letter, e.g., "E")
     * @return true if successful (non-critical - boot may work without bcdboot)
     */
    bool makeBootable(const QString& driveLetter);
    
    /**
     * @brief Verify that the bootable flag is set on the partition
     * @param driveLetter Drive letter to verify
     * @return true if bootable flag is confirmed
     */
    bool verifyBootableFlag(const QString& driveLetter);
    
    /**
     * @brief Verify extraction integrity by comparing ISO contents with extracted files
     * @param isoPath Path to source ISO file
     * @param destPath Destination path where files were extracted
     * @param sevenZipPath Path to 7z.exe executable
     * @return true if all critical files match in size and presence
     */
    bool verifyExtractionIntegrity(const QString& isoPath, const QString& destPath, const QString& sevenZipPath);
    
    /**
     * @brief Final comprehensive verification - ONLY path to success
     * @param driveLetter Drive letter to verify
     * @return true ONLY if ALL verifications pass
     */
    bool finalVerification(const QString& driveLetter);
    
    /**
     * @brief Get current drive letter for the disk number
     * @return Drive letter (e.g., "E") or empty if not found
     */
    QString getDriveLetterFromDiskNumber();
    
    bool m_cancelled;
    QString m_lastError;
    QString m_volumeLabel;  // Volume label extracted from ISO
    QString m_diskNumber;   // Hardware disk number (e.g., "1" for PhysicalDrive1)
};
