// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file linux_iso_downloader.cpp
 * @brief Linux ISO download orchestrator using bundled aria2c
 *
 * Downloads Linux distribution ISOs directly from official sources using
 * multi-connection aria2c for maximum throughput. Verifies SHA256/SHA1
 * checksums post-download when available.
 *
 * Unlike the Windows ISO downloader which assembles ISOs from UUP files,
 * Linux ISOs are single-file direct downloads -- simpler pipeline.
 */

#include "sak/linux_iso_downloader.h"

#include "sak/bundled_tools_manager.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/network_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace {
constexpr qsizetype kAria2cBinaryUnitSuffixLength = 3;
constexpr int kAria2cDownloadedCaptureGroup = 1;
constexpr int kAria2cTotalCaptureGroup = 2;
constexpr int kAria2cPercentCaptureGroup = 3;
constexpr int kAria2cSpeedCaptureGroup = 4;
constexpr int kChecksumRecordMinimumParts = 2;
// Capture-group indices of the BSD-style checksum record regex
// "^([A-Za-z0-9-]+)\\s*\\(([^)]+)\\)\\s*=\\s*(\\S+)$": group 1 is the algorithm,
// group 2 the filename, group 3 the digest.
constexpr int kBsdRecordFileNameGroup = 2;
constexpr int kBsdRecordDigestGroup = 3;
constexpr qsizetype kSha256HexLength = 64;
constexpr qsizetype kSha1HexLength = 40;
constexpr int kChecksumComputingProgress = 97;
constexpr int kChecksumVerifyingProgress = 95;
constexpr qint64 kChecksumReadBufferSize = 8 * sak::kBytesPerMB;
// A checksum file (or a release note carrying the digest) is small; a peer that
// streams past this ceiling is refused rather than buffered into memory.
constexpr qint64 kMaxChecksumBytes = 16 * sak::kBytesPerMB;

QString formatSize(qint64 bytes) {
    return sak::formatBytes(bytes);
}

// Turn a failed checksum fetch into a message the user can act on. A mirror redirector
// (e.g. download.fedoraproject.org) can bounce an HTTPS request onto a plain-HTTP mirror;
// NoLessSafeRedirectPolicy refuses that downgrade, and Qt reports InsecureRedirectError.
// That refusal is a safety measure -- not the user's fault -- so name the cause plainly
// instead of surfacing a bare framework string the user cannot interpret.
QString checksumFetchErrorMessage(QNetworkReply* reply) {
    if (reply->error() == QNetworkReply::InsecureRedirectError) {
        return QStringLiteral(
            "Checksum download refused for safety: the download mirror redirected to an "
            "insecure (unencrypted HTTP) server, which was blocked so the checksum cannot be "
            "tampered with in transit. This is a mirror-side problem, not a problem with your "
            "settings -- please retry, which usually selects a different mirror.");
    }
    return QStringLiteral("Checksum fetch failed: ") + reply->errorString();
}

/// @brief Parse aria2c speed string (e.g. "2.3MiB", "512KiB") to MB/s
double parseAria2cSpeedMBps(const QString& dlSpeedStr) {
    if (dlSpeedStr.endsWith("MiB")) {
        return dlSpeedStr.chopped(kAria2cBinaryUnitSuffixLength).toDouble();
    }
    if (dlSpeedStr.endsWith("KiB")) {
        return dlSpeedStr.chopped(kAria2cBinaryUnitSuffixLength).toDouble() / sak::kBytesPerKBf;
    }
    if (dlSpeedStr.endsWith("GiB")) {
        return dlSpeedStr.chopped(kAria2cBinaryUnitSuffixLength).toDouble() * sak::kBytesPerKBf;
    }
    // Numeric aria2c value is raw bytes/sec.
    return dlSpeedStr.toDouble() / sak::kBytesPerMBf;
}
}  // anonymous namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

LinuxISODownloader::LinuxISODownloader(QObject* parent)
    : QObject(parent)
    , m_catalog(std::make_unique<LinuxDistroCatalog>(this))
    , m_progressTimer(new QTimer(this)) {
    m_progressTimer->setInterval(sak::kTimerProgressPollMs);  // 1-second progress polling
    connect(m_progressTimer, &QTimer::timeout, this, &LinuxISODownloader::onProgressPollTimer);

    // Connect catalog signals for GitHub version checking
    connect(m_catalog.get(),
            &LinuxDistroCatalog::versionCheckCompleted,
            this,
            &LinuxISODownloader::onVersionCheckCompleted);
    connect(m_catalog.get(),
            &LinuxDistroCatalog::versionCheckFailed,
            this,
            &LinuxISODownloader::onVersionCheckFailed);

    sak::logInfo("LinuxISODownloader initialized");
}

