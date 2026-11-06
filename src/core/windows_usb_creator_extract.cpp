// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/media_read_back.h"
#include "sak/process_runner.h"
#include "sak/windows_usb_creator.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QThread>

#include <algorithm>

namespace {
constexpr qsizetype kSevenZipCommentPrefixLength = 10;
constexpr qsizetype kSevenZipPathPrefixLength = 7;
constexpr qsizetype kSevenZipSizePrefixLength = 7;
constexpr qsizetype kSevenZipFolderPrefixLength = 9;
constexpr int kExtractionWorkspaceMultiplier = 2;
// Taking a 7 GB install.wim out of an ISO, and DISM splitting it with its integrity check, each
// take minutes on a slow disk.
constexpr int kSplitStepTimeoutMs = 45 * 60 * 1000;
constexpr qsizetype kSplitErrorTailChars = 300;
constexpr qsizetype kFat32LabelMaxChars = 11;
constexpr int kCapacityDisplayPrecision = 2;
constexpr int kProgressDisplayPrecision = 1;
constexpr int kExtractProgressStart = 15;
constexpr int kExtractProgressSpan = 35;
// The split, after the extraction and before the 60% of a verified extraction: taking the image
// out, DISM, then one step per part copied (testpc1: about 12 minutes on a USB 2 link in all).
constexpr int kSplitProgressStart = 50;
constexpr int kSplitProgressDismDone = 52;
constexpr int kSplitProgressCopySpan = 7;
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
    // Require a real path-segment boundary: a bare startsWith lets a sibling like
    // C:/AppEvil pass containment under C:/App. canonicalFilePath uses '/'.
    if (canonical.compare(canonicalDir, Qt::CaseInsensitive) == 0) {
        return true;
    }
    const QString canonicalDirWithSep =
        canonicalDir.endsWith(QLatin1Char('/')) ? canonicalDir : canonicalDir + QLatin1Char('/');
    return canonical.startsWith(canonicalDirWithSep, Qt::CaseInsensitive);
}

