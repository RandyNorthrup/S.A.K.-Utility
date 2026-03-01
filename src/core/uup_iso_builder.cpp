// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/uup_iso_builder.h"
#include "sak/bundled_tools_manager.h"
#include "sak/logger.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QStandardPaths>
#include <QDateTime>
#include <QFileInfo>
#include <QDirIterator>
#include <QRegularExpression>
#include <QTextStream>
#include <QFile>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ============================================================================
// Construction / Destruction
// ============================================================================

UupIsoBuilder::UupIsoBuilder(QObject* parent)
    : QObject(parent)
{
    m_progressPollTimer = new QTimer(this);
    m_progressPollTimer->setInterval(1000);
    connect(m_progressPollTimer, &QTimer::timeout,
            this, &UupIsoBuilder::onProgressPollTimer);
}

UupIsoBuilder::~UupIsoBuilder()
{
    cancel();
}

// ============================================================================
// Public API
// ============================================================================

void UupIsoBuilder::startBuild(const QList<UupDumpApi::FileInfo>& files,
                               const QString& outputIsoPath,
                               const QString& edition,
                               const QString& lang,
                               const QString& updateId)
{
    if (isRunning()) {
        Q_EMIT buildError("A build is already in progress");
        return;
    }

    m_cancelled = false;
    m_files = files;
    m_outputIsoPath = outputIsoPath;
    m_edition = edition;
    m_lang = lang;
    m_updateId = updateId;
    m_downloadPercent = 0;
    m_conversionPercent = 0;
    m_currentSpeedMBps = 0.0;
    m_downloadedBytes = 0;
    m_allFilesAlreadyDownloaded = false;
    m_converterOutputTail.clear();

    // Calculate total download size
    m_totalDownloadBytes = 0;
    for (const auto& file : m_files) {
        m_totalDownloadBytes += file.size;
    }

    sak::logInfo("Starting UUP ISO build: " + std::to_string(m_files.size()) +
                  " files, " + std::to_string(m_totalDownloadBytes / (1024 * 1024)) +
                  " MB total, edition=" + m_edition.toStdString() +
                  ", lang=" + m_lang.toStdString());

    executePreparation();
}

void UupIsoBuilder::cancel()
{
    m_cancelled = true;
    m_progressPollTimer->stop();

    if (m_aria2Process && m_aria2Process->state() != QProcess::NotRunning) {
        m_aria2Process->kill();
        m_aria2Process->waitForFinished(5000);
    }
    if (m_converterProcess && m_converterProcess->state() != QProcess::NotRunning) {
        m_converterProcess->kill();
        m_converterProcess->waitForFinished(5000);
    }

    if (m_phase != Phase::Idle && m_phase != Phase::Completed) {
        m_phase = Phase::Failed;
        Q_EMIT phaseChanged(Phase::Failed, "Build cancelled by user");
    }

    // Don't clean up work dir on cancel — allows resuming later
    // cleanupWorkDir();  // Only cleaned up on successful build completion
}

// ============================================================================
// Tool Path Resolution
// ============================================================================