LinuxISODownloader::~LinuxISODownloader() {
    cancel();
    // Join the background hash task before members are destroyed: a QtConcurrent::run future is
    // non-cancelable and its lambda reads m_savePath, so tearing down while it runs is a UAF.
    // Wait only here (not in cancel()) so a live-object cancel never blocks the GUI thread.
    if (m_hashFuture.isRunning()) {
        // SAK-ALLOW-BLOCKING: a QtConcurrent::run future is not cancelable, so there is
        // nothing to give up TO -- the only alternative to waiting is the use-after-free
        // described above.
        m_hashFuture.waitForFinished();
    }
}

// ============================================================================
// Download Entry Point
// ============================================================================

void LinuxISODownloader::startDownload(const QString& distroId, const QString& savePath) {
    // The constructor creates m_catalog and nothing reassigns it. An empty or unknown distroId
    // is rejected below by the distroById lookup.
    Q_ASSERT(m_catalog);
    if (isDownloading()) {
        Q_EMIT downloadError("A download is already in progress");
        return;
    }

    m_cancelled = false;
    ++m_operationGeneration;  // supersede any stale in-flight verify from a prior run
    m_currentDistroId = distroId;
    m_savePath = savePath;
    m_rollingFilenamePattern.clear();  // set only for a rolling DirectURL distro below

    auto distro = m_catalog->distroById(distroId);
    if (distro.id.isEmpty()) {
        Q_EMIT downloadError("Unknown distribution: " + distroId);
        return;
    }

    sak::logInfo("Starting Linux ISO download: " + distro.name.toStdString() + " " +
                 distro.version.toStdString());

    // For GitHub-hosted distros, check latest version first
    if (distro.sourceType == LinuxDistroCatalog::SourceType::GitHubRelease) {
        setPhase(Phase::ResolvingVersion, "Checking for latest version...");
        Q_EMIT statusMessage(QString("Checking latest %1 release...").arg(distro.name));
        m_catalog->checkLatestVersion(distroId);
    } else {
        // Direct URL / SourceForge -- resolve immediately and download
        m_downloadUrl = m_catalog->resolveDownloadUrl(distro);
        m_checksumUrl = m_catalog->resolveChecksumUrl(distro);
        m_checksumType = distro.checksumType;
        m_expectedFileName = m_catalog->resolveFileName(distro);
        m_totalSize = distro.approximateSize;
        m_sourceType = distro.sourceType;
        m_rollingFilenamePattern = distro.rollingFilenamePattern;

        if (m_downloadUrl.isEmpty()) {
            Q_EMIT downloadError("Could not resolve download URL for " + distro.name);
            return;
        }
        if (!requirePinnedChecksum(distro.name)) {
            return;
        }

        if (m_rollingFilenamePattern.isEmpty()) {
            startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
        } else {
            // Rolling release: the pinned filename would 404, so derive the current
            // one from the checksum file first, then download (R5-G22-10).
            startRollingFilenameDiscovery();
        }
    }
}

bool LinuxISODownloader::requirePinnedChecksum(const QString& distroName) {
    // A downloaded ISO is written to bootable media, so an unverifiable one is refused up
    // front rather than downloaded and handed on with a warning. SourceForge downloads in
    // particular redirect from the initial HTTPS URL onto plain-HTTP mirrors where no TLS
    // certificate applies; the pinned checksum, fetched over HTTPS, is what makes that leg
    // safe. Without it there is nothing to verify against.
    if (m_checksumUrl.isEmpty() || m_checksumType.isEmpty()) {
        const QString error =
            "No pinned checksum is published for " + distroName +
            "; refusing to download an ISO whose integrity cannot be verified before it is "
            "written to removable media.";
        sak::logError(error.toStdString());
        setPhase(Phase::Failed, "No pinned checksum available");
        Q_EMIT downloadError(error);
        return false;
    }
    const QUrl checksumUrl(m_checksumUrl);
    if (!checksumUrl.isValid() || checksumUrl.scheme().toLower() != "https") {
        const QString error = "Rejected non-HTTPS checksum URL for " + distroName + ": " +
                              m_checksumUrl;
        sak::logError(error.toStdString());
        setPhase(Phase::Failed, "Checksum URL is not HTTPS");
        Q_EMIT downloadError(error);
        return false;
    }
    return true;
}

void LinuxISODownloader::startRollingFilenameDiscovery() {
    // Reuse the ResolvingVersion phase: to the UI this is still "working out what
    // to download" before the transfer begins.
    setPhase(Phase::ResolvingVersion, "Resolving current filename...");
    Q_EMIT statusMessage("Resolving the current image filename...");

    // Fetch the SAME pinned HTTPS checksum file the post-download verify uses, so
    // the current filename is derived from the project's own published manifest.
    // requirePinnedChecksum already validated the URL is HTTPS and well-formed.
    auto* nam = new QNetworkAccessManager(this);
    QNetworkRequest request{QUrl(m_checksumUrl)};
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");
    // An HTTPS request that redirects onto a plain-HTTP mirror is refused outright
    // rather than silently followed -- the discovered filename feeds a URL that is
    // written to removable media, so its transport must not be downgraded.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // Bound the transfer so a stalled peer aborts and surfaces an error via the finished
    // handler instead of hanging (the oversize guard below only fires once bytes arrive).
    request.setTransferTimeout(sak::kHttpMetadataTransferTimeoutMs);

    const quint64 generation = m_operationGeneration.load();
    auto* reply = nam->get(request);
    // Fail closed on an oversized response so readAll() can never buffer an
    // unbounded body into memory.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > kMaxChecksumBytes || total > kMaxChecksumBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, generation]() {
        onRollingFilenameReplyFinished(reply, nam, generation);
    });
}

