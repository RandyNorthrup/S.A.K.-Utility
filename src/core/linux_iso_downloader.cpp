// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file linux_iso_downloader.cpp
 * @brief Linux ISO download orchestrator using bundled aria2c
 *
 * Downloads Linux distribution ISOs directly from official sources using
 * multi-connection aria2c for maximum throughput. Verifies SHA256/SHA1
 * checksums post-download when available.
 *
 * Unlike the Windows ISO downloader which assembles ISOs from UUP files,
 * Linux ISOs are single-file direct downloads — simpler pipeline.
 */

#include "sak/linux_iso_downloader.h"
#include "sak/bundled_tools_manager.h"
#include "sak/logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QNetworkRequest>
#include <QNetworkReply>

namespace {
QString formatSize(qint64 bytes)
{
    if (bytes < 1024) return QString("%1 B").arg(bytes);
    if (bytes < 1024 * 1024) return QString("%1 KB").arg(bytes / 1024.0, 0, 'f', 1);
    if (bytes < 1024LL * 1024 * 1024) return QString("%1 MB").arg(bytes / (1024.0 * 1024.0), 0, 'f', 1);
    return QString("%1 GB").arg(bytes / (1024.0 * 1024.0 * 1024.0), 0, 'f', 2);
}
} // anonymous namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

LinuxISODownloader::LinuxISODownloader(QObject* parent)
    : QObject(parent)
    , m_catalog(std::make_unique<LinuxDistroCatalog>(this))
    , m_progressTimer(new QTimer(this))
{
    m_progressTimer->setInterval(1000); // 1-second progress polling
    connect(m_progressTimer, &QTimer::timeout,
            this, &LinuxISODownloader::onProgressPollTimer);

    // Connect catalog signals for GitHub version checking
    connect(m_catalog.get(), &LinuxDistroCatalog::versionCheckCompleted,
            this, &LinuxISODownloader::onVersionCheckCompleted);
    connect(m_catalog.get(), &LinuxDistroCatalog::versionCheckFailed,
            this, &LinuxISODownloader::onVersionCheckFailed);

    sak::log_info("LinuxISODownloader initialized");
}

LinuxISODownloader::~LinuxISODownloader()
{
    cancel();
}

// ============================================================================
// Download Entry Point
// ============================================================================

void LinuxISODownloader::startDownload(const QString& distroId,
                                       const QString& savePath)
{
    if (isDownloading()) {
        Q_EMIT downloadError("A download is already in progress");
        return;
    }

    m_cancelled = false;
    m_currentDistroId = distroId;
    m_savePath = savePath;

    auto distro = m_catalog->distroById(distroId);
    if (distro.id.isEmpty()) {
        Q_EMIT downloadError("Unknown distribution: " + distroId);
        return;
    }

    sak::log_info("Starting Linux ISO download: " + distro.name.toStdString() +
                  " " + distro.version.toStdString());

    // For GitHub-hosted distros, check latest version first
    if (distro.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease) {
        setPhase(Phase::ResolvingVersion, "Checking for latest version...");
        Q_EMIT statusMessage(QString("Checking latest %1 release...").arg(distro.name));
        m_catalog->checkLatestVersion(distroId);
    } else {
        // Direct URL / SourceForge — resolve immediately and download
        m_downloadUrl = m_catalog->resolveDownloadUrl(distro);
        m_checksumUrl = m_catalog->resolveChecksumUrl(distro);
        m_checksumType = distro.checksumType;
        m_expectedFileName = m_catalog->resolveFileName(distro);
        m_totalSize = distro.approximateSize;

        if (m_downloadUrl.isEmpty()) {
            Q_EMIT downloadError("Could not resolve download URL for " + distro.name);
            return;
        }

        startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
    }
}

// ============================================================================
// Version Check Callbacks
// ============================================================================