QString UupIsoBuilder::findAria2Path() const
{
    auto& tools = sak::BundledToolsManager::instance();

    // Primary location: tools/uup/aria2c.exe
    QString path = tools.toolPath("uup", "aria2c.exe");
    if (QFileInfo::exists(path))
        return path;

    // Fallback: recursive search in tools/uup/
    QDir uupDir(tools.toolsPath() + "/uup");
    if (uupDir.exists()) {
        QDirIterator it(uupDir.absolutePath(), {"aria2c.exe"},
                        QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
            return it.next();
    }

    sak::logError("aria2c.exe not found in bundled tools");
    return {};
}

QString UupIsoBuilder::findConverterDir() const
{
    auto& tools = sak::BundledToolsManager::instance();

    // Primary location: tools/uup/converter/
    QString converterDir = tools.toolsPath() + "/uup/converter";
    if (QDir(converterDir).exists() &&
        QFileInfo::exists(converterDir + "/convert-UUP.cmd")) {
        return converterDir;
    }

    // Fallback: find convert-UUP.cmd anywhere under tools/uup/
    QDir uupDir(tools.toolsPath() + "/uup");
    if (uupDir.exists()) {
        QDirIterator it(uupDir.absolutePath(), {"convert-UUP.cmd"},
                        QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext()) {
            QFileInfo fi(it.next());
            return fi.absolutePath();
        }
    }

    sak::logError("UUP converter (convert-UUP.cmd) not found in bundled tools");
    return {};
}

QString UupIsoBuilder::find7zPath() const
{
    auto& tools = sak::BundledToolsManager::instance();

    // Check chocolatey tools first (already bundled)
    QString path = tools.toolPath("chocolatey/tools", "7z.exe");
    if (QFileInfo::exists(path))
        return path;

    // Check uup tools for 7zr.exe
    path = tools.toolPath("uup", "7zr.exe");
    if (QFileInfo::exists(path))
        return path;

    sak::logWarning("7z.exe/7zr.exe not found in bundled tools. "
                     "Ensure tools/chocolatey/tools/ or tools/uup/ contains a 7-Zip executable.");
    return {};
}

QString UupIsoBuilder::findUupMediaConverterPath() const
{
    auto& tools = sak::BundledToolsManager::instance();

    // Preferred location if bundled explicitly.
    QString path = tools.toolPath("uup", "UUPMediaConverter.exe");
    if (QFileInfo::exists(path))
        return path;

    path = tools.toolPath("uup", "uupmediaconverter.exe");
    if (QFileInfo::exists(path))
        return path;

    // Fallback: recursive search under tools/uup.
    QDir uupDir(tools.toolsPath() + "/uup");
    if (uupDir.exists()) {
        QDirIterator it(uupDir.absolutePath(), {"UUPMediaConverter.exe", "uupmediaconverter.exe"},
                        QDir::Files, QDirIterator::Subdirectories);
        if (it.hasNext())
            return it.next();
    }

    sak::logWarning("UUPMediaConverter.exe not found in bundled tools. "
                     "Run 'powershell -File scripts/bundle_uup_tools.ps1' to bundle required tools.");
    return {};
}

// ============================================================================
// Phase 1: Preparation
// ============================================================================

void UupIsoBuilder::executePreparation()
{
    m_phase = Phase::PreparingDownload;
    Q_EMIT phaseChanged(Phase::PreparingDownload, "Preparing download environment...");
    Q_EMIT progressUpdated(0, "Validating bundled tools...");

    // ---- Validate required tools ----
    QString aria2Path = findAria2Path();
    if (aria2Path.isEmpty()) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "aria2c.exe not found in bundled tools. "
            "Run scripts/bundle_uup_tools.ps1 and rebuild the application.");
        return;
    }
    sak::logInfo("Found aria2c: " + aria2Path.toStdString());

    QString converterDir = findConverterDir();
    if (converterDir.isEmpty()) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "UUP converter tools (convert-UUP.cmd) not found in bundled tools. "
            "Run scripts/bundle_uup_tools.ps1 and rebuild the application.");
        return;
    }
    sak::logInfo("Found converter: " + converterDir.toStdString());

    // ---- Create work directory (deterministic name for resume support) ----
    Q_EMIT progressUpdated(1, "Creating work directory...");

    QString tempBase = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (!m_updateId.isEmpty()) {
        // Deterministic, short directory name for resume support.
        // Keeping this short helps DISM/AppX operations that are sensitive to
        // deep temp paths.
        const QString resumeKey = m_updateId + "|" + m_lang + "|" + m_edition.toLower();
        const QString shortHash = QString::fromLatin1(
            QCryptographicHash::hash(resumeKey.toUtf8(), QCryptographicHash::Md5).toHex().left(12));
        m_workDir = QDir(tempBase).filePath(QString("sak_uup_%1").arg(shortHash));
    } else {
        // Fallback: timestamp-based (no resume)
        QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        m_workDir = QDir(tempBase).filePath(QString("sak_uup_%1").arg(timestamp));
    }

    QDir workDir(m_workDir);
    if (!workDir.mkpath(".")) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(QString("Failed to create work directory: %1").arg(m_workDir));
        return;
    }

    if (!workDir.mkpath("UUPs")) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to create UUPs subdirectory in work directory");
        return;
    }

    // Check if this is a resumed build
    QString downloadDir = workDir.filePath("UUPs");
    QDir dlDir(downloadDir);
    int existingFiles = 0;
    qint64 existingBytes = 0;
    if (dlDir.exists()) {
        QDirIterator existingIt(dlDir.absolutePath(), QDir::Files);
        while (existingIt.hasNext()) {
            existingIt.next();
            // Don't count .aria2 control files
            if (!existingIt.fileName().endsWith(".aria2")) {
                existingFiles++;
                existingBytes += existingIt.fileInfo().size();
            }
        }
    }

    if (existingFiles > 0) {
        double existingGB = existingBytes / (1024.0 * 1024.0 * 1024.0);
        sak::logInfo("Resuming download — found " + std::to_string(existingFiles) +
                     " existing files (" + std::to_string(static_cast<int>(existingGB * 100) / 100) +
                     " GB) in work directory");
        Q_EMIT progressUpdated(1, QString("Resuming download \u2014 %1 files already present (%2 GB)")
            .arg(existingFiles).arg(existingGB, 0, 'f', 2));
    }

    sak::logInfo("Work directory created: " + m_workDir.toStdString());

    // ---- Generate aria2c input file ----
    Q_EMIT progressUpdated(2, "Generating download manifest...");

    QString aria2InputPath = workDir.filePath("aria2_script.txt");
    if (!generateAria2InputFile(aria2InputPath)) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to generate aria2c download manifest");
        return;
    }

    // Check if the aria2 input file is empty (all files already downloaded)
    QFileInfo aria2FileInfo(aria2InputPath);
    m_allFilesAlreadyDownloaded = (aria2FileInfo.size() == 0);

    // ---- Generate ConvertConfig.ini ----
    Q_EMIT progressUpdated(3, "Creating converter configuration...");

    QString configPath = workDir.filePath("ConvertConfig.ini");
    QFile configFile(configPath);
    if (!configFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to create ConvertConfig.ini");
        return;
    }

    {
        QTextStream cfg(&configFile);
        cfg << "[convert-UUP]\n"
            << "AutoStart  =1\n"   // 1 = create ISO with install.wim
            << "AddUpdates =1\n"
            << "Cleanup    =1\n"
            << "ResetBase  =0\n"
            << "NetFx3     =0\n"
            << "StartVirtual=0\n"
            << "wim2esd    =0\n"
            << "wim2swm    =0\n"
            << "SkipISO    =0\n"
            << "SkipWinRE  =0\n"
            << "ForceDism  =0\n"
            << "RefESD     =0\n"
            << "UpdtBootFiles=1\n"
            << "AutoExit   =1\n"   // Exit without waiting for keypress
            << "SkipEdge   =0\n";
    }
    configFile.close();

    // ---- Copy converter files to work directory ----
    Q_EMIT progressUpdated(4, "Setting up converter tools...");

    QDir srcDir(converterDir);
    QDirIterator converterIt(srcDir.absolutePath(),
                             QDir::Files | QDir::NoDotAndDotDot,
                             QDirIterator::Subdirectories);
    int copiedCount = 0;
    int skippedConverterFiles = 0;
    while (converterIt.hasNext()) {
        QString srcPath = converterIt.next();
        QString relativePath = srcDir.relativeFilePath(srcPath);
        QString destPath = workDir.filePath(relativePath);

        QFileInfo destInfo(destPath);
        QDir().mkpath(destInfo.absolutePath());

        // Skip files already present (resume scenario)
        if (destInfo.exists()) {
            skippedConverterFiles++;
            continue;
        }

        if (QFile::copy(srcPath, destPath)) {
            copiedCount++;
        } else {
            sak::logWarning("Failed to copy converter file: " + relativePath.toStdString());
        }
    }
    if (skippedConverterFiles > 0) {
        sak::logInfo("Skipped " + std::to_string(skippedConverterFiles) +
                      " converter files already in work directory");
    }
    sak::logInfo("Copied " + std::to_string(copiedCount) + " converter files to work directory");

    if (m_cancelled) return;

    Q_EMIT progressUpdated(PHASE_PREPARE_WEIGHT, "Preparation complete");
    sak::logInfo("UUP build preparation complete");

    // Proceed to download phase
    executeDownload();
}

