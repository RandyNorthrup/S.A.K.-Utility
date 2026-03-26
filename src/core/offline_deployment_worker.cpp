// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file offline_deployment_worker.cpp
/// @brief Background worker implementation for batch offline deployment

#include "sak/offline_deployment_worker.h"

#include "sak/logger.h"
#include "sak/offline_deployment_constants.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QtConcurrent>
#include <QTextStream>
#include <QUrl>
#include <QXmlStreamReader>

namespace sak {

// ============================================================================
// Helpers
// ============================================================================

/// @brief Parse .nuspec in extract_dir for the first dependency package ID.
/// Prefers dependencies ending in ".install" (e.g. 7zip → 7zip.install).
static QString findNuspecDependencyId(const QString& extract_dir) {
    QDirIterator iter(extract_dir, {"*.nuspec"}, QDir::Files);
    if (!iter.hasNext()) {
        return {};
    }

    QFile file(iter.next());
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }

    QXmlStreamReader xml(&file);
    QString preferred;
    QString first;

    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QStringLiteral("dependency")) {
            QString id = xml.attributes().value("id").toString();
            if (id.isEmpty()) {
                continue;
            }
            if (first.isEmpty()) {
                first = id;
            }
            if (id.endsWith(".install")) {
                preferred = id;
                break;
            }
        }
    }

    return preferred.isEmpty() ? first : preferred;
}

// ============================================================================
// Construction / Destruction
// ============================================================================

OfflineDeploymentWorker::OfflineDeploymentWorker(QObject* parent) : QObject(parent) {}

OfflineDeploymentWorker::~OfflineDeploymentWorker() {
    cancel();
    if (m_operation_future.isRunning()) {
        m_operation_future.waitForFinished();
    }
}

// ============================================================================
// Build Deployment Bundle
// ============================================================================

void OfflineDeploymentWorker::buildDeploymentBundle(
    const QVector<QPair<QString, QString>>& packages,
    const QString& output_dir,
    const QString& description) {
    if (m_running) {
        Q_EMIT operationError("An operation is already running");
        return;
    }

    if (packages.isEmpty()) {
        Q_EMIT operationError("No packages specified");
        return;
    }

    if (packages.size() > offline::kMaxPackagesPerBuild) {
        Q_EMIT operationError(QString("Too many packages: %1 (max %2)")
                                  .arg(packages.size())
                                  .arg(offline::kMaxPackagesPerBuild));
        return;
    }

    m_running = true;
    m_cancelled = false;

    {
        QMutexLocker lock(&m_mutex);
        m_jobs.clear();
        m_jobs.reserve(packages.size());
        for (const auto& [pkg_id, version] : packages) {
            BatchInternalizationJob job;
            job.package_id = pkg_id;
            job.version = version;
            m_jobs.append(job);
        }
    }

    Q_EMIT operationStarted(packages.size());
    Q_EMIT logMessage(QString("Building deployment bundle: %1 package(s)").arg(packages.size()));

    m_operation_future = QtConcurrent::run(
        [this, output_dir, description]() { executeBuildBundle(output_dir, description); });
}

void OfflineDeploymentWorker::executeBuildBundle(const QString& output_dir,
                                                 const QString& description) {
    BuildBundleContext ctx;
    ctx.packages_dir = output_dir + "/" + offline::kPackagesSubdir;
    ctx.work_dir = output_dir + "/_work";
    ctx.output_dir = output_dir;
    QDir(ctx.packages_dir).mkpath(".");
    QDir(ctx.work_dir).mkpath(".");

    DeploymentManifest manifest;
    manifest.manifest_version = offline::kManifestVersion;
    manifest.created_date = QDateTime::currentDateTime().toString(Qt::ISODate);
    manifest.creator = "S.A.K. Utility";
    manifest.description = description;
    manifest.output_dir = output_dir;

    QMutexLocker lock(&m_mutex);
    ctx.total_jobs = m_jobs.size();
    lock.unlock();

    for (int idx = 0; idx < ctx.total_jobs; ++idx) {
        if (m_cancelled) {
            break;
        }

        if (internalizeOnePackage(idx, ctx, manifest)) {
            ctx.completed_count++;
        } else {
            ctx.failed_count++;
        }
    }

    finalizeBundle(manifest, ctx);
}