void LinuxISODownloader::onRollingFilenameReplyFinished(QNetworkReply* reply,
                                                        QNetworkAccessManager* nam,
                                                        quint64 generation) {
    Q_ASSERT(reply);
    Q_ASSERT(nam);
    reply->deleteLater();
    nam->deleteLater();

    if (!shouldApplyVerifyResult(m_cancelled, generation, m_operationGeneration.load())) {
        return;  // cancelled, or superseded by a newer download
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString error = checksumFetchErrorMessage(reply);
        sak::logWarning(error.toStdString());
        setPhase(Phase::Failed, "Filename discovery failed");
        Q_EMIT downloadError(error);
        return;
    }

    const QString checksumData = QString::fromUtf8(reply->readAll());
    const QString filename = LinuxDistroCatalog::filenameFromChecksums(checksumData,
                                                                       m_rollingFilenamePattern);
    if (filename.isEmpty()) {
        const QString error =
            "Could not determine the current download filename from the published checksums for " +
            m_currentDistroId + "; refusing to guess a version that may not exist. Please retry.";
        sak::logWarning(error.toStdString());
        setPhase(Phase::Failed, "Filename discovery failed");
        Q_EMIT downloadError(error);
        return;
    }

    applyDiscoveredFilename(filename);
    startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
}

void LinuxISODownloader::applyDiscoveredFilename(const QString& filename) {
    // Swap ONLY the last path segment of the download URL and the save path to the
    // discovered filename, keeping the pinned host/directory and the user's chosen
    // folder. filenameFromChecksums already guaranteed a bare filename (no path
    // separators), so this cannot redirect the download elsewhere. The checksum
    // verify reads QFileInfo(m_savePath).fileName(), so all three must agree.
    m_expectedFileName = filename;

    const int urlSlash = static_cast<int>(m_downloadUrl.lastIndexOf('/'));
    if (urlSlash >= 0) {
        m_downloadUrl = m_downloadUrl.left(urlSlash + 1) + filename;
    }
    const QFileInfo saveInfo(m_savePath);
    m_savePath = saveInfo.absolutePath() + '/' + filename;

    sak::logInfo("Rolling-release filename resolved: " + filename.toStdString() + " -> " +
                 m_downloadUrl.toStdString());
}

// ============================================================================
// Version Check Callbacks
// ============================================================================

void LinuxISODownloader::onVersionCheckCompleted(const QString& distroId,
                                                 const LinuxDistroCatalog::DistroInfo& distro,
                                                 bool changed) {
    if (distroId != m_currentDistroId || m_cancelled) {
        return;
    }

    if (changed) {
        Q_EMIT statusMessage(QString("Found latest version: %1").arg(distro.version));
    }

    m_downloadUrl = m_catalog->resolveDownloadUrl(distro);
    m_checksumUrl = m_catalog->resolveChecksumUrl(distro);
    m_checksumType = distro.checksumType;
    m_expectedFileName = m_catalog->resolveFileName(distro);
    m_totalSize = distro.approximateSize;
    m_sourceType = distro.sourceType;

    if (m_downloadUrl.isEmpty()) {
        setPhase(Phase::Failed, "Download URL not available");
        Q_EMIT downloadError("Could not resolve download URL for " + distro.name +
                             ". The GitHub release may not contain an ISO asset.");
        return;
    }
    if (!requirePinnedChecksum(distro.name)) {
        return;
    }

    sak::logInfo("Resolved download URL: " + m_downloadUrl.toStdString());
    startAria2cDownload(m_downloadUrl, m_savePath, m_expectedFileName);
}

void LinuxISODownloader::onVersionCheckFailed(const QString& distroId, const QString& error) {
    // m_catalog is created by the constructor and never reassigned; it echoes back the id
    // startDownload handed to checkLatestVersion, and that id had already resolved to a distro.
    Q_ASSERT(m_catalog);
    Q_ASSERT(!distroId.isEmpty());
    if (distroId != m_currentDistroId || m_cancelled) {
        return;
    }

    auto distro = m_catalog->distroById(distroId);
    sak::logWarning("Version check failed for " + distroId.toStdString() + ": " +
                    error.toStdString());
    setPhase(Phase::Failed, "Version check failed");
    Q_EMIT downloadError(QString("Could not verify latest %1 release: %2").arg(distro.name, error));
}

// ============================================================================
// aria2c Download
// ============================================================================