bool UupIsoBuilder::isFileAlreadyDownloaded(const UupDumpApi::FileInfo& fileInfo,
                                            const QString& downloadDir) const
{
    QString filePath = QDir(downloadDir).filePath(fileInfo.fileName);
    QFileInfo localFile(filePath);
    if (!localFile.exists() || !localFile.isFile())
        return false;

    // Size check — must match expected size (if known)
    if (fileInfo.size > 0 && localFile.size() != fileInfo.size)
        return false;

    // SHA-1 verification (if hash available)
    if (!fileInfo.sha1.isEmpty()) {
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly))
            return false;

        QCryptographicHash hasher(QCryptographicHash::Sha1);
        if (!hasher.addData(&f))
            return false;
        f.close();

        QString computedHash = hasher.result().toHex().toLower();
        if (computedHash != fileInfo.sha1.toLower())
            return false;
    }

    return true;
}

bool UupIsoBuilder::generateAria2InputFile(const QString& outputPath)
{
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
        return false;

    QTextStream stream(&file);
    int validFiles = 0;
    int skippedFiles = 0;
    qint64 skippedBytes = 0;

    // Determine the download directory (UUPs subfolder of work dir)
    QString downloadDir = QDir(m_workDir).filePath("UUPs");

    for (const auto& fileInfo : m_files) {
        if (fileInfo.url.isEmpty()) {
            sak::logWarning("Skipping file with empty URL: " + fileInfo.fileName.toStdString());
            continue;
        }

        // Check if file is already fully downloaded and verified
        if (isFileAlreadyDownloaded(fileInfo, downloadDir)) {
            skippedFiles++;
            skippedBytes += fileInfo.size;
            sak::logInfo("Already downloaded (verified): " + fileInfo.fileName.toStdString());
            continue;
        }

        stream << fileInfo.url << "\n";
        stream << "  out=" << fileInfo.fileName << "\n";

        if (!fileInfo.sha1.isEmpty()) {
            stream << "  checksum=sha-1=" << fileInfo.sha1 << "\n";
        }

        // Smaller files don't benefit from many connections; save slots for large files
        if (fileInfo.size > 0 && fileInfo.size < 5 * 1024 * 1024) {
            stream << "  split=1\n";
            stream << "  max-connection-per-server=1\n";
        }

        stream << "\n";
        validFiles++;
    }

    file.close();

    if (skippedFiles > 0) {
        double skippedMB = skippedBytes / (1024.0 * 1024.0);
        sak::logInfo("Resume: skipped " + std::to_string(skippedFiles) +
                      " already-downloaded files (" + std::to_string(static_cast<int>(skippedMB)) + " MB)");
        Q_EMIT progressUpdated(2, QString("Skipped %1 already-downloaded files (%2 MB)")
            .arg(skippedFiles).arg(skippedMB, 0, 'f', 0));
    }

    sak::logInfo("Generated aria2c input file: " + std::to_string(validFiles) +
                  " files to download, " + std::to_string(skippedFiles) + " already complete");

    // If all files are already downloaded, no need to run aria2c
    if (validFiles == 0 && skippedFiles > 0) {
        sak::logInfo("All files already downloaded — skipping aria2c");
        return true;  // Still return true; caller should check if file has content
    }

    return validFiles > 0;
}

// ============================================================================
// Phase 2: Download via aria2c
// ============================================================================