bool OfflineDeploymentWorker::internalizeOnePackage(int idx,
                                                    const BuildBundleContext& ctx,
                                                    DeploymentManifest& manifest) {
    QMutexLocker lock(&m_mutex);
    QString pkg_id = m_jobs[idx].package_id;
    QString version = m_jobs[idx].version;
    m_jobs[idx].status = InternalizationStatus::DownloadingNupkg;
    lock.unlock();

    QMetaObject::invokeMethod(
        this,
        [this, completed_count = ctx.completed_count, total_jobs = ctx.total_jobs, pkg_id]() {
            Q_EMIT batchProgress(completed_count, total_jobs, pkg_id);
            Q_EMIT logMessage(QString("Internalizing: %1").arg(pkg_id));
        },
        Qt::QueuedConnection);

    // Run internalization synchronously via event loop
    QEventLoop loop;
    InternalizationResult result;
    bool got_result = false;

    PackageInternalizationEngine engine;
    connect(&engine,
            &PackageInternalizationEngine::packageComplete,
            &loop,
            [&](const InternalizationResult& res) {
                result = res;
                got_result = true;
                loop.quit();
            });

    connect(&engine,
            &PackageInternalizationEngine::progressChanged,
            this,
            [this, pkg_id](const InternalizationProgress& progress) {
                QMetaObject::invokeMethod(
                    this,
                    [this, pkg_id, progress]() {
                        Q_EMIT logMessage(QString("[%1] %2").arg(pkg_id, progress.status_message));
                    },
                    Qt::QueuedConnection);
            });

    engine.internalizePackage(pkg_id, version, ctx.packages_dir, ctx.work_dir);

    if (!got_result) {
        loop.exec();
    }

    lock.relock();
    if (result.success) {
        m_jobs[idx].status = InternalizationStatus::Complete;
        m_jobs[idx].output_path = result.output_nupkg_path;
        m_jobs[idx].checksum = result.checksum;

        DeploymentManifestEntry entry;
        entry.package_id = pkg_id;
        entry.version = version;
        entry.nupkg_filename = QFileInfo(result.output_nupkg_path).fileName();
        entry.checksum = result.checksum;
        entry.size_bytes = result.internalized_size;
        entry.internalized = true;
        manifest.packages.append(entry);
        manifest.total_size_bytes += result.internalized_size;
    } else {
        m_jobs[idx].status = InternalizationStatus::Failed;
        m_jobs[idx].error_message = result.error_message;
    }
    lock.unlock();

    QMetaObject::invokeMethod(
        this,
        [this, pkg_id, result]() {
            Q_EMIT packageProgress(pkg_id,
                                   result.success,
                                   result.success ? "Complete" : result.error_message);
        },
        Qt::QueuedConnection);

    return result.success;
}

void OfflineDeploymentWorker::finalizeBundle(const DeploymentManifest& manifest,
                                             const BuildBundleContext& ctx) {
    // Write manifest
    if (!manifest.packages.isEmpty()) {
        bool manifest_ok = writeManifest(manifest, ctx.output_dir);
        if (manifest_ok) {
            writeReadme(manifest, ctx.output_dir);
            QMetaObject::invokeMethod(
                this,
                [this, output_dir = ctx.output_dir]() {
                    Q_EMIT manifestWritten(output_dir + "/" + offline::kManifestFilename);
                },
                Qt::QueuedConnection);
        }
    }

    // Clean up work directory
    QDir(ctx.work_dir).removeRecursively();

    // Build final stats
    BatchStats stats;
    stats.total = ctx.total_jobs;
    stats.completed = ctx.completed_count;
    stats.failed = ctx.failed_count;
    stats.total_bytes = manifest.total_size_bytes;

    QMutexLocker lock(&m_mutex);
    for (const auto& job : m_jobs) {
        if (job.status == InternalizationStatus::Cancelled) {
            stats.cancelled++;
        } else if (job.status == InternalizationStatus::Pending) {
            stats.pending++;
        }
    }
    lock.unlock();

    m_running = false;

    QMetaObject::invokeMethod(
        this,
        [this, stats]() {
            Q_EMIT logMessage(QString("Bundle complete: %1 succeeded, %2 failed")
                                  .arg(stats.completed)
                                  .arg(stats.failed));
            Q_EMIT operationCompleted(stats);
        },
        Qt::QueuedConnection);
}

