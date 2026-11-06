// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QList>
#include <QMutex>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <optional>

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
 * 1. Clean the disk, create one active MBR primary partition, format it NTFS (diskpart)
 * 2. Extract ISO contents directly using 7z (no mounting required) and verify them
 * 3. Final comprehensive verification, including that partition 1 is active
 *
 * The media boots from the boot files the ISO itself carries (bootmgr, boot\BCD, efi\).
 * There is no bcdboot step: bcdboot needs a Windows directory as its source and install
 * media has none, so it could only fail or write the wrong thing. The legacy "active" flag is
 * set once, when the partition is created, and verified at the end.
 *
 * The extraction uses the 7z.exe that ships inside the bundled portable Chocolatey
 * (tools/chocolatey/tools/7z.exe); its security posture is tracked with Chocolatey's.
 */
class WindowsUSBCreator : public QObject {
    Q_OBJECT

public:
    explicit WindowsUSBCreator(QObject* parent = nullptr);
    ~WindowsUSBCreator();

    /// @brief How a bounded wait for Windows to surface a new partition/volume ended.
    enum class VolumeWaitOutcome {
        Ready,     ///< The probe reported the volume present
        TimedOut,  ///< The deadline passed first; the caller fails closed
        Cancelled  ///< The user cancelled while waiting
    };

    /// @brief Wait until @p probe reports the volume present, at most @p timeout_ms.
    /// Windows surfaces a freshly created partition, its volume and its drive letter
    /// asynchronously. This returns the moment @p probe succeeds instead of sleeping a fixed
    /// time and hoping, and gives up at the deadline so the caller can fail with a clear
    /// error. @p probe runs first, before any sleep. Pure apart from the sleeps; testable.
    [[nodiscard]] static VolumeWaitOutcome waitForVolume(const std::function<bool()>& probe,
                                                         const std::function<bool()>& cancelled,
                                                         int timeout_ms,
                                                         int poll_interval_ms);

    /// @brief True iff diskpart stdout contains a known hard-failure marker.
    ///        diskpart frequently exits 0 even when an individual command failed,
    ///        so a clean exit code alone is not proof of success. Pure/testable.
    [[nodiscard]] static bool diskpartOutputIsError(const QString& output);

    /// @brief The diskpart script that gives partition 1 of @p diskNumber the next free drive
    ///        letter. Pure/testable.
    [[nodiscard]] static QString assignDriveLetterScript(const QString& diskNumber);

    /// @brief Defense in depth: only a real regular file (not a symlink/reparse
    ///        point) that canonically resides under @p appDir may be executed as a
    ///        bundled tool. Fails closed. Pure/testable.
    [[nodiscard]] static bool isSafeBundledExecutable(const QString& path, const QString& appDir);

    /// @brief Whether an image holds a Windows install image, the file this writer needs.
    enum class InstallImageCheck {
        Found,     ///< sources\\install.wim, .esd or .swm is inside: this writer can make media.
        NotFound,  ///< It is not there, or the file is not an archive 7z can list.
        Failed,    ///< 7z could not be run, so nothing is known about the image.
    };

    /// @brief Look inside @p isoPath for a Windows install image. A volume label cannot decide
    ///        this: anything may carry "WIN" in its label or be built with Microsoft's tools, and a
    ///        rescue ISO routed here has its drive erased before the missing file is noticed. This
    ///        only lists the image, so a large file costs a fraction of a second.
    [[nodiscard]] static InstallImageCheck checkInstallImage(const QString& isoPath);
#ifdef SAK_CORE_TEST_HOOKS
    /// @brief Test-only: make checkInstallImage answer this, as it does with its tool missing.
    static void setInstallImageCheckForTesting(std::optional<InstallImageCheck> answer);
#endif  // SAK_CORE_TEST_HOOKS
    /// @brief checkInstallImage with the 7z to use named, so a test can run the real tool.
    [[nodiscard]] static InstallImageCheck checkInstallImageWith(const QString& isoPath,
                                                                 const QString& sevenZipPath);
    /// @brief Whether a 7z listing names a Windows install image. Pure; unit-testable.
    [[nodiscard]] static bool listingHasInstallImage(const QString& listing);

