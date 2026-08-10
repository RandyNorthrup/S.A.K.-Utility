// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>

#include <atomic>

namespace sak {
struct ProcessResult;

/// @brief Allowlist a raw volume label (from an untrusted ISO) to letters/digits and " _-.",
///        capped at the 32-char NTFS max and trimmed, so it can never inject PowerShell
///        metacharacters when interpolated into a Set-Volume -NewFileSystemLabel command.
[[nodiscard]] QString sanitizeVolumeLabel(const QString& raw);
}  // namespace sak

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

    /// @brief True iff a bcdboot run should be treated as a success. Pure;
    ///        unit-testable. A timeout, cancellation, or non-zero exit is a
    ///        failure -- boot support must NOT be certified in those cases.
    [[nodiscard]] static bool bcdbootReportsSuccess(bool timedOut, bool cancelled, int exitCode);

    /// @brief True iff diskpart stdout contains a known hard-failure marker.
    ///        diskpart frequently exits 0 even when an individual command failed,
    ///        so a clean exit code alone is not proof of success. Pure/testable.
    [[nodiscard]] static bool diskpartOutputIsError(const QString& output);

    /// @brief Defense in depth: only a real regular file (not a symlink/reparse
    ///        point) that canonically resides under @p appDir may be executed as a
    ///        bundled tool. Fails closed. Pure/testable.
    [[nodiscard]] static bool isSafeBundledExecutable(const QString& path, const QString& appDir);

    /// @brief True iff @p path names a file whose presence and size decide whether
    ///        an extracted Windows install medium is usable. Accepts either
    ///        separator: 7z reports ISO paths with backslashes on Windows.
    ///        Pure/testable - it was private and untested, and shipped with an
    ///        over-escaped literal that made the backslash forms unmatchable.
    [[nodiscard]] static bool isCriticalWindowsFile(const QString& path);

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
    /// @brief Reset every per-run field to its pristine value so a prior run's
    /// pinned disk/ISO identity or volume label cannot leak into this one.
    void resetPerRunState(const QString& diskNumber);

    /// @brief Cancellation checkpoint between destructive steps: if a cancel was
    /// requested, report it (setError + failed()) and return true so the caller
    /// fails closed.
    bool cancellationRequested();

    /// @brief Validate ISO and disk number inputs before USB creation
    bool validateUSBInputs(const QString& isoPath, const QString& diskNumber);

    /// @brief Engine-level guard: refuse to clean/format a disk that is the
    /// current OS boot or system disk, or is read-only. Defense in depth behind
    /// the GUI's removable-only selection so a bad caller cannot wipe the OS disk.
    /// Also pins the disk's UniqueId and size for later TOCTOU re-verification.
    bool guardTargetDiskSafe(const QString& diskNumber);

    /// @brief TOCTOU guard re-run immediately before the destructive DiskPart
    /// clean: confirms @p diskNumber still resolves to the exact same safe disk
    /// (matching UniqueId + size) that guardTargetDiskSafe() vetted. Fails closed
    /// if a hot-plug reassigned the number to a different disk.
    bool reverifyTargetDiskIdentity(const QString& diskNumber);

    /// @brief Resolve a Windows system executable to its absolute
    /// %SystemRoot%\\System32 path so it can never be satisfied by a hijacked copy
    /// earlier in the process search order. Empty if %SystemRoot% is unset or the
    /// file is absent (fail closed -- never guess C:\\Windows).
    [[nodiscard]] static QString system32ExePath(const QString& exeName);

    /// @brief Stage @p script to a temp file and run the absolute System32
    /// diskpart on it. Fails closed if diskpart cannot be resolved/staged, the run
    /// times out/cancels, exits non-zero, or its stdout contains an error marker.
    bool runDiskpartScript(const QString& script, int timeoutMs, QString& outputOut);

    /// @brief Evaluate a finished diskpart process result, failing closed on
    /// timeout/cancel/non-zero exit or an error marker in stdout.
    bool checkDiskpartResult(const sak::ProcessResult& result);

    /// @brief Format drive, wait for partition, and verify NTFS filesystem (Step 1)
    QString formatAndVerifyDrive(const QString& diskNumber);

    /// @brief Verify formatted drive is NTFS filesystem
    bool verifyNtfsFilesystem(const QString& driveLetter);

    /// @brief Validate and normalize raw drive letter from PowerShell
    QString validateDriveLetter(const QString& rawLetter);

    /// @brief Extract ISO contents and verify critical files (Step 2)
    bool extractAndVerifyFiles(const QString& isoPath, const QString& driveLetter);

    /// @brief TOCTOU guard run before extraction writes anything: confirm @p driveLetter still
    /// resolves to the ORIGINAL target disk (m_diskNumber). A hot-plug that reassigned the letter
    /// to another disk's volume between format and extraction would otherwise let 7z overwrite that
    /// unrelated volume. Fails closed on any mismatch or query failure.
    bool verifyDriveLetterMapsToTarget(const QString& driveLetter);

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
     * @brief Validate the source ISO and resolve the trusted bundled 7z before extraction
     * @param sourcePath Path to source ISO file
     * @param sevenZipPath [out] Verified path to the bundled 7z.exe
     * @return true if the ISO exists, 7z is trusted, and no cancel was requested
     */
    bool copyISO_prepareExtraction(const QString& sourcePath, QString& sevenZipPath);

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
    bool copyISO_runExtraction(const QString& sevenZipPath,
                               const QString& sourcePath,
                               const QString& cleanDest);

    /**
     * @brief Parse 7z output and emit progress signals
     * @param output Raw 7z stdout
     * output string
     * @param totalBytes [in/out] Total bytes reported by 7z
     * @param processedBytes [in/out] Processed bytes reported by 7z
     * @param lastProgressPercent [in/out] Last emitted progress percentage
     */
    void copyISO_parseExtractionProgress(const QString& output,
                                         qint64& totalBytes,
                                         qint64& processedBytes,
                                         int& lastProgressPercent);

    /**
     * @brief Log 7z extraction result and check exit code
     * @param result Finished
     * process result
     * @return true if exit code is 0
     */
    bool copyISO_logExtractionResult(const sak::ProcessResult& result);

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
     * @brief Configure boot files using bcdboot
     * @param driveLetter Target drive letter (single letter, e.g., "E")
     * @return true only if boot files were actually configured; false (with
     *         m_lastError set) if bcdboot is unavailable or does not succeed --
     *         boot support must be gated on bcdboot, not merely file presence.
     */
    bool makeBootable(const QString& driveLetter);

    /// @brief Execute the bcdboot process to configure boot files (BIOS + UEFI)
    /// @return true only when bcdboot completes successfully; false otherwise
    bool runBcdboot(const QString& bcdbootPath, const QString& cleanDrive);

    /// @brief Resolve a runnable bcdboot.exe. SECURITY: only ever the host's
    ///        Authenticode-signed %SystemRoot%\\System32 copy -- NEVER a copy
    ///        extracted from the (untrusted) ISO/media, which could be an
    ///        attacker-planted binary. Empty if it cannot be resolved (e.g.
    ///        %SystemRoot% unset), whereupon the caller fails closed.
    [[nodiscard]] static QString resolveBcdbootPath();

    /**
     * @brief Verify that the bootable flag is set on the partition
     * @param driveLetter Drive letter to verify
     * @return true if bootable flag is confirmed
     */
    bool verifyBootableFlag(const QString& driveLetter);

    /// @brief Set the active/bootable flag on partition 1 and verify
    bool setAndVerifyBootFlag(const QString& diskNumber, const QString& driveLetter);

    /// @brief Run diskpart to check if the partition on the given disk is active/bootable
    bool checkPartitionActive(const QString& diskNumber);

    /**
     * @brief Verify extraction integrity by comparing ISO contents with extracted files
     * @param isoPath Path to source ISO file
     * @param destPath Destination path where files were extracted
     * @param sevenZipPath Path to 7z.exe executable
     * @return true if all critical files match in size and presence
     */
    bool verifyExtractionIntegrity(const QString& isoPath,
                                   const QString& destPath,
                                   const QString& sevenZipPath);

    /// @brief Parse ISO listing output and return critical Windows file paths with sizes
    QList<QPair<QString, qint64>> parseIsoCriticalFiles(const QStringList& lines);

    /// @brief Verify that critical files exist on disk with matching sizes
    bool verifyCriticalFilesOnDisk(const QList<QPair<QString, qint64>>& criticalFiles,
                                   const QString& destPath);

    /// @brief True iff one critical file exists on disk with the exact expected
    /// (positive) ISO size. A non-positive expected size fails closed.
    bool criticalFileOnDiskMatches(const QPair<QString, qint64>& fileInfo, const QString& destPath);

    /// @brief Run Step 5, the sole path to a verified success: announce, then
    /// verify via finalVerification(), reporting through failed() and failing
    /// closed on any verification failure.
    bool runFinalVerification(const QString& driveLetter);

    /**
     * @brief Final comprehensive verification - ONLY path to success
     * @param driveLetter Drive letter to verify
     * @return true ONLY if ALL verifications pass
     */
    bool finalVerification(const QString& driveLetter);

    /// @brief Normalize a raw drive letter to validated "X:\\" form for final verification
    bool normalizeVerificationDrive(const QString& driveLetter, QString& cleanDrive);

    /// @brief Verify critical boot files and install images exist on the drive
    bool verifyBootAndInstallFiles(const QString& cleanDrive);

    /// @brief Log final verification success details
    void logFinalVerificationSuccess(int fileCount);

    /**
     * @brief Get current drive letter for the disk number
     * @return Drive letter (e.g., "E") or empty if not found
     */
    QString getDriveLetterFromDiskNumber();

    /// @brief Thread-safe setter for m_lastError
    void setError(const QString& error) {
        QMutexLocker locker(&m_errorMutex);
        m_lastError = error;
    }

    std::atomic<bool> m_cancelled{false};
    /// Single-flight guard: rejects a second concurrent createBootableUSB() on the same
    /// instance so its per-run QString/qint64 state (disk/ISO identity, label) cannot be
    /// raced across threads. Set on entry, cleared on every exit path.
    std::atomic<bool> m_running{false};
    mutable QMutex m_errorMutex;   ///< Guards m_lastError for cross-thread access
    QString m_lastError;           ///< Protected by m_errorMutex
    QString m_volumeLabel;         // Volume label extracted from ISO
    QString m_diskNumber;          // Hardware disk number (e.g., "1" for PhysicalDrive1)
    QString m_targetDiskUniqueId;  ///< Get-Disk UniqueId pinned by guardTargetDiskSafe()
    qint64 m_targetDiskSizeBytes{
        -1};                     ///< Target disk capacity from guardTargetDiskSafe(); -1 = unknown
    qint64 m_isoSizeBytes{-1};   ///< ISO size pinned at validation for TOCTOU immutability check
    qint64 m_isoModifiedMs{-1};  ///< ISO mtime (ms since epoch) pinned for immutability check
};