// ============================================================================
// Install From Bundle
// ============================================================================

void OfflineDeploymentWorker::installFromBundle(const QString& manifest_path,
                                                const QString& choco_source_dir) {
    if (m_running) {
        Q_EMIT operationError("An operation is already running");
        return;
    }

    auto manifest = readManifest(manifest_path);
    if (manifest.packages.isEmpty()) {
        Q_EMIT operationError("Manifest is empty or unreadable");
        return;
    }

    m_running = true;
    m_cancelled = false;

    Q_EMIT operationStarted(manifest.packages.size());
    Q_EMIT logMessage(
        QString("Installing from bundle: %1 package(s)").arg(manifest.packages.size()));

    m_operation_future = QtConcurrent::run([this, manifest, choco_source_dir]() {
        int completed = 0;
        int failed = 0;
        int total = manifest.packages.size();

        for (const auto& entry : manifest.packages) {
            if (m_cancelled) {
                break;
            }

            QMetaObject::invokeMethod(
                this,
                [this, completed, total, entry]() {
                    Q_EMIT batchProgress(completed, total, entry.package_id);
                    Q_EMIT logMessage(
                        QString("Installing: %1 v%2").arg(entry.package_id, entry.version));
                },
                Qt::QueuedConnection);

            // Install via choco install from local source
            QProcess process;
            process.setProgram("choco");
            process.setArguments({"install",
                                  entry.package_id,
                                  "--version",
                                  entry.version,
                                  "--source",
                                  choco_source_dir,
                                  "--yes",
                                  "--no-progress",
                                  "--force"});

            process.start();
            bool started = process.waitForStarted(offline::kInstallTimeoutPerPackageMs);

            if (!started) {
                failed++;
                QMetaObject::invokeMethod(
                    this,
                    [this, entry]() {
                        Q_EMIT packageProgress(entry.package_id, false, "Process failed to start");
                    },
                    Qt::QueuedConnection);
                continue;
            }

            bool finished = process.waitForFinished(offline::kInstallTimeoutPerPackageMs);

            if (!finished) {
                process.kill();
                failed++;
                QMetaObject::invokeMethod(
                    this,
                    [this, entry]() {
                        Q_EMIT packageProgress(entry.package_id, false, "Installation timed out");
                    },
                    Qt::QueuedConnection);
                continue;
            }

            bool success = (process.exitCode() == 0);
            if (success) {
                completed++;
            } else {
                failed++;
            }

            QString output = process.readAllStandardOutput();
            QMetaObject::invokeMethod(
                this,
                [this, entry, success, output]() {
                    Q_EMIT packageProgress(entry.package_id,
                                           success,
                                           success ? "Installed" : output.left(200));
                },
                Qt::QueuedConnection);
        }

        BatchStats stats;
        stats.total = total;
        stats.completed = completed;
        stats.failed = failed;

        m_running = false;

        QMetaObject::invokeMethod(
            this, [this, stats]() { Q_EMIT operationCompleted(stats); }, Qt::QueuedConnection);
    });
}

// ============================================================================
// Direct Download
// ============================================================================