void LinuxISODownloader::onVersionCheckCompleted(
    const QString& distroId,
    const LinuxDistroCatalog::DistroInfo& distro,
    bool changed)
{
    if (distroId != m_currentDistroId || m_cancelled) return;

    if (changed) {
        Q_EMIT statusMessage(QString("Found latest version: %1").arg(distro.version));
    }

    m_downloadUrl = m_catalog->resolveDownloadUrl(distro);
    m_checksumUrl = m_catalog->resolveChecksumUrl(distro);
    m_checksumType = distro.checksumType;
    m_expectedFileName = m_catalog->resolveFileName(distro);
    m_totalSize = distro.approximateSize;

    if (m_downloadUrl.isEmpty()) {
        setPhase(Phase::Failed, "Download URL not available");
        Q_EMIT downloadError("Could not resolve download URL for " + distro.name +
                            ". The GitHub release may not contain an ISO asset.");
        return;
    }

    sak::log_info("Resolved download URL: " + m_downloadUrl.toStdString());
    startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
}

void LinuxISODownloader::onVersionCheckFailed(const QString& distroId,
                                               const QString& error)
{
    if (distroId != m_currentDistroId || m_cancelled) return;

    // Fall back to hardcoded version
    auto distro = m_catalog->distroById(distroId);
    sak::log_warning("Version check failed for " + distroId.toStdString() +
                    ": " + error.toStdString() + " — using hardcoded version");

    Q_EMIT statusMessage("Version check failed — using known version " + distro.version);

    // Try to construct URL from known version
    m_downloadUrl = m_catalog->resolveDownloadUrl(distro);
    m_checksumUrl = m_catalog->resolveChecksumUrl(distro);
    m_checksumType = distro.checksumType;
    m_expectedFileName = m_catalog->resolveFileName(distro);
    m_totalSize = distro.approximateSize;

    if (m_downloadUrl.isEmpty()) {
        setPhase(Phase::Failed, "Download URL not available");
        Q_EMIT downloadError("Could not resolve download URL for " + distro.name);
        return;
    }

    startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
}

// ============================================================================
// aria2c Download
// ============================================================================

void LinuxISODownloader::startAria2cDownload(const QString& url,
                                             const QString& savePath,
                                             const QString& fileName)
{
    if (m_cancelled) return;

    // Find aria2c
    QString aria2Path = findAria2c();
    if (aria2Path.isEmpty()) {
        setPhase(Phase::Failed, "aria2c not found");
        Q_EMIT downloadError(
            "aria2c.exe not found in bundled tools. "
            "Run scripts/bundle_uup_tools.ps1 and rebuild the application.");
        return;
    }

    setPhase(Phase::Downloading, "Downloading ISO...");
    Q_EMIT statusMessage(QString("Downloading %1...").arg(fileName));

    // Validate download URL scheme — only allow HTTPS
    QUrl downloadUrl(url);
    if (!downloadUrl.isValid() || downloadUrl.scheme().toLower() != "https") {
        Q_EMIT downloadError("Rejected non-HTTPS download URL: " + url);
        return;
    }

    // Determine output directory and filename
    QFileInfo saveInfo(savePath);
    QString outDir = saveInfo.absolutePath();
    QString outFile = saveInfo.fileName();

    // Create output directory if needed
    QDir().mkpath(outDir);

    // Build aria2c arguments for single-file download
    // Clean up any previous QProcess to prevent leaks
    if (m_aria2cProcess) {
        m_aria2cProcess->disconnect();
        m_aria2cProcess->deleteLater();
        m_aria2cProcess = nullptr;
    }
    m_aria2cProcess = new QProcess(this);
    m_aria2cProcess->setProcessChannelMode(QProcess::MergedChannels);

    connect(m_aria2cProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &LinuxISODownloader::onAria2cFinished);

    QStringList args;
    args << url
         << "--dir=" + outDir
         << "--out=" + outFile
         // ── Parallelism ──
         << "--max-connection-per-server=16"
         << "--split=16"
         << "--min-split-size=1M"
         // ── Resumability ──
         << "--continue=true"
         << "--auto-file-renaming=false"
         << "--allow-overwrite=true"
         // ── Performance tuning ──
         << "--file-allocation=none"
         << "--disk-cache=64M"
         << "--piece-length=1M"
         // ── Stall & retry handling ──
         << "--lowest-speed-limit=50K"
         << "--max-tries=5"
         << "--retry-wait=3"
         << "--connect-timeout=10"
         << "--timeout=60"
         << "--max-file-not-found=3"
         // ── TLS ──
         << "--check-certificate=true"
         // ── Output formatting ──
         << "--summary-interval=1"
         << "--human-readable=false"
         << "--enable-color=false"
         << "--console-log-level=notice";

    sak::log_info("Starting aria2c: " + aria2Path.toStdString() +
                  " → " + savePath.toStdString());

    m_aria2cProcess->start(aria2Path, args);

    if (!m_aria2cProcess->waitForStarted(10000)) {
        setPhase(Phase::Failed, "Failed to start aria2c");
        Q_EMIT downloadError("Failed to start aria2c: " + m_aria2cProcess->errorString());
        return;
    }

    // Start progress polling
    m_progressTimer->start();
}

