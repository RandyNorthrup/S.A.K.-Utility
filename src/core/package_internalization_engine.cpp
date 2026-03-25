// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_internalization_engine.cpp
/// @brief Engine implementation for internalizing Chocolatey packages

#include "sak/package_internalization_engine.h"

#include "sak/logger.h"
#include "sak/offline_deployment_constants.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

PackageInternalizationEngine::PackageInternalizationEngine(QObject* parent)
    : QObject(parent), m_network_manager(new QNetworkAccessManager(this)) {}

PackageInternalizationEngine::~PackageInternalizationEngine() {
    cancel();
}

// ============================================================================
// Public API
// ============================================================================

void PackageInternalizationEngine::internalizePackage(const QString& package_id,
                                                      const QString& version,
                                                      const QString& output_dir,
                                                      const QString& work_dir) {
    if (m_busy) {
        Q_EMIT errorOccurred(package_id, "Engine is already busy");
        return;
    }

    m_busy = true;
    m_cancelled = false;
    m_current_progress = {};
    m_current_progress.package_id = package_id;
    m_current_progress.version = version;

    sak::logInfo("[InternalizationEngine] Starting: {} v{}",
                 package_id.toStdString(),
                 version.toStdString());

    InternalizationResult result;
    result.package_id = package_id;
    result.version = version;

    // Ensure work and output directories exist
    QDir(work_dir).mkpath(".");
    QDir(output_dir).mkpath(".");

    QString extract_dir = work_dir + "/" + package_id + "." + version;
    QDir(extract_dir).mkpath(".");

    // Step 1: Download the .nupkg
    QString nupkg_path = work_dir + "/" + package_id + "." + version + ".nupkg";
    if (!downloadNupkg(package_id, version, nupkg_path, result)) {
        m_busy = false;
        return;
    }

    result.original_size = QFileInfo(nupkg_path).size();

    // Step 2: Extract the nupkg
    emitProgress(InternalizationStatus::Extracting, "Extracting package...");
    if (!extractNupkg(nupkg_path, extract_dir)) {
        finishWithError(result, "Failed to extract .nupkg archive");
        return;
    }

    if (m_cancelled) {
        m_busy = false;
        return;
    }

    // Step 3: Parse the install script
    emitProgress(InternalizationStatus::ParsingScript, "Parsing install script...");
    QString script_path = findInstallScript(extract_dir);

    if (script_path.isEmpty()) {
        sak::logInfo(
            "[InternalizationEngine] No install script found, "
            "skipping binary internalization");
        repackAndFinish(result, extract_dir, output_dir, "Repacking (no binaries)...");
        return;
    }

    auto parsed = m_parser.parseFile(script_path);

    if (parsed.resources.isEmpty()) {
        sak::logInfo(
            "[InternalizationEngine] No download URLs in script "
            "for {}",
            package_id.toStdString());
        repackAndFinish(result, extract_dir, output_dir, "Repacking (no external downloads)...");
        return;
    }

    // Step 4: Download all referenced binaries
    QString tools_dir = QFileInfo(script_path).dir().absolutePath();
    if (!downloadAllBinaries(parsed, tools_dir, result)) {
        m_busy = false;
        return;
    }

    // Step 5: Rewrite the install script
    emitProgress(InternalizationStatus::RewritingScript, "Rewriting install script...");
    auto rewrite_result =
        m_rewriter.rewriteToFile(parsed, buildLocalFilenameMap(parsed), script_path);
    if (!rewrite_result.success) {
        finishWithError(result, "Script rewrite failed: " + rewrite_result.error_message);
        return;
    }

    // Steps 6-8: Clean, repack, and checksum
    repackAndFinish(result, extract_dir, output_dir, "Repacking .nupkg...");
}

// ============================================================================
// Internalization Helpers
// ============================================================================

bool PackageInternalizationEngine::downloadNupkg(const QString& package_id,
                                                 const QString& version,
                                                 const QString& nupkg_path,
                                                 InternalizationResult& result) {
    emitProgress(InternalizationStatus::DownloadingNupkg, "Downloading .nupkg...");

    QString nupkg_url =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, package_id, version);

    QEventLoop loop;
    bool download_ok = false;
    QString download_error;

    QNetworkRequest request{QUrl(nupkg_url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "SAK-Utility/1.0");
    request.setTransferTimeout(offline::kDownloadTimeoutMs);

    auto* reply = m_network_manager->get(request);

    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        m_current_progress.bytes_downloaded = received;
        m_current_progress.bytes_total = total;
        emitProgress(InternalizationStatus::DownloadingNupkg, "Downloading .nupkg...");
    });

    connect(reply, &QNetworkReply::finished, &loop, [&]() {
        if (reply->error() == QNetworkReply::NoError) {
            QFile file(nupkg_path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                download_ok = true;
            } else {
                download_error = "Cannot write nupkg file";
            }
        } else {
            download_error = reply->errorString();
        }
        reply->deleteLater();
        loop.quit();
    });

    loop.exec();

    if (m_cancelled) {
        return false;
    }

    if (!download_ok) {
        finishWithError(result, "Nupkg download failed: " + download_error);
        return false;
    }

    return true;
}