    // ---- FAT32 layout ----
    // Windows 11 needs UEFI with Secure Boot, and UEFI firmware reads FAT32, not NTFS: the media
    // was one NTFS partition, which a typical UEFI PC does not boot. It is one FAT32 partition, and
    // an install.wim too large for FAT32 is split by Windows' own DISM into .swm parts, which
    // Windows Setup reads as they are.
    /// The largest file FAT32 holds.
    static constexpr qint64 kFat32MaxFileBytes = 4'294'967'295;
    /// The largest FAT32 volume diskpart formats; a larger stick gets a partition this size.
    static constexpr int kFat32MaxPartitionMb = 32'768;
    /// The size DISM splits a large install.wim into, under FAT32's limit with room to spare.
    static constexpr int kInstallSplitPartMb = 3800;
    /// @brief The FAT32 partition for a disk of @p disk_bytes: 0 for the whole disk, else its cap.
    [[nodiscard]] static int fat32PartitionMb(qint64 disk_bytes);
    /// @brief The diskpart script that empties disk @p disk and makes its one active partition.
    [[nodiscard]] static QString partitionScript(const QString& disk, int partition_mb);
    /// @brief The diskpart script that formats that partition FAT32 with @p label.
    [[nodiscard]] static QString formatFat32Script(const QString& disk, const QString& label);
    /// @brief Whether an install image of @p bytes is too large for FAT32 and must be split.
    [[nodiscard]] static bool installImageNeedsSplit(qint64 bytes) {
        return bytes > kFat32MaxFileBytes;
    }
    /// @brief DISM's arguments to split @p wim into @p swm (install.swm, install2.swm, ...).
    [[nodiscard]] static QStringList splitImageArguments(const QString& wim, const QString& swm);
    /// @brief One part of a split install image: its name and the SHA-256 it was written with.
    struct SplitPart {
        QString name;
        QByteArray sha256;
        qint64 bytes{0};
    };
    /// @brief Split @p wim (in a work folder) with @p dism into parts, copy each part into
    ///        @p sources_dir and record its hash. False, with lastError set, on any failure.
    [[nodiscard]] bool splitInstallImageInto(const QString& dism,
                                             const QString& wim,
                                             const QString& sources_dir);
    /// @brief Read every recorded part back from @p sources_dir and compare its hash. False, with
    ///        lastError naming the part, on a missing or changed one.
    [[nodiscard]] bool verifySplitParts(const QString& sources_dir);
    /// @brief The parts the last split wrote, in order.
    [[nodiscard]] const QList<SplitPart>& splitParts() const { return m_split_parts; }
    /// @brief What is missing from the extracted files at @p cleanDest ("X:\"), or empty. With
    ///        @p install_split the install image is not asked for: it was left out to be split,
    ///        and its parts are written and read back after this check.
    [[nodiscard]] static QString extractedBootFilesProblem(const QString& cleanDest,
                                                           bool install_split);
    /// @brief 7-Zip's arguments to extract @p iso into @p dest, its progress on stdout, which
    ///        the build reads. With @p install_split, install.wim is left out to be split.
    [[nodiscard]] static QStringList extractionArguments(const QString& iso,
                                                         const QString& dest,
                                                         bool install_split);
    /// @brief The newest percentage in a chunk of 7-Zip's progress output, or -1 for none.
    [[nodiscard]] static int extractionPercentIn(const QString& chunk);

    /// @brief What the status line says when the media has passed its final checks: what was
    ///        checked, a FAT32 volume with the UEFI boot file and the install image read back.
    [[nodiscard]] static QString finalVerificationStatus();

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

    /// @brief Whether this run has started writing to the target disk (its diskpart clean).
    ///        A failure before that leaves the drive as it was, and the panel says so.
    [[nodiscard]] bool targetWasWritten() const { return m_target_written.load(); }