void UupIsoBuilder::executeDownload()
{
    if (m_cancelled) return;

    m_phase = Phase::DownloadingFiles;
    Q_EMIT phaseChanged(Phase::DownloadingFiles, "Downloading Windows UUP files...");
    m_phaseTimer.start();

    // If all files were already downloaded and verified, skip aria2c entirely
    if (m_allFilesAlreadyDownloaded) {
        sak::logInfo("All UUP files already present — skipping download phase");
        Q_EMIT progressUpdated(PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT,
                               "All files already downloaded — proceeding to conversion");
        executeConversion();
        return;
    }

    QString aria2Path = findAria2Path();
    QString inputFile = QDir(m_workDir).filePath("aria2_script.txt");
    QString downloadDir = QDir(m_workDir).filePath("UUPs");

    m_aria2Process = std::make_unique<QProcess>(this);
    m_aria2Process->setWorkingDirectory(m_workDir);
    m_aria2Process->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_aria2Process.get(), &QProcess::readyReadStandardOutput,
            this, &UupIsoBuilder::onAria2ReadyRead);
    connect(m_aria2Process.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UupIsoBuilder::onAria2Finished);

    QStringList args;
    args << "--input-file=" + inputFile
         << "--dir=" + downloadDir
         // ── User-Agent (critical: Microsoft CDN may block aria2c UA) ──
         << "--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
            "AppleWebKit/537.36 (KHTML, like Gecko) "
            "Chrome/131.0.0.0 Safari/537.36"
         // ── Parallelism ──
         << "--max-connection-per-server=16"
         << "--split=16"
         << "--min-split-size=1M"
         << "--max-concurrent-downloads=10"
         // ── Resumability & integrity ──
         << "--continue=true"
         << "--check-integrity=true"
         << "--auto-file-renaming=false"
         << "--allow-overwrite=true"
         // ── Performance tuning ──
         << "--file-allocation=none"
         << "--disk-cache=64M"
         << "--optimize-concurrent-downloads=true"
         << "--stream-piece-selector=inorder"
         << "--piece-length=1M"
         // ── Stall & retry handling ──
         << "--lowest-speed-limit=50K"
         << "--max-tries=5"
         << "--retry-wait=3"
         << "--connect-timeout=10"
         << "--timeout=60"
         << "--max-file-not-found=3"
         // ── Security ──
         // Microsoft CDN serves HTTP-only; certificate check applies to HTTPS only
         << "--check-certificate=false"
         // ── Output formatting ──
         << "--summary-interval=1"
         << "--human-readable=false"
         << "--enable-color=false"
         << "--console-log-level=notice";

    sak::logInfo("Starting aria2c download: " + aria2Path.toStdString());
    m_aria2Process->start(aria2Path, args);

    if (!m_aria2Process->waitForStarted(10000)) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to start aria2c: " + m_aria2Process->errorString());
        return;
    }

    // Start periodic progress polling (scans download directory for file sizes)
    m_progressPollTimer->start();
}

void UupIsoBuilder::onAria2ReadyRead()
{
    if (!m_aria2Process) return;

    QByteArray data = m_aria2Process->readAllStandardOutput();
    QString output = QString::fromUtf8(data);

    // aria2c uses \r for in-place progress updates
    QStringList lines = output.split(QRegularExpression("[\\r\\n]+"), Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        parseAria2Progress(line.trimmed());
    }
}

void UupIsoBuilder::parseAria2Progress(const QString& line)
{
    if (line.isEmpty()) return;

    // Parse download speed: [DL:12345678] (bytes/sec with --human-readable=false)
    static const QRegularExpression speedPattern(R"(\[DL:(\d+)\])");
    QRegularExpressionMatch speedMatch = speedPattern.match(line);
    if (speedMatch.hasMatch()) {
        qint64 bytesPerSec = speedMatch.captured(1).toLongLong();
        m_currentSpeedMBps = bytesPerSec / (1024.0 * 1024.0);
        Q_EMIT speedUpdated(m_currentSpeedMBps);
    }

    // Parse human-readable speed fallback: DL:50MiB or DL:1.2GiB
    if (!speedMatch.hasMatch()) {
        static const QRegularExpression hrSpeedPattern(
            R"(DL:([0-9.]+)\s*(KiB|MiB|GiB))");
        QRegularExpressionMatch hrMatch = hrSpeedPattern.match(line);
        if (hrMatch.hasMatch()) {
            double val = hrMatch.captured(1).toDouble();
            QString unit = hrMatch.captured(2);
            if (unit == "KiB") val /= 1024.0;
            else if (unit == "GiB") val *= 1024.0;
            m_currentSpeedMBps = val;
            Q_EMIT speedUpdated(m_currentSpeedMBps);
        }
    }

    // Log significant aria2c messages
    if (line.contains("Download complete:", Qt::CaseInsensitive)) {
        sak::logInfo("aria2c: " + line.toStdString());
    }

    // Detect errors
    if (line.contains("Exception caught", Qt::CaseInsensitive) ||
        (line.contains("error", Qt::CaseInsensitive) &&
         line.contains("code=", Qt::CaseInsensitive) &&
         !line.contains("code=0", Qt::CaseInsensitive))) {
        sak::logWarning("aria2c: " + line.toStdString());
    }
}