void PackageInternalizationEngine::repackAndFinish(InternalizationResult& result,
                                                   const QString& extract_dir,
                                                   const QString& output_dir,
                                                   const QString& status_message) {
    emitProgress(InternalizationStatus::Repacking, status_message);
    cleanNugetArtifacts(extract_dir);

    QString output_nupkg = output_dir + "/" + result.package_id + "." + result.version + ".nupkg";

    if (repackNupkg(extract_dir, output_nupkg)) {
        result.success = true;
        result.output_nupkg_path = output_nupkg;
        result.checksum = computeChecksum(output_nupkg);
        result.internalized_size = QFileInfo(output_nupkg).size();

        sak::logInfo("[InternalizationEngine] Internalized {} v{}: {} -> {} bytes, {} files",
                     result.package_id.toStdString(),
                     result.version.toStdString(),
                     result.original_size,
                     result.internalized_size,
                     static_cast<int>(result.internalized_files.size()));

        emitProgress(InternalizationStatus::Complete, "Internalization complete");
    } else {
        result.error_message = "Failed to repack .nupkg";
        sak::logError("[InternalizationEngine] {}", result.error_message.toStdString());
    }

    Q_EMIT packageComplete(result);
    m_busy = false;
}

void PackageInternalizationEngine::finishWithError(InternalizationResult& result,
                                                   const QString& error) {
    result.error_message = error;
    sak::logError("[InternalizationEngine] {}", error.toStdString());
    Q_EMIT packageComplete(result);
    m_busy = false;
}

bool PackageInternalizationEngine::downloadAllBinaries(const ParsedInstallScript& parsed,
                                                       const QString& tools_dir,
                                                       InternalizationResult& result) {
    emitProgress(InternalizationStatus::DownloadingBinaries, "Downloading binaries...");

    QStringList urls_to_download = collectBinaryUrls(parsed);
    m_current_progress.binary_total = urls_to_download.size();

    for (int idx = 0; idx < urls_to_download.size(); ++idx) {
        if (m_cancelled) {
            return false;
        }

        m_current_progress.binary_index = idx + 1;
        emitProgress(
            InternalizationStatus::DownloadingBinaries,
            QString("Downloading binary %1 of %2...").arg(idx + 1).arg(urls_to_download.size()));

        const QString& url = urls_to_download[idx];
        QUrl parsed_url(url);
        QString filename = parsed_url.fileName();
        if (filename.isEmpty()) {
            filename = QString("binary_%1").arg(idx + 1);
        }

        QString output_path = tools_dir + "/" + filename;

        QEventLoop loop;
        bool ok = false;
        QString error;

        downloadBinary(url, output_path, [&](bool success, const QString& err) {
            ok = success;
            error = err;
            loop.quit();
        });

        loop.exec();

        if (!ok) {
            sak::logWarning("[InternalizationEngine] Binary download failed: {} - {}",
                            url.toStdString(),
                            error.toStdString());
            finishWithError(result, "Binary download failed: " + error);
            return false;
        }

        result.internalized_files.append(filename);
    }

    return !m_cancelled;
}

QStringList PackageInternalizationEngine::collectBinaryUrls(
    const ParsedInstallScript& parsed) const {
    QStringList urls;
    for (const auto& resource : parsed.resources) {
        if (!resource.url.isEmpty() && !urls.contains(resource.url)) {
            urls.append(resource.url);
        }
        if (!resource.url_64bit.isEmpty() && !urls.contains(resource.url_64bit)) {
            urls.append(resource.url_64bit);
        }
    }
    return urls;
}

QHash<QString, QString> PackageInternalizationEngine::buildLocalFilenameMap(
    const ParsedInstallScript& parsed) const {
    QHash<QString, QString> local_filenames;
    int idx = 0;
    for (const auto& url : collectBinaryUrls(parsed)) {
        ++idx;
        QUrl parsed_url(url);
        QString filename = parsed_url.fileName();
        if (filename.isEmpty()) {
            filename = QString("binary_%1").arg(idx);
        }
        local_filenames.insert(url, filename);
    }
    return local_filenames;
}

void PackageInternalizationEngine::cancel() {
    m_cancelled = true;
}

bool PackageInternalizationEngine::isBusy() const {
    return m_busy;
}

// ============================================================================
// Package Extraction
// ============================================================================

bool PackageInternalizationEngine::extractNupkg(const QString& nupkg_path,
                                                const QString& extract_dir) {
    // .nupkg files are ZIP archives — use PowerShell Expand-Archive
    QProcess process;
    process.setProgram("powershell.exe");
    process.setArguments({"-NoProfile",
                          "-NonInteractive",
                          "-Command",
                          QString("Expand-Archive -Path '%1' -DestinationPath '%2' -Force")
                              .arg(nupkg_path, extract_dir)});

    process.start();
    if (!process.waitForStarted(offline::kPackTimeoutMs)) {
        sak::logError("[InternalizationEngine] Extract process failed to start");
        return false;
    }

    if (!process.waitForFinished(offline::kPackTimeoutMs)) {
        process.kill();
        sak::logError("[InternalizationEngine] Extract timed out");
        return false;
    }

    if (process.exitCode() != 0) {
        QString err = process.readAllStandardError();
        sak::logError("[InternalizationEngine] Extract failed: {}", err.toStdString());
        return false;
    }

    return true;
}