void LinuxISODownloader::startAria2cDownload(const QString& url,
                                             const QString& savePath,
                                             const QString& fileName) {
    if (m_cancelled) {
        return;
    }

    QString aria2Path;
    if (!prepareAria2cDownload(url, fileName, &aria2Path)) {
        return;
    }

    const QFileInfo saveInfo(savePath);
    const QString outDir = saveInfo.absolutePath();
    const QString outFile = saveInfo.fileName();

    if (!QDir().mkpath(outDir)) {
        sak::logWarning("Failed to create ISO download directory: {}", outDir.toStdString());
    }

    resetAria2cProcess();
    connect(m_aria2cProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            &LinuxISODownloader::onAria2cFinished);
    connect(m_aria2cProcess, &QProcess::started, this, [this]() {
        if (!m_cancelled && m_phase == Phase::Downloading) {
            m_progressTimer->start();
        }
    });
    connect(m_aria2cProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart && m_phase == Phase::Downloading) {
            setPhase(Phase::Failed, "Failed to start aria2c");
            Q_EMIT downloadError("Failed to start aria2c: " + m_aria2cProcess->errorString());
        }
    });

    const QStringList args = buildAria2cArguments(url, outDir, outFile);

    sak::logInfo("Starting aria2c: " + aria2Path.toStdString() + " -> " + savePath.toStdString());

    m_aria2cProcess->start(aria2Path, args);
}

bool LinuxISODownloader::prepareAria2cDownload(const QString& url,
                                               const QString& fileName,
                                               QString* aria2Path) {
    *aria2Path = findAria2c();
    if (aria2Path->isEmpty()) {
        setPhase(Phase::Failed, "aria2c not found");
        Q_EMIT downloadError(
            "aria2c.exe not found in bundled tools. "
            "Run scripts/bundle_uup_tools.ps1 and rebuild the application.");
        return false;
    }

    setPhase(Phase::Downloading, "Downloading ISO...");
    Q_EMIT statusMessage(QString("Downloading %1...").arg(fileName));

    const QUrl downloadUrl(url);
    if (downloadUrl.isValid() && downloadUrl.scheme().toLower() == "https") {
        return true;
    }
    setPhase(Phase::Failed, "Invalid download URL");
    Q_EMIT downloadError("Rejected non-HTTPS download URL: " + url);
    return false;
}

void LinuxISODownloader::resetAria2cProcess() {
    if (m_aria2cProcess != nullptr) {
        m_aria2cProcess->disconnect();
        m_aria2cProcess->deleteLater();
        m_aria2cProcess = nullptr;
    }
    m_aria2cProcess = new QProcess(this);
    m_aria2cProcess->setProcessChannelMode(QProcess::MergedChannels);
}

QStringList LinuxISODownloader::buildAria2cArguments(const QString& url,
                                                     const QString& outDir,
                                                     const QString& outFile) const {
    QStringList args;
    args << url << "--dir=" + outDir << "--out=" + outFile;

    // SourceForge URLs redirect through mirror selection and may serve
    // from HTTP mirrors. Use single-connection mode to avoid mirror
    // inconsistencies and allow HTTP redirects from the initial HTTPS URL.
    const bool isSourceForge = url.contains("sourceforge.net", Qt::CaseInsensitive);
    if (isSourceForge) {
        // -- SourceForge-specific settings --
        args << "--max-connection-per-server=" + QString::number(sak::kAria2SingleConn)
             << "--split=" + QString::number(sak::kAria2SingleSplit) << "--min-split-size=20M"
             << "--check-certificate=true"  // Always verify TLS certificates
             << "--follow-metalink=mem"     // SF may serve metalink responses
             << "--follow-torrent=false"
             << "--max-tries=" +
                    QString::number(sak::kAria2MaxTriesHigh)  // More retries for mirror selection
             << "--retry-wait=" + QString::number(sak::kAria2RetryWaitLongSec)
             << "--connect-timeout=" + QString::number(sak::kAria2ConnectTimeoutLongSec)
             << "--timeout=" + QString::number(sak::kAria2TimeoutLongSec)
             << "--max-file-not-found=5"
             << "--lowest-speed-limit=10K";  // Lenient speed limit for SF
    } else {
        // -- Standard multi-connection settings --
        args << "--max-connection-per-server=" + QString::number(sak::kAria2MaxConnsPerServer)
             << "--split=" + QString::number(sak::kAria2Split) << "--min-split-size=1M"
             << "--check-certificate=true"
             << "--lowest-speed-limit=50K"
             << "--max-tries=" + QString::number(sak::kAria2MaxTries)
             << "--retry-wait=" + QString::number(sak::kAria2RetryWaitSec)
             << "--connect-timeout=" + QString::number(sak::kAria2ConnectTimeoutSec)
             << "--timeout=" + QString::number(sak::kAria2TimeoutSec) << "--max-file-not-found=3";
    }

    // -- Common settings --
    args  // -- User-Agent (critical: many CDNs/SourceForge block
          //    aria2c's default UA string) --
        << "--user-agent=Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
           "AppleWebKit/537.36 (KHTML, like Gecko) "
           "Chrome/131.0.0.0 Safari/537.36"
        // -- Resumability --
        << "--continue=true"
        << "--auto-file-renaming=false"
        << "--allow-overwrite=true"
        // -- Performance tuning --
        << "--file-allocation=none"
        << "--disk-cache=64M"
        << "--piece-length=1M"
        // -- Output formatting --
        << "--summary-interval=1"
        << "--human-readable=false"
        << "--enable-color=false"
        << "--console-log-level=notice";

    return args;
}