void UupIsoBuilder::onProgressPollTimer()
{
    if (m_phase == Phase::DownloadingFiles && m_totalDownloadBytes > 0) {
        // Scan download directory to compute actual overall progress
        QString downloadDir = QDir(m_workDir).filePath("UUPs");
        QDir dir(downloadDir);
        qint64 totalDownloaded = 0;

        QDirIterator it(dir.absolutePath(), QDir::Files);
        while (it.hasNext()) {
            it.next();
            totalDownloaded += it.fileInfo().size();
        }

        m_downloadedBytes = totalDownloaded;
        m_downloadPercent = static_cast<int>(
            (totalDownloaded * 100) / m_totalDownloadBytes);
        if (m_downloadPercent > 100) m_downloadPercent = 100;

        // Build progress detail string
        double downloadedGB = totalDownloaded / (1024.0 * 1024.0 * 1024.0);
        double totalGB = m_totalDownloadBytes / (1024.0 * 1024.0 * 1024.0);
        QString detail = QString("Downloaded %1 GB / %2 GB")
                             .arg(downloadedGB, 0, 'f', 2)
                             .arg(totalGB, 0, 'f', 2);

        if (m_currentSpeedMBps > 0.01) {
            double remainingMB =
                (m_totalDownloadBytes - totalDownloaded) / (1024.0 * 1024.0);
            int etaSec = static_cast<int>(remainingMB / m_currentSpeedMBps);
            int etaMin = etaSec / 60;
            etaSec %= 60;
            detail += QString(" | %1 MB/s | ETA: %2:%3")
                          .arg(m_currentSpeedMBps, 0, 'f', 1)
                          .arg(etaMin)
                          .arg(etaSec, 2, 10, QChar('0'));
        }

        int overall = PHASE_PREPARE_WEIGHT +
                      (m_downloadPercent * PHASE_DOWNLOAD_WEIGHT / 100);
        Q_EMIT progressUpdated(overall, detail);

    } else if (m_phase == Phase::ConvertingToISO) {
        // Watchdog: if converter has already exited but finished() was not
        // observed yet, finalize through the normal completion handler.
        if (m_converterProcess &&
            m_converterProcess->state() == QProcess::NotRunning) {
            onConverterFinished(m_converterProcess->exitCode(),
                                m_converterProcess->exitStatus());
            return;
        }

        // During conversion, report output ISO growth at the destination path
        // (UUPMediaConverter writes directly to m_outputIsoPath).
        QFileInfo outputIso(m_outputIsoPath);
        if (outputIso.exists() && outputIso.size() > 0) {
            qint64 isoSize = outputIso.size();
            double sizeGB = isoSize / (1024.0 * 1024.0 * 1024.0);
            int conversionProgress = m_conversionPercent;
            if (conversionProgress >= 100) {
                conversionProgress = 99;
            }
            Q_EMIT progressUpdated(
                PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT +
                    (conversionProgress * PHASE_CONVERT_WEIGHT / 100),
                QString("Creating ISO (%1 GB)...").arg(sizeGB, 0, 'f', 2));
        } else if (m_converterProcess &&
                   m_converterProcess->state() == QProcess::Running) {
            int conversionProgress = m_conversionPercent;
            if (conversionProgress >= 100) {
                conversionProgress = 99;
            }
            int overall = PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT +
                          (conversionProgress * PHASE_CONVERT_WEIGHT / 100);
            Q_EMIT progressUpdated(
                overall,
                QString("Converting UUP files... (%1%)").arg(conversionProgress));
        }
    }
}

void UupIsoBuilder::onAria2Finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_progressPollTimer->stop();

    if (m_cancelled) return;

    if (exitStatus == QProcess::CrashExit) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("aria2c crashed unexpectedly during download");
        return;
    }

    if (exitCode != 0) {
        // aria2c exit code 7 = "unfinished downloads" which may be recoverable
        if (exitCode == 7) {
            sak::logWarning("aria2c: some downloads may be incomplete (exit code 7)");
        } else {
            m_phase = Phase::Failed;
            QString errorDetail;
            if (m_aria2Process) {
                errorDetail = QString::fromUtf8(
                    m_aria2Process->readAllStandardOutput()).right(500);
            }
            Q_EMIT buildError(
                QString("Download failed (aria2c exit code %1). %2")
                    .arg(exitCode)
                    .arg(errorDetail));
            return;
        }
    }

    // Verify at least some files were downloaded
    QString downloadDir = QDir(m_workDir).filePath("UUPs");
    QDir dir(downloadDir);
    int fileCount = 0;
    QDirIterator checkIt(dir.absolutePath(), QDir::Files);
    while (checkIt.hasNext()) {
        checkIt.next();
        fileCount++;
    }

    if (fileCount == 0) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("No files were downloaded. Check network connection "
                         "and that the selected build is still available.");
        return;
    }

    m_downloadPercent = 100;
    sak::logInfo("UUP file download complete: " +
                  std::to_string(fileCount) + " files");

    Q_EMIT progressUpdated(PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT,
                           "Download complete. Starting conversion...");

    executeConversion();
}

// ============================================================================
// Phase 3: UUP → ISO Conversion
// ============================================================================

