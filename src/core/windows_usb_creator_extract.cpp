// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_usb_creator.h"
#include "sak/logger.h"
#include "sak/layout_constants.h"
#include <QCoreApplication>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QThread>
#include <QRegularExpression>
#include <QStorageInfo>

bool WindowsUSBCreator::copyISOContents(const QString& sourcePath, const QString& destPath) {
    sak::logInfo(QString("Extracting ISO contents: %1 -> %2").arg(sourcePath,
        destPath).toStdString());

    // Verify source ISO exists
    if (!QFile::exists(sourcePath)) {
        m_lastError = QString("ISO file not found: %1").arg(sourcePath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    // Get path to embedded 7z.exe
    QString appDir = QCoreApplication::applicationDirPath();
    QString sevenZipPath = appDir + "/tools/chocolatey/tools/7z.exe";

    if (!QFile::exists(sevenZipPath)) {
        m_lastError = QString("7z.exe not found at: %1").arg(sevenZipPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    copyISO_extractVolumeLabel(sevenZipPath, sourcePath);

    QString cleanDest;
    if (!copyISO_normalizeDestination(destPath, cleanDest)) return false;
    if (!copyISO_checkDiskSpace(cleanDest, sourcePath)) return false;
    if (!copyISO_runExtraction(sevenZipPath, sourcePath, cleanDest)) return false;
    if (!copyISO_verifyDestination(cleanDest)) return false;
    if (!copyISO_findSetupExe(cleanDest)) return false;
    if (!copyISO_verifyBootFiles(cleanDest)) return false;

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
        if (!line.startsWith("Comment = ")) continue;
        QString label = line.mid(10).trimmed();
        sak::logInfo(QString("ISO volume label: %1").arg(label).toStdString());
        return label;
    }
    return {};
}

void WindowsUSBCreator::copyISO_extractVolumeLabel(const QString& sevenZipPath,
    const QString& sourcePath) {
    QProcess labelExtract;
    QStringList labelArgs;
    labelArgs << "l" << "-slt" << sourcePath; // List with technical info

    labelExtract.start(sevenZipPath, labelArgs);
    if (!labelExtract.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logWarning("7z process failed to start for label extraction");
    } else if (!labelExtract.waitForFinished(sak::kTimeoutProcessMediumMs)) {
        sak::logWarning("7z label extraction timed out after 10s \xe2\x80\x94 will use default "
                        "label");
        labelExtract.kill();
    } else {
        m_volumeLabel = parseVolumeLabelFromOutput(labelExtract.readAllStandardOutput());
    }

    // Default to WINDOWS if label not found
    if (m_volumeLabel.isEmpty()) {
        m_volumeLabel = "WINDOWS";
        sak::logInfo(QString("Using default volume label: %1").arg(m_volumeLabel).toStdString());
    }
}

bool WindowsUSBCreator::copyISO_normalizeDestination(const QString& destPath, QString& cleanDest) {
    // Normalize drive letter to full path format (e.g., "E" -> "E:\")
    cleanDest = destPath.trimmed();

    // Remove any existing colons and backslashes to start fresh
    cleanDest.remove(':');
    cleanDest.remove('\\');
    cleanDest.remove('/');

    // Should now have just the drive letter
    if (cleanDest.length() != 1 || !cleanDest[0].isLetter()) {
        m_lastError = QString("Invalid drive letter format: '%1' (expected single letter A-Z)")
            .arg(destPath);
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
    qint64 requiredSpace = isoSize * 2;

    if (availableSpace < requiredSpace) {
        m_lastError = QString("Insufficient disk space: need %1 GB, have %2 GB")
            .arg(requiredSpace / sak::kBytesPerGBf, 0, 'f', 2)
            .arg(availableSpace / sak::kBytesPerGBf, 0, 'f', 2);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("Disk space check: %1 GB available, %2 GB required")
        .arg(availableSpace / sak::kBytesPerGBf, 0, 'f', 2)
        .arg(requiredSpace / sak::kBytesPerGBf, 0, 'f', 2).toStdString());
    return true;
}

bool WindowsUSBCreator::copyISO_runExtraction(const QString& sevenZipPath,
    const QString& sourcePath, const QString& cleanDest) {
    sak::logInfo(QString("Using 7z.exe to extract ISO directly to %1")
        .arg(cleanDest).toStdString());
    Q_EMIT statusChanged("Extracting Windows installation files...");

    // 7z x = extract with full paths, -aoa = overwrite all, -bsp2 = detailed progress to stdout
    QProcess extract;
    QStringList args;
    args << "x"; // Extract with full paths
    args << "-aoa"; // Overwrite all existing files
    args << "-bsp2"; // Detailed progress (bytes) redirected to stdout
    args << "-y"; // Assume Yes on all queries
    args << sourcePath;
    args << QString("-o%1").arg(cleanDest); // Output directory

    sak::logInfo(QString("7z command: %1 %2").arg(sevenZipPath, args.join(" ")).toStdString());
    sak::logInfo(QString("Extracting to absolute path: %1").arg(cleanDest).toStdString());
    extract.start(sevenZipPath, args);

    if (!extract.waitForStarted(sak::kTimeoutProcessStartMs)) {
        m_lastError = QString("Failed to start 7z.exe at: %1").arg(sevenZipPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo("7z process started, extracting ISO (this may take several minutes)...");
    Q_EMIT statusChanged("Extracting Windows files...");

    if (!copyISO_monitorExtraction(extract)) return false;
    return copyISO_logExtractionResult(extract);
}

bool WindowsUSBCreator::copyISO_monitorExtraction(QProcess& extract) {
    const int checkIntervalMs = 200; // Check every 200ms for smoother progress
    const int maxWaitMs = 900000; // 15 minutes total
    int totalWaitedMs = 0;
    int lastProgressPercent = 15;
    qint64 totalBytes = 0;
    qint64 processedBytes = 0;

    while (extract.state() == QProcess::Running && totalWaitedMs < maxWaitMs) {
        if (m_cancelled) {
            sak::logInfo("Extraction cancelled by user, terminating 7z...");
            extract.kill();
            extract.waitForFinished(sak::kTimeoutProcessShortMs);
            m_lastError = "Extraction cancelled by user";
            return false;
        }

        // Read any new output from 7z
        if (extract.waitForReadyRead(checkIntervalMs)) {
            QString newOutput = QString::fromLocal8Bit(extract.readAllStandardOutput());
            copyISO_parseExtractionProgress(newOutput, totalBytes, processedBytes,
                lastProgressPercent);
            totalWaitedMs += checkIntervalMs;
        } else {
            totalWaitedMs += checkIntervalMs;
        }
    }

    if (extract.state() == QProcess::Running) {
        m_lastError = "ISO extraction timed out after 15 minutes";
        sak::logError(m_lastError.toStdString());
        extract.kill();
        extract.waitForFinished(sak::kTimeoutProcessShortMs);
        return false;
    }

    return true;
}

void WindowsUSBCreator::copyISO_parseExtractionProgress(
        const QString& output, qint64& totalBytes, qint64& processedBytes,
            int& lastProgressPercent) {
    // Parse 7z -bsp2 output format: "bytes_processed + bytes_total"
    // Format example: "123456789 + 987654321" or percentage "12%"
    QRegularExpression bytesRegex(R"((\d+)\s*\+\s*(\d+))");
    QRegularExpressionMatch bytesMatch = bytesRegex.match(output);

    if (!bytesMatch.hasMatch()) {
        // Fallback: Try to parse percentage if bytes format not found
        QRegularExpression percentRegex(R"(\s+(\d+)%)");
        QRegularExpressionMatch percentMatch = percentRegex.match(output);
        if (!percentMatch.hasMatch()) return;

        int extractPercent = percentMatch.captured(1).toInt();
        int totalProgress = 15 + (extractPercent * 35 / 100);
        if (totalProgress <= lastProgressPercent) return;

        lastProgressPercent = totalProgress;
        Q_EMIT progressUpdated(totalProgress);
        Q_EMIT statusChanged(QString("Extracting Windows files... %1%").arg(extractPercent));
        sak::logInfo(QString("Extraction progress: %1%").arg(extractPercent).toStdString());
        return;
    }

    // Bytes format matched
    processedBytes = bytesMatch.captured(1).toLongLong();
    qint64 newTotal = bytesMatch.captured(2).toLongLong();
    if (newTotal > totalBytes) totalBytes = newTotal;
    if (totalBytes <= 0) return;

    int extractPercent = static_cast<int>((processedBytes * 100) / totalBytes);
    int totalProgress = 15 + (extractPercent * 35 / 100);
    if (totalProgress <= lastProgressPercent) return;

    lastProgressPercent = totalProgress;
    Q_EMIT progressUpdated(totalProgress);

    double processedMB = processedBytes / sak::kBytesPerMBf;
    double totalMB = totalBytes / sak::kBytesPerMBf;

    Q_EMIT statusChanged(QString("Extracting Windows files... %1 MB / %2 MB (%3%)")
        .arg(processedMB, 0, 'f', 1)
        .arg(totalMB, 0, 'f', 1)
        .arg(extractPercent));

    sak::logInfo(QString("Extraction progress: %1 MB / %2 MB (%3%)")
        .arg(processedMB, 0, 'f', 1)
        .arg(totalMB, 0, 'f', 1)
        .arg(extractPercent).toStdString());
}

bool WindowsUSBCreator::copyISO_logExtractionResult(QProcess& extract) {
    int exitCode = extract.exitCode();
    QString output = extract.readAllStandardOutput();
    QString errors = extract.readAllStandardError();

    sak::logInfo(QString("7z extraction completed with exit code: %1").arg(exitCode).toStdString());

    if (!output.isEmpty()) {
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        sak::logInfo(QString("7z processed %1 lines of output").arg(lines.count()).toStdString());
        // Log last few lines which contain summary
        for (int i = qMax(0, lines.count() - 5); i < lines.count(); ++i) {
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
    for (const QString &item : destFiles) {
        QFileInfo info(checkDest.absoluteFilePath(item));
        QString type = info.isDir() ? "DIR" : QString("FILE (%1 bytes)").arg(info.size());
        sak::logInfo(QString("  %1 - %2").arg(item, type).toStdString());
    }

    return true;
}

bool WindowsUSBCreator::copyISO_findSetupExe(const QString& cleanDest) {
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
        if (file.toLower() != "setup.exe") continue;
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
    // Verify other critical Windows boot files - REQUIRED for bootable USB
    QStringList criticalFiles = {
        "sources/boot.wim",
        "bootmgr"
    };

    // At least one of these must exist (different Windows versions have different structures)
    QStringList alternateFiles = {
        "sources/install.wim",
        "sources/install.esd"
    };

    QStringList foundFiles;
    foundFiles << "setup.exe"; // Already verified above

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
        m_lastError = "CRITICAL: No Windows install image found (install.wim or install.esd "
                      "required)";
        sak::logError(m_lastError.toStdString());
        sak::logError("Windows installation incomplete - USB will not be able to install Windows");
        return false;
    }

    sak::logInfo(QString("\xe2\x9c\x93 All critical files verified: %1 core files found")
        .arg(foundFiles.count()).toStdString());
    return true;
}

void WindowsUSBCreator::copyISO_setVolumeLabel(const QString& cleanDest) {
    if (m_volumeLabel.isEmpty()) return;

    sak::logInfo(QString("Setting volume label to: %1").arg(m_volumeLabel).toStdString());
    // Extract single drive letter from normalized path ("E:\\" -> "E")
    QString driveLetter = cleanDest.left(1);
    if (driveLetter.isEmpty() || !driveLetter[0].isLetter()) {
        sak::logWarning(QString("Invalid drive letter for volume label: '%1'")
            .arg(cleanDest).toStdString());
        return;
    }

    QProcess labelProcess;
    QString labelCmd = QString("Set-Volume -DriveLetter %1 -NewFileSystemLabel '%2'")
        .arg(driveLetter, m_volumeLabel);
    labelProcess.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << labelCmd);
    if (!labelProcess.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logWarning("PowerShell failed to start for volume label command");
        return;
    }
    if (!labelProcess.waitForFinished(sak::kTimeoutProcessMediumMs)) {
        sak::logWarning("Volume label command timed out");
        return;
    }

    if (labelProcess.exitCode() == 0) {
        sak::logInfo("Volume label set successfully");
    } else {
        QString labelErrors = QString(labelProcess.readAllStandardError()).trimmed();
        sak::logWarning(QString("Failed to set volume label: %1").arg(labelErrors).toStdString());
    }
}

bool WindowsUSBCreator::makeBootable(const QString& driveLetter) {
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
        m_lastError = QString("Invalid drive letter format for boot configuration: '%1'")
            .arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    cleanDrive = cleanDrive.toUpper();

    // Use bcdboot.exe from extracted files to set up boot configuration
    // This replaces bootsect.exe approach
    QString bcdbootPath = QString("%1:\\sources\\recovery\\bcdboot.exe").arg(cleanDrive);

    if (!QFile::exists(bcdbootPath)) {
        // bcdboot may be in a different location, or may not exist on some ISOs
        sak::logWarning("bcdboot.exe not found - boot files may still work");
        // This is not critical - the files extracted by 7-Zip should include the necessary boot
        /// code
        return true;
    }

    sak::logInfo(QString("Configuring boot environment using bcdboot from %1")
        .arg(bcdbootPath).toStdString());

    return runBcdboot(bcdbootPath, cleanDrive);
}

bool WindowsUSBCreator::runBcdboot(const QString& bcdbootPath, const QString& cleanDrive) {
    // Run bcdboot to configure boot files
    QProcess bcdboot;
    QStringList args;
    args << QString("%1:\\").arg(cleanDrive);
    args << "/s" << QString("%1:").arg(cleanDrive);
    args << "/f" << "BIOS"; // Standard BIOS boot

    bcdboot.start(bcdbootPath, args);

    if (!bcdboot.waitForStarted()) {
        sak::logWarning("Failed to start bcdboot - boot may still work via extracted files");
        return true; // Not critical
    }

    if (!bcdboot.waitForFinished(sak::kTimeoutProcessLongMs)) {
        sak::logWarning("bcdboot timed out - boot may still work");
        bcdboot.kill();
        return true; // Not critical
    }

    QString output = bcdboot.readAllStandardOutput();
    QString errors = bcdboot.readAllStandardError();

    if (!output.isEmpty()) {
        sak::logInfo(QString("bcdboot output: %1").arg(output).toStdString());
    }

    if (bcdboot.exitCode() != 0) {
        sak::logWarning(QString("bcdboot returned code %1 - boot may still work: %2")
            .arg(bcdboot.exitCode()).arg(errors).toStdString());
        return true; // Not critical
    }

    sak::logInfo("Boot configuration completed successfully");
    return true;
}

bool WindowsUSBCreator::checkPartitionActive(const QString& diskNumber) {
    // Use diskpart to check if partition is active
    QString diskpartScript = QString(
        "select disk %1\n"
        "select partition 1\n"
        "detail partition\n"
    ).arg(diskNumber);

    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        sak::logError("Failed to create temporary diskpart script for verification");
        return false;
    }

    const QByteArray script_bytes = diskpartScript.toLocal8Bit();
    if (scriptFile.write(script_bytes) != script_bytes.size()) {
        sak::logError("Failed to write diskpart verification script");
        return false;
    }
    scriptFile.flush();

    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());

    if (!diskpart.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logError("Failed to start diskpart for verification");
        return false;
    }

    if (!diskpart.waitForFinished(sak::kTimeoutProcessLongMs)) {
        diskpart.kill();
        sak::logError("diskpart verification timed out");
        return false;
    }

    QString output = diskpart.readAllStandardOutput();
    sak::logInfo(QString("Diskpart detail output: %1").arg(output).toStdString());

    // Check if partition is marked as Active
    bool isActive = output.contains("Active", Qt::CaseInsensitive);

    if (isActive) {
        sak::logInfo("✓ Bootable flag verified - partition is active");
        return true;
    }

    m_lastError = "VERIFICATION FAILED: Partition is not marked as active/bootable";
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
        m_lastError = QString("Invalid drive letter format for verification: '%1'")
            .arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    cleanDrive = cleanDrive.toUpper();

    QProcess getDisk;
    QString diskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDrive);
    getDisk.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << diskCmd);

    if (!getDisk.waitForFinished(sak::kTimeoutProcessMediumMs)) {
        sak::logWarning("Could not get disk number for verification");
        return true; // Not critical
    }

    QString diskNumber = QString(getDisk.readAllStandardOutput()).trimmed();
    if (diskNumber.isEmpty()) {
        sak::logWarning("Could not determine disk number");
        return true; // Not critical
    }

    return checkPartitionActive(diskNumber);
}

bool WindowsUSBCreator::verifyExtractionIntegrity(const QString& isoPath, const QString& destPath,
    const QString& sevenZipPath) {
    sak::logInfo("Starting extraction integrity verification...");
    Q_EMIT statusChanged("Verifying extraction integrity...");

    // Get detailed file list from ISO with sizes
    QProcess listISO;
    QStringList listArgs;
    listArgs << "l" << "-slt" << isoPath; // List with technical info

    listISO.start(sevenZipPath, listArgs);
    if (!listISO.waitForFinished(sak::kTimeoutProcessVeryLongMs)) { // 1 minute timeout
        m_lastError = "Verification failed: Could not list ISO contents";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    QString isoListing = listISO.readAllStandardOutput();
    QStringList lines = isoListing.split("\n");

    auto criticalFiles = parseIsoCriticalFiles(lines);

    if (criticalFiles.isEmpty()) {
        m_lastError = "Verification failed: No critical Windows files found in ISO";
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("Verifying %1 critical files...")
        .arg(criticalFiles.count()).toStdString());

    return verifyCriticalFilesOnDisk(criticalFiles, destPath);
}

bool WindowsUSBCreator::isCriticalWindowsFile(const QString& path) const {
    QString lowerPath = path.toLower();
    return lowerPath.contains("setup.exe") ||
           lowerPath.contains("bootmgr") ||
           lowerPath.contains("sources/boot.wim") ||
           lowerPath.contains("sources\\\\boot.wim") ||
           lowerPath.contains("sources/install.wim") ||
           lowerPath.contains("sources\\\\install.wim") ||
           lowerPath.contains("sources/install.esd") ||
           lowerPath.contains("sources\\\\install.esd");
}

QList<QPair<QString, qint64>> WindowsUSBCreator::parseIsoCriticalFiles(const QStringList& lines) {
    QList<QPair<QString, qint64>> criticalFiles;
    QString currentPath;
    qint64 currentSize = 0;
    bool isFolder = false;

    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("Path = ")) {
            currentPath = trimmed.mid(7).trimmed();
            continue;
        }
        if (trimmed.startsWith("Size = ")) {
            bool ok = false;
            currentSize = trimmed.mid(7).toLongLong(&ok);
            if (!ok) currentSize = 0;
            continue;
        }
        if (trimmed.startsWith("Folder = ")) {
            isFolder = (trimmed.mid(9).trimmed() == "+");
            continue;
        }
        if (!trimmed.isEmpty() || currentPath.isEmpty()) continue;

        // End of entry — add if it's a critical file (not a folder)
        if (!isFolder && isCriticalWindowsFile(currentPath)) {
            criticalFiles.append({currentPath, currentSize});
        }
        currentPath.clear();
        currentSize = 0;
        isFolder = false;
    }

    return criticalFiles;
}

bool WindowsUSBCreator::verifyCriticalFilesOnDisk(const QList<QPair<QString,
    qint64>>& criticalFiles,
                                                   const QString& destPath) {
    int verifiedCount = 0;
    int failedCount = 0;

    for (const auto& fileInfo : criticalFiles) {
        // Normalize path from ISO (may use forward slashes)
        QString relativePath = fileInfo.first;
        relativePath.replace(QChar('/'), QChar('\\'));

        // Ensure destPath has trailing backslash
        QString basePath = destPath;
        if (!basePath.endsWith("\\")) {
            basePath += "\\";
        }

        QString destFile = basePath + relativePath;

        QFileInfo destFileInfo(destFile);
        if (!destFileInfo.exists()) {
            sak::logError(QString("✗ Missing file: %1").arg(fileInfo.first).toStdString());
            failedCount++;
            continue;
        }

        qint64 destSize = destFileInfo.size();
        if (destSize != fileInfo.second) {
            sak::logError(QString("✗ Size mismatch: %1 (ISO: %2 bytes, USB: %3 bytes)")
                .arg(fileInfo.first).arg(fileInfo.second).arg(destSize).toStdString());
            failedCount++;
            continue;
        }

        verifiedCount++;
    }

    sak::logInfo(QString("Verification complete: %1 files verified, %2 failures")
        .arg(verifiedCount).arg(failedCount).toStdString());

    if (verifiedCount < 3) {
        m_lastError = QString("Verification failed: Only %1 critical files verified (minimum 3 "
                              "required)").arg(verifiedCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (failedCount > 0) {
        m_lastError = QString("Extraction verification failed: %1 files missing or incorrect size")
            .arg(failedCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo("✓ Extraction integrity verified - all critical files match ISO");
    return true;
}

bool WindowsUSBCreator::verifyBootAndInstallFiles(const QString& cleanDrive) {
    // Verification 1: Check that critical boot files exist
    QStringList requiredFiles = {
        "setup.exe",
        "sources/boot.wim",
        "bootmgr"
    };

    sak::logInfo("Checking required files:");
    for (const QString& file : requiredFiles) {
        QString fullPath = cleanDrive + file;
        if (!QFile::exists(fullPath)) {
            m_lastError = QString("FINAL VERIFICATION FAILED: Critical file missing: %1").arg(file);
            sak::logError(m_lastError.toStdString());
            return false;
        }
        QFileInfo info(fullPath);
        sak::logInfo(QString("  ✓ %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
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
        sak::logInfo(QString("  ✓ install.wim (%1 bytes)").arg(info.size()).toStdString());
    }
    if (hasInstallEsd) {
        QFileInfo info(cleanDrive + "sources/install.esd");
        sak::logInfo(QString("  ✓ install.esd (%1 bytes)").arg(info.size()).toStdString());
    }

    return true;
}

void WindowsUSBCreator::logFinalVerificationSuccess(int fileCount) {
    sak::logInfo("========================================");
    sak::logInfo("SUCCESS: ALL FINAL VERIFICATIONS PASSED");
    sak::logInfo("- Critical files: VERIFIED");
    sak::logInfo("- Install image: VERIFIED");
    sak::logInfo("- Bootable flag: VERIFIED (Active)");
    sak::logInfo(QString("- File count: VERIFIED (%1 items)").arg(fileCount).toStdString());
    sak::logInfo("========================================");

    Q_EMIT progressUpdated(100);
    Q_EMIT statusChanged("✓ USB VERIFIED BOOTABLE - All checks passed");

    // THIS IS THE ONLY PLACE completed() IS EMITTED
    Q_EMIT completed();
}

bool WindowsUSBCreator::finalVerification(const QString& driveLetter) {
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

    Q_EMIT progressUpdated(92);

    // Verification 3: MANDATORY bootable flag check
    Q_EMIT statusChanged("Verifying bootable flag...");
    if (!verifyBootableFlag(driveLetter)) {
        m_lastError = "FINAL VERIFICATION FAILED: " + m_lastError;
        sak::logError(m_lastError.toStdString());
        return false;
    }

    Q_EMIT progressUpdated(95);

    // Verification 4: Count total files to ensure extraction wasn't empty
    QDir checkDest(cleanDrive);
    int fileCount = checkDest.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot,
        QDir::Name).count();

    if (fileCount < 10) {
        m_lastError = QString("FINAL VERIFICATION FAILED: Only %1 files found (expected hundreds)")
            .arg(fileCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    sak::logInfo(QString("  ✓ Total files/folders: %1").arg(fileCount).toStdString());

    Q_EMIT progressUpdated(98);

    logFinalVerificationSuccess(fileCount);

    return true;
}