void OfflineDeploymentWorker::directDownload(const QVector<QPair<QString, QString>>& packages,
                                             const QString& output_dir) {
    if (m_running) {
        Q_EMIT operationError("An operation is already running");
        return;
    }

    if (packages.isEmpty()) {
        Q_EMIT operationError("No packages specified");
        return;
    }

    m_running = true;
    m_cancelled = false;

    Q_EMIT operationStarted(packages.size());
    Q_EMIT logMessage(QString("Direct download: %1 package(s)").arg(packages.size()));

    m_operation_future = QtConcurrent::run(
        [this, packages, output_dir]() { executeDirectDownload(packages, output_dir); });
}

void OfflineDeploymentWorker::executeDirectDownload(
    const QVector<QPair<QString, QString>>& packages, const QString& output_dir) {
    QDir(output_dir).mkpath(".");
    QNetworkAccessManager nam;
    nam.setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);

    PackageInternalizationEngine resolver;
    int completed = 0;
    int failed = 0;
    int total = packages.size();

    for (const auto& [pkg_id, version] : packages) {
        if (m_cancelled) {
            break;
        }

        QString resolved_version = version;
        if (resolved_version.isEmpty() || resolved_version == "latest") {
            resolved_version = resolver.resolveLatestVersion(pkg_id);
            if (resolved_version.isEmpty()) {
                ++failed;
                sak::logError("[DirectDownload] Version resolve failed: {}", pkg_id.toStdString());
                QMetaObject::invokeMethod(
                    this,
                    [this, pkg_id]() {
                        Q_EMIT packageProgress(pkg_id, false, "Version resolution failed");
                    },
                    Qt::QueuedConnection);
                continue;
            }
        }

        QMetaObject::invokeMethod(
            this,
            [this, completed, total, pkg_id, resolved_version]() {
                Q_EMIT batchProgress(completed, total, pkg_id);
                Q_EMIT logMessage(
                    QString("Downloading installers: %1 v%2").arg(pkg_id, resolved_version));
            },
            Qt::QueuedConnection);

        int files = downloadOnePackageInstallers(pkg_id, resolved_version, output_dir, nam);

        bool ok = (files > 0);
        if (ok) {
            ++completed;
        } else {
            ++failed;
        }

        QString msg = ok ? QString("Downloaded %1 file(s)").arg(files)
                         : QString("No installers found");
        QMetaObject::invokeMethod(
            this,
            [this, pkg_id, ok, msg]() { Q_EMIT packageProgress(pkg_id, ok, msg); },
            Qt::QueuedConnection);
    }

    BatchStats stats;
    stats.total = total;
    stats.completed = completed;
    stats.failed = failed;
    m_running = false;

    QMetaObject::invokeMethod(
        this, [this, stats]() { Q_EMIT operationCompleted(stats); }, Qt::QueuedConnection);
}

// ============================================================================
// Direct Download Helpers
// ============================================================================

void OfflineDeploymentWorker::emitLog(const QString& message) {
    QMetaObject::invokeMethod(
        this, [this, message]() { Q_EMIT logMessage(message); }, Qt::QueuedConnection);
}

int OfflineDeploymentWorker::downloadOnePackageInstallers(const QString& pkg_id,
                                                          const QString& resolved_version,
                                                          const QString& output_dir,
                                                          QNetworkAccessManager& nam) {
    QString temp_dir = output_dir + "/_sak_temp_" + pkg_id;
    QDir(temp_dir).mkpath(".");

    // Steps 1-2: Download and extract the nupkg
    QString extract_dir = downloadAndExtractNupkg(pkg_id, resolved_version, temp_dir, nam);
    if (extract_dir.isEmpty()) {
        QDir(temp_dir).removeRecursively();
        return 0;
    }

    // Step 3: Find install script (or resolve meta-package dependency)
    InstallScriptParser parser;
    PackageInternalizationEngine engine;
    QString script_path = engine.findInstallScript(extract_dir);
    QString pkg_extract_dir = extract_dir;

    if (script_path.isEmpty()) {
        auto [dep_script,
              dep_extract] = resolveMetaPackageDependency(pkg_id, extract_dir, temp_dir, nam);
        if (dep_script.isEmpty()) {
            QDir(temp_dir).removeRecursively();
            return 0;
        }
        script_path = dep_script;
        pkg_extract_dir = dep_extract;
    }

    // Parse for download URLs (primary installer only)
    auto parsed = parser.parseFile(script_path);
    QStringList urls = collectPrimaryUrls(parsed);

    // Embedded installer fallback, then URL download
    int result = 0;
    if (urls.isEmpty()) {
        result = copyEmbeddedInstallers(pkg_id, pkg_extract_dir, output_dir);
        if (result == 0) {
            emitLog(QString("[%1] No download URLs or embedded files").arg(pkg_id));
        }
    } else {
        result = downloadUrlsToDir(pkg_id, urls, output_dir, nam);
    }

    QDir(temp_dir).removeRecursively();
    return result;
}