void UupIsoBuilder::executeConversion()
{
    if (m_cancelled) return;

    m_phase = Phase::ConvertingToISO;
    Q_EMIT phaseChanged(Phase::ConvertingToISO, "Converting UUP files to bootable ISO...");
    m_phaseTimer.start();
    m_conversionPercent = 0;

    if (!isRunningAsAdmin()) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "Conversion requires elevated privileges (Administrator). "
            "The UUP converter performs offline AppX provisioning with DISM, "
            "which fails without elevation. Restart S.A.K. Utility as "
            "Administrator and try again.");
        return;
    }

    // Native-separator path to the UUPs download folder.
    QString uupsDir = QDir::toNativeSeparators(
        QDir(m_workDir).filePath("UUPs"));
    QString nativeWorkDir = QDir::toNativeSeparators(m_workDir);
    QString conversionTempDir = QDir(m_workDir).filePath("c");
    QDir conversionDir(conversionTempDir);
    if (conversionDir.exists()) {
        conversionDir.removeRecursively();
    }
    if (!QDir().mkpath(conversionTempDir)) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to create conversion temp directory: " + conversionTempDir);
        return;
    }
    QString nativeConversionTempDir = QDir::toNativeSeparators(conversionTempDir);
    QString uupMediaConverter = findUupMediaConverterPath();
    QString outputIsoPath = QDir::toNativeSeparators(
        QFileInfo(m_outputIsoPath).absoluteFilePath());
    m_usingUupMediaConverter = QFileInfo::exists(uupMediaConverter);
    if (!m_usingUupMediaConverter) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "UUPMediaConverter.exe was not found in bundled tools. "
            "Conversion is configured to use UUPMediaConverter only.");
        return;
    }

    // Ensure destination directory exists before converter starts.
    QFileInfo outputInfo(outputIsoPath);
    if (!QDir().mkpath(outputInfo.absolutePath())) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to create output directory: " +
                          outputInfo.absolutePath());
        return;
    }

    // Remove pre-existing output file to avoid converter confusion.
    if (QFile::exists(outputIsoPath)) {
        QFile::remove(outputIsoPath);
    }

    m_converterProcess = std::make_unique<QProcess>(this);
    m_converterProcess->setWorkingDirectory(QFileInfo(uupMediaConverter).absolutePath());
    m_converterProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_converterProcess.get(), &QProcess::readyReadStandardOutput,
            this, &UupIsoBuilder::onConverterReadyRead);
    connect(m_converterProcess.get(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UupIsoBuilder::onConverterFinished);
    connect(m_converterProcess.get(), &QProcess::started, this, [this]() {
        sak::logInfo("UUPMediaConverter process started");
        Q_EMIT progressUpdated(
            PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT + 1,
            "UUP conversion started...");
    });
    connect(m_converterProcess.get(), &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        QString errorText;
        switch (error) {
        case QProcess::FailedToStart:
            errorText = "converter failed to start";
            break;
        case QProcess::Crashed:
            errorText = "converter process crashed";
            break;
        case QProcess::Timedout:
            errorText = "converter process timed out";
            break;
        case QProcess::WriteError:
            errorText = "converter process write error";
            break;
        case QProcess::ReadError:
            errorText = "converter process read error";
            break;
        case QProcess::UnknownError:
        default:
            errorText = "converter process unknown error";
            break;
        }
        sak::logError("UUPMediaConverter error: " + errorText.toStdString() +
                      " - " + m_converterProcess->errorString().toStdString());
        Q_EMIT progressUpdated(
            PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT,
            "Converter runtime error: " + errorText);
    });

    // Set environment for non-interactive execution
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("UUP_AUTOMATIC", "1");
    m_converterProcess->setProcessEnvironment(env);

    // Upstream OSTooling converter path:
    // UUPMediaConverter desktop-convert -u <UUPs> -i <ISO> -l <lang> -e <edition>
    QStringList args;
    args << "desktop-convert"
         << "-u" << uupsDir
            << "-i" << outputIsoPath
         << "-l" << m_lang
                << "-t" << nativeConversionTempDir
            << "--no-key-prompt";
    if (!m_edition.isEmpty()) {
        args << "-e" << m_edition;
    }

    sak::logInfo("Starting UUPMediaConverter: " +
                 uupMediaConverter.toStdString() +
                 " desktop-convert -u \"" + uupsDir.toStdString() +
                 "\" -i \"" + outputIsoPath.toStdString() +
                 "\" -l " + m_lang.toStdString());
    m_converterProcess->start(uupMediaConverter, args);

    if (!m_converterProcess->waitForStarted(10000)) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Failed to start converter: " +
                         m_converterProcess->errorString());
        return;
    }

    // Resume progress polling to track ISO file creation
    m_progressPollTimer->start();
}

void UupIsoBuilder::onConverterReadyRead()
{
    if (!m_converterProcess) return;

    QByteArray data = m_converterProcess->readAllStandardOutput();
    QString output = QString::fromUtf8(data);
    if (!output.isEmpty()) {
        m_converterOutputTail += output;
        constexpr int kMaxTailChars = 16000;
        if (m_converterOutputTail.size() > kMaxTailChars) {
            m_converterOutputTail = m_converterOutputTail.right(kMaxTailChars);
        }
    }

    QStringList lines = output.split(QRegularExpression("[\\r\\n]+"),
                                     Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        parseConverterProgress(trimmed);
    }
}