bool WindowsUSBCreator::copyISO_prepareExtraction(const QString& sourcePath,
                                                  QString& sevenZipPath) {
    // sourcePath is not asserted: an empty value fails the QFile::exists check below and
    // reports through m_lastError rather than aborting a Debug build. sevenZipPath is a
    // write-only out-param (empty on entry).

    // Verify source ISO exists
    if (!QFile::exists(sourcePath)) {
        setError(QString("ISO file not found: %1").arg(sourcePath));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Get path to embedded 7z.exe
    const QString appDir = QCoreApplication::applicationDirPath();
    sevenZipPath = appDir + "/tools/chocolatey/tools/7z.exe";

    // Defense in depth: only execute the bundled 7z if it is a real regular file
    // that canonically resides under the app directory -- never a symlink/reparse
    // point that could redirect execution outside the install tree.
    if (!isSafeBundledExecutable(sevenZipPath, appDir)) {
        setError(QString("Refusing to run untrusted or missing 7z.exe at: %1").arg(sevenZipPath));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    copyISO_extractVolumeLabel(sevenZipPath, sourcePath);

    // A cancel observed during the (quick) label probe must abort BEFORE any extraction writes,
    // rather than being swallowed into a default label and proceeding to a multi-minute extract.
    if (m_cancelled.load()) {
        setError("ISO extraction cancelled by user");
        sak::logInfo(m_lastError.toStdString());
        return false;
    }

    return true;
}

bool WindowsUSBCreator::copyISOContents(const QString& sourcePath, const QString& destPath) {
    // destPath is not asserted: an empty or malformed value is rejected by
    // copyISO_normalizeDestination, reporting through m_lastError rather than
    // aborting a Debug build.
    sak::logInfo(
        QString("Extracting ISO contents: %1 -> %2").arg(sourcePath, destPath).toStdString());

    QString sevenZipPath;
    if (!copyISO_prepareExtraction(sourcePath, sevenZipPath)) {
        return false;
    }

    QString cleanDest;
    if (!copyISO_normalizeDestination(destPath, cleanDest)) {
        return false;
    }
    if (!copyISO_checkDiskSpace(cleanDest, sourcePath)) {
        return false;
    }
    if (!copyISO_planInstallImage(sevenZipPath, sourcePath)) {
        return false;
    }
    if (!copyISO_runExtraction(sevenZipPath, sourcePath, cleanDest)) {
        return false;
    }
    if (!copyISO_verifyExtracted(cleanDest)) {
        return false;
    }

    copyISO_setVolumeLabel(cleanDest);

    sak::logInfo("ISO extraction completed successfully");

    // Verify extraction integrity by comparing file list and sizes
    if (!verifyExtractionIntegrity(sourcePath, cleanDest, sevenZipPath)) {
        setError("Extraction verification failed - files do not match ISO contents");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return !m_install_split || copyISO_splitInstallImage(sevenZipPath, sourcePath, cleanDest);
}

bool WindowsUSBCreator::copyISO_verifyExtracted(const QString& cleanDest) {
    return copyISO_verifyDestination(cleanDest) && copyISO_findSetupExe(cleanDest) &&
           copyISO_verifyBootFiles(cleanDest);
}

bool WindowsUSBCreator::copyISO_planInstallImage(const QString& sevenZipPath,
                                                 const QString& sourcePath) {
    // FAT32 holds no file of 4 GiB or more. A Windows 11 install.wim is larger than that, so it
    // is left out of the extraction and split; an install.esd cannot be split and is refused.
    const auto listing = sak::runProcess(sevenZipPath,
                                         {QStringLiteral("l"), QStringLiteral("-slt"), sourcePath},
                                         sak::kTimeoutProcessVeryLongMs,
                                         [this]() { return m_cancelled.load(); });
    if (!listing.succeeded() || listing.output_truncated) {
        setError(QStringLiteral("Could not list the ISO to size its install image"));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    m_install_split = false;
    for (const auto& [path, bytes] :
         parseIsoCriticalFiles(listing.std_out.split(QLatin1Char('\n')))) {
        const QString normalized = QDir::fromNativeSeparators(path).toLower();
        if (!installImageNeedsSplit(bytes)) {
            continue;
        }
        if (normalized.endsWith(QStringLiteral("sources/install.esd"))) {
            setError(QStringLiteral("This ISO's install.esd is larger than FAT32 can hold (%1 "
                                    "bytes) and cannot be split; use an ISO with install.wim")
                         .arg(bytes));
            sak::logError(m_lastError.toStdString());
            return false;
        }
        if (normalized.endsWith(QStringLiteral("sources/install.wim"))) {
            m_install_split = true;
            sak::logInfo(QString("install.wim is %1 bytes, larger than FAT32 holds: it will be "
                                 "split into %2 MB parts")
                             .arg(bytes)
                             .arg(kInstallSplitPartMb)
                             .toStdString());
        }
    }
    return true;
}

bool WindowsUSBCreator::copyISO_splitInstallImage(const QString& sevenZipPath,
                                                  const QString& sourcePath,
                                                  const QString& cleanDest) {
    const QString dism = system32ExePath(QStringLiteral("dism.exe"));
    if (dism.isEmpty()) {
        setError(QStringLiteral("Cannot resolve the system dism.exe to split install.wim"));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    // The split needs the image as a file, so it is taken out of the ISO into a work folder on
    // this PC first; the folder holds the image and its parts at once, and goes at the end.
    QTemporaryDir work(QDir(QDir::tempPath()).filePath(QStringLiteral("sak_usb_split_XXXXXX")));
    const QStorageInfo space(QDir::tempPath());
    if (!work.isValid() ||
        space.bytesAvailable() < kExtractionWorkspaceMultiplier * m_isoSizeBytes) {
        setError(QStringLiteral("Splitting install.wim needs about %1 GB free in %2")
                     .arg(kExtractionWorkspaceMultiplier * m_isoSizeBytes / sak::kBytesPerGB)
                     .arg(QDir::toNativeSeparators(QDir::tempPath())));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    Q_EMIT progressUpdated(kSplitProgressStart);
    Q_EMIT statusChanged("Step 2/3: Taking install.wim out of the ISO to split it...");
    const auto extracted =
        sak::runProcess(sevenZipPath,
                        {QStringLiteral("e"),
                         QStringLiteral("-y"),
                         sourcePath,
                         QStringLiteral("sources\\install.wim"),
                         QStringLiteral("-o%1").arg(QDir::toNativeSeparators(work.path()))},
                        kSplitStepTimeoutMs,
                        [this]() { return m_cancelled.load(); });
    const QString wim = QDir(work.path()).filePath(QStringLiteral("install.wim"));
    if (!extracted.succeeded() || !QFileInfo::exists(wim)) {
        setError(QStringLiteral("Could not take install.wim out of the ISO to split it"));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    return splitInstallImageInto(dism, wim, QDir(cleanDest).filePath(QStringLiteral("sources")));
}

namespace {
// SHA-256 of a file, read in pieces; empty when it cannot be read.
QByteArray fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return hash.result();
}

// install.swm, install2.swm, ... in the order Setup reads them.
QStringList orderedSplitParts(const QString& folder) {
    QStringList parts = QDir(folder).entryList({QStringLiteral("install*.swm")}, QDir::Files);
    std::ranges::sort(parts, [](const QString& a, const QString& b) {
        const auto number = [](const QString& name) {
            const QString digits = name.mid(7, name.size() - 11);  // "install" .. ".swm"
            return digits.isEmpty() ? 1 : digits.toInt();
        };
        return number(a) < number(b);
    });
    return parts;
}
}  // namespace

bool WindowsUSBCreator::splitInstallImageInto(const QString& dism,
                                              const QString& wim,
                                              const QString& sources_dir) {
    m_split_parts.clear();
    const QString parts_dir = QFileInfo(wim).absolutePath() + QStringLiteral("/parts");
    if (!QDir().mkpath(parts_dir)) {
        setError(QStringLiteral("Could not make a folder for the split install image"));
        return false;
    }
    Q_EMIT statusChanged("Step 2/3: Splitting install.wim for FAT32 (DISM)...");
    const auto split = sak::runProcess(
        dism,
        splitImageArguments(wim, QDir(parts_dir).filePath(QStringLiteral("install.swm"))),
        kSplitStepTimeoutMs,
        [this]() { return m_cancelled.load(); });
    const QStringList parts = orderedSplitParts(parts_dir);
    if (!split.succeeded() || parts.isEmpty() || parts.first() != QStringLiteral("install.swm")) {
        setError(QStringLiteral("DISM could not split install.wim (exit code %1): %2")
                     .arg(split.exit_code)
                     .arg(split.std_out.trimmed().right(kSplitErrorTailChars)));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    QFile::remove(wim);  // the parts hold it now, and the work folder needs the room
    Q_EMIT progressUpdated(kSplitProgressDismDone);
    for (int i = 0; i < parts.size(); ++i) {
        Q_EMIT statusChanged(QString("Step 2/3: Copying install image part %1 of %2...")
                                 .arg(i + 1)
                                 .arg(parts.size()));
        const QString from = QDir(parts_dir).filePath(parts.at(i));
        const QString to = QDir(sources_dir).filePath(parts.at(i));
        SplitPart part{.name = parts.at(i),
                       .sha256 = fileSha256(from),
                       .bytes = QFileInfo(from).size()};
        if (part.sha256.isEmpty() || part.bytes > kFat32MaxFileBytes || !QFile::copy(from, to)) {
            setError(QStringLiteral("Could not copy %1 to the USB drive").arg(parts.at(i)));
            sak::logError(m_lastError.toStdString());
            return false;
        }
        m_split_parts.append(part);
        Q_EMIT progressUpdated(kSplitProgressDismDone +
                               static_cast<int>((i + 1) * kSplitProgressCopySpan / parts.size()));
    }
    sak::logInfo(QString("install.wim split into %1 part(s) on the USB drive")
                     .arg(m_split_parts.size())
                     .toStdString());
    return true;
}

bool WindowsUSBCreator::verifySplitParts(const QString& sources_dir) {
    if (m_split_parts.isEmpty() ||
        QFileInfo::exists(QDir(sources_dir).filePath(QStringLiteral("install.wim")))) {
        setError(
            QStringLiteral("FINAL VERIFICATION FAILED: the split install image is not on the "
                           "drive as written"));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    for (int i = 0; i < m_split_parts.size(); ++i) {
        const SplitPart& part = m_split_parts.at(i);
        Q_EMIT statusChanged(QString("Reading back install image part %1 of %2...")
                                 .arg(i + 1)
                                 .arg(m_split_parts.size()));
        const QString path = QDir(sources_dir).filePath(part.name);
        // From the stick itself: a part just copied is still in Windows' file cache, and a read
        // the cache answers proves only what was written (testpc1: 17.5 s for 2.8 GB).
        if (QFileInfo(path).size() != part.bytes ||
            sak::sha256FromMedia(path, [this]() { return m_cancelled.load(); }) != part.sha256) {
            setError(QStringLiteral("FINAL VERIFICATION FAILED: %1 on the drive does not match "
                                    "what was written")
                         .arg(part.name));
            sak::logError(m_lastError.toStdString());
            return false;
        }
        sak::logInfo(QString("  [x] %1 (%2 bytes, SHA-256 %3)")
                         .arg(part.name)
                         .arg(part.bytes)
                         .arg(QString::fromLatin1(part.sha256.toHex()))
                         .toStdString());
    }
    return true;
}

QString WindowsUSBCreator::parseVolumeLabelFromOutput(const QString& output) {
    const QStringList lines = output.split('\n');
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
    // destPath is not asserted: an empty or malformed value is rejected by the
    // single-letter check below. cleanDest is a write-only out-param (empty on entry).
    // Normalize drive letter to full path format (e.g., "E" -> "E:\")
    cleanDest = destPath.trimmed();

    // Remove any existing colons and backslashes to start fresh
    cleanDest.remove(':');
    cleanDest.remove('\\');
    cleanDest.remove('/');

    // Should now have just the drive letter
    if (cleanDest.length() != 1 || !cleanDest[0].isLetter()) {
        setError(QString("Invalid drive letter format: '%1' (expected single letter A-Z)")
                     .arg(destPath));
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
    const QStorageInfo storage(cleanDest);
    if (!storage.isValid() || !storage.isReady()) {
        setError(QString("Cannot access destination drive %1").arg(cleanDest));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    const qint64 availableSpace = storage.bytesAvailable();
    const QFileInfo isoInfo(sourcePath);
    const qint64 isoSize = isoInfo.size();

    // Require at least 2x ISO size for extraction (compressed files expand)
    const qint64 requiredSpace = isoSize * kExtractionWorkspaceMultiplier;

    if (availableSpace < requiredSpace) {
        setError(QString("Insufficient disk space: need %1 GB, have %2 GB")
                     .arg(static_cast<double>(requiredSpace) / sak::kBytesPerGBf,
                          0,
                          'f',
                          kCapacityDisplayPrecision)
                     .arg(static_cast<double>(availableSpace) / sak::kBytesPerGBf,
                          0,
                          'f',
                          kCapacityDisplayPrecision));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("Disk space check: %1 GB available, %2 GB required")
                     .arg(static_cast<double>(availableSpace) / sak::kBytesPerGBf,
                          0,
                          'f',
                          kCapacityDisplayPrecision)
                     .arg(static_cast<double>(requiredSpace) / sak::kBytesPerGBf,
                          0,
                          'f',
                          kCapacityDisplayPrecision)
                     .toStdString());
    return true;
}

bool WindowsUSBCreator::copyISO_runExtraction(const QString& sevenZipPath,
                                              const QString& sourcePath,
                                              const QString& cleanDest) {
    sak::logInfo(
        QString("Using 7z.exe to extract ISO directly to %1").arg(cleanDest).toStdString());
    Q_EMIT statusChanged("Extracting Windows installation files...");

    const QStringList args = extractionArguments(sourcePath, cleanDest, m_install_split);

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
             [this, &started](qint64) {
                 started = true;
                 sak::logInfo(
                     "7z process started, extracting ISO (this may take several minutes)...");
                 Q_EMIT statusChanged("Extracting Windows files...");
             },
         .should_cancel = [this]() { return m_cancelled.load(); }});

    if (!started) {
        QString err = QString("Failed to start 7z.exe at: %1").arg(sevenZipPath);
        if (!result.std_err.isEmpty()) {
            err += QStringLiteral(": ") + result.std_err.trimmed();
        }
        setError(err);
        sak::logError(err.toStdString());
        return false;
    }
    if (result.cancelled) {
        setError("Extraction cancelled by user");
        sak::logInfo(m_lastError.toStdString());
        return false;
    }
    if (result.timed_out) {
        setError("ISO extraction timed out after 15 minutes");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return copyISO_logExtractionResult(result);
}

QStringList WindowsUSBCreator::extractionArguments(const QString& iso,
                                                   const QString& dest,
                                                   bool install_split) {
    // 7z x = extract with full paths, -aoa = overwrite all. -bsp1 sends the progress line to
    // stdout, which the build reads; -bsp2, used before, sends it to stderr, so on testpc1 the bar
    // sat at 13% for the whole extraction and the MB count never showed.
    QStringList args{QStringLiteral("x"),
                     QStringLiteral("-aoa"),
                     QStringLiteral("-bsp1"),
                     QStringLiteral("-y"),
                     iso,
                     QStringLiteral("-o%1").arg(dest)};
    if (install_split) {
        args << QStringLiteral("-x!sources\\install.wim");  // too large for FAT32: split later
    }
    return args;
}

int WindowsUSBCreator::extractionPercentIn(const QString& chunk) {
    // 7-Zip rewrites its progress line in place: " 20% - sources\boot.wim" then CR, spaces, CR,
    // " 46% - ...". A chunk can hold several; the last is the newest.
    static const QRegularExpression kPercent(QStringLiteral(R"((?:^|\s)(\d{1,3})%)"));
    int percent = -1;
    for (auto it = kPercent.globalMatch(chunk); it.hasNext();) {
        percent = it.next().captured(1).toInt();
    }
    return percent <= sak::kPercentMax ? percent : -1;
}

void WindowsUSBCreator::copyISO_parseExtractionProgress(const QString& output,
                                                        qint64& totalBytes,
                                                        qint64& processedBytes,
                                                        int& lastProgressPercent) {
    // 7z's progress: a percentage ("12%"), or "bytes_processed + bytes_total" in some builds.
    const QRegularExpression bytesRegex(R"((\d+)\s*\+\s*(\d+))");
    const QRegularExpressionMatch bytesMatch = bytesRegex.match(output);

    if (!bytesMatch.hasMatch()) {
        const int extractPercent = extractionPercentIn(output);
        if (extractPercent < 0) {
            return;
        }

        const int totalProgress = kExtractProgressStart +
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
    const qint64 newTotal = bytesMatch.captured(kSevenZipTotalBytesCaptureGroup).toLongLong();
    totalBytes = std::max(newTotal, totalBytes);
    if (totalBytes <= 0) {
        return;
    }

    const int extractPercent = static_cast<int>((processedBytes * sak::kPercentMax) / totalBytes);
    const int totalProgress = kExtractProgressStart +
                              (extractPercent * kExtractProgressSpan / sak::kPercentMax);
    if (totalProgress <= lastProgressPercent) {
        return;
    }

    lastProgressPercent = totalProgress;
    Q_EMIT progressUpdated(totalProgress);

    const double processedMB = static_cast<double>(processedBytes) / sak::kBytesPerMBf;
    const double totalMB = static_cast<double>(totalBytes) / sak::kBytesPerMBf;

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
    const int exitCode = result.exit_code;
    const QString output = result.std_out;
    const QString errors = result.std_err;

    sak::logInfo(QString("7z extraction completed with exit code: %1").arg(exitCode).toStdString());

    if (!output.isEmpty()) {
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        sak::logInfo(QString("7z processed %1 lines of output").arg(lines.count()).toStdString());
        // Log last few lines which contain summary
        for (int i = static_cast<int>(qMax(0, lines.count() - kExtractionSummaryLineCount));
             i < lines.count();
             ++i) {
            sak::logInfo(QString("  %1").arg(lines[i].trimmed()).toStdString());
        }
    }

    if (!errors.isEmpty()) {
        sak::logError(QString("7z stderr: %1").arg(errors).toStdString());
    }

    // 7z exit codes: 0 = success, non-zero = error
    if (exitCode != 0) {
        setError(QString("7z extraction failed with exit code %1").arg(exitCode));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    return true;
}

bool WindowsUSBCreator::copyISO_verifyDestination(const QString& cleanDest) {
    // copyISOContents only calls this with the "X:\" path copyISO_normalizeDestination built.
    Q_ASSERT(!cleanDest.isEmpty());
    // No settle delay: 7z has exited, so every file it wrote is closed and visible through the
    // file system. A fixed sleep here proved nothing and only lengthened every run.

    // Verify critical Windows files were extracted to the destination
    sak::logInfo(QString("Verifying critical files exist at: %1").arg(cleanDest).toStdString());

    const QDir checkDest(cleanDest);

    // Verify directory exists before listing
    if (!checkDest.exists()) {
        setError(QString("Destination directory does not exist: %1").arg(cleanDest));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    const QStringList destFiles = checkDest.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    sak::logInfo(QString("Destination now contains %1 items").arg(destFiles.count()).toStdString());

    if (destFiles.isEmpty()) {
        setError("Extraction completed but destination directory is empty");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Show ALL files/folders extracted with their types
    sak::logInfo("Complete listing of extracted items:");
    for (const QString& item : destFiles) {
        const QFileInfo info(checkDest.absoluteFilePath(item));
        const QString type = info.isDir() ? "DIR" : QString("FILE (%1 bytes)").arg(info.size());
        sak::logInfo(QString("  %1 - %2").arg(item, type).toStdString());
    }

    return true;
}

bool WindowsUSBCreator::copyISO_findSetupExe(const QString& cleanDest) {
    // copyISOContents only calls this with the "X:\" path copyISO_normalizeDestination built.
    Q_ASSERT(!cleanDest.isEmpty());
    const QDir checkDest(cleanDest);
    QString setupPath = checkDest.absoluteFilePath("setup.exe");

    // Verify setup.exe exists (absolute requirement for Windows boot)
    sak::logInfo(QString("Checking for setup.exe at: %1").arg(setupPath).toStdString());

    if (QFile::exists(setupPath)) {
        sak::logInfo(QString("\xe2\x9c\x93 setup.exe found at: %1").arg(setupPath).toStdString());
        return true;
    }

    // Try case-insensitive search in root directory
    sak::logWarning("setup.exe not found with exact case, searching case-insensitively...");
    const QStringList rootFiles = checkDest.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString& file : rootFiles) {
        if (file.toLower() != "setup.exe") {
            continue;
        }
        setupPath = cleanDest + file;
        sak::logInfo(QString("Found setup file with different case: %1").arg(file).toStdString());
        sak::logInfo(QString("\xe2\x9c\x93 setup.exe found at: %1").arg(setupPath).toStdString());
        return true;
    }

    setError("CRITICAL: setup.exe not found after extraction");
    sak::logError(m_lastError.toStdString());
    sak::logError(QString("Checked path: %1").arg(setupPath).toStdString());
    if (!rootFiles.isEmpty()) {
        sak::logError(QString("Files in root: %1").arg(rootFiles.join(", ")).toStdString());
    }
    sak::logError("ISO extraction may have failed or ISO may be corrupt");
    return false;
}

bool WindowsUSBCreator::copyISO_verifyBootFiles(const QString& cleanDest) {
    // copyISOContents only calls this with the "X:\" path copyISO_normalizeDestination built.
    Q_ASSERT(!cleanDest.isEmpty());
    const QString problem = extractedBootFilesProblem(cleanDest, m_install_split);
    if (!problem.isEmpty()) {
        setError(problem);
        sak::logError(m_lastError.toStdString());
        sak::logError("Windows installation files incomplete - USB will not boot or install");
        return false;
    }
    sak::logInfo("\xe2\x9c\x93 All critical files verified");
    return true;
}

QString WindowsUSBCreator::extractedBootFilesProblem(const QString& cleanDest, bool install_split) {
    // Verify other critical Windows boot files - REQUIRED for bootable USB
    for (const QString& file : {QStringLiteral("sources/boot.wim"), QStringLiteral("bootmgr")}) {
        if (!QFile::exists(cleanDest + file)) {
            return QString("CRITICAL: Required file not found: %1").arg(file);
        }
        sak::logInfo(QString("\xe2\x9c\x93 Found: %1").arg(file).toStdString());
    }
    // A split install.wim was left out of the extraction on purpose: its parts are copied after
    // this check and read back by hash at the final verification. testpc1: requiring the image
    // here failed every FAT32 build of a Windows 11 ISO as soon as its extraction ended.
    if (install_split) {
        sak::logInfo("install.wim is split after the extraction; its parts are checked at the end");
        return {};
    }
    // At least one of these must exist (different Windows versions have different structures)
    for (const QString& file :
         {QStringLiteral("sources/install.wim"), QStringLiteral("sources/install.esd")}) {
        if (QFile::exists(cleanDest + file)) {
            sak::logInfo(QString("\xe2\x9c\x93 Found install image: %1").arg(file).toStdString());
            return {};
        }
    }
    return QStringLiteral(
        "CRITICAL: No Windows install image found (install.wim or install.esd required)");
}

void WindowsUSBCreator::copyISO_setVolumeLabel(const QString& cleanDest) {
    // No asserts: an empty label returns below, and an empty cleanDest is caught by the
    // drive-letter check further down.
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
    // FAT32 keeps 11 characters of a label.
    QString safeLabel = m_volumeLabel.left(kFat32LabelMaxChars);
    safeLabel.replace(QChar('\''), QStringLiteral("''"));
    const QString labelCmd =
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
        const QString labelErrors = label_result.std_err.trimmed();
        sak::logWarning(QString("Failed to set volume label: %1").arg(labelErrors).toStdString());
    }
}

// There is no boot-file step. bcdboot needs a Windows directory as its source, and install
// media has none, so running it against the media root could only fail or write the wrong
// thing (Codex review 3, finding 5). The extracted ISO already carries the boot files it was
// built with (bootmgr, boot\BCD, efi\), which is what makes it bootable; the partition was
// made active when it was created. The volume is FAT32, which UEFI firmware reads, so the same
// files start a PC in UEFI and in legacy BIOS mode (Codex review 3, finding 4).

bool WindowsUSBCreator::checkPartitionActive(const QString& diskNumber) {
    // Integer-validate before interpolating into the diskpart script (same class of
    // value as createBootableUSB validates), so no stray tokens can be injected.
    static const QRegularExpression diskNumRe(QStringLiteral("^\\d{1,3}$"));
    if (!diskNumRe.match(diskNumber).hasMatch()) {
        setError(QString("Invalid disk number for partition check: '%1'").arg(diskNumber));
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
    const bool isActive = activeYesRe.match(output).hasMatch();

    if (isActive) {
        sak::logInfo("[x] Bootable flag verified - partition is active");
        return true;
    }

    setError("VERIFICATION FAILED: Partition is not marked as active/bootable");
    sak::logError(m_lastError.toStdString());
    sak::logError("USB drive will NOT be bootable - bootable flag must be set");
    return false;
}

bool WindowsUSBCreator::verifyBootableFlag(const QString& driveLetter) {
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
        setError(QString("Invalid drive letter format for verification: '%1'").arg(driveLetter));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    cleanDrive = cleanDrive.toUpper();

    const QString diskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDrive);
    const auto disk_result =
        sak::runPowerShell(diskCmd, sak::kTimeoutProcessMediumMs, true, false, [this]() {
            return m_cancelled.load();
        });

    if (!disk_result.succeeded()) {
        setError("VERIFICATION FAILED: Could not query partition disk number");
        sak::logError(m_lastError.toStdString());
        return false;  // fail closed: an unverified boot flag is not a pass
    }

    const QString diskNumber = disk_result.std_out.trimmed();
    if (diskNumber.isEmpty()) {
        setError("VERIFICATION FAILED: Could not determine disk number for boot-flag check");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Identity pin: the drive letter must still map back to the ORIGINAL target
    // disk. A hot-plug that reassigned the letter to a different disk would
    // otherwise let us "verify" the wrong disk. Compare as integers so "1"/"01"
    // never causes a spurious mismatch, but require BOTH sides to parse: a bare
    // toInt() defaults to 0 on non-numeric input, so two unparseable values would
    // spuriously "match". Fail closed on any divergence or unparseable number.
    bool currentOk = false;
    bool targetOk = false;
    const int currentDisk = diskNumber.toInt(&currentOk);
    const int targetDisk = m_diskNumber.toInt(&targetOk);
    if (!currentOk || !targetOk || currentDisk != targetDisk) {
        setError(QString("VERIFICATION FAILED: drive %1 now maps to disk %2, not target disk %3")
                     .arg(cleanDrive, diskNumber, m_diskNumber));
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
        setError("ISO changed during processing (size/timestamp mismatch) -- aborting");
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
        setError("Verification failed: Could not list ISO contents");
        sak::logError(m_lastError.toStdString());
        return false;
    }
    // A truncated listing means some ISO members are not represented, so the critical-file
    // cross-check below could silently skip a mandatory file. Fail closed rather than certify
    // integrity from a partial listing.
    if (list_result.output_truncated) {
        setError("Verification failed: ISO listing was truncated -- cannot confirm critical files");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    const QString isoListing = list_result.std_out;
    const QStringList lines = isoListing.split("\n");

    auto criticalFiles = parseIsoCriticalFiles(lines);
    if (m_install_split) {
        // Not on the drive whole: its parts are checked by hash at the final verification.
        criticalFiles.removeIf([](const QPair<QString, qint64>& file) {
            return QDir::fromNativeSeparators(file.first)
                .endsWith(QStringLiteral("sources/install.wim"), Qt::CaseInsensitive);
        });
    }

    if (criticalFiles.isEmpty()) {
        setError("Verification failed: No critical Windows files found in ISO");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(
        QString("Verifying %1 critical files...").arg(criticalFiles.count()).toStdString());

    return verifyCriticalFilesOnDisk(criticalFiles, destPath);
}

bool WindowsUSBCreator::isCriticalWindowsFile(const QString& path) {
    // The backslash arms of this test used to read "sources\\\\boot.wim". That is four
    // backslash characters in the source, so the compiler produced a runtime string
    // containing TWO backslashes, and a real Windows path has one. 7z reports ISO members
    // with backslashes, so boot.wim, install.wim and install.esd were never recognised and
    // silently dropped out of the integrity check while it still reported success over the
    // files it did match.
    //
    // Normalising the separator once removes the whole class: there is now a single
    // forward-slash form of each name to keep correct.
    const QString normalized = QDir::fromNativeSeparators(path).toLower();
    return normalized.contains(QStringLiteral("setup.exe")) ||
           normalized.contains(QStringLiteral("bootmgr")) ||
           normalized.contains(QStringLiteral("sources/boot.wim")) ||
           normalized.contains(QStringLiteral("sources/install.wim")) ||
           normalized.contains(QStringLiteral("sources/install.esd"));
}

QList<QPair<QString, qint64>> WindowsUSBCreator::parseIsoCriticalFiles(const QStringList& lines) {
    // verifyExtractionIntegrity passes QString::split output, which always yields at
    // least one element even for empty input.
    Q_ASSERT(!lines.isEmpty());
    QList<QPair<QString, qint64>> criticalFiles;
    QString currentPath;
    qint64 currentSize = 0;
    bool isFolder = false;

    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
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
    // relativePath came from the UNTRUSTED ISO listing. Reject traversal, absolute,
    // UNC, drive-qualified, and alternate-data-stream forms so a crafted listing can
    // never make verification stat a file outside the extraction root. Fail closed.
    if (relativePath.contains(QStringLiteral("..")) || relativePath.contains(QLatin1Char(':')) ||
        relativePath.startsWith(QLatin1Char('\\'))) {
        sak::logError(QString("[ ]-- Unsafe critical file path rejected: %1")
                          .arg(fileInfo.first)
                          .toStdString());
        return false;
    }
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
        setError(QString("Verification failed: Only %1 critical files verified (minimum %2 "
                         "required)")
                     .arg(verifiedCount)
                     .arg(kMinimumVerifiedCriticalFiles));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (failedCount > 0) {
        setError(QString("Extraction verification failed: %1 files missing or incorrect size")
                     .arg(failedCount));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo("[x] Extraction integrity verified - all critical files match ISO");
    return true;
}

bool WindowsUSBCreator::verifyBootAndInstallFiles(const QString& cleanDrive) {
    // finalVerification, the only caller, rejects anything that is not a single drive
    // letter and passes the normalized "X:\" form it builds from it.
    Q_ASSERT(!cleanDrive.isEmpty());
    // Verification 0: the volume UEFI firmware reads. NTFS media does not start a UEFI PC.
    const QString file_system = QString::fromUtf8(QStorageInfo(cleanDrive).fileSystemType());
    if (file_system.compare(QLatin1String("FAT32"), Qt::CaseInsensitive) != 0) {
        setError(QString("FINAL VERIFICATION FAILED: the drive is %1, not FAT32, so a UEFI PC "
                         "would not start from it")
                     .arg(file_system));
        sak::logError(m_lastError.toStdString());
        return false;
    }
    // Verification 1: Check that critical boot files exist, the UEFI loader among them
    const QStringList requiredFiles = {
        "setup.exe", "sources/boot.wim", "bootmgr", "efi/boot/bootx64.efi"};

    sak::logInfo("Checking required files:");
    for (const QString& file : requiredFiles) {
        const QString fullPath = cleanDrive + file;
        if (!QFile::exists(fullPath)) {
            setError(QString("FINAL VERIFICATION FAILED: Critical file missing: %1").arg(file));
            sak::logError(m_lastError.toStdString());
            return false;
        }
        const QFileInfo info(fullPath);
        sak::logInfo(QString("  [x] %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
    }

    // Verification 2: Check for install image
    Q_EMIT statusChanged("Verifying Windows install image...");
    if (m_install_split) {
        return verifySplitParts(cleanDrive + "sources");
    }
    const bool hasInstallWim = QFile::exists(cleanDrive + "sources/install.wim");
    const bool hasInstallEsd = QFile::exists(cleanDrive + "sources/install.esd");

    if (!hasInstallWim && !hasInstallEsd) {
        setError("FINAL VERIFICATION FAILED: No Windows install image found");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (hasInstallWim) {
        const QFileInfo info(cleanDrive + "sources/install.wim");
        sak::logInfo(QString("  [x] install.wim (%1 bytes)").arg(info.size()).toStdString());
    }
    if (hasInstallEsd) {
        const QFileInfo info(cleanDrive + "sources/install.esd");
        sak::logInfo(QString("  [x] install.esd (%1 bytes)").arg(info.size()).toStdString());
    }

    return true;
}

QString WindowsUSBCreator::finalVerificationStatus() {
    return QStringLiteral(
        "[x] USB files verified: FAT32 with the UEFI boot file, and the install image read back.");
}

bool WindowsUSBCreator::listingHasInstallImage(const QString& listing) {
    // Directly in the top-level sources folder, whatever the case: a Windows install image has
    // this one place, and an install.wim deeper in some other tool's tree says nothing.
    static const QRegularExpression kInstallImage(
        QStringLiteral(R"((^|\s)sources[\\/]install\.(wim|esd|swm)\s*$)"),
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption);
    return kInstallImage.match(listing).hasMatch();
}

#ifdef SAK_CORE_TEST_HOOKS
namespace {
std::optional<WindowsUSBCreator::InstallImageCheck> g_install_image_check_for_testing;
}  // namespace

void WindowsUSBCreator::setInstallImageCheckForTesting(std::optional<InstallImageCheck> answer) {
    g_install_image_check_for_testing = answer;
}
#endif  // SAK_CORE_TEST_HOOKS

WindowsUSBCreator::InstallImageCheck WindowsUSBCreator::checkInstallImage(const QString& isoPath) {
#ifdef SAK_CORE_TEST_HOOKS
    if (g_install_image_check_for_testing.has_value()) {
        return *g_install_image_check_for_testing;
    }
#endif  // SAK_CORE_TEST_HOOKS
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString sevenZipPath = appDir + "/tools/chocolatey/tools/7z.exe";
    if (!isSafeBundledExecutable(sevenZipPath, appDir)) {
        sak::logError(QString("Cannot look inside %1: 7z.exe is missing or untrusted at %2")
                          .arg(isoPath, sevenZipPath)
                          .toStdString());
        return InstallImageCheck::Failed;
    }
    return checkInstallImageWith(isoPath, sevenZipPath);
}

WindowsUSBCreator::InstallImageCheck WindowsUSBCreator::checkInstallImageWith(
    const QString& isoPath, const QString& sevenZipPath) {
    const sak::ProcessResult result = sak::runProcess(sevenZipPath,
                                                      {QStringLiteral("l"),
                                                       QStringLiteral("-ba"),
                                                       QStringLiteral("-sccUTF-8"),
                                                       QStringLiteral("--"),
                                                       isoPath,
                                                       QStringLiteral("sources\\install.*")},
                                                      sak::kTimeoutProcessLongMs);
    // exit_code stays negative when the process never started. A real 7z exit is never negative.
    if (result.timed_out || result.exit_status != 0 || result.output_truncated ||
        result.exit_code < 0) {
        sak::logError(QString("Could not list %1 with 7z (exit %2)")
                          .arg(isoPath)
                          .arg(result.exit_code)
                          .toStdString());
        return InstallImageCheck::Failed;
    }
    // A raw disk image 7z cannot open exits non-zero with no listing, and that is a real answer:
    // there is no install image in it that the Windows writer could use.
    return listingHasInstallImage(result.std_out) ? InstallImageCheck::Found
                                                  : InstallImageCheck::NotFound;
}

void WindowsUSBCreator::logFinalVerificationSuccess(int fileCount) {
    // finalVerification counts up from 0 with a QDirIterator and returns early below the
    // 10-item minimum, so the count reaching here is always positive.
    Q_ASSERT(fileCount >= 0);
    sak::logInfo("========================================");
    sak::logInfo("SUCCESS: ALL FINAL VERIFICATIONS PASSED");
    sak::logInfo("- Critical files: VERIFIED");
    sak::logInfo("- Install image: VERIFIED");
    sak::logInfo("- Bootable flag: VERIFIED (Active)");
    sak::logInfo(QString("- File count: VERIFIED (%1 items)").arg(fileCount).toStdString());
    sak::logInfo("========================================");

    Q_EMIT progressUpdated(sak::kPercentMax);
    Q_EMIT statusChanged(finalVerificationStatus());

    // THIS IS THE ONLY PLACE completed() IS EMITTED
    Q_EMIT completed();
}

bool WindowsUSBCreator::normalizeVerificationDrive(const QString& driveLetter,
                                                   QString& cleanDrive) {
    // Normalize drive letter to standard path format
    cleanDrive = driveLetter.trimmed();

    // Remove any existing path separators
    cleanDrive.remove(":");
    cleanDrive.remove("\\");
    cleanDrive.remove("/");

    // Validate we have exactly one letter
    if (cleanDrive.length() != 1 || !cleanDrive[0].isLetter()) {
        setError(QString("FINAL VERIFICATION FAILED: Invalid drive letter format: '%1'")
                     .arg(driveLetter));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Build standard path: "E" -> "E:\\"
    cleanDrive = cleanDrive.toUpper() + ":\\";
    return true;
}

bool WindowsUSBCreator::finalVerification(const QString& driveLetter) {
    sak::logInfo("========================================");
    sak::logInfo("FINAL VERIFICATION - This is the ONLY path to success");
    sak::logInfo("========================================");

    QString cleanDrive;
    if (!normalizeVerificationDrive(driveLetter, cleanDrive)) {
        return false;
    }

    sak::logInfo(QString("Final verification path: %1").arg(cleanDrive).toStdString());

    Q_EMIT statusChanged("Verifying all critical files...");

    if (!verifyBootAndInstallFiles(cleanDrive)) {
        return false;
    }

    Q_EMIT progressUpdated(kCriticalFilesVerifiedProgress);

    // Verification 3: MANDATORY bootable flag check
    Q_EMIT statusChanged("Verifying bootable flag...");
    if (!verifyBootableFlag(driveLetter)) {
        setError("FINAL VERIFICATION FAILED: " + lastError());
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
        setError(QString("FINAL VERIFICATION FAILED: Only %1 files found (expected hundreds)")
                     .arg(fileCount));
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("  \xe2\x9c\x93 Total files: %1").arg(fileCount).toStdString());

    Q_EMIT progressUpdated(kFileCountVerifiedProgress);

    // Honor a cancel that arrived during the final enumeration: completed() is the sole success
    // signal, so it must not fire once cancellation has been requested.
    if (m_cancelled.load()) {
        setError("FINAL VERIFICATION FAILED: cancelled before completion");
        sak::logError(m_lastError.toStdString());
        return false;
    }

    logFinalVerificationSuccess(fileCount);

    return true;
}
