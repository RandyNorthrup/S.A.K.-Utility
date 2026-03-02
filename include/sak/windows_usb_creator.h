// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QObject>
#include <QPair>
#include <QString>
#include <QMutex>
#include <atomic>

class QProcess;

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
    /// @brief Validate ISO and disk number inputs before USB creation
    bool validateUSBInputs(const QString& isoPath, const QString& diskNumber);

    /// @brief Format drive, wait for partition, and verify NTFS filesystem (Step 1)
    QString formatAndVerifyDrive(const QString& diskNumber);

    /// @brief Extract ISO contents and verify critical files (Step 2)
    bool extractAndVerifyFiles(const QString& isoPath, const QString& driveLetter);

    /// @brief Configure boot files and set bootable flag (Steps 3-4)
    bool configureBootAndVerify(const QString& diskNumber, const QString& driveLetter);

    /**
     * @brief Format drive as NTFS using diskpart
     * @param diskNumber Disk number (hardware ID) to format
     * @return true if successful
     */
    bool formatDriveNTFS(const QString& diskNumber);

    /// @brief Clean disk and create MBR partition using diskpart
    bool cleanAndPartitionDisk(const QString& diskNumber);
    /// @brief Format the partition as NTFS using diskpart
    bool formatPartitionNTFS(const QString& diskNumber);

    /**
     * @brief Extract ISO contents directly to USB drive using 7z
     * @param sourcePath Path to ISO file
     * @param destPath Destination drive letter (single letter, e.g., "E")
     * @return true if successful
     */
    bool copyISOContents(const QString& sourcePath, const QString& destPath);

    /**
     * @brief Extract volume label from ISO metadata using 7z
     * @param sevenZipPath Path to 7z.exe
     * @param sourcePath Path to ISO file
     */
    void copyISO_extractVolumeLabel(const QString& sevenZipPath, const QString& sourcePath);

    /// @brief Parse 7z listing output for volume label ("Comment = ..." line)
    QString parseVolumeLabelFromOutput(const QString& output);

    /**
     * @brief Normalize a drive letter destination path to "X:\\" format
     * @param destPath Raw destination path input
     * @param cleanDest [out] Normalized path (e.g., "E:\\")
     * @return true if valid drive letter
     */
    bool copyISO_normalizeDestination(const QString& destPath, QString& cleanDest);

    /**
     * @brief Check that destination drive has sufficient disk space
     * @param cleanDest Normalized destination path
     * @param sourcePath Path to source ISO for size estimation
     * @return true if enough space available
     */
    bool copyISO_checkDiskSpace(const QString& cleanDest, const QString& sourcePath);

    /**
     * @brief Run 7z extraction: start process, monitor progress, log result
     * @param sevenZipPath Path to 7z.exe
     * @param sourcePath Path to ISO file
     * @param cleanDest Normalized destination path
     * @return true if extraction succeeded
     */
    bool copyISO_runExtraction(const QString& sevenZipPath, const QString& sourcePath,
        const QString& cleanDest);

    /**
     * @brief Monitor a running 7z extraction process for progress and cancellation
     * @param extract Running QProcess reference
     * @return true if process finished normally (not timed out or cancelled)
     */
    bool copyISO_monitorExtraction(QProcess& extract);

    /**
     * @brief Parse 7z output and emit progress signals
     * @param output Raw 7z stdout output string
     * @param totalBytes [in/out] Total bytes reported by 7z
     * @param processedBytes [in/out] Processed bytes reported by 7z
     * @param lastProgressPercent [in/out] Last emitted progress percentage
     */
    void copyISO_parseExtractionProgress(const QString& output, qint64& totalBytes,
        qint64& processedBytes, int& lastProgressPercent);

    /**
     * @brief Log 7z extraction result and check exit code
     * @param extract Finished QProcess reference
     * @return true if exit code is 0
     */
    bool copyISO_logExtractionResult(QProcess& extract);

    /**
     * @brief Verify destination directory contents after extraction
     * @param cleanDest Normalized destination path
     * @return true if destination is valid and non-empty
     */
    bool copyISO_verifyDestination(const QString& cleanDest);

    /**
     * @brief Find setup.exe in destination (case-insensitive search)
     * @param cleanDest Normalized destination path
     * @return true if setup.exe found
     */
    bool copyISO_findSetupExe(const QString& cleanDest);

    /**
     * @brief Verify critical Windows boot files exist (boot.wim, bootmgr, install image)
     * @param cleanDest Normalized destination path
     * @return true if all critical files present
     */
    bool copyISO_verifyBootFiles(const QString& cleanDest);

    /**
     * @brief Set the volume label on the destination drive from ISO metadata
     * @param cleanDest Normalized destination path
     */
    void copyISO_setVolumeLabel(const QString& cleanDest);

    /**
     * @brief Configure boot files using bcdboot (if available)
     * @param driveLetter Target drive letter (single letter, e.g., "E")
     * @return true if successful (non-critical - boot may work without bcdboot)
     */
    bool makeBootable(const QString& driveLetter);

    /// @brief Execute the bcdboot process to configure boot files
    /// @return true on success or non-critical failure
    bool runBcdboot(const QString& bcdbootPath, const QString& cleanDrive);

    /**
     * @brief Verify that the bootable flag is set on the partition
     * @param driveLetter Drive letter to verify
     * @return true if bootable flag is confirmed
     */
    bool verifyBootableFlag(const QString& driveLetter);

    /// @brief Run diskpart to check if the partition on the given disk is active/bootable
    bool checkPartitionActive(const QString& diskNumber);

    /**
     * @brief Verify extraction integrity by comparing ISO contents with extracted files
     * @param isoPath Path to source ISO file
     * @param destPath Destination path where files were extracted
     * @param sevenZipPath Path to 7z.exe executable
     * @return true if all critical files match in size and presence
     */
    bool verifyExtractionIntegrity(const QString& isoPath, const QString& destPath,
        const QString& sevenZipPath);

    /// @brief Check if a file path matches a critical Windows installation file
    bool isCriticalWindowsFile(const QString& path) const;

    /// @brief Parse ISO listing output and return critical Windows file paths with sizes
    QList<QPair<QString, qint64>> parseIsoCriticalFiles(const QStringList& lines);

    /// @brief Verify that critical files exist on disk with matching sizes
    bool verifyCriticalFilesOnDisk(const QList<QPair<QString, qint64>>& criticalFiles,
                                   const QString& destPath);

    /**
     * @brief Final comprehensive verification - ONLY path to success
     * @param driveLetter Drive letter to verify
     * @return true ONLY if ALL verifications pass
     */
    bool finalVerification(const QString& driveLetter);

    /// @brief Verify critical boot files and install images exist on the drive
    bool verifyBootAndInstallFiles(const QString& cleanDrive);

    /// @brief Log final verification success details
    void logFinalVerificationSuccess(int fileCount);

    /**
     * @brief Get current drive letter for the disk number
     * @return Drive letter (e.g., "E") or empty if not found
     */
    QString getDriveLetterFromDiskNumber();

    std::atomic<bool> m_cancelled{false};
    mutable QMutex m_errorMutex;          ///< Guards m_lastError for cross-thread access
    QString m_lastError;                  ///< Protected by m_errorMutex
    QString m_volumeLabel;  // Volume label extracted from ISO
    QString m_diskNumber;   // Hardware disk number (e.g., "1" for PhysicalDrive1)
};