QString OfflineDeploymentWorker::downloadAndExtractNupkg(const QString& pkg_id,
                                                         const QString& resolved_version,
                                                         const QString& temp_dir,
                                                         QNetworkAccessManager& nam) {
    QString nupkg_url =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, pkg_id, resolved_version);
    QString nupkg_path = temp_dir + "/" + pkg_id + ".nupkg";

    if (!downloadFileFromUrl(nupkg_url, nupkg_path, nam)) {
        emitLog(QString("[%1] nupkg download failed").arg(pkg_id));
        return {};
    }

    QString extract_dir = temp_dir + "/extracted";
    QString extract_error;
    PackageInternalizationEngine engine;
    if (!engine.extractNupkg(nupkg_path, extract_dir, extract_error)) {
        emitLog(QString("[%1] Extract failed: %2").arg(pkg_id, extract_error));
        return {};
    }

    return extract_dir;
}

QPair<QString, QString> OfflineDeploymentWorker::resolveMetaPackageDependency(
    const QString& pkg_id,
    const QString& extract_dir,
    const QString& temp_dir,
    QNetworkAccessManager& nam) {
    QString dep_id = findNuspecDependencyId(extract_dir);
    if (dep_id.isEmpty()) {
        emitLog(QString("[%1] No chocolateyInstall.ps1 found").arg(pkg_id));
        return {};
    }

    emitLog(QString("[%1] Meta-package → resolving %2").arg(pkg_id, dep_id));

    PackageInternalizationEngine dep_engine;
    QString dep_version = dep_engine.resolveLatestVersion(dep_id);
    if (dep_version.isEmpty()) {
        emitLog(QString("[%1] Version resolve failed for %2").arg(pkg_id, dep_id));
        return {};
    }

    QString dep_nupkg_url =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, dep_id, dep_version);
    QString dep_nupkg_path = temp_dir + "/" + dep_id + ".nupkg";
    if (!downloadFileFromUrl(dep_nupkg_url, dep_nupkg_path, nam)) {
        emitLog(QString("[%1] Dependency nupkg download failed").arg(dep_id));
        return {};
    }

    QString dep_extract = temp_dir + "/dep_extracted";
    QString dep_error;
    PackageInternalizationEngine engine;
    if (!engine.extractNupkg(dep_nupkg_path, dep_extract, dep_error)) {
        emitLog(QString("[%1] Dependency extract failed: %2").arg(dep_id, dep_error));
        return {};
    }

    QString script_path = engine.findInstallScript(dep_extract);
    if (script_path.isEmpty()) {
        emitLog(QString("[%1] Dependency %2 also has no script").arg(pkg_id, dep_id));
        return {};
    }

    return {script_path, dep_extract};
}

int OfflineDeploymentWorker::copyEmbeddedInstallers(const QString& pkg_id,
                                                    const QString& pkg_extract_dir,
                                                    const QString& output_dir) {
    QDir tools_dir(pkg_extract_dir + "/tools");
    if (!tools_dir.exists()) {
        return 0;
    }

    QStringList embedded = tools_dir.entryList({"*.exe", "*.msi"}, QDir::Files);
    if (embedded.isEmpty()) {
        return 0;
    }

    emitLog(QString("[%1] Found %2 embedded installer(s)").arg(pkg_id).arg(embedded.size()));
    int copied = 0;
    for (const auto& name : embedded) {
        QString src = tools_dir.filePath(name);
        QString dest = output_dir + "/" + name;
        if (QFile::copy(src, dest)) {
            ++copied;
            sak::logInfo("[DirectDownload] Embedded: {}", name.toStdString());
        } else {
            emitLog(QString("[%1] Copy failed: %2").arg(pkg_id, name));
        }
    }
    return copied;
}