void UupIsoBuilder::parseConverterProgress(const QString& line)
{
    if (line.isEmpty()) return;

    sak::logDebug("Converter: " + line.toStdString());

    // ---- Parse stage-specific messages ----
    bool hasPercent = false;
    QString detail;

    // UUPMediaConverter emits tagged stage percentages like:
    // [PreparingFiles][73%] ...
    // [CreatingWindowsInstaller][52%] ...
    // [CreatingISO][15%] ...
    static const QRegularExpression stagePercentPattern(
        R"(\[(PreparingFiles|CreatingWindowsInstaller|CreatingISO)\]\[(\d{1,3})%\])",
        QRegularExpression::CaseInsensitiveOption);
    QRegularExpressionMatch stageMatch = stagePercentPattern.match(line);
    if (stageMatch.hasMatch()) {
        const QString stage = stageMatch.captured(1).toLower();
        const int stagePct = stageMatch.captured(2).toInt();
        int mappedProgress = m_conversionPercent;

        if (stage == "creatingwindowsinstaller") {
            mappedProgress = 5 + (stagePct * 45 / 100);   // 5..50
            detail = QString("Creating Windows installer image... (%1%)").arg(stagePct);
        } else if (stage == "preparingfiles") {
            mappedProgress = 50 + (stagePct * 40 / 100);  // 50..90
            detail = QString("Preparing conversion files... (%1%)").arg(stagePct);
        } else if (stage == "creatingiso") {
            mappedProgress = 90 + (stagePct * 9 / 100);   // 90..99
            detail = QString("Creating bootable ISO image... (%1%)").arg(stagePct);
        }

        if (mappedProgress > m_conversionPercent) {
            m_conversionPercent = mappedProgress;
        }
        hasPercent = true;
    } else if (line.contains("Exporting image", Qt::CaseInsensitive) ||
               line.contains("Applying image", Qt::CaseInsensitive)) {
        static const QRegularExpression imagePattern(
            R"(image\s+(\d+)\s+of\s+(\d+))", QRegularExpression::CaseInsensitiveOption);
        QRegularExpressionMatch imgMatch = imagePattern.match(line);
        if (imgMatch.hasMatch()) {
            detail = QString("Processing image %1 of %2")
                         .arg(imgMatch.captured(1), imgMatch.captured(2));
            int current = imgMatch.captured(1).toInt();
            int total = imgMatch.captured(2).toInt();
            if (total > 0) {
                int mapped = 20 + (current * 30 / total); // 20..50
                if (mapped > m_conversionPercent) {
                    m_conversionPercent = mapped;
                }
                hasPercent = true;
            }
        } else {
            detail = "Processing Windows images...";
            if (m_conversionPercent < 20) {
                m_conversionPercent = 20;
            }
        }
    } else if (line.contains("[ReadingMetadata]", Qt::CaseInsensitive)) {
        detail = "Reading build metadata...";
        if (m_conversionPercent < 5) {
            m_conversionPercent = 5;
        }
    } else if (line.contains("[ApplyingImage]", Qt::CaseInsensitive)) {
        detail = "Applying Windows image...";
        if (m_conversionPercent < 45) {
            m_conversionPercent = 45;
        }
    } else if (line.contains("Done", Qt::CaseInsensitive) &&
               line.length() < 30) {
        detail = "Conversion complete";
        m_conversionPercent = 100;
    }

    if (detail.isEmpty() && hasPercent) {
        int visiblePercent = m_conversionPercent;
        if (visiblePercent >= 100) {
            visiblePercent = 99;
        }
        detail = QString("Converting UUP files... (%1%)").arg(visiblePercent);
    }

    // Log warnings/errors
    if (line.contains("error", Qt::CaseInsensitive) &&
        !line.contains("errorlevel", Qt::CaseInsensitive) &&
        !line.contains("if error", Qt::CaseInsensitive)) {
        sak::logWarning("Converter: " + line.toStdString());
    }

    if (!detail.isEmpty()) {
        int conversionProgress = m_conversionPercent;
        if (conversionProgress >= 100) {
            conversionProgress = 99;
        }
        int overall = PHASE_PREPARE_WEIGHT + PHASE_DOWNLOAD_WEIGHT +
                      (conversionProgress * PHASE_CONVERT_WEIGHT / 100);
        Q_EMIT progressUpdated(overall, detail);
    }
}

void UupIsoBuilder::onConverterFinished(int exitCode,
                                        QProcess::ExitStatus exitStatus)
{
    m_progressPollTimer->stop();

    if (m_cancelled) return;

    if (exitStatus == QProcess::CrashExit) {
        m_phase = Phase::Failed;
        Q_EMIT buildError("Converter process crashed unexpectedly");
        return;
    }

    if (exitCode != 0) {
        m_phase = Phase::Failed;
        QString errorDetail = m_converterOutputTail.trimmed();
        if (errorDetail.isEmpty() && m_converterProcess) {
            errorDetail = QString::fromUtf8(
                m_converterProcess->readAllStandardOutput()).trimmed();
        }
        if (errorDetail.length() > 2000)
            errorDetail = errorDetail.right(2000);
        Q_EMIT buildError(
            QString("Converter failed with exit code %1. %2")
                .arg(exitCode)
                .arg(errorDetail));
        return;
    }

    sak::logInfo("UUP converter finished successfully");

    QFileInfo finalInfo(m_outputIsoPath);
    if (!finalInfo.exists() || finalInfo.size() <= 0) {
        QString tail = m_converterOutputTail.trimmed();
        if (tail.length() > 2000)
            tail = tail.right(2000);

        QString appxHint;
        if (tail.contains("external tool for appx installation", Qt::CaseInsensitive) ||
            tail.contains("appx", Qt::CaseInsensitive)) {
            appxHint =
                "\n\nDetected AppX provisioning failure. This step requires an "
                "elevated Administrator process.";
        }

        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "UUPMediaConverter reported success but no output ISO was created: " +
            m_outputIsoPath + appxHint + "\n\nLast output:\n" + tail);
        return;
    }

    qint64 fileSize = finalInfo.size();
    cleanupWorkDir();
    m_phase = Phase::Completed;
    Q_EMIT phaseChanged(Phase::Completed, "ISO build complete!");
    Q_EMIT progressUpdated(100, "ISO build complete!");
    Q_EMIT buildCompleted(m_outputIsoPath, fileSize);
    sak::logInfo("UUP ISO build complete: " + m_outputIsoPath.toStdString() +
                 " (" + std::to_string(fileSize / (1024 * 1024)) + " MB)");
}