namespace {

struct Aria2cExitEntry {
    int code;
    const char* message;
};

static constexpr Aria2cExitEntry kAria2cExitCodes[] = {
    {.code = 1, .message = "aria2c reported a generic failure (exit code 1)"},
    {.code = 2, .message = "Connection timed out"},
    {.code = 3, .message = "Resource not found (404)"},
    {.code = 4, .message = "Max retries reached \xe2\x80\x94 check your internet connection"},
    {.code = 5, .message = "Download speed too slow"},
    {.code = 6, .message = "Network error"},
    {.code = 7, .message = "Download incomplete \xe2\x80\x94 some files could not be finished"},
    {.code = 9, .message = "Disk space insufficient"},
    {.code = 13, .message = "File already exists and could not be overwritten"},
    {.code = 24, .message = "DNS resolution failed"},
};

}  // namespace

QString LinuxISODownloader::aria2cExitCodeMessage(int exit_code) {
    const auto* it =
        std::ranges::find_if(kAria2cExitCodes,

                             [exit_code](const auto& e) { return e.code == exit_code; });
    if (it != std::end(kAria2cExitCodes)) {
        return QString::fromUtf8(it->message);
    }
    return QString("aria2c exited with code %1").arg(exit_code);
}

void LinuxISODownloader::onAria2cFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    // The constructor creates m_progressTimer; m_aria2cProcess is null-checked below because it
    // is created lazily per download.
    Q_ASSERT(m_progressTimer);
    m_progressTimer->stop();

    // Read any remaining output
    if (m_aria2cProcess != nullptr) {
        const QString output = QString::fromUtf8(m_aria2cProcess->readAllStandardOutput());
        if (!output.trimmed().isEmpty()) {
            sak::logInfo("aria2c final output: " + output.trimmed().toStdString());
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
        const QString errorMsg = aria2cExitCodeMessage(exitCode);
        sak::logError("aria2c failed: " + errorMsg.toStdString());
        setPhase(Phase::Failed, errorMsg);
        Q_EMIT downloadError(errorMsg);
        return;
    }

    // Download succeeded -- verify file exists
    const QFileInfo downloadedFile(m_savePath);
    if (!downloadedFile.exists() || downloadedFile.size() == 0) {
        setPhase(Phase::Failed, "Downloaded file is missing or empty");
        Q_EMIT downloadError(
            "The downloaded file could not be found after aria2c completed. "
            "The server may have returned an error page instead of the ISO.");
        return;
    }

    sak::logInfo("Download complete: " + m_savePath.toStdString() + " (" +
                 std::to_string(downloadedFile.size() / sak::kBytesPerMB) + " MB)");

    // An ISO that cannot be checked against a pinned checksum is never handed on as a
    // completed download: the transport may have been redirected onto a plain-HTTP mirror and
    // the file goes on to be written to bootable media. requirePinnedChecksum already refused
    // the start, so reaching here without one means the state was lost -- fail closed.
    if (m_checksumUrl.isEmpty() || m_checksumType.isEmpty()) {
        const QString error =
            "Downloaded ISO has no pinned checksum to verify against; refusing to report it as "
            "complete. The file was left at " +
            m_savePath;
        sak::logError(error.toStdString());
        setPhase(Phase::Failed, "No pinned checksum available");
        Q_EMIT downloadError(error);
        return;
    }
    verifyChecksum();
}

// ============================================================================
// Progress Polling
// ============================================================================

void LinuxISODownloader::onProgressPollTimer() {
    if (m_phase != Phase::Downloading) {
        return;
    }

    // Read aria2c stdout for progress info
    if (m_aria2cProcess == nullptr) {
        return;
    }

    const QByteArray data = m_aria2cProcess->readAllStandardOutput();
    if (data.isEmpty()) {
        return;
    }

    const QString output = QString::fromUtf8(data);
    const QStringList lines = output.split(QRegularExpression("[\r\n]"), Qt::SkipEmptyParts);

    for (const QString& rawLine : lines) {
        const QString line = rawLine.trimmed();

        // aria2c progress lines look like:
        // [#abcdef 1234567/9876543(12%) CN:16 DL:45.2MiB]
        // or in human-readable=false mode:
        // [#abcdef 1234567/9876543(12%) CN:16 DL:47394816]
        static const QRegularExpression progressRegex(
            R"(\[#\w+\s+(\d+)/(\d+)\((\d+)%\).*DL:(\S+)\])");

        auto match = progressRegex.match(line);
        if (match.hasMatch()) {
            const qint64 downloaded = match.captured(kAria2cDownloadedCaptureGroup).toLongLong();
            const qint64 total = match.captured(kAria2cTotalCaptureGroup).toLongLong();
            const int percent = match.captured(kAria2cPercentCaptureGroup).toInt();
            const double speedMBps = parseAria2cSpeedMBps(match.captured(kAria2cSpeedCaptureGroup));

            const QString detail = QString("%1 / %2").arg(formatSize(downloaded),
                                                          formatSize(total));

            Q_EMIT progressUpdated(percent, detail);
            Q_EMIT speedUpdated(speedMBps);
            continue;
        }

        // Log significant messages
        if (line.contains("ERROR", Qt::CaseInsensitive) ||
            line.contains("WARNING", Qt::CaseInsensitive)) {
            sak::logWarning("aria2c: " + line.toStdString());
        }
    }
}