    /// @brief How long the disk-safety check waits for Windows to describe the target. A cold
    ///        PowerShell with the Storage module took 3.2 s on an idle physical PC; 5 s ran out
    ///        in a busy VM and failed every Windows USB job at once.
    static constexpr int kDiskSafetyProbeTimeoutMs = 30'000;

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
    bool verifyFat32Filesystem(const QString& driveLetter);

    /// @brief Validate and normalize raw drive letter from PowerShell
    QString validateDriveLetter(const QString& rawLetter);

    /// @brief Extract ISO contents and verify critical files (Step 2)
    bool extractAndVerifyFiles(const QString& isoPath, const QString& driveLetter);

    /// @brief TOCTOU guard run before extraction writes anything: confirm @p driveLetter still
    /// resolves to the ORIGINAL target disk (m_diskNumber). A hot-plug that reassigned the letter
    /// to another disk's volume between format and extraction would otherwise let 7z overwrite that
    /// unrelated volume. Fails closed on any mismatch or query failure.
    bool verifyDriveLetterMapsToTarget(const QString& driveLetter);

    /// @brief Wait (bounded) for partition 1 of @p diskNumber to have a volume Windows can
    /// format. Fails closed with m_lastError set on timeout or cancel.
    bool waitForNewPartitionVolume(const QString& diskNumber);

    /// @brief Wait (bounded) for the formatted volume's drive letter. Empty on timeout or
    /// cancel, with m_lastError naming the disk and the bound.
    QString waitForFormattedDriveLetter(const QString& diskNumber);

    /// @brief Give the formatted volume a drive letter if Windows has not, after a short wait.
    ///        Windows assigns one by itself only while automount is on, and in testing it did
    ///        not for a USB disk it treats as fixed. Never fails the run itself: the bounded
    ///        wait for the letter that follows decides.
    void assignDriveLetterIfMissing(const QString& diskNumber);

    /**
     * @brief Format drive as NTFS using diskpart
     * @param diskNumber Disk number (hardware ID) to format
     * @return true if successful
     */
    bool formatDriveFat32(const QString& diskNumber);

    /// @brief Clean disk and create MBR partition using diskpart
    bool cleanAndPartitionDisk(const QString& diskNumber);
    /// @brief Format the partition as NTFS using diskpart
    bool formatPartitionFat32(const QString& diskNumber);

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
    /// The extracted tree's own checks: destination, setup.exe and the boot files.
    bool copyISO_verifyExtracted(const QString& cleanDest);
    /// Lists the ISO and decides whether its install.wim must be split for FAT32.
    bool copyISO_planInstallImage(const QString& sevenZipPath, const QString& sourcePath);
    /// Takes install.wim out of the ISO and splits it onto the drive.
    bool copyISO_splitInstallImage(const QString& sevenZipPath,
                                   const QString& sourcePath,
                                   const QString& cleanDest);
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

    /// @brief Run Step 3, the sole path to a verified success: announce, then
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
    std::atomic<bool> m_target_written{false};  ///< Set just before the diskpart clean
    mutable QMutex m_errorMutex;                ///< Guards m_lastError for cross-thread access
    QString m_lastError;                        ///< Protected by m_errorMutex
    QString m_volumeLabel;                      // Volume label extracted from ISO
    QString m_diskNumber;          // Hardware disk number (e.g., "1" for PhysicalDrive1)
    QString m_targetDiskUniqueId;  ///< Get-Disk UniqueId pinned by guardTargetDiskSafe()
    qint64 m_targetDiskSizeBytes{
        -1};                      ///< Target disk capacity from guardTargetDiskSafe(); -1 = unknown
    qint64 m_isoSizeBytes{-1};    ///< ISO size pinned at validation for TOCTOU immutability check
    qint64 m_isoModifiedMs{-1};   ///< ISO mtime (ms since epoch) pinned for immutability check
    bool m_install_split{false};  ///< install.wim is too large for FAT32 and was split
    QList<SplitPart> m_split_parts;  ///< the parts written, with the hashes they were written with
};
