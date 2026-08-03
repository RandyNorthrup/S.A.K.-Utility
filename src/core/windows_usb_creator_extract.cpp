// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/windows_usb_creator.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QThread>

namespace {
constexpr qsizetype kSevenZipCommentPrefixLength = 10;
constexpr qsizetype kSevenZipPathPrefixLength = 7;
constexpr qsizetype kSevenZipSizePrefixLength = 7;
constexpr qsizetype kSevenZipFolderPrefixLength = 9;
constexpr int kExtractionWorkspaceMultiplier = 2;
constexpr int kCapacityDisplayPrecision = 2;
constexpr int kProgressDisplayPrecision = 1;
constexpr int kExtractProgressStart = 15;
constexpr int kExtractProgressSpan = 35;
constexpr int kSevenZipTotalBytesCaptureGroup = 2;
constexpr int kExtractionSummaryLineCount = 5;
constexpr int kMinimumVerifiedCriticalFiles = 3;
constexpr int kCriticalFilesVerifiedProgress = 92;
constexpr int kBootableFlagVerifiedProgress = 95;
constexpr int kFileCountVerifiedProgress = 98;
}  // namespace

bool WindowsUSBCreator::isSafeBundledExecutable(const QString& path, const QString& appDir) {
    const QFileInfo fi(path);
    // Must be a real, existing regular file -- never a symlink/reparse/junction.
    if (!fi.exists() || !fi.isFile() || fi.isSymLink()) {
        return false;
    }
    // The fully resolved path must live inside the resolved application directory,
    // so a redirected/relative path can never point execution outside the tree.
    const QString canonical = fi.canonicalFilePath();
    const QString canonicalDir = QFileInfo(appDir).canonicalFilePath();
    if (canonical.isEmpty() || canonicalDir.isEmpty()) {
        return false;
    }
    return canonical.startsWith(canonicalDir, Qt::CaseInsensitive);
}