void LinuxISODownloader::onAria2cFinished(int exitCode,
                                          QProcess::ExitStatus exitStatus)
{
    m_progressTimer->stop();

    // Read any remaining output
    if (m_aria2cProcess) {
        QString output = QString::fromUtf8(m_aria2cProcess->readAllStandardOutput());
        if (!output.trimmed().isEmpty()) {
            sak::log_info("aria2c final output: " + output.trimmed().toStdString());
        }
    }

    if (m_cancelled) {
        cleanupPartialFiles();
        return;
    }

    if (exitStatus == QProcess::CrashExit) {
        setPhase(Phase::Failed, "aria2c crashed");
        Q_EMIT downloadError("aria2c crashed unexpectedly during download");
        return;
    }

    if (exitCode != 0) {
        QString errorMsg;
        switch (exitCode) {
        case 1:  errorMsg = "Unknown error occurred"; break;
        case 2:  errorMsg = "Connection timed out"; break;
        case 3:  errorMsg = "Resource not found (404)"; break;
        case 4:  errorMsg = "Max retries reached — check your internet connection"; break;
        case 5:  errorMsg = "Download speed too slow"; break;
        case 6:  errorMsg = "Network error"; break;
        case 7:  errorMsg = "Download incomplete — some files could not be finished"; break;
        case 9:  errorMsg = "Disk space insufficient"; break;
        case 13: errorMsg = "File already exists and could not be overwritten"; break;
        case 24: errorMsg = "DNS resolution failed"; break;
        default: errorMsg = QString("aria2c exited with code %1").arg(exitCode); break;
        }

        sak::log_error("aria2c failed: " + errorMsg.toStdString());
        setPhase(Phase::Failed, errorMsg);
        Q_EMIT downloadError(errorMsg);
        return;
    }

    // Download succeeded — verify file exists
    QFileInfo downloadedFile(m_savePath);
    if (!downloadedFile.exists() || downloadedFile.size() == 0) {
        setPhase(Phase::Failed, "Downloaded file is missing or empty");
        Q_EMIT downloadError("The downloaded file could not be found after aria2c completed. "
                            "The server may have returned an error page instead of the ISO.");
        return;
    }

    sak::log_info("Download complete: " + m_savePath.toStdString() +
                  " (" + std::to_string(downloadedFile.size() / (1024 * 1024)) + " MB)");

    // Proceed to checksum verification if available
    if (!m_checksumUrl.isEmpty() && !m_checksumType.isEmpty()) {
        verifyChecksum();
    } else {
        // No checksum available — complete without verification
        setPhase(Phase::Completed, "Download complete (no checksum verification available)");
        Q_EMIT statusMessage("Download complete — no checksum available for this distribution");
        Q_EMIT downloadComplete(m_savePath, downloadedFile.size());
    }
}

// ============================================================================
// Progress Polling
// ============================================================================