// ============================================================================
// Checksum Verification
// ============================================================================

qsizetype LinuxISODownloader::expectedHashHexLength() const {
    return m_checksumType == "sha1" ? kSha1HexLength : kSha256HexLength;
}

bool LinuxISODownloader::isHexDigestOfLength(const QString& token, qsizetype hex_length) {
    if (token.size() != hex_length) {
        return false;
    }
    return std::ranges::all_of(token, [](QChar ch) {
        const QChar lower = ch.toLower();
        return (lower >= QLatin1Char('0') && lower <= QLatin1Char('9')) ||
               (lower >= QLatin1Char('a') && lower <= QLatin1Char('f'));
    });
}

QString LinuxISODownloader::checksumSectionAlgorithm(const QString& line) {
    // Matches a per-algorithm heading such as "### SHA256SUMS:", "SHA1SUMS" or "# B3SUMS:".
    static const QRegularExpression headingRe(QStringLiteral("^#*\\s*([A-Za-z0-9]+)SUMS:?$"));
    const auto match = headingRe.match(line.trimmed());
    return match.hasMatch() ? match.captured(1).toUpper() : QString();
}

QString LinuxISODownloader::bsdRecordDigest(const QString& line,
                                            const QString& expectedFileName,
                                            const QString& algorithm,
                                            qsizetype hex_length) {
    // BSD-style record: "SHA256 (Fedora-Workstation-Live-44-1.7.x86_64.iso) = <hash>".
    static const QRegularExpression bsdRe(
        QStringLiteral("^([A-Za-z0-9-]+)\\s*\\(([^)]+)\\)\\s*=\\s*(\\S+)$"));
    const auto match = bsdRe.match(line.trimmed());
    if (!match.hasMatch()) {
        return {};
    }
    // The algorithm is named on the line itself, so follow that label exactly as a section
    // heading is followed: a SHA256 record must never satisfy a SHA-512 configuration.
    // "SHA-256" and "SHA256" are the same algorithm spelled two ways.
    QString lineAlgorithm = match.captured(1).toUpper();
    QString wantedAlgorithm = algorithm.toUpper();
    lineAlgorithm.remove(QLatin1Char('-'));
    wantedAlgorithm.remove(QLatin1Char('-'));
    if (lineAlgorithm != wantedAlgorithm ||
        match.captured(kBsdRecordFileNameGroup) != expectedFileName) {
        return {};
    }
    const QString digest = match.captured(kBsdRecordDigestGroup);
    return isHexDigestOfLength(digest, hex_length) ? digest.toLower() : QString();
}

QString LinuxISODownloader::hashFromRecordLine(const QString& line,
                                               const QString& expectedFileName) const {
    // Split on whitespace (hash  filename OR hash *filename)
    const QStringList parts = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    if (parts.size() == 1) {
        // A bare sidecar holding only the digest. It must still LOOK like a digest of the
        // configured algorithm; a lone prose word is not a hash and must not be returned.
        const QString only = parts.first();
        return isHexDigestOfLength(only, expectedHashHexLength()) ? only.toLower() : QString();
    }
    for (qsizetype i = 0; i + 1 < parts.size(); i += kChecksumRecordMinimumParts) {
        QString filename = parts.at(i + 1);
        if (filename.startsWith('*')) {
            filename = filename.mid(1);
        }
        if (filename == expectedFileName &&
            isHexDigestOfLength(parts.at(i), expectedHashHexLength())) {
            return parts.at(i).toLower();
        }
    }
    return {};
}