QStringList OfflineDeploymentWorker::collectPrimaryUrls(const ParsedInstallScript& parsed) {
    QStringList urls;
    if (parsed.resources.isEmpty()) {
        return urls;
    }
    const auto& primary = parsed.resources.first();
    if (!primary.url.isEmpty()) {
        urls.append(primary.url);
    }
    if (!primary.url_64bit.isEmpty()) {
        urls.append(primary.url_64bit);
    }
    return urls;
}

int OfflineDeploymentWorker::downloadUrlsToDir(const QString& pkg_id,
                                               const QStringList& urls,
                                               const QString& output_dir,
                                               QNetworkAccessManager& nam) {
    int downloaded = 0;
    for (const auto& url : urls) {
        if (m_cancelled) {
            break;
        }
        QString filename = QUrl(url).fileName();
        if (filename.isEmpty()) {
            filename = QString("%1_installer_%2").arg(pkg_id).arg(downloaded + 1);
        }
        QString dest = output_dir + "/" + filename;
        if (downloadFileFromUrl(url, dest, nam)) {
            ++downloaded;
            sak::logInfo("[DirectDownload] Saved: {}", filename.toStdString());
        } else {
            emitLog(QString("[%1] Download failed: %2").arg(pkg_id, url));
        }
    }

    if (downloaded == 0 && !urls.isEmpty()) {
        emitLog(
            QString("[%1] Found %2 URL(s) but all downloads failed").arg(pkg_id).arg(urls.size()));
    }
    return downloaded;
}

bool OfflineDeploymentWorker::downloadFileFromUrl(const QString& url,
                                                  const QString& output_path,
                                                  QNetworkAccessManager& nam) {
    QNetworkRequest request{QUrl(url)};
    request.setHeader(QNetworkRequest::UserAgentHeader, "SAK-Utility/1.0");
    request.setTransferTimeout(offline::kDownloadTimeoutMs);

    QEventLoop loop;
    bool ok = false;

    auto* reply = nam.get(request);
    QObject::connect(reply, &QNetworkReply::finished, &loop, [&]() {
        if (reply->error() == QNetworkReply::NoError) {
            QFile file(output_path);
            if (file.open(QIODevice::WriteOnly)) {
                file.write(reply->readAll());
                file.close();
                ok = true;
            }
        } else {
            int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            sak::logError("[DirectDownload] HTTP {} for {}: {}",
                          status,
                          url.toStdString(),
                          reply->errorString().toStdString());
        }
        reply->deleteLater();
        loop.quit();
    });

    loop.exec();
    return ok;
}

// ============================================================================
// Cancel / Status
// ============================================================================

void OfflineDeploymentWorker::cancel() {
    m_cancelled = true;
    m_engine.cancel();
}

bool OfflineDeploymentWorker::isRunning() const {
    return m_running;
}

BatchStats OfflineDeploymentWorker::getStats() const {
    QMutexLocker lock(&m_mutex);
    BatchStats stats;
    stats.total = m_jobs.size();
    for (const auto& job : m_jobs) {
        switch (job.status) {
        case InternalizationStatus::Complete:
            stats.completed++;
            break;
        case InternalizationStatus::Failed:
            stats.failed++;
            break;
        case InternalizationStatus::Cancelled:
            stats.cancelled++;
            break;
        default:
            stats.pending++;
            break;
        }
    }
    return stats;
}

// ============================================================================
// Manifest I/O
// ============================================================================