void LinuxISODownloader::onProgressPollTimer()
{
    if (m_phase != Phase::Downloading) return;

    // Read aria2c stdout for progress info
    if (!m_aria2cProcess) return;

    QByteArray data = m_aria2cProcess->readAllStandardOutput();
    if (data.isEmpty()) return;

    QString output = QString::fromUtf8(data);
    QStringList lines = output.split(QRegularExpression("[\r\n]"),
                                     Qt::SkipEmptyParts);

    for (const QString& rawLine : lines) {
        QString line = rawLine.trimmed();

        // aria2c progress lines look like:
        // [#abcdef 1234567/9876543(12%) CN:16 DL:45.2MiB]
        // or in human-readable=false mode:
        // [#abcdef 1234567/9876543(12%) CN:16 DL:47394816]
        static QRegularExpression progressRegex(
            R"(\[#\w+\s+(\d+)/(\d+)\((\d+)%\).*DL:(\S+)\])");

        auto match = progressRegex.match(line);
        if (match.hasMatch()) {
            qint64 downloaded = match.captured(1).toLongLong();
            qint64 total = match.captured(2).toLongLong();
            int percent = match.captured(3).toInt();
            QString dlSpeedStr = match.captured(4);

            // Parse speed — could be bytes or human-readable
            double speedMBps = 0.0;
            if (dlSpeedStr.endsWith("MiB")) {
                speedMBps = dlSpeedStr.chopped(3).toDouble();
            } else if (dlSpeedStr.endsWith("KiB")) {
                speedMBps = dlSpeedStr.chopped(3).toDouble() / 1024.0;
            } else if (dlSpeedStr.endsWith("GiB")) {
                speedMBps = dlSpeedStr.chopped(3).toDouble() * 1024.0;
            } else {
                // Raw bytes
                speedMBps = dlSpeedStr.toDouble() / (1024.0 * 1024.0);
            }

            QString detail = QString("%1 / %2")
                .arg(formatSize(downloaded), formatSize(total));

            Q_EMIT progressUpdated(percent, detail);
            Q_EMIT speedUpdated(speedMBps);
            continue;
        }

        // Log significant messages
        if (line.contains("ERROR", Qt::CaseInsensitive) ||
            line.contains("WARNING", Qt::CaseInsensitive)) {
            sak::log_warning("aria2c: " + line.toStdString());
        }
    }
}

// ============================================================================
// Checksum Verification
// ============================================================================

void LinuxISODownloader::verifyChecksum()
{
    setPhase(Phase::VerifyingChecksum, "Verifying checksum...");
    Q_EMIT statusMessage("Downloading checksum file...");
    Q_EMIT progressUpdated(95, "Verifying integrity...");

    // Fetch checksum file
    auto* nam = new QNetworkAccessManager(this);
    QUrl checksumUrl(m_checksumUrl);
    QNetworkRequest request(checksumUrl);
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");

    auto* reply = nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam]() {
        reply->deleteLater();
        nam->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            sak::log_warning("Checksum fetch failed: " + reply->errorString().toStdString());
            // Don't fail the download — just skip verification
            Q_EMIT statusMessage("Checksum verification skipped (could not fetch checksum file)");

            QFileInfo fileInfo(m_savePath);
            setPhase(Phase::Completed, "Download complete (checksum fetch failed)");
            Q_EMIT downloadComplete(m_savePath, fileInfo.size());
            return;
        }

        QString checksumData = QString::fromUtf8(reply->readAll());
        QString expectedFileName = QFileInfo(m_savePath).fileName();

        // Parse checksum file — format is typically:
        // <hash>  <filename>   (Ubuntu SHA256SUMS)
        // <hash>  filename     (SystemRescue .sha256)
        // <hash>               (single-file checksum)
        QString expectedHash;
        QStringList checksumLines = checksumData.split('\n', Qt::SkipEmptyParts);

        for (const QString& line : checksumLines) {
            QString trimmed = line.trimmed();
            if (trimmed.isEmpty() || trimmed.startsWith('#')) continue;

            // Split on whitespace (hash  filename OR hash *filename)
            QStringList parts = trimmed.split(QRegularExpression("\\s+"),
                                              Qt::SkipEmptyParts);
            if (parts.size() >= 2) {
                QString filename = parts.last();
                // Remove leading * (binary mode indicator)
                if (filename.startsWith('*')) {
                    filename = filename.mid(1);
                }
                if (filename == expectedFileName) {
                    expectedHash = parts.first().toLower();
                    break;
                }
            } else if (parts.size() == 1) {
                // Single hash in file — assume it's for our file
                expectedHash = parts.first().toLower();
            }
        }

        if (expectedHash.isEmpty()) {
            sak::log_warning("Could not find matching hash in checksum file for: " +
                           expectedFileName.toStdString());
            Q_EMIT statusMessage("Checksum verification skipped (no matching entry found)");

            QFileInfo fileInfo(m_savePath);
            setPhase(Phase::Completed, "Download complete");
            Q_EMIT downloadComplete(m_savePath, fileInfo.size());
            return;
        }

        // Compute hash in background thread
        Q_EMIT statusMessage("Computing " + m_checksumType.toUpper() + " checksum...");
        Q_EMIT progressUpdated(97, "Computing checksum...");

        auto algorithm = (m_checksumType == "sha1")
            ? QCryptographicHash::Sha1
            : QCryptographicHash::Sha256;

        auto* watcher = new QFutureWatcher<QString>(this);
        connect(watcher, &QFutureWatcher<QString>::finished, this,
                [this, watcher, expectedHash]() {
            QString actualHash = watcher->result();
            watcher->deleteLater();

            bool match = (actualHash == expectedHash);
            onChecksumVerified(match, expectedHash, actualHash);
        });

        auto future = QtConcurrent::run([this, algorithm]() -> QString {
            QFile file(m_savePath);
            if (!file.open(QIODevice::ReadOnly)) {
                return QString();
            }

            QCryptographicHash hash(algorithm);
            const qint64 bufferSize = 8 * 1024 * 1024; // 8 MB chunks
            while (!file.atEnd()) {
                hash.addData(file.read(bufferSize));
            }
            return hash.result().toHex().toLower();
        });

        watcher->setFuture(future);
    });
}