QString LinuxISODownloader::parseExpectedHash(const QString& checksumData,
                                              const QString& expectedFileName) const {
    const QStringList checksumLines = checksumData.split('\n', Qt::SkipEmptyParts);
    // Empty until a per-algorithm heading is seen. A plain sums file (one algorithm, no
    // headings) never sets it and is read exactly as before. A release note that groups
    // several algorithms DOES set it, and that is the only thing that can tell a SHA-256
    // digest from a BLAKE3 one: both are 64 hex characters and both name the same ISO, so
    // matching on shape alone would return whichever block happened to come first.
    QString section;

    for (const QString& line : checksumLines) {
        const QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        if (const QString heading = checksumSectionAlgorithm(trimmed); !heading.isEmpty()) {
            section = heading;
            continue;
        }
        if (trimmed.startsWith('#')) {
            continue;
        }
        // A BSD-style record carries its own algorithm label, so it is self-describing and is
        // read regardless of any surrounding section.
        const QString bsd =
            bsdRecordDigest(trimmed, expectedFileName, m_checksumType, expectedHashHexLength());
        if (!bsd.isEmpty()) {
            return bsd;
        }
        if (!section.isEmpty() && section != m_checksumType.toUpper()) {
            continue;  // A digest for an algorithm we are not verifying with.
        }
        if (const QString hash = hashFromRecordLine(trimmed, expectedFileName); !hash.isEmpty()) {
            return hash;
        }
    }

    return {};
}

void LinuxISODownloader::onChecksumReplyFinished(QNetworkReply* reply,
                                                 QNetworkAccessManager* nam,
                                                 quint64 generation) {
    // Only verifyChecksum reaches here, with the manager it just allocated and the reply
    // QNetworkAccessManager::get returned for it; neither can be null.
    Q_ASSERT(reply);
    Q_ASSERT(nam);
    reply->deleteLater();
    nam->deleteLater();

    if (!shouldApplyVerifyResult(m_cancelled, generation, m_operationGeneration.load())) {
        return;  // cancelled, or superseded by a newer download; emit no terminal signal
    }

    if (reply->error() != QNetworkReply::NoError) {
        const QString error = checksumFetchErrorMessage(reply);
        sak::logWarning(error.toStdString());
        setPhase(Phase::Failed, "Checksum fetch failed");
        Q_EMIT downloadError(error);
        return;
    }

    const QString checksumData = QString::fromUtf8(reply->readAll());
    const QString expectedFileName = QFileInfo(m_savePath).fileName();
    const QString expectedHash = parseExpectedHash(checksumData, expectedFileName);

    if (expectedHash.isEmpty()) {
        const QString error = "Could not find matching hash in checksum file for: " +
                              expectedFileName;
        sak::logWarning(error.toStdString());
        setPhase(Phase::Failed, "Checksum entry missing");
        Q_EMIT downloadError(error);
        return;
    }

    // Compute hash in background thread
    Q_EMIT statusMessage("Computing " + m_checksumType.toUpper() + " checksum...");
    Q_EMIT progressUpdated(kChecksumComputingProgress, "Computing checksum...");

    auto algorithm = (m_checksumType == "sha1") ? QCryptographicHash::Sha1
                                                : QCryptographicHash::Sha256;
    launchChecksumHash(algorithm, expectedHash);
}

void LinuxISODownloader::launchChecksumHash(QCryptographicHash::Algorithm algorithm,
                                            const QString& expectedHash) {
    // Tag this verify with the current operation generation and hash the file
    // captured NOW (by value) -- so a later download that reassigns m_savePath
    // cannot make this background hash read the wrong file, and a cancelled or
    // superseded result is discarded instead of acting on stale state.
    const quint64 generation = m_operationGeneration.load();
    const QString hashPath = m_savePath;

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(
        watcher,
        &QFutureWatcher<QString>::finished,
        this,
        [this, watcher, expectedHash, generation]() {
            const QString actualHash = watcher->result();
            watcher->deleteLater();
            if (!shouldApplyVerifyResult(m_cancelled, generation, m_operationGeneration.load())) {
                return;  // cancelled or superseded by a newer download
            }
            onChecksumVerified(actualHash == expectedHash, expectedHash, actualHash);
        });

    // Give this hash its OWN cancel flag and signal any prior hash to stop first. The
    // background task captures the flag and path BY VALUE and never dereferences `this`,
    // so an overlapping or superseded task (a cancel + restart can leave one running)
    // can never use-after-free the downloader, and resetting m_cancelled for a new
    // download cannot revive an old hash.
    if (m_hashCancel) {
        m_hashCancel->store(true);
    }
    m_hashCancel = std::make_shared<std::atomic<bool>>(false);
    const std::shared_ptr<std::atomic<bool>> cancelFlag = m_hashCancel;

    m_hashFuture = QtConcurrent::run([algorithm, hashPath, cancelFlag]() -> QString {
        QFile file(hashPath);
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }

        QCryptographicHash hash(algorithm);
        const qint64 bufferSize = kChecksumReadBufferSize;
        while (!file.atEnd()) {
            if (cancelFlag->load()) {
                return QString();  // abort a long hash promptly on cancel
            }
            hash.addData(file.read(bufferSize));
        }
        return hash.result().toHex().toLower();
    });

    watcher->setFuture(m_hashFuture);
}

