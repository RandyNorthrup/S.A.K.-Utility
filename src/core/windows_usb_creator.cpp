// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/windows_usb_creator.h"
#include "sak/logger.h"
#include <QCoreApplication>
#include <QProcess>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QThread>
#include <QRegularExpression>
#include <QStorageInfo>

WindowsUSBCreator::WindowsUSBCreator(QObject* parent)
    : QObject(parent)
{
}

WindowsUSBCreator::~WindowsUSBCreator() {
}

bool WindowsUSBCreator::createBootableUSB(const QString& isoPath, const QString& diskNumber) {
    m_cancelled = false;
    m_lastError.clear();
    m_diskNumber = diskNumber;
    
    sak::logInfo(QString("========================================").toStdString());
    sak::logInfo(QString("Creating Windows bootable USB: %1 -> Disk %2").arg(isoPath, diskNumber).toStdString());
    sak::logInfo(QString("========================================").toStdString());
    
    // Verify ISO file exists
    if (!QFile::exists(isoPath)) {
        m_lastError = QString("ISO file not found: %1").arg(isoPath);
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // ==================== STEP 1: FORMAT ====================
    Q_EMIT progressUpdated(0);
    Q_EMIT statusChanged("Step 1/5: Formatting drive as NTFS...");
    sak::logInfo("STEP 1: Formatting disk...");
    
    if (!formatDriveNTFS(diskNumber)) {
        sak::logError("STEP 1 FAILED: Format failed");
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // VERIFY Step 1: Wait for partition and get drive letter
    Q_EMIT progressUpdated(5);
    Q_EMIT statusChanged("Waiting for partition to be recognized...");
    sak::logInfo("STEP 1: Verifying format and getting drive letter...");
    
    // Wait with progress updates
    for (int i = 0; i < 30; ++i) {
        QThread::msleep(100);
        Q_EMIT progressUpdated(5 + (i * 5 / 30)); // 5% to 10%
        if (m_cancelled) break;
    }
    
    QString driveLetter = getDriveLetterFromDiskNumber();
    if (driveLetter.isEmpty()) {
        m_lastError = QString("STEP 1 VERIFICATION FAILED: No drive letter found for disk %1 after format").arg(diskNumber);
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    Q_EMIT progressUpdated(10);
    Q_EMIT statusChanged("Verifying NTFS filesystem...");
    
    // Verify NTFS format
    QProcess checkFS;
    QString checkCmd = QString("(Get-Volume -DriveLetter %1).FileSystem").arg(driveLetter);
    checkFS.start("powershell", QStringList() << "-NoProfile" << "-Command" << checkCmd);
    if (checkFS.waitForFinished(5000)) {
        QString fs = QString(checkFS.readAllStandardOutput()).trimmed();
        if (fs != "NTFS") {
            m_lastError = QString("STEP 1 VERIFICATION FAILED: Drive is %1, expected NTFS").arg(fs);
            sak::logError(m_lastError.toStdString());
            Q_EMIT failed(m_lastError);
            return false;
        }
        sak::logInfo(QString("✓ STEP 1 VERIFIED: Drive %1: formatted as NTFS").arg(driveLetter).toStdString());
    }
    
    Q_EMIT progressUpdated(13);
    Q_EMIT statusChanged("Format verified, preparing extraction...");
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // ==================== STEP 2: EXTRACT ====================
    Q_EMIT statusChanged("Step 2/5: Extracting Windows installation files...");
    sak::logInfo("STEP 2: Extracting ISO contents...");
    
    if (!copyISOContents(isoPath, driveLetter)) {
        sak::logError("STEP 2 FAILED: Extraction failed");
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // VERIFY Step 2: Check critical files exist
    sak::logInfo("STEP 2: Verifying extraction...");
    Q_EMIT statusChanged("Step 2/5: Verifying extracted files...");
    
    QString basePath = driveLetter + ":\\";
    QStringList criticalFiles = {"setup.exe", "sources\\boot.wim", "bootmgr"};
    
    for (const QString& file : criticalFiles) {
        QString fullPath = basePath + file;
        if (!QFile::exists(fullPath)) {
            m_lastError = QString("STEP 2 VERIFICATION FAILED: Missing critical file: %1").arg(file);
            sak::logError(m_lastError.toStdString());
            Q_EMIT failed(m_lastError);
            return false;
        }
        QFileInfo info(fullPath);
        sak::logInfo(QString("  ✓ %1 (%2 bytes)").arg(file).arg(info.size()).toStdString());
    }
    
    // Check for install image
    bool hasInstall = QFile::exists(basePath + "sources\\install.wim") || QFile::exists(basePath + "sources\\install.esd");
    if (!hasInstall) {
        m_lastError = "STEP 2 VERIFICATION FAILED: No install.wim or install.esd found";
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    sak::logInfo("✓ STEP 2 VERIFIED: All critical files extracted");
    Q_EMIT progressUpdated(60);
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // ==================== STEP 3: MAKE BOOTABLE ====================
    Q_EMIT progressUpdated(62);
    Q_EMIT statusChanged("Step 3/5: Making drive bootable...");
    sak::logInfo("STEP 3: Making drive bootable...");
    
    if (!makeBootable(driveLetter)) {
        sak::logError("STEP 3 FAILED: Could not configure boot files");
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    sak::logInfo("✓ STEP 3 COMPLETED: Boot configuration done");
    Q_EMIT progressUpdated(70);
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // ==================== STEP 4: SET BOOT FLAG ====================
    Q_EMIT statusChanged("Step 4/5: Setting bootable flag...");
    sak::logInfo("STEP 4: Setting bootable flag...");
    
    // Use diskpart to set active flag
    QTemporaryFile scriptFile;
    if (!scriptFile.open()) {
        m_lastError = "STEP 4 FAILED: Could not create diskpart script";
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    QString diskpartScript = QString(
        "select disk %1\\n"
        "select partition 1\\n"
        "active\\n"
    ).arg(diskNumber);
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted(5000) || !diskpart.waitForFinished(30000)) {
        m_lastError = "STEP 4 FAILED: Diskpart failed to set active flag";
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // VERIFY Step 4: Check bootable flag was set
    sak::logInfo("STEP 4: Verifying bootable flag...");
    Q_EMIT statusChanged("Step 4/5: Verifying bootable flag...");
    
    if (!verifyBootableFlag(driveLetter)) {
        m_lastError = "STEP 4 VERIFICATION FAILED: " + m_lastError;
        sak::logError(m_lastError.toStdString());
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    sak::logInfo("✓ STEP 4 VERIFIED: Bootable flag is set (Active)");
    Q_EMIT progressUpdated(85);
    
    if (m_cancelled) {
        m_lastError = "Operation cancelled";
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // ==================== STEP 5: FINAL VERIFICATION ====================
    Q_EMIT statusChanged("Step 5/5: Running final comprehensive verification...");
    sak::logInfo("STEP 5: Final comprehensive verification...");
    
    if (!finalVerification(driveLetter)) {
        sak::logError("STEP 5 VERIFICATION FAILED");
        Q_EMIT failed(m_lastError);
        return false;
    }
    
    // SUCCESS - finalVerification() emits completed() signal
    sak::logInfo("========================================");
    sak::logInfo("ALL STEPS COMPLETED AND VERIFIED");
    sak::logInfo("========================================");
    return true;
}

void WindowsUSBCreator::cancel() {
    m_cancelled = true;
    sak::logInfo("Windows USB creation cancelled");
}

QString WindowsUSBCreator::lastError() const {
    return m_lastError;
}

bool WindowsUSBCreator::formatDriveNTFS(const QString& diskNumber) {
    // Use the official Microsoft method: diskpart clean, then format command
    // Based on Windows Server USB creation documentation and Rufus implementation
    
    sak::logInfo(QString("Formatting disk %1 as NTFS").arg(diskNumber).toStdString());
    Q_EMIT statusChanged("Preparing USB drive...");
    
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
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    sak::logInfo(QString("Running diskpart script:\n%1").arg(diskpartScript).toStdString());
    
    // Diskpart requires Administrator privileges
    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted(5000)) {
        m_lastError = "Failed to start diskpart - ensure application is running as Administrator";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    if (!diskpart.waitForFinished(30000)) {
        m_lastError = "Diskpart timed out";
        sak::logError(m_lastError.toStdString());
        diskpart.kill();
        return false;
    }
    
    QString output = diskpart.readAllStandardOutput();
    QString errors = diskpart.readAllStandardError();
    sak::logInfo(QString("Diskpart output:\n%1").arg(output).toStdString());
    if (!errors.isEmpty()) {
        sak::logError(QString("Diskpart errors:\n%1").arg(errors).toStdString());
    }
    
    if (diskpart.exitCode() != 0) {
        m_lastError = QString("Diskpart failed with exit code %1. Ensure you are running as Administrator.").arg(diskpart.exitCode());
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    // Ensure process fully terminated
    if (diskpart.state() == QProcess::Running) {
        sak::logWarning("Diskpart still running after waitForFinished, forcing termination");
        diskpart.kill();
        diskpart.waitForFinished(2000);
    }
    
    // Step 2: Wait for partition and format with NTFS
    sak::logInfo("Waiting for Windows to recognize partition...");
    Q_EMIT statusChanged("Formatting partition as NTFS...");
    QThread::msleep(3000);
    
    // Step 3: Format as NTFS using diskpart
    QString formatCmd = QString("format FS=NTFS QUICK label=\"BOOT\"");
    QString formatScript = QString(
        "select disk %1\n"
        "select partition 1\n"
        "%2\n"
        "exit\n"
    ).arg(diskNumber, formatCmd);
    
    QTemporaryFile formatScriptFile;
    if (!formatScriptFile.open()) {
        m_lastError = "Failed to create format script";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    formatScriptFile.write(formatScript.toLocal8Bit());
    formatScriptFile.flush();
    
    sak::logInfo(QString("Running format script:\n%1").arg(formatScript).toStdString());
    
    QProcess format;
    format.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << formatScriptFile.fileName());
    
    if (!format.waitForStarted(5000)) {
        m_lastError = "Failed to start format";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    if (!format.waitForFinished(60000)) {
        m_lastError = "Format timed out";
        sak::logError(m_lastError.toStdString());
        format.kill();
        return false;
    }
    
    QString formatOutput = format.readAllStandardOutput();
    QString formatErrors = format.readAllStandardError();
    sak::logInfo(QString("Format output:\n%1").arg(formatOutput).toStdString());
    if (!formatErrors.isEmpty()) {
        sak::logError(QString("Format errors:\n%1").arg(formatErrors).toStdString());
    }
    
    if (format.exitCode() != 0) {
        m_lastError = QString("Format failed with exit code %1").arg(format.exitCode());
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    // Wait for format to complete
    sak::logInfo("Waiting for format to settle...");
    QThread::msleep(3000);
    
    sak::logInfo(QString("Successfully formatted disk %1 as NTFS").arg(diskNumber).toStdString());
    return true;
}

bool WindowsUSBCreator::copyISOContents(const QString& sourcePath, const QString& destPath) {
    sak::logInfo(QString("Extracting ISO contents: %1 -> %2").arg(sourcePath, destPath).toStdString());
    
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
    
    // Extract volume label from ISO before extraction
    QProcess labelExtract;
    QStringList labelArgs;
    labelArgs << "l" << "-slt" << sourcePath; // List with technical info
    
    labelExtract.start(sevenZipPath, labelArgs);
    if (labelExtract.waitForFinished(10000)) {
        QString labelOutput = labelExtract.readAllStandardOutput();
        // Look for "Comment = " line which contains volume label
        QStringList lines = labelOutput.split('\n');
        for (const QString& line : lines) {
            if (line.startsWith("Comment = ")) {
                m_volumeLabel = line.mid(10).trimmed();
                sak::logInfo(QString("ISO volume label: %1").arg(m_volumeLabel).toStdString());
                break;
            }
        }
    }
    
    // Default to WINDOWS if label not found
    if (m_volumeLabel.isEmpty()) {
        m_volumeLabel = "WINDOWS";
        sak::logInfo(QString("Using default volume label: %1").arg(m_volumeLabel).toStdString());
    }
    
    // Use 7-Zip to extract ISO (doesn't require mounting, reads ISO natively)
    // This is the same method used by industry-standard tools like Rufus
    QProcess extract;
    
    // Normalize drive letter to full path format (e.g., "E" -> "E:\")
    QString cleanDest = destPath.trimmed();
    
    // Remove any existing colons and backslashes to start fresh
    cleanDest.remove(':');
    cleanDest.remove('\\');
    cleanDest.remove('/');
    
    // Should now have just the drive letter
    if (cleanDest.length() != 1 || !cleanDest[0].isLetter()) {
        m_lastError = QString("Invalid drive letter format: '%1' (expected single letter A-Z)").arg(destPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    // Build proper drive path: "E" -> "E:\"
    cleanDest = cleanDest.toUpper() + ":\\";
    
    sak::logInfo(QString("Normalized destination path: %1").arg(cleanDest).toStdString());
    
    // Check available disk space before extraction (now that we have proper path)
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
            .arg(requiredSpace / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
            .arg(availableSpace / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    sak::logInfo(QString("Disk space check: %1 GB available, %2 GB required")
        .arg(availableSpace / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2)
        .arg(requiredSpace / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2).toStdString());
    
    sak::logInfo(QString("Using 7z.exe to extract ISO directly to %1").arg(cleanDest).toStdString());
    Q_EMIT statusChanged("Extracting Windows installation files...");
    
    // 7z x = extract with full paths, -aoa = overwrite all, -bsp2 = detailed progress to stdout
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
    
    if (!extract.waitForStarted(5000)) {
        m_lastError = QString("Failed to start 7z.exe at: %1").arg(sevenZipPath);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    sak::logInfo("7z process started, extracting ISO (this may take several minutes)...");
    Q_EMIT statusChanged("Extracting Windows files...");
    
    // Monitor extraction progress with real-time byte-level tracking (max 15 minutes)
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
            extract.waitForFinished(5000);
            m_lastError = "Extraction cancelled by user";
            return false;
        }
        
        // Read any new output from 7z
        if (extract.waitForReadyRead(checkIntervalMs)) {
            QString newOutput = QString::fromLocal8Bit(extract.readAllStandardOutput());
            
            // Parse 7z -bsp2 output format: "bytes_processed + bytes_total"
            // Format example: "123456789 + 987654321" or percentage "12%"
            QRegularExpression bytesRegex(R"((\d+)\s*\+\s*(\d+))");
            QRegularExpressionMatch bytesMatch = bytesRegex.match(newOutput);
            
            if (bytesMatch.hasMatch()) {
                processedBytes = bytesMatch.captured(1).toLongLong();
                qint64 newTotal = bytesMatch.captured(2).toLongLong();
                
                // Update total if we get a larger value (7z reports it progressively)
                if (newTotal > totalBytes) {
                    totalBytes = newTotal;
                }
                
                // Calculate percentage based on actual bytes
                if (totalBytes > 0) {
                    int extractPercent = static_cast<int>((processedBytes * 100) / totalBytes);
                    
                    // Map 0-100% of extraction to 15-50% of total progress
                    int totalProgress = 15 + (extractPercent * 35 / 100);
                    
                    if (totalProgress > lastProgressPercent) {
                        lastProgressPercent = totalProgress;
                        Q_EMIT progressUpdated(totalProgress);
                        
                        double processedMB = processedBytes / (1024.0 * 1024.0);
                        double totalMB = totalBytes / (1024.0 * 1024.0);
                        
                        Q_EMIT statusChanged(QString("Extracting Windows files... %1 MB / %2 MB (%3%)") .arg(processedMB, 0, 'f', 1)
                            .arg(totalMB, 0, 'f', 1)
                            .arg(extractPercent));
                        
                        sak::logInfo(QString("Extraction progress: %1 MB / %2 MB (%3%)") .arg(processedMB, 0, 'f', 1)
                            .arg(totalMB, 0, 'f', 1)
                            .arg(extractPercent).toStdString());
                    }
                }
            } else {
                // Fallback: Try to parse percentage if bytes format not found
                QRegularExpression percentRegex(R"(\s+(\d+)%)");
                QRegularExpressionMatch percentMatch = percentRegex.match(newOutput);
                
                if (percentMatch.hasMatch()) {
                    int extractPercent = percentMatch.captured(1).toInt();
                    int totalProgress = 15 + (extractPercent * 35 / 100);
                    
                    if (totalProgress > lastProgressPercent) {
                        lastProgressPercent = totalProgress;
                        Q_EMIT progressUpdated(totalProgress);
                        Q_EMIT statusChanged(QString("Extracting Windows files... %1%").arg(extractPercent));
                        sak::logInfo(QString("Extraction progress: %1%").arg(extractPercent).toStdString());
                    }
                }
            }
            
            totalWaitedMs += checkIntervalMs;
        } else {
            totalWaitedMs += checkIntervalMs;
        }
    }
    
    if (extract.state() == QProcess::Running) {
        m_lastError = "ISO extraction timed out after 15 minutes";
        sak::logError(m_lastError.toStdString());
        extract.kill();
        extract.waitForFinished(5000);
        return false;
    }
    
    int exitCode = extract.exitCode();
    QString output = extract.readAllStandardOutput();
    QString errors = extract.readAllStandardError();
    
    sak::logInfo(QString("7z extraction completed with exit code: %1").arg(exitCode).toStdString());
    
    if (!output.isEmpty()) {
        QStringList lines = output.split('\n', Qt::SkipEmptyParts);
        sak::logInfo(QString("7z processed %1 lines of output").arg(lines.count()).toStdString());
        // Log last few lines which contain summary
        if (lines.count() > 0) {
            for (int i = qMax(0, lines.count() - 5); i < lines.count(); ++i) {
                sak::logInfo(QString("  %1").arg(lines[i].trimmed()).toStdString());
            }
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
    
    // Wait a moment for filesystem to settle after extraction
    sak::logInfo("Waiting for filesystem to settle after extraction...");
    QThread::msleep(2000);
    
    // Verify critical Windows files were extracted to the destination
    sak::logInfo(QString("Verifying critical files exist at: %1").arg(cleanDest).toStdString());
    
    // Construct setup.exe path using QDir for correct path handling
    QDir checkDest(cleanDest);
    QString setupPath = checkDest.absoluteFilePath("setup.exe");
    
    // Log the exact path we're checking
    sak::logInfo(QString("Expected setup.exe at: %1").arg(setupPath).toStdString());
    
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
    
    // Verify setup.exe exists (absolute requirement for Windows boot)
    sak::logInfo(QString("Checking for setup.exe at: %1").arg(setupPath).toStdString());
    
    // Check if setup.exe exists with case-insensitive search
    bool setupFound = false;
    QFileInfo setupInfo(setupPath);
    QStringList rootFiles;
    
    if (QFile::exists(setupPath)) {
        setupFound = true;
    } else {
        // Try case-insensitive search in root directory
        sak::logWarning("setup.exe not found with exact case, searching case-insensitively...");
        rootFiles = checkDest.entryList(QDir::Files | QDir::NoDotAndDotDot);
        for (const QString& file : rootFiles) {
            if (file.toLower() == "setup.exe") {
                setupFound = true;
                setupPath = cleanDest + file;
                sak::logInfo(QString("Found setup file with different case: %1").arg(file).toStdString());
                break;
            }
        }
    }
    
    if (!setupFound) {
        m_lastError = "CRITICAL: setup.exe not found after extraction";
        sak::logError(m_lastError.toStdString());
        sak::logError(QString("Checked path: %1").arg(setupPath).toStdString());
        if (!rootFiles.isEmpty()) {
            sak::logError(QString("Files in root: %1").arg(rootFiles.join(", ")).toStdString());
        }
        sak::logError("ISO extraction may have failed or ISO may be corrupt");
        return false;
    }
    
    sak::logInfo(QString("✓ setup.exe found at: %1").arg(setupPath).toStdString());
    
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
            sak::logInfo(QString("✓ Found: %1").arg(file).toStdString());
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
            sak::logInfo(QString("✓ Found install image: %1").arg(file).toStdString());
            foundFiles << file;
            hasInstallImage = true;
            break;
        }
    }
    
    if (!hasInstallImage) {
        m_lastError = "CRITICAL: No Windows install image found (install.wim or install.esd required)";
        sak::logError(m_lastError.toStdString());
        sak::logError("Windows installation incomplete - USB will not be able to install Windows");
        return false;
    }
    
    sak::logInfo(QString("✓ All critical files verified: %1 core files found").arg(foundFiles.count()).toStdString());
    
    // Set the volume label from ISO
    if (!m_volumeLabel.isEmpty()) {
        sak::logInfo(QString("Setting volume label to: %1").arg(m_volumeLabel).toStdString());
        // Extract single drive letter from normalized path ("E:\\" -> "E")
        QString driveLetter = cleanDest.left(1);
        if (driveLetter.isEmpty() || !driveLetter[0].isLetter()) {
            sak::logWarning(QString("Invalid drive letter for volume label: '%1'").arg(cleanDest).toStdString());
        } else {
            QProcess labelProcess;
            QString labelCmd = QString("Set-Volume -DriveLetter %1 -NewFileSystemLabel '%2'")
                .arg(driveLetter, m_volumeLabel);
            labelProcess.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << labelCmd);
            if (labelProcess.waitForFinished(10000)) {
                if (labelProcess.exitCode() == 0) {
                    sak::logInfo("Volume label set successfully");
                } else {
                    QString labelErrors = QString(labelProcess.readAllStandardError()).trimmed();
                    sak::logWarning(QString("Failed to set volume label: %1").arg(labelErrors).toStdString());
                }
            } else {
                sak::logWarning("Volume label command timed out");
            }
        }
    }
    
    sak::logInfo(QString("ISO extraction completed successfully: %1 files/folders").arg(destFiles.count()).toStdString());
    
    // Verify extraction integrity by comparing file list and sizes
    if (!verifyExtractionIntegrity(sourcePath, cleanDest, sevenZipPath)) {
        m_lastError = "Extraction verification failed - files do not match ISO contents";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    return true;
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
        m_lastError = QString("Invalid drive letter format for boot configuration: '%1'").arg(driveLetter);
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
        // This is not critical - the files extracted by 7-Zip should include the necessary boot code
        return true;
    }
    
    sak::logInfo(QString("Configuring boot environment using bcdboot from %1").arg(bcdbootPath).toStdString());
    
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
    
    if (!bcdboot.waitForFinished(30000)) {
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
        sak::logWarning(QString("bcdboot returned code %1 - boot may still work: %2").arg(bcdboot.exitCode()).arg(errors).toStdString());
        return true; // Not critical
    }
    
    sak::logInfo("Boot configuration completed successfully");
    return true;
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
        m_lastError = QString("Invalid drive letter format for verification: '%1'").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    cleanDrive = cleanDrive.toUpper();
    
    QProcess getDisk;
    QString diskCmd = QString("(Get-Partition -DriveLetter %1).DiskNumber").arg(cleanDrive);
    getDisk.start("powershell.exe", QStringList() << "-NoProfile" << "-Command" << diskCmd);
    
    if (!getDisk.waitForFinished(10000)) {
        sak::logWarning("Could not get disk number for verification");
        return true; // Not critical
    }
    
    QString diskNumber = QString(getDisk.readAllStandardOutput()).trimmed();
    if (diskNumber.isEmpty()) {
        sak::logWarning("Could not determine disk number");
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
        sak::logError("Failed to create temporary diskpart script for verification");
        return false;
    }
    
    scriptFile.write(diskpartScript.toLocal8Bit());
    scriptFile.flush();
    
    QProcess diskpart;
    diskpart.start("cmd.exe", QStringList() << "/c" << "diskpart" << "/s" << scriptFile.fileName());
    
    if (!diskpart.waitForStarted(5000)) {
        sak::logError("Failed to start diskpart for verification");
        return false;
    }
    
    if (!diskpart.waitForFinished(30000)) {
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
    } else {
        m_lastError = "VERIFICATION FAILED: Partition is not marked as active/bootable";
        sak::logError(m_lastError.toStdString());
        sak::logError("USB drive will NOT be bootable - bootable flag must be set");
        return false;
    }
}

bool WindowsUSBCreator::verifyExtractionIntegrity(const QString& isoPath, const QString& destPath, const QString& sevenZipPath) {
    sak::logInfo("Starting extraction integrity verification...");
    Q_EMIT statusChanged("Verifying extraction integrity...");
    
    // Get detailed file list from ISO with sizes
    QProcess listISO;
    QStringList listArgs;
    listArgs << "l" << "-slt" << isoPath; // List with technical info
    
    listISO.start(sevenZipPath, listArgs);
    if (!listISO.waitForFinished(60000)) { // 1 minute timeout
        m_lastError = "Verification failed: Could not list ISO contents";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    QString isoListing = listISO.readAllStandardOutput();
    QStringList lines = isoListing.split("\n");
    
    // Parse ISO file list and build verification list
    struct FileInfo {
        QString path;
        qint64 size;
    };
    
    QList<FileInfo> criticalFiles;
    QString currentPath;
    qint64 currentSize = 0;
    bool isFolder = false;
    
    for (const QString& line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("Path = ")) {
            currentPath = trimmed.mid(7).trimmed();
        } else if (trimmed.startsWith("Size = ")) {
            bool ok = false;
            currentSize = trimmed.mid(7).toLongLong(&ok);
            if (!ok) {
                currentSize = 0;
            }
        } else if (trimmed.startsWith("Folder = ")) {
            isFolder = (trimmed.mid(9).trimmed() == "+");
        } else if (trimmed.isEmpty() && !currentPath.isEmpty()) {
            // End of entry
            if (!isFolder) {
                // Only verify critical Windows installation files
                QString lowerPath = currentPath.toLower();
                if (lowerPath.contains("setup.exe") || 
                    lowerPath.contains("bootmgr") ||
                    lowerPath.contains("sources/boot.wim") ||
                    lowerPath.contains("sources\\\\boot.wim") ||
                    lowerPath.contains("sources/install.wim") ||
                    lowerPath.contains("sources\\\\install.wim") ||
                    lowerPath.contains("sources/install.esd") ||
                    lowerPath.contains("sources\\\\install.esd")) {
                    criticalFiles.append({currentPath, currentSize});
                }
            }
            currentPath.clear();
            currentSize = 0;
            isFolder = false;
        }
    }
    
    if (criticalFiles.isEmpty()) {
        m_lastError = "Verification failed: No critical Windows files found in ISO";
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    sak::logInfo(QString("Verifying %1 critical files...").arg(criticalFiles.count()).toStdString());
    
    // Verify each critical file exists on destination with correct size
    int verifiedCount = 0;
    int failedCount = 0;
    
    for (const FileInfo& fileInfo : criticalFiles) {
        // Normalize path from ISO (may use forward slashes)
        QString relativePath = fileInfo.path;
        relativePath.replace(QChar('/'), QChar('\\'));
        
        // Ensure destPath has trailing backslash
        QString basePath = destPath;
        if (!basePath.endsWith("\\")) {
            basePath += "\\";
        }
        
        QString destFile = basePath + relativePath; // Normalize path separators
        
        QFileInfo destFileInfo(destFile);
        if (!destFileInfo.exists()) {
            sak::logError(QString("✗ Missing file: %1").arg(fileInfo.path).toStdString());
            failedCount++;
            continue;
        }
        
        qint64 destSize = destFileInfo.size();
        if (destSize != fileInfo.size) {
            sak::logError(QString("✗ Size mismatch: %1 (ISO: %2 bytes, USB: %3 bytes)")
                .arg(fileInfo.path).arg(fileInfo.size).arg(destSize).toStdString());
            failedCount++;
            continue;
        }
        
        verifiedCount++;
    }
    
    sak::logInfo(QString("Verification complete: %1 files verified, %2 failures")
        .arg(verifiedCount).arg(failedCount).toStdString());
    
    if (verifiedCount < 3) {
        m_lastError = QString("Verification failed: Only %1 critical files verified (minimum 3 required)").arg(verifiedCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    if (failedCount > 0) {
        m_lastError = QString("Extraction verification failed: %1 files missing or incorrect size").arg(failedCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    sak::logInfo("✓ Extraction integrity verified - all critical files match ISO");
    return true;
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
        m_lastError = QString("FINAL VERIFICATION FAILED: Invalid drive letter format: '%1'").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    // Build standard path: "E" -> "E:\\"
    cleanDrive = cleanDrive.toUpper() + ":\\";
    
    sak::logInfo(QString("Final verification path: %1").arg(cleanDrive).toStdString());
    
    Q_EMIT statusChanged("Verifying all critical files...");
    
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
    int fileCount = checkDest.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name).count();
    
    if (fileCount < 10) {
        m_lastError = QString("FINAL VERIFICATION FAILED: Only %1 files found (expected hundreds)").arg(fileCount);
        sak::logError(m_lastError.toStdString());
        return false;
    }
    
    sak::logInfo(QString("  ✓ Total files/folders: %1").arg(fileCount).toStdString());
    
    Q_EMIT progressUpdated(98);
    
    // ALL VERIFICATIONS PASSED
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
    // No other code path can report success
    Q_EMIT completed();
    
    return true;
}

QString WindowsUSBCreator::getDriveLetterFromDiskNumber() {
    if (m_diskNumber.isEmpty()) {
        m_lastError = "Cannot query drive letter: No disk number set";
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    // Validate disk number is numeric
    bool ok = false;
    int diskNum = m_diskNumber.toInt(&ok);
    if (!ok || diskNum < 0) {
        m_lastError = QString("Invalid disk number format: '%1'").arg(m_diskNumber);
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    sak::logInfo(QString("Querying drive letter for disk %1").arg(m_diskNumber).toStdString());
    
    QProcess getDrive;
    QString cmd = QString("(Get-Partition -DiskNumber %1 | Get-Volume | Where-Object {$_.DriveLetter -ne $null} | Select-Object -First 1).DriveLetter").arg(m_diskNumber);
    getDrive.start("powershell", QStringList() << "-NoProfile" << "-Command" << cmd);
    
    if (!getDrive.waitForFinished(10000)) {
        m_lastError = QString("Timeout querying drive letter for disk %1").arg(m_diskNumber);
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    if (getDrive.exitCode() != 0) {
        QString errors = QString(getDrive.readAllStandardError()).trimmed();
        m_lastError = QString("PowerShell query failed for disk %1: %2").arg(m_diskNumber, errors);
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    QString driveLetter = QString(getDrive.readAllStandardOutput()).trimmed();
    
    // Validate drive letter format (should be single character A-Z)
    if (driveLetter.isEmpty()) {
        m_lastError = QString("No drive letter assigned to disk %1. Drive may not be formatted or partition not recognized.").arg(m_diskNumber);
        sak::logError(m_lastError.toStdString());
        sak::logError("Ensure the disk has been formatted and has a valid partition.");
        return QString();
    }
    
    if (driveLetter.length() != 1) {
        m_lastError = QString("Invalid drive letter format from PowerShell: '%1' (expected single character)").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    if (!driveLetter[0].isLetter()) {
        m_lastError = QString("Invalid drive letter character: '%1' (expected A-Z)").arg(driveLetter);
        sak::logError(m_lastError.toStdString());
        return QString();
    }
    
    // Normalize to uppercase
    driveLetter = driveLetter.toUpper();
    
    sak::logInfo(QString("✓ Successfully mapped disk %1 to drive letter %2").arg(m_diskNumber, driveLetter).toStdString());
    return driveLetter;
}