bool OfflineDeploymentWorker::writeManifest(const DeploymentManifest& manifest,
                                            const QString& output_dir) {
    QJsonObject root;
    root["manifest_version"] = manifest.manifest_version;
    root["created"] = manifest.created_date;
    root["creator"] = manifest.creator;
    root["description"] = manifest.description;
    root["total_size_bytes"] = manifest.total_size_bytes;

    QJsonArray packages_arr;
    for (const auto& entry : manifest.packages) {
        QJsonObject pkg;
        pkg["package_id"] = entry.package_id;
        pkg["version"] = entry.version;
        pkg["filename"] = entry.nupkg_filename;
        pkg["checksum"] = entry.checksum;
        pkg["size_bytes"] = entry.size_bytes;
        pkg["internalized"] = entry.internalized;

        QJsonArray deps;
        for (const auto& dep : entry.dependencies) {
            deps.append(dep);
        }
        pkg["dependencies"] = deps;

        packages_arr.append(pkg);
    }
    root["packages"] = packages_arr;

    QString manifest_path = output_dir + "/" + offline::kManifestFilename;
    QFile file(manifest_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logError("[OfflineDeploymentWorker] Cannot write manifest: {}",
                      manifest_path.toStdString());
        return false;
    }

    QJsonDocument doc(root);
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();

    sak::logInfo("[OfflineDeploymentWorker] Manifest written: {}", manifest_path.toStdString());
    return true;
}

void OfflineDeploymentWorker::writeReadme(const DeploymentManifest& manifest,
                                          const QString& output_dir) const {
    QString readme_path = output_dir + "/" + offline::kReadmeFilename;
    QFile file(readme_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    QTextStream stream(&file);
    stream << "S.A.K. Utility - Offline Deployment Package\n";
    stream << "============================================\n\n";
    stream << "Created: " << manifest.created_date << "\n";
    if (!manifest.description.isEmpty()) {
        stream << "Description: " << manifest.description << "\n";
    }
    stream << "Packages: " << manifest.packages.size() << "\n\n";

    stream << "Included Packages:\n";
    for (const auto& entry : manifest.packages) {
        stream << "  - " << entry.package_id << " v" << entry.version;
        if (entry.internalized) {
            stream << " (internalized)";
        }
        stream << "\n";
    }

    stream << "\nInstallation:\n";
    stream << "  1. Copy this folder to the target machine\n";
    stream << "  2. Open S.A.K. Utility on the target machine\n";
    stream << "  3. Go to App Management > Offline Deploy tab\n";
    stream << "  4. Click 'Install from Bundle' and select manifest.json\n";

    file.close();
}

DeploymentManifest OfflineDeploymentWorker::readManifest(const QString& path) const {
    DeploymentManifest manifest;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logError("[OfflineDeploymentWorker] Cannot read manifest: {}", path.toStdString());
        return manifest;
    }

    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    file.close();

    if (parse_error.error != QJsonParseError::NoError) {
        sak::logError("[OfflineDeploymentWorker] Manifest parse error: {}",
                      parse_error.errorString().toStdString());
        return manifest;
    }

    QJsonObject root = doc.object();
    manifest.manifest_version = root["manifest_version"].toString();
    manifest.created_date = root["created"].toString();
    manifest.creator = root["creator"].toString();
    manifest.description = root["description"].toString();
    manifest.total_size_bytes = root["total_size_bytes"].toInteger();

    QJsonArray packages_arr = root["packages"].toArray();
    for (const auto& val : packages_arr) {
        QJsonObject pkg = val.toObject();
        DeploymentManifestEntry entry;
        entry.package_id = pkg["package_id"].toString();
        entry.version = pkg["version"].toString();
        entry.nupkg_filename = pkg["filename"].toString();
        entry.checksum = pkg["checksum"].toString();
        entry.size_bytes = pkg["size_bytes"].toInteger();
        entry.internalized = pkg["internalized"].toBool();

        QJsonArray deps = pkg["dependencies"].toArray();
        for (const auto& dep : deps) {
            entry.dependencies.append(dep.toString());
        }

        manifest.packages.append(entry);
    }

    return manifest;
}

}  // namespace sak