void LinuxISODownloader::verifyChecksum() {
    setPhase(Phase::VerifyingChecksum, "Verifying checksum...");
    Q_EMIT statusMessage("Downloading checksum file...");
    Q_EMIT progressUpdated(kChecksumVerifyingProgress, "Verifying integrity...");

    // Fetch checksum file
    auto* nam = new QNetworkAccessManager(this);
    const QUrl checksumUrl(m_checksumUrl);
    QNetworkRequest request(checksumUrl);
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");
    // The checksum is the only thing standing between a mirror-served ISO and the disk it is
    // written to, so its own transport must not be downgraded. Pinned explicitly rather than
    // left to the framework default: an HTTPS request that redirects onto a plain-HTTP mirror
    // is refused outright instead of silently followed.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    // Bound the transfer: if the peer connects then stalls without sending data, abort and
    // surface an error via the finished handler's error() branch instead of hanging forever.
    // The oversize guard below only fires once bytes actually arrive, so it cannot catch a stall.
    request.setTransferTimeout(sak::kHttpMetadataTransferTimeoutMs);

    // Tag this fetch with the current operation generation so a reply that outlives a
    // cancel + restart is discarded instead of verifying the new download against the old
    // checksum file (which could fail it, or worse delete the new file).
    const quint64 generation = m_operationGeneration.load();

    auto* reply = nam->get(request);
    // Fail closed on an oversized checksum response: abort once the peer streams past the
    // ceiling so readAll() below can never buffer an unbounded body into memory.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > kMaxChecksumBytes || total > kMaxChecksumBytes) {
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, nam, generation]() {
        onChecksumReplyFinished(reply, nam, generation);
    });
}

void LinuxISODownloader::onChecksumVerified(bool match,
                                            const QString& expected,
                                            const QString& actual) {
    const QFileInfo fileInfo(m_savePath);

    if (actual.isEmpty()) {
        // Fail closed: a checksum the user requested could not be computed (file
        // unreadable/locked), so the download is NOT verified. Do not delete -- the content is
        // unknown-good and the open failure is likely transient, so leave it for a retry.
        const QString error = "Could not read the downloaded file to verify its checksum: " +
                              m_savePath;
        sak::logError(error.toStdString());
        setPhase(Phase::Failed, "Checksum verification failed");
        Q_EMIT downloadError(error);
        return;
    }

    if (match) {
        sak::logInfo("Checksum verified: " + actual.toStdString());
        Q_EMIT statusMessage(m_checksumType.toUpper() + " checksum verified successfully");
        setPhase(Phase::Completed, "Download complete -- checksum verified");
        Q_EMIT downloadComplete(m_savePath, fileInfo.size());
    } else {
        sak::logError("Checksum mismatch! Expected: " + expected.toStdString() +
                      " Actual: " + actual.toStdString());
        setPhase(Phase::Failed, "Checksum verification failed");

        // Remove the corrupted file
        QFile::remove(m_savePath);

        Q_EMIT downloadError(QString("Checksum verification failed!\n\n"
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

void LinuxISODownloader::cancel() {
    // The constructor creates m_progressTimer and m_catalog, and nothing reassigns them.
    // m_aria2cProcess is created lazily by startDownload; it is legitimately null when cancel()
    // runs from the dtor of a never-started downloader. The null-guarded kill below handles it.
    Q_ASSERT(m_progressTimer);
    Q_ASSERT(m_catalog);
    m_cancelled = true;
    ++m_operationGeneration;        // invalidate any background hash still running
    if (m_hashCancel) {
        m_hashCancel->store(true);  // signal the self-contained hash task to stop
    }
    m_progressTimer->stop();
    m_catalog->cancelAll();

    if ((m_aria2cProcess != nullptr) && m_aria2cProcess->state() != QProcess::NotRunning) {
        m_aria2cProcess->kill();
    }

    setPhase(Phase::Idle, "Cancelled");
    Q_EMIT statusMessage("Download cancelled");
}

// ============================================================================
// Helpers
// ============================================================================

void LinuxISODownloader::setPhase(Phase phase, const QString& description) {
    m_phase = phase;
    Q_EMIT phaseChanged(phase, description);
}

QString LinuxISODownloader::findAria2c() const {
    auto& tools = sak::BundledToolsManager::instance();

    const QString path = tools.toolPath("uup", "aria2c.exe");
    if (QFileInfo::exists(path)) {
        return path;
    }

    sak::logError(
        "aria2c.exe not found at required bundled path. "
        "Run scripts/bundle_uup_tools.ps1 to install it.");
    return {};
}

void LinuxISODownloader::cleanupPartialFiles() {
    // Remove .aria2 control file
    const QString aria2ControlFile = m_savePath + ".aria2";
    if (QFile::exists(aria2ControlFile)) {
        QFile::remove(aria2ControlFile);
        sak::logInfo("Removed aria2 control file: " + aria2ControlFile.toStdString());
    }

    // Remove partial download
    if (QFile::exists(m_savePath)) {
        QFile::remove(m_savePath);
        sak::logInfo("Removed partial download: " + m_savePath.toStdString());
    }
}