void LinuxISODownloader::onChecksumVerified(bool match,
                                            const QString& expected,
                                            const QString& actual)
{
    QFileInfo fileInfo(m_savePath);

    if (actual.isEmpty()) {
        sak::log_warning("Failed to compute checksum for: " + m_savePath.toStdString());
        Q_EMIT statusMessage("Checksum verification skipped (file read error)");
        setPhase(Phase::Completed, "Download complete");
        Q_EMIT downloadComplete(m_savePath, fileInfo.size());
        return;
    }

    if (match) {
        sak::log_info("Checksum verified: " + actual.toStdString());
        Q_EMIT statusMessage(m_checksumType.toUpper() + " checksum verified successfully");
        setPhase(Phase::Completed, "Download complete — checksum verified");
        Q_EMIT downloadComplete(m_savePath, fileInfo.size());
    } else {
        sak::log_error("Checksum mismatch! Expected: " + expected.toStdString() +
                      " Actual: " + actual.toStdString());
        setPhase(Phase::Failed, "Checksum verification failed");

        // Remove the corrupted file
        QFile::remove(m_savePath);

        Q_EMIT downloadError(
            QString("Checksum verification failed!\n\n"
                    "Expected: %1\n"
                    "Actual:   %2\n\n"
                    "The downloaded file has been removed. "
                    "Please try downloading again.")
                .arg(expected, actual));
    }
}

// ============================================================================
// Cancel
// ============================================================================

void LinuxISODownloader::cancel()
{
    m_cancelled = true;
    m_progressTimer->stop();
    m_catalog->cancelAll();

    if (m_aria2cProcess && m_aria2cProcess->state() != QProcess::NotRunning) {
        m_aria2cProcess->kill();
        m_aria2cProcess->waitForFinished(5000);
    }

    setPhase(Phase::Idle, "Cancelled");
    Q_EMIT statusMessage("Download cancelled");
}

// ============================================================================
// Helpers
// ============================================================================

void LinuxISODownloader::setPhase(Phase phase, const QString& description)
{
    m_phase = phase;
    Q_EMIT phaseChanged(phase, description);
}

QString LinuxISODownloader::findAria2c() const
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

    sak::log_error("aria2c.exe not found in bundled tools");
    return {};
}

void LinuxISODownloader::cleanupPartialFiles()
{
    // Remove .aria2 control file
    QString aria2ControlFile = m_savePath + ".aria2";
    if (QFile::exists(aria2ControlFile)) {
        QFile::remove(aria2ControlFile);
        sak::log_info("Removed aria2 control file: " + aria2ControlFile.toStdString());
    }

    // Remove partial download
    if (QFile::exists(m_savePath)) {
        QFile::remove(m_savePath);
        sak::log_info("Removed partial download: " + m_savePath.toStdString());
    }
}