bool WindowsUSBCreator::copyISOContents(const QString& sourcePath, const QString& destPath) {
    Q_ASSERT(!sourcePath.isEmpty());
    Q_ASSERT(!destPath.isEmpty());
    sak::logInfo(
        QString("Extracting ISO contents: %1 -> %2").arg(sourcePath, destPath).toStdString());

    // Verify source ISO exists
    if (!QFile::exists(sourcePath)) {
        m_lastError = QString("ISO file not found: %1").arg(sourcePath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Get path to embedded 7z.exe
    QString appDir = QCoreApplication::applicationDirPath();
    QString sevenZipPath = appDir + "/tools/chocolatey/tools/7z.exe";

    // Defense in depth: only execute the bundled 7z if it is a real regular file
    // that canonically resides under the app directory -- never a symlink/reparse
    // point that could redirect execution outside the install tree.
    if (!isSafeBundledExecutable(sevenZipPath, appDir)) {
        m_lastError =
            QString("Refusing to run untrusted or missing 7z.exe at: %1").arg(sevenZipPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    copyISO_extractVolumeLabel(sevenZipPath, sourcePath);

    QString cleanDest;
    if (!copyISO_normalizeDestination(destPath, cleanDest)) {
        return false;
    }
    if (!copyISO_checkDiskSpace(cleanDest, sourcePath)) {
        return false;
    }
    if (!copyISO_runExtraction(sevenZipPath, sourcePath, cleanDest)) {
        return false;
    }
    if (!copyISO_verifyDestination(cleanDest)) {
        return false;
    }
    if (!copyISO_findSetupExe(cleanDest)) {
        return false;
    }
    if (!copyISO_verifyBootFiles(cleanDest)) {
        return false;
    }

    copyISO_setVolumeLabel(cleanDest);

    sak::logInfo("ISO extraction completed successfully");

    // Verify extraction integrity by comparing file list and sizes
    if (!verifyExtractionIntegrity(sourcePath, cleanDest, sevenZipPath)) {
        m_lastError = "Extraction verification failed - files do not match ISO contents";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return true;
}

QString WindowsUSBCreator::parseVolumeLabelFromOutput(const QString& output) {
    QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (!line.startsWith("Comment = ")) {
            continue;
        }
        QString label = line.mid(kSevenZipCommentPrefixLength).trimmed();
        sak::logInfo(QString("ISO volume label: %1").arg(label).toStdString());
        return label;
    }
    return {};
}

QString sak::sanitizeVolumeLabel(const QString& raw) {
    constexpr qsizetype kMaxVolumeLabelLength = 32;  // NTFS max label
    static const QString kExtraAllowed = QStringLiteral(" _-.");
    QString clean;
    for (const QChar c : raw) {
        if (clean.size() >= kMaxVolumeLabelLength) {
            break;
        }
        if (c.isLetterOrNumber() || kExtraAllowed.contains(c)) {
            clean.append(c);
        }
    }
    return clean.trimmed();
}

void WindowsUSBCreator::copyISO_extractVolumeLabel(const QString& sevenZipPath,
                                                   const QString& sourcePath) {
    QStringList labelArgs;
    labelArgs << "l" << "-slt" << sourcePath;  // List with technical info

    const auto label_result =
        sak::runProcess(sevenZipPath, labelArgs, sak::kTimeoutProcessMediumMs, [this]() {
            return m_cancelled.load();
        });
    if (!label_result.succeeded()) {
        sak::logWarning(
            "7z label extraction failed/timed out after 10s -- will use default "
            "label");
    } else {
        // Allowlist the ISO-derived label before it is ever interpolated into a PowerShell
        // Set-Volume command (elevated), so a crafted "Comment" cannot inject commands.
        m_volumeLabel = sak::sanitizeVolumeLabel(parseVolumeLabelFromOutput(label_result.std_out));
    }

    // Default to WINDOWS if label not found. Correct-by-design, not an error-hiding
    // fallback: a volume MUST carry some name, the label is purely cosmetic (no
    // bearing on bootability or data integrity), and this is a fixed safe constant
    // -- never attacker-influenced. m_volumeLabel is reset per run in
    // createBootableUSB(), so a prior run's label can no longer persist here.
    if (m_volumeLabel.isEmpty()) {
        m_volumeLabel = "WINDOWS";
        sak::logInfo(QString("Using default volume label: %1").arg(m_volumeLabel).toStdString());
    }
}

bool WindowsUSBCreator::copyISO_normalizeDestination(const QString& destPath, QString& cleanDest) {
    Q_ASSERT(!destPath.isEmpty());
    // cleanDest is a write-only out-param (empty on entry); do not assert it.
    // Normalize drive letter to full path format (e.g., "E" -> "E:\")
    cleanDest = destPath.trimmed();

    // Remove any existing colons and backslashes to start fresh
    cleanDest.remove(':');
    cleanDest.remove('\\');
    cleanDest.remove('/');

    // Should now have just the drive letter
    if (cleanDest.length() != 1 || !cleanDest[0].isLetter()) {
        m_lastError =
            QString("Invalid drive letter format: '%1' (expected single letter A-Z)").arg(destPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Build proper drive path: "E" -> "E:\"
    cleanDest = cleanDest.toUpper() + ":\\";

    sak::logInfo(QString("Normalized destination path: %1").arg(cleanDest).toStdString());
    return true;
}

bool WindowsUSBCreator::copyISO_checkDiskSpace(const QString& cleanDest,
                                               const QString& sourcePath) {
    QStorageInfo storage(cleanDest);
    if (!storage.isValid() || !storage.isReady()) {
        m_lastError = QString("Cannot access destination drive %1").arg(cleanDest);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    qint64 availableSpace = storage.bytesAvailable();
    QFileInfo isoInfo(sourcePath);
    qint64 isoSize = isoInfo.size();

    // Require at least 2x ISO size for extraction (compressed files expand)
    qint64 requiredSpace = isoSize * kExtractionWorkspaceMultiplier;

    if (availableSpace < requiredSpace) {
        m_lastError =
            QString("Insufficient disk space: need %1 GB, have %2 GB")
                .arg(requiredSpace / sak::kBytesPerGBf, 0, 'f', kCapacityDisplayPrecision)
                .arg(availableSpace / sak::kBytesPerGBf, 0, 'f', kCapacityDisplayPrecision);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("Disk space check: %1 GB available, %2 GB required")
                     .arg(availableSpace / sak::kBytesPerGBf, 0, 'f', kCapacityDisplayPrecision)
                     .arg(requiredSpace / sak::kBytesPerGBf, 0, 'f', kCapacityDisplayPrecision)
                     .toStdString());
    return true;
}

bool WindowsUSBCreator::copyISO_runExtraction(const QString& sevenZipPath,
                                              const QString& sourcePath,
                                              const QString& cleanDest) {
    sak::logInfo(
        QString("Using 7z.exe to extract ISO directly to %1").arg(cleanDest).toStdString());
    Q_EMIT statusChanged("Extracting Windows installation files...");

    // 7z x = extract with full paths, -aoa = overwrite all, -bsp2 = detailed progress to stdout
    QStringList args;
    args << "x";                             // Extract with full paths
    args << "-aoa";                          // Overwrite all existing files
    args << "-bsp2";                         // Detailed progress (bytes) redirected to stdout
    args << "-y";                            // Assume Yes on all queries
    args << sourcePath;
    args << QString("-o%1").arg(cleanDest);  // Output directory

    sak::logInfo(QString("7z command: %1 %2").arg(sevenZipPath, args.join(" ")).toStdString());
    sak::logInfo(QString("Extracting to absolute path: %1").arg(cleanDest).toStdString());
    int lastProgressPercent = kExtractProgressStart;
    qint64 totalBytes = 0;
    qint64 processedBytes = 0;
    bool started = false;

    const auto result = sak::runProcessStreaming(
        {.program = sevenZipPath,
         .args = args,
         .timeout_ms = 900'000,
         .on_output =
             [this, &totalBytes, &processedBytes, &lastProgressPercent](const QString& chunk,
                                                                        bool is_stderr) {
                 if (!is_stderr) {
                     copyISO_parseExtractionProgress(
                         chunk, totalBytes, processedBytes, lastProgressPercent);
                 }
             },
         .on_started =
             [this, &started]([[maybe_unused]] qint64 process_id) {
                 started = true;
                 sak::logInfo(
                     "7z process started, extracting ISO (this may take several minutes)...");
                 Q_EMIT statusChanged("Extracting Windows files...");
             },
         .should_cancel = [this]() { return m_cancelled.load(); }});

    if (!started) {
        m_lastError = QString("Failed to start 7z.exe at: %1").arg(sevenZipPath);
        if (!result.std_err.isEmpty()) {
            m_lastError += QStringLiteral(": ") + result.std_err.trimmed();
        }
        sak::logError(m_lastError.toStdString());
        return false;
    }
    if (result.cancelled) {
        m_lastError = "Extraction cancelled by user";
        sak::logInfo(m_lastError.toStdString());
        return false;
    }
    if (result.timed_out) {
        m_lastError = "ISO extraction timed out after 15 minutes";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return copyISO_logExtractionResult(result);
}

void WindowsUSBCreator::copyISO_parseExtractionProgress(const QString& output,
                                                        qint64& totalBytes,
                                                        qint64& processedBytes,
                                                        int& lastProgressPercent) {
    // Parse 7z -bsp2 output format: "bytes_processed + bytes_total"
    // Format example: "123456789 + 987654321" or percentage "12%"
    QRegularExpression bytesRegex(R"((\d+)\s*\+\s*(\d+))");
    QRegularExpressionMatch bytesMatch = bytesRegex.match(output);

    if (!bytesMatch.hasMatch()) {
        // Fallback: Try to parse percentage if bytes format not found
        QRegularExpression percentRegex(R"(\s+(\d+)%)");
        QRegularExpressionMatch percentMatch = percentRegex.match(output);
        if (!percentMatch.hasMatch()) {
            return;
        }

        int extractPercent = percentMatch.captured(1).toInt();
        int totalProgress = kExtractProgressStart +
                            (extractPercent * kExtractProgressSpan / sak::kPercentMax);
        if (totalProgress <= lastProgressPercent) {
            return;
        }

        lastProgressPercent = totalProgress;
        Q_EMIT progressUpdated(totalProgress);
        Q_EMIT statusChanged(QString("Extracting Windows files... %1%").arg(extractPercent));
        sak::logInfo(QString("Extraction progress: %1%").arg(extractPercent).toStdString());
        return;
    }

    // Bytes format matched
    processedBytes = bytesMatch.captured(1).toLongLong();
    qint64 newTotal = bytesMatch.captured(kSevenZipTotalBytesCaptureGroup).toLongLong();
    if (newTotal > totalBytes) {
        totalBytes = newTotal;
    }
    if (totalBytes <= 0) {
        return;
    }

    int extractPercent = static_cast<int>((processedBytes * sak::kPercentMax) / totalBytes);
    int totalProgress = kExtractProgressStart +
                        (extractPercent * kExtractProgressSpan / sak::kPercentMax);
    if (totalProgress <= lastProgressPercent) {
        return;
    }

    lastProgressPercent = totalProgress;
    Q_EMIT progressUpdated(totalProgress);

    double processedMB = processedBytes / sak::kBytesPerMBf;
    double totalMB = totalBytes / sak::kBytesPerMBf;

    Q_EMIT statusChanged(QString("Extracting Windows files... %1 MB / %2 MB (%3%)")
                             .arg(processedMB, 0, 'f', kProgressDisplayPrecision)
                             .arg(totalMB, 0, 'f', kProgressDisplayPrecision)
                             .arg(extractPercent));

    sak::logInfo(QString("Extraction progress: %1 MB / %2 MB (%3%)")
                     .arg(processedMB, 0, 'f', kProgressDisplayPrecision)
                     .arg(totalMB, 0, 'f', kProgressDisplayPrecision)
                     .arg(extractPercent)
                     .toStdString());
}

bool WindowsUSBCreator::copyISO_logExtractionResult(const sak::ProcessResult& result) {
    int exitCode = result.exit_code;
    const QString output = result.std_out;
    const QString errors = result.std_err;

    sak::logInfo(QString("7z extraction completed with exit code: %1").arg(exitCode).toStdString());

    if (!output.isEmpty()) {
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        sak::logInfo(QString("7z processed %1 lines of output").arg(lines.count()).toStdString());
        // Log last few lines which contain summary
        for (int i = qMax(0, lines.count() - kExtractionSummaryLineCount); i < lines.count(); ++i) {
            sak::logInfo(QString("  %1").arg(lines[i].trimmed()).toStdString());
        }
    }

    if (!errors.isEmpty()) {
        sak::logError(QString("7z stderr: %1").arg(errors).toStdString());
    }

    // 7z exit codes: 0 = success, non-zero = error
    if (exitCode != 0) {
        m_lastError = QString("7z extraction failed with exit code %1").arg(exitCode);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return true;
}

bool WindowsUSBCreator::copyISO_verifyDestination(const QString& cleanDest) {
    Q_ASSERT(!cleanDest.isEmpty());
    // Wait a moment for filesystem to settle after extraction
    sak::logInfo("Waiting for filesystem to settle after extraction...");
    QThread::msleep(sak::kTimerServiceDelayMs);

    // Verify critical Windows files were extracted to the destination
    sak::logInfo(QString("Verifying critical files exist at: %1").arg(cleanDest).toStdString());

    QDir checkDest(cleanDest);

    // Verify directory exists before listing
    if (!checkDest.exists()) {
        m_lastError = QString("Destination directory does not exist: %1").arg(cleanDest);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    QStringList destFiles = checkDest.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    sak::logInfo(QString("Destination now contains %1 items").arg(destFiles.count()).toStdString());

    if (destFiles.isEmpty()) {
        m_lastError = "Extraction completed but destination directory is empty";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Show ALL files/folders extracted with their types
    sak::logInfo("Complete listing of extracted items:");
    for (const QString& item : destFiles) {
        QFileInfo info(checkDest.absoluteFilePath(item));
        QString type = info.isDir() ? "DIR" : QString("FILE (%1 bytes)").arg(info.size());
        sak::logInfo(QString("  %1 - %2").arg(item, type).toStdString());
    }

    return true;
}

bool WindowsUSBCreator::copyISO_findSetupExe(const QString& cleanDest) {
    Q_ASSERT(!cleanDest.isEmpty());
    QDir checkDest(cleanDest);
    QString setupPath = checkDest.absoluteFilePath("setup.exe");

    // Verify setup.exe exists (absolute requirement for Windows boot)
    sak::logInfo(QString("Checking for setup.exe at: %1").arg(setupPath).toStdString());

    if (QFile::exists(setupPath)) {
        sak::logInfo(QString("\xe2\x9c\x93 setup.exe found at: %1").arg(setupPath).toStdString());
        return true;
    }

    // Try case-insensitive search in root directory
    sak::logWarning("setup.exe not found with exact case, searching case-insensitively...");
    QStringList rootFiles = checkDest.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : rootFiles) {
        if (file.toLower() != "setup.exe") {
            continue;
        }
        setupPath = cleanDest + file;
        sak::logInfo(QString("Found setup file with different case: %1").arg(file).toStdString());
        sak::logInfo(QString("\xe2\x9c\x93 setup.exe found at: %1").arg(setupPath).toStdString());
        return true;
    }

    m_lastError = "CRITICAL: setup.exe not found after extraction";
    sak::logError(m_lastError.toStdString());
    sak::logError(QString("Checked path: %1").arg(setupPath).toStdString());
    if (!rootFiles.isEmpty()) {
        sak::logError(QString("Files in root: %1").arg(rootFiles.join(", ")).toStdString());
    }
    sak::logError("ISO extraction may have failed or ISO may be corrupt");
    return false;
}

bool WindowsUSBCreator::copyISO_verifyBootFiles(const QString& cleanDest) {
    Q_ASSERT(!cleanDest.isEmpty());
    // Verify other critical Windows boot files - REQUIRED for bootable USB
    QStringList criticalFiles = {"sources/boot.wim", "bootmgr"};

    // At least one of these must exist (different Windows versions have different structures)
    QStringList alternateFiles = {"sources/install.wim", "sources/install.esd"};

    QStringList foundFiles;
    foundFiles << "setup.exe";  // Already verified above

    // Check required critical files
    for (const QString& file : criticalFiles) {
        QString fullPath = cleanDest + file;
        if (QFile::exists(fullPath)) {
            sak::logInfo(QString("\xe2\x9c\x93 Found: %1").arg(file).toStdString());
            foundFiles << file;
        } else {
            m_lastError = QString("CRITICAL: Required file not found: %1").arg(file);
            sak::logError(m_lastError.toStdString());
            sak::logError("Windows installation files incomplete - USB will not boot");
            return false;
        }
    }

    // Check that at least ONE alternate file exists
    bool hasInstallImage = false;
    for (const QString& file : alternateFiles) {
        QString fullPath = cleanDest + file;
        if (QFile::exists(fullPath)) {
            sak::logInfo(QString("\xe2\x9c\x93 Found install image: %1").arg(file).toStdString());
            foundFiles << file;
            hasInstallImage = true;
            break;
        }
    }

    if (!hasInstallImage) {
        m_lastError =
            "CRITICAL: No Windows install image found (install.wim or install.esd "
            "required)";
        sak::logError(m_lastError.toStdString());
        sak::logError("Windows installation incomplete - USB will not be able to install Windows");
        return false;
    }

    sak::logInfo(QString("\xe2\x9c\x93 All critical files verified: %1 core files found")
                     .arg(foundFiles.count())
                     .toStdString());
    return true;
}

void WindowsUSBCreator::copyISO_setVolumeLabel(const QString& cleanDest) {
    Q_ASSERT(!m_volumeLabel.isEmpty());
    Q_ASSERT(!cleanDest.isEmpty());
    if (m_volumeLabel.isEmpty()) {
        return;
    }

    sak::logInfo(QString("Setting volume label to: %1").arg(m_volumeLabel).toStdString());
    // Extract single drive letter from normalized path ("E:\\" -> "E")
    QString driveLetter = cleanDest.left(1);
    if (driveLetter.isEmpty() || !driveLetter[0].isLetter()) {
        sak::logWarning(
            QString("Invalid drive letter for volume label: '%1'").arg(cleanDest).toStdString());
        return;
    }

    // m_volumeLabel is already allowlisted; also double any single quote as defense in depth so a
    // future allowlist relaxation cannot reopen the single-quoted-string break-out.
    QString safeLabel = m_volumeLabel;
    safeLabel.replace(QChar('\''), QStringLiteral("''"));
    QString labelCmd =
        QString("Set-Volume -DriveLetter %1 -NewFileSystemLabel '%2'").arg(driveLetter, safeLabel);
    const auto label_result =
        sak::runPowerShell(labelCmd, sak::kTimeoutProcessMediumMs, true, false, [this]() {
            return m_cancelled.load();
        });
    if (label_result.timed_out || label_result.cancelled) {
        sak::logWarning("Volume label command timed out");
        return;
    }

    // Best-effort by design: the volume label is cosmetic and has no bearing on
    // bootability or data integrity, so a Set-Volume failure is logged but does NOT
    // fail the creation (failing usable media over a cosmetic rename would be the
    // harmful outcome). Every correctness-relevant property is verified elsewhere.
    if (label_result.exit_code == 0) {
        sak::logInfo("Volume label set successfully");
    } else {
        QString labelErrors = label_result.std_err.trimmed();
        sak::logWarning(QString("Failed to set volume label: %1").arg(labelErrors).toStdString());
    }
}

bool WindowsUSBCreator::makeBootable(const QString& driveLetter) {
    Q_ASSERT(!driveLetter.isEmpty());
    sak::logInfo(QString("Configuring boot files on %1").arg(driveLetter).toStdString());

    // Normalize drive letter to single character
    QString cleanDrive = driveLetter.trimmed();
    if (cleanDrive.endsWith(":")) {
        cleanDrive = cleanDrive.left(1);
    }
    cleanDrive.remove("\\");
    cleanDrive.remove("/");

    // Validate we have a single letter
    if (cleanDrive.length() != 1 || !cleanDrive[0].isLetter()) {
        m_lastError =
            QString("Invalid drive letter format for boot configuration: '%1'").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    cleanDrive = cleanDrive.toUpper();

    // SECURITY: resolve bcdboot ONLY to the host's Authenticode-signed System32
    // copy -- never a copy extracted from the (untrusted) ISO, which could be an
    // attacker-planted binary that would then run elevated. If the host copy
    // cannot be resolved we must NOT certify the media bootable.
    const QString bcdbootPath = resolveBcdbootPath();
    if (bcdbootPath.isEmpty()) {
        const QString err = "Host System32 bcdboot.exe not found -- cannot configure boot files";
        setError(err);
        sak::logError(err.toStdString());
        return false;
    }

    sak::logInfo(QString("Configuring boot environment using bcdboot from %1")
                     .arg(bcdbootPath)
                     .toStdString());

    return runBcdboot(bcdbootPath, cleanDrive);
}

QString WindowsUSBCreator::resolveBcdbootPath() {
    // ONLY the host's %SystemRoot%\System32 bcdboot.exe (Authenticode-signed,
    // present on every running Windows). A media-sourced copy is NEVER trusted or
    // executed. %SystemRoot% is resolved with no C:\Windows fallback: fail closed.
    return system32ExePath(QStringLiteral("bcdboot.exe"));
}

bool WindowsUSBCreator::bcdbootReportsSuccess(bool timedOut, bool cancelled, int exitCode) {
    return !timedOut && !cancelled && exitCode == 0;
}

bool WindowsUSBCreator::runBcdboot(const QString& bcdbootPath, const QString& cleanDrive) {
    Q_ASSERT(!bcdbootPath.isEmpty());
    Q_ASSERT(!cleanDrive.isEmpty());
    // KNOWN LIMITATIONS (tracked; see Codex-review-3 findings 4 and 5 -- fixing
    // either is a design change beyond this file, so they are NOT silently masked
    // here; the run is still gated on bcdboot's real exit below):
    //  - #5: bcdboot's positional <source> must be a Windows directory (e.g.
    //    X:\Windows). Install media has no \Windows tree, so a drive-root source
    //    can make bcdboot error; a correct fix must supply a valid Windows source
    //    (or drop bcdboot in favour of the ISO's own already-extracted BCD).
    //  - #4: the media is NTFS with no FAT32 ESP / UEFI:NTFS shim, so /f ALL's UEFI
    //    files may be unreadable by firmware that cannot boot NTFS. A universal fix
    //    needs a FAT32 ESP or bundled UEFI:NTFS loader (Rufus-style).
    QStringList args;
    args << QString("%1:\\").arg(cleanDrive);
    args << "/s" << QString("%1:").arg(cleanDrive);
    // /f ALL writes BOTH BIOS and UEFI boot files; the previous "BIOS" only
    // configured legacy boot yet the media was reported UEFI-bootable.
    args << "/f" << "ALL";

    const auto bcdboot_result = sak::runProcess(
        bcdbootPath, args, sak::kTimeoutProcessLongMs, [this]() { return m_cancelled.load(); });

    if (!bcdboot_result.std_out.isEmpty()) {
        sak::logInfo(QString("bcdboot output: %1").arg(bcdboot_result.std_out).toStdString());
    }

    // Gate boot certification on bcdboot actually succeeding. A timeout,
    // cancellation, or non-zero exit means the boot config did NOT complete.
    if (!bcdbootReportsSuccess(
            bcdboot_result.timed_out, bcdboot_result.cancelled, bcdboot_result.exit_code)) {
        const QString err =
            QString("bcdboot did not complete (timed_out=%1, cancelled=%2, exit=%3): %4")
                .arg(bcdboot_result.timed_out)
                .arg(bcdboot_result.cancelled)
                .arg(bcdboot_result.exit_code)
                .arg(bcdboot_result.std_err.trimmed());
        setError(err);
        sak::logError(err.toStdString());
        return false;
    }

    sak::logInfo("Boot configuration completed successfully");
    return true;
}

bool WindowsUSBCreator::checkPartitionActive(const QString& diskNumber) {
    Q_ASSERT(!diskNumber.isEmpty());
    // Integer-validate before interpolating into the diskpart script (same class of
    // value as createBootableUSB validates), so no stray tokens can be injected.
    static const QRegularExpression diskNumRe(QStringLiteral("^\\d{1,3}$"));
    if (!diskNumRe.match(diskNumber).hasMatch()) {
        m_lastError = QString("Invalid disk number for partition check: '%1'").arg(diskNumber);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    const QString diskpartScript = QString(
                                       "select disk %1\n"
                                       "select partition 1\n"
                                       "detail partition\n")
                                       .arg(diskNumber);

    QString output;
    if (!runDiskpartScript(diskpartScript, sak::kTimeoutProcessLongMs, output)) {
        return false;
    }
    sak::logInfo(QString("Diskpart detail output: %1").arg(output).toStdString());

    // Match the VALUE, not just the label: `detail partition` always prints an "Active : No/Yes"
    // line, so a substring check for "Active" passed even for a non-bootable partition. Absent
    // value line (GPT/localized) -> no match -> fails closed below.
    static const QRegularExpression activeYesRe(QStringLiteral("Active\\s*:\\s*Yes"),
                                                QRegularExpression::CaseInsensitiveOption);
    bool isActive = activeYesRe.match(output).hasMatch();

    if (isActive) {
        sak::logInfo("[x] Bootable flag verified - partition is active");
        return true;
    }

    m_lastError = "VERIFICATION FAILED: Partition is not marked as active/bootable";
    sak::logError(m_lastError.toStdString());
    sak::logError("USB drive will NOT be bootable - bootable flag must be set");
    return false;
}

bool WindowsUSBCreator::verifyBootableFlag(const QString& driveLetter) {
    Q_ASSERT(!driveLetter.isEmpty());
    Q_EMIT statusChanged("Verifying bootable flag...");
    sak::logInfo(QString("Verifying bootable flag on drive %1").arg(driveLetter).toStdString());

    // Normalize drive letter to single character
    QString cleanDrive = driveLetter.trimmed();
    if (cleanDrive.endsWith(":")) {
        cleanDrive = cleanDrive.left(1);
    }
    cleanDrive.remove("\\");
    cleanDrive.remove("/");

    // Validate format
    if (cleanDrive.length() != 1 || !cleanDrive[0].isLetter()) {
        m_lastError =
            QString("Invalid drive letter format for verification: '%1'").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    cleanDrive = cleanDrive.toUpper();

    QString diskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDrive);
    const auto disk_result =
        sak::runPowerShell(diskCmd, sak::kTimeoutProcessMediumMs, true, false, [this]() {
            return m_cancelled.load();
        });

    if (!disk_result.succeeded()) {
        m_lastError = "VERIFICATION FAILED: Could not query partition disk number";
        sak::logError(m_lastError.toStdString());
        return false;  // fail closed: an unverified boot flag is not a pass
    }

    QString diskNumber = disk_result.std_out.trimmed();
    if (diskNumber.isEmpty()) {
        m_lastError = "VERIFICATION FAILED: Could not determine disk number for boot-flag check";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Identity pin: the drive letter must still map back to the ORIGINAL target
    // disk. A hot-plug that reassigned the letter to a different disk would
    // otherwise let us "verify" the wrong disk. Compare as integers so "1"/"01"
    // never causes a spurious mismatch. Fail closed on any divergence.
    if (diskNumber.toInt() != m_diskNumber.toInt()) {
        m_lastError =
            QString("VERIFICATION FAILED: drive %1 now maps to disk %2, not target disk %3")
                .arg(cleanDrive, diskNumber, m_diskNumber);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return checkPartitionActive(diskNumber);
}

bool WindowsUSBCreator::verifyExtractionIntegrity(const QString& isoPath,
                                                  const QString& destPath,
                                                  const QString& sevenZipPath) {
    sak::logInfo("Starting extraction integrity verification...");
    Q_EMIT statusChanged("Verifying extraction integrity...");

    // TOCTOU immutability check: the user-selected ISO is re-opened here (and was
    // re-opened during extraction). Confirm it has not been swapped/modified since
    // it was pinned at validation -- fail closed on any size/mtime change.
    const QFileInfo isoInfo(isoPath);
    if (isoInfo.size() != m_isoSizeBytes ||
        isoInfo.lastModified().toMSecsSinceEpoch() != m_isoModifiedMs) {
        m_lastError = "ISO changed during processing (size/timestamp mismatch) -- aborting";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Get detailed file list from ISO with sizes
    QStringList listArgs;
    listArgs << "l" << "-slt" << isoPath;  // List with technical info

    const auto list_result = sak::runProcess(sevenZipPath,
                                             listArgs,
                                             sak::kTimeoutProcessVeryLongMs,  // 1 minute timeout
                                             [this]() { return m_cancelled.load(); });
    if (!list_result.succeeded()) {
        m_lastError = "Verification failed: Could not list ISO contents";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    QString isoListing = list_result.std_out;
    QStringList lines = isoListing.split("\n");

    auto criticalFiles = parseIsoCriticalFiles(lines);

    if (criticalFiles.isEmpty()) {
        m_lastError = "Verification failed: No critical Windows files found in ISO";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(
        QString("Verifying %1 critical files...").arg(criticalFiles.count()).toStdString());

    return verifyCriticalFilesOnDisk(criticalFiles, destPath);
}

bool WindowsUSBCreator::isCriticalWindowsFile(const QString& path) const {
    Q_ASSERT(!path.isEmpty());
    QString lowerPath = path.toLower();
    return lowerPath.contains("setup.exe") || lowerPath.contains("bootmgr") ||
           lowerPath.contains("sources/boot.wim") || lowerPath.contains("sources\\\\boot.wim") ||
           lowerPath.contains("sources/install.wim") ||
           lowerPath.contains("sources\\\\install.wim") ||
           lowerPath.contains("sources/install.esd") ||
           lowerPath.contains("sources\\\\install.esd");
}

QList<QPair<QString, qint64>> WindowsUSBCreator::parseIsoCriticalFiles(const QStringList& lines) {
    Q_ASSERT(!lines.isEmpty());
    QList<QPair<QString, qint64>> criticalFiles;
    QString currentPath;
    qint64 currentSize = 0;
    bool isFolder = false;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("Path = ")) {
            currentPath = trimmed.mid(kSevenZipPathPrefixLength).trimmed();
            continue;
        }
        if (trimmed.startsWith("Size = ")) {
            bool ok = false;
            currentSize = trimmed.mid(kSevenZipSizePrefixLength).toLongLong(&ok);
            if (!ok) {
                // A malformed size must not degrade to 0 (which would then "match" a
                // truncated/empty file on disk); mark it invalid so verification
                // fails closed for that critical file.
                currentSize = -1;
            }
            continue;
        }
        if (trimmed.startsWith("Folder = ")) {
            isFolder = (trimmed.mid(kSevenZipFolderPrefixLength).trimmed() == "+");
            continue;
        }
        if (!trimmed.isEmpty() || currentPath.isEmpty()) {
            continue;
        }

        // End of entry -- add if it's a critical file (not a folder)
        if (!isFolder && isCriticalWindowsFile(currentPath)) {
            criticalFiles.append({currentPath, currentSize});
        }
        currentPath.clear();
        currentSize = 0;
        isFolder = false;
    }

    return criticalFiles;
}

bool WindowsUSBCreator::criticalFileOnDiskMatches(const QPair<QString, qint64>& fileInfo,
                                                  const QString& destPath) {
    // A critical file whose expected (ISO) size is non-positive is malformed or
    // zero-length; fail closed rather than letting a 0==0 match pass.
    if (fileInfo.second <= 0) {
        sak::logError(
            QString("[ ]-- Invalid expected size for: %1").arg(fileInfo.first).toStdString());
        return false;
    }
    // Normalize path from ISO (may use forward slashes); ensure trailing backslash.
    QString relativePath = fileInfo.first;
    relativePath.replace(QChar('/'), QChar('\\'));
    QString basePath = destPath;
    if (!basePath.endsWith("\\")) {
        basePath += "\\";
    }
    const QFileInfo destFileInfo(basePath + relativePath);
    if (!destFileInfo.exists()) {
        sak::logError(QString("[ ]-- Missing file: %1").arg(fileInfo.first).toStdString());
        return false;
    }
    if (destFileInfo.size() != fileInfo.second) {
        sak::logError(QString("[ ]-- Size mismatch: %1 (ISO: %2 bytes, USB: %3 bytes)")
                          .arg(fileInfo.first)
                          .arg(fileInfo.second)
                          .arg(destFileInfo.size())
                          .toStdString());
        return false;
    }
    return true;
}

bool WindowsUSBCreator::verifyCriticalFilesOnDisk(
    const QList<QPair<QString, qint64>>& criticalFiles, const QString& destPath) {
    int verifiedCount = 0;
    int failedCount = 0;

    for (const auto& fileInfo : criticalFiles) {
        if (criticalFileOnDiskMatches(fileInfo, destPath)) {
            verifiedCount++;
        } else {
            failedCount++;
        }
    }

    sak::logInfo(QString("Verification complete: %1 files verified, %2 failures")
                     .arg(verifiedCount)
                     .arg(failedCount)
                     .toStdString());

    if (verifiedCount < kMinimumVerifiedCriticalFiles) {
        m_lastError = QString(
                          "Verification failed: Only %1 critical files verified (minimum %2 "
                          "required)")
                          .arg(verifiedCount)
                          .arg(kMinimumVerifiedCriticalFiles);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (failedCount > 0) {
        m_lastError = QString("Extraction verification failed: %1 files missing or incorrect size")
                          .arg(failedCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo("[x] Extraction integrity verified - all critical files match ISO");
    return true;
}

bool WindowsUSBCreator::verifyBootAndInstallFiles(const QString& cleanDrive) {
    Q_ASSERT(!cleanDrive.isEmpty());
    // Verification 1: Check that critical boot files exist
    QStringList requiredFiles = {"setup.exe", "sources/boot.wim", "bootmgr"};

    sak::logInfo("Checking required files:");
    for (const QString& file : requiredFiles) {
        QString fullPath = cleanDrive + file;
        if (!QFile::exists(fullPath)) {
            m_lastError = QString("FINAL VERIFICATION FAILED: Critical file missing: %1").arg(file);
            sak::logError(m_lastError.toStdString());
            return false;
        }
        QFileInfo info(fullPath);
        sak::logInfo(QString("  [x] %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
    }

    // Verification 2: Check for install image
    Q_EMIT statusChanged("Verifying Windows install image...");
    bool hasInstallWim = QFile::exists(cleanDrive + "sources/install.wim");
    bool hasInstallEsd = QFile::exists(cleanDrive + "sources/install.esd");

    if (!hasInstallWim && !hasInstallEsd) {
        m_lastError = "FINAL VERIFICATION FAILED: No Windows install image found";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (hasInstallWim) {
        QFileInfo info(cleanDrive + "sources/install.wim");
        sak::logInfo(QString("  [x] install.wim (%1 bytes)").arg(info.size()).toStdString());
    }
    if (hasInstallEsd) {
        QFileInfo info(cleanDrive + "sources/install.esd");
        sak::logInfo(QString("  [x] install.esd (%1 bytes)").arg(info.size()).toStdString());
    }

    return true;
}

void WindowsUSBCreator::logFinalVerificationSuccess(int fileCount) {
    Q_ASSERT(fileCount >= 0);
    sak::logInfo("========================================");
    sak::logInfo("SUCCESS: ALL FINAL VERIFICATIONS PASSED");
    sak::logInfo("- Critical files: VERIFIED");
    sak::logInfo("- Install image: VERIFIED");
    sak::logInfo("- Bootable flag: VERIFIED (Active)");
    sak::logInfo(QString("- File count: VERIFIED (%1 items)").arg(fileCount).toStdString());
    sak::logInfo("========================================");

    Q_EMIT progressUpdated(sak::kPercentMax);
    Q_EMIT statusChanged("[x] USB VERIFIED BOOTABLE - All checks passed");

    // THIS IS THE ONLY PLACE completed() IS EMITTED
    Q_EMIT completed();
}

bool WindowsUSBCreator::finalVerification(const QString& driveLetter) {
    Q_ASSERT(!driveLetter.isEmpty());
    Q_ASSERT(driveLetter.length() >= 1);
    sak::logInfo("========================================");
    sak::logInfo("FINAL VERIFICATION - This is the ONLY path to success");
    sak::logInfo("========================================");

    // Normalize drive letter to standard path format
    QString cleanDrive = driveLetter.trimmed();

    // Remove any existing path separators
    cleanDrive.remove(":");
    cleanDrive.remove("\\");
    cleanDrive.remove("/");

    // Validate we have exactly one letter
    if (cleanDrive.length() != 1 || !cleanDrive[0].isLetter()) {
        m_lastError = QString("FINAL VERIFICATION FAILED: Invalid drive letter format: '%1'")
                          .arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Build standard path: "E" -> "E:\\"
    cleanDrive = cleanDrive.toUpper() + ":\\";

    sak::logInfo(QString("Final verification path: %1").arg(cleanDrive).toStdString());

    Q_EMIT statusChanged("Verifying all critical files...");

    if (!verifyBootAndInstallFiles(cleanDrive)) {
        return false;
    }

    Q_EMIT progressUpdated(kCriticalFilesVerifiedProgress);

    // Verification 3: MANDATORY bootable flag check
    Q_EMIT statusChanged("Verifying bootable flag...");
    if (!verifyBootableFlag(driveLetter)) {
        m_lastError = "FINAL VERIFICATION FAILED: " + m_lastError;
        sak::logError(m_lastError.toStdString());
        return false;
    }

    Q_EMIT progressUpdated(kBootableFlagVerifiedProgress);

    // Verification 4: Count total files to ensure extraction wasn't empty
    constexpr int kMinExpectedFileCount = 10;
    int fileCount = 0;
    QDirIterator iter(cleanDrive, QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        iter.next();
        ++fileCount;
    }

    if (fileCount < kMinExpectedFileCount) {
        m_lastError = QString("FINAL VERIFICATION FAILED: Only %1 files found (expected hundreds)")
                          .arg(fileCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("  \xe2\x9c\x93 Total files: %1").arg(fileCount).toStdString());

    Q_EMIT progressUpdated(kFileCountVerifiedProgress);

    logFinalVerificationSuccess(fileCount);

    return true;
}