// ============================================================================
// Phase 4: Finalization
// ============================================================================

void UupIsoBuilder::finalizeBuild()
{
    if (m_cancelled) return;

    Q_EMIT progressUpdated(98, "Locating generated ISO file...");

    // Search for ISO files in the work directory
    QDir workDir(m_workDir);
    QStringList isoFiles;
    QDirIterator it(workDir.absolutePath(), {"*.iso", "*.ISO"},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        isoFiles.append(it.next());
    }

    if (isoFiles.isEmpty()) {
        m_phase = Phase::Failed;
        Q_EMIT buildError(
            "No ISO file was created by the converter. "
            "The conversion may have failed silently. Check that the "
            "UUP files are valid and compatible with the selected edition.");
        return;
    }

    // Use the largest ISO file (in case converter created multiple)
    QString sourceIso;
    qint64 largestSize = 0;
    for (const QString& isoPath : isoFiles) {
        QFileInfo fi(isoPath);
        if (fi.size() > largestSize) {
            largestSize = fi.size();
            sourceIso = isoPath;
        }
    }

    sak::logInfo("Found generated ISO: " + sourceIso.toStdString() +
                  " (" + std::to_string(largestSize / (1024 * 1024)) + " MB)");

    Q_EMIT progressUpdated(99, "Moving ISO to destination...");

    // Ensure output directory exists
    QFileInfo outputInfo(m_outputIsoPath);
    QDir().mkpath(outputInfo.absolutePath());

    // Remove existing output file if present
    if (QFile::exists(m_outputIsoPath)) {
        if (!QFile::remove(m_outputIsoPath)) {
            sak::logWarning("Failed to remove existing output file: " +
                          m_outputIsoPath.toStdString());
        }
    }

    // Try rename first (fast, same-volume), fall back to copy (cross-volume)
    bool moved = QFile::rename(sourceIso, m_outputIsoPath);
    if (!moved) {
        sak::logInfo("Cross-volume move detected, copying ISO file...");
        Q_EMIT progressUpdated(99, "Copying ISO to destination (this may take a moment)...");

        if (!QFile::copy(sourceIso, m_outputIsoPath)) {
            m_phase = Phase::Failed;
            Q_EMIT buildError(
                QString("Failed to copy ISO to destination: %1")
                    .arg(m_outputIsoPath));
            return;
        }
        QFile::remove(sourceIso);
    }

    // Get final file info
    QFileInfo finalInfo(m_outputIsoPath);
    qint64 fileSize = finalInfo.size();

    // Clean up work directory (removes several GB of temp files)
    cleanupWorkDir();

    // Success!
    m_phase = Phase::Completed;
    Q_EMIT phaseChanged(Phase::Completed, "ISO build complete!");
    Q_EMIT progressUpdated(100, "ISO build complete!");
    Q_EMIT buildCompleted(m_outputIsoPath, fileSize);

    sak::logInfo("UUP ISO build complete: " + m_outputIsoPath.toStdString() +
                  " (" + std::to_string(fileSize / (1024 * 1024)) + " MB)");
}

bool UupIsoBuilder::isRunningAsAdmin()
{
#ifdef Q_OS_WIN
    BOOL isAdmin = FALSE;
    PSID adminGroup = nullptr;
    SID_IDENTIFIER_AUTHORITY ntAuthority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&ntAuthority, 2,
                                  SECURITY_BUILTIN_DOMAIN_RID,
                                  DOMAIN_ALIAS_RID_ADMINS,
                                  0, 0, 0, 0, 0, 0, &adminGroup)) {
        CheckTokenMembership(nullptr, adminGroup, &isAdmin);
        FreeSid(adminGroup);
    }
    return isAdmin != FALSE;
#else
    return geteuid() == 0;
#endif
}

void UupIsoBuilder::cleanupWorkDir()
{
    if (m_workDir.isEmpty()) return;

    QDir workDir(m_workDir);
    if (workDir.exists()) {
        sak::logInfo("Cleaning up work directory: " + m_workDir.toStdString());
        if (!workDir.removeRecursively()) {
            sak::logWarning("Could not fully remove work directory "
                          "(files may be in use): " +
                          m_workDir.toStdString());
        }
    }
    m_workDir.clear();
}

void UupIsoBuilder::updateOverallProgress()
{
    int overall = PHASE_PREPARE_WEIGHT;

    if (m_phase == Phase::DownloadingFiles ||
        m_phase == Phase::ConvertingToISO ||
        m_phase == Phase::Completed) {
        overall += (m_downloadPercent * PHASE_DOWNLOAD_WEIGHT / 100);
    }

    if (m_phase == Phase::ConvertingToISO ||
        m_phase == Phase::Completed) {
        overall += (m_conversionPercent * PHASE_CONVERT_WEIGHT / 100);
    }

    if (overall > 100) overall = 100;

    // This method is used for internal state tracking;
    // actual emission happens in the specific phase handlers
}