// ============================================================================
// Script Discovery
// ============================================================================

QString PackageInternalizationEngine::findInstallScript(const QString& extract_dir) const {
    // Standard Chocolatey location: tools/chocolateyInstall.ps1
    QString standard_path = extract_dir + "/tools/chocolateyInstall.ps1";
    if (QFile::exists(standard_path)) {
        return standard_path;
    }

    // Search recursively for the script
    QDirIterator iter(
        extract_dir, {"chocolateyInstall.ps1"}, QDir::Files, QDirIterator::Subdirectories);

    if (iter.hasNext()) {
        return iter.next();
    }

    return {};
}

// ============================================================================
// Binary Download
// ============================================================================

void PackageInternalizationEngine::downloadBinary(
    const QString& url,
    const QString& output_path,
    std::function<void(bool, const QString&)> callback) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "SAK-Utility/1.0");
    request.setTransferTimeout(offline::kDownloadTimeoutMs);

    auto* reply = m_network_manager->get(request);

    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 received, qint64 total) {
        m_current_progress.bytes_downloaded = received;
        m_current_progress.bytes_total = total;
        Q_EMIT progressChanged(m_current_progress);
    });

    connect(reply, &QNetworkReply::finished, this, [reply, output_path, callback]() {
        reply->deleteLater();

        if (reply->error() != QNetworkReply::NoError) {
            callback(false, reply->errorString());
            return;
        }

        QByteArray data = reply->readAll();
        if (data.isEmpty()) {
            callback(false, "Empty response");
            return;
        }

        QFile file(output_path);
        if (!file.open(QIODevice::WriteOnly)) {
            callback(false, "Cannot write file: " + output_path);
            return;
        }

        file.write(data);
        file.close();
        callback(true, {});
    });
}

// ============================================================================
// NuGet Artifact Cleanup
// ============================================================================

void PackageInternalizationEngine::cleanNugetArtifacts(const QString& extract_dir) const {
    // Remove NuGet metadata files that are not needed in internalized packages
    static const QStringList kArtifactPatterns = {"_rels", "[Content_Types].xml", "package"};

    QDir dir(extract_dir);

    // Remove _rels directory
    QDir rels_dir(extract_dir + "/_rels");
    if (rels_dir.exists()) {
        rels_dir.removeRecursively();
    }

    // Remove [Content_Types].xml
    QString content_types = extract_dir + "/[Content_Types].xml";
    if (QFile::exists(content_types)) {
        QFile::remove(content_types);
    }

    // Remove package/ directory (NuGet services metadata)
    QDir pkg_dir(extract_dir + "/package");
    if (pkg_dir.exists()) {
        pkg_dir.removeRecursively();
    }

    // Remove .psmdcp files
    QDirIterator iter(extract_dir, {"*.psmdcp"}, QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        QFile::remove(iter.next());
    }
}

// ============================================================================
// Repacking
// ============================================================================

bool PackageInternalizationEngine::repackNupkg(const QString& source_dir,
                                               const QString& output_path) {
    // Use PowerShell Compress-Archive to create the ZIP/.nupkg
    QProcess process;
    process.setProgram("powershell.exe");
    process.setArguments({"-NoProfile",
                          "-NonInteractive",
                          "-Command",
                          QString("Compress-Archive -Path '%1/*' -DestinationPath '%2' -Force")
                              .arg(source_dir, output_path)});

    process.start();
    if (!process.waitForStarted(offline::kPackTimeoutMs)) {
        sak::logError("[InternalizationEngine] Repack process failed to start");
        return false;
    }

    if (!process.waitForFinished(offline::kPackTimeoutMs)) {
        process.kill();
        sak::logError("[InternalizationEngine] Repack timed out");
        return false;
    }

    if (process.exitCode() != 0) {
        QString err = process.readAllStandardError();
        sak::logError("[InternalizationEngine] Repack failed: {}", err.toStdString());
        return false;
    }

    return QFile::exists(output_path);
}

// ============================================================================
// Checksum
// ============================================================================

QString PackageInternalizationEngine::computeChecksum(const QString& file_path) const {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        sak::logWarning("[InternalizationEngine] Cannot open for checksum: {}",
                        file_path.toStdString());
        return {};
    }

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    constexpr qint64 kBlockSize = offline::kChecksumBlockSize;

    while (!file.atEnd()) {
        hasher.addData(file.read(kBlockSize));
    }

    return hasher.result().toHex();
}

// ============================================================================
// Progress Reporting
// ============================================================================

void PackageInternalizationEngine::emitProgress(InternalizationStatus status,
                                                const QString& message) {
    m_current_progress.status = status;
    m_current_progress.status_message = message;
    Q_EMIT progressChanged(m_current_progress);
}

}  // namespace sak
