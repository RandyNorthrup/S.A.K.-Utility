// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file offline_deployment_worker.cpp
/// @brief Background worker implementation for batch offline deployment

#include "sak/offline_deployment_worker.h"

#include "sak/bundled_tools_manager.h"
#include "sak/logger.h"
#include "sak/network_transfer_runner.h"
#include "sak/offline_deployment_constants.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QSet>
#include <QtConcurrent>
#include <QTemporaryDir>
#include <QTextStream>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

namespace sak {

namespace {
constexpr qsizetype kInstallErrorPreviewChars = 200;

/// @brief Manifest-facing view of an engine OfflineReadiness classification.
struct OfflineReadinessInfo {
    bool ready;
    QString note;
};

OfflineReadinessInfo readinessInfo(OfflineReadiness readiness) {
    switch (readiness) {
    case OfflineReadiness::Internalized:
        return {true, QStringLiteral("installer binaries internalized")};
    case OfflineReadiness::SelfContained:
        return {true, QStringLiteral("self-contained (no external download needed)")};
    case OfflineReadiness::RequiresNetwork:
        break;
    }
    return {false,
            QStringLiteral("requires internet at install time (installer not internalized)")};
}

/// @brief Honest one-line summary of a completed bundle build.
QString bundleCompletionMessage(const BatchStats& stats) {
    QString summary = QString("Bundle complete: %1 succeeded (%2 fully offline-capable")
                          .arg(stats.completed)
                          .arg(stats.offline_capable);
    if (stats.requires_network > 0) {
        summary += QString(", %1 still require internet at install").arg(stats.requires_network);
    }
    return summary + QString("), %1 failed").arg(stats.failed);
}

/// @brief Honest one-line summary of a completed bundle install.
QString installCompletionMessage(const BatchStats& stats) {
    QString summary =
        QString("Install complete: %1 installed, %2 failed").arg(stats.completed).arg(stats.failed);
    if (stats.skipped > 0) {
        summary += QString(", %1 skipped (air-gap: not fully packed)").arg(stats.skipped);
    }
    if (stats.cancelled > 0) {
        summary += QString(", %1 cancelled").arg(stats.cancelled);
    }
    if (stats.pending > 0) {
        summary += QString(", %1 not attempted").arg(stats.pending);
    }
    return summary;
}

/// @brief Validate the id + version a manifest entry will pass to choco: a safe
/// package component AND non-option-like (defense in depth vs a tampered manifest).
bool entryInstallTokensValid(const DeploymentManifestEntry& entry) {
    return PackageInternalizationEngine::isSafePackageComponent(entry.package_id) &&
           OfflineDeploymentWorker::isSafeInstallToken(entry.package_id) &&
           OfflineDeploymentWorker::isSafeInstallToken(entry.version);
}

/// @brief Build the header fields (no packages) of a deployment manifest.
DeploymentManifest makeManifestHeader(const QString& output_dir,
                                      const QString& description,
                                      PayloadMode mode) {
    DeploymentManifest manifest;
    manifest.manifest_version = offline::kManifestVersion;
    manifest.created_date = QDateTime::currentDateTime().toString(Qt::ISODate);
    manifest.creator = "S.A.K. Utility";
    manifest.description = description;
    manifest.output_dir = output_dir;
    manifest.payload_mode = mode;
    return manifest;
}

/// @brief Honest one-line outcome for a finished choco install attempt.
QString installOutcomeText(const ProcessResult& process, bool success) {
    if (process.cancelled) {
        return QStringLiteral("Installation cancelled");
    }
    if (process.timed_out) {
        return QStringLiteral("Installation timed out");
    }
    if (success) {
        return QStringLiteral("Installed");
    }
    return (process.std_out.isEmpty() ? process.std_err : process.std_out)
        .left(kInstallErrorPreviewChars);
}
}  // namespace

// ============================================================================
// Helpers
// ============================================================================

bool OfflineDeploymentWorker::isChocolateyFrameworkId(const QString& id) {
    // The 'chocolatey' framework package bootstraps Chocolatey onto a machine
    // (machine ChocolateyInstall env + PATH + %ProgramData%\chocolatey). A
    // portable tool must never bundle/install it; the bundled portable choco
    // provides the framework at deploy time. Many packages (git.install and
    // virtually every *.install) declare it as a dependency, so it enters the
    // closure and must be dropped. chocolatey.extension / chocolatey-core.extension
    // are legitimate content and are NOT the framework.
    return id.compare(QLatin1String("chocolatey"), Qt::CaseInsensitive) == 0;
}

bool OfflineDeploymentWorker::isSafeInstallToken(const QString& token) {
    // isSafePackageComponent alone does NOT reject a leading '-', so a manifest id
    // like "--production" (or a crafted version) could inject a Chocolatey flag.
    if (token.isEmpty() || token.startsWith(QLatin1Char('-'))) {
        return false;
    }
    for (const QChar c : token) {
        if (c.isSpace() || c.unicode() < 0x20) {
            return false;
        }
    }
    return true;
}

/// @brief Parse .nuspec in extract_dir for the first dependency package ID.
/// Prefers dependencies ending in ".install" (e.g. 7zip -> 7zip.install).
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
// Path-safety seams (pure; unit-tested)
// ============================================================================

OfflineDeploymentWorker::WorkDirDisposition OfflineDeploymentWorker::classifyWorkDir(
    bool exists, bool has_ownership_marker, bool is_empty) {
    if (!exists) {
        return WorkDirDisposition::CreateFresh;
    }
    if (has_ownership_marker) {
        return WorkDirDisposition::ReuseOwned;  // leftover from a prior build we created
    }
    // Adopt an empty unstamped dir; refuse a non-empty one (may hold user data).
    return is_empty ? WorkDirDisposition::CreateFresh : WorkDirDisposition::RefuseForeign;
}

QString OfflineDeploymentWorker::safeInstallerFilename(const QString& raw_name,
                                                       const QString& fallback) {
    // Reduce to a single path segment: QUrl::fileName() decodes percent-encoding,
    // so a crafted URL (e.g. ..%2F..%2Fx) can arrive bearing separators or "..".
    const QString base = QFileInfo(raw_name).fileName();
    if (base.isEmpty() || base == QStringLiteral(".") || base == QStringLiteral("..") ||
        base.contains(QLatin1Char('/')) || base.contains(QLatin1Char('\\'))) {
        return fallback;
    }
    return base;
}

QString OfflineDeploymentWorker::sanitizeManifestFilename(const QString& raw_name) {
    const QString base = QFileInfo(raw_name).fileName();
    if (base.isEmpty() || base == QStringLiteral(".") || base == QStringLiteral("..") ||
        base.contains(QLatin1Char('/')) || base.contains(QLatin1Char('\\'))) {
        return {};
    }
    return base;
}

bool OfflineDeploymentWorker::prepareOwnedWorkDir(const QString& work_dir, QString& error_out) {
    const QString marker = work_dir + "/" + offline::kWorkDirOwnershipMarker;
    const QDir dir(work_dir);
    const bool exists = dir.exists();
    const WorkDirDisposition disposition =
        classifyWorkDir(exists, QFile::exists(marker), exists && dir.isEmpty());

    if (disposition == WorkDirDisposition::RefuseForeign) {
        error_out = "Refusing to reuse existing work directory not created by S.A.K.: " + work_dir;
        return false;
    }
    if (!QDir().mkpath(work_dir)) {
        error_out = "Failed to create work directory: " + work_dir;
        return false;
    }
    // Stamp ownership so a later removeRecursively() only ever deletes our tree.
    QFile stamp(marker);
    if (stamp.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        stamp.write("S.A.K. Utility offline-deployment work directory\n");
        stamp.close();
    }
    return true;
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
    const QString& description,
    PayloadMode mode) {
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
    Q_EMIT logMessage(QString("Building %1: %2 package(s)")
                          .arg(mode == PayloadMode::List
                                   ? QStringLiteral("Thin Bundle (metadata-only)")
                                   : QStringLiteral("Full Bundle (self-contained)"))
                          .arg(packages.size()));

    if (mode == PayloadMode::List) {
        m_operation_future = QtConcurrent::run([this, output_dir, description]() {
            executeBuildListManifest(output_dir, description);
        });
        return;
    }
    m_operation_future = QtConcurrent::run(
        [this, output_dir, description]() { executeBuildBundle(output_dir, description); });
}

void OfflineDeploymentWorker::executeBuildBundle(const QString& output_dir,
                                                 const QString& description) {
    BuildBundleContext ctx;
    ctx.packages_dir = output_dir + "/" + offline::kPackagesSubdir;
    ctx.work_dir = output_dir + "/_work";
    ctx.output_dir = output_dir;

    // Establish ownership of _work BEFORE any content is written there, so the
    // recursive cleanup in finalizeBundle can never wipe a foreign directory.
    QString work_err;
    if (!prepareOwnedWorkDir(ctx.work_dir, work_err)) {
        m_running = false;
        QMetaObject::invokeMethod(
            this, [this, work_err]() { Q_EMIT operationError(work_err); }, Qt::QueuedConnection);
        return;
    }
    QDir(ctx.packages_dir).mkpath(".");

    // Expand the user's package list into the full transitive dependency closure
    // (honoring each dependency's declared version range) BEFORE internalizing, so
    // the bundle is genuinely self-contained for offline install. Replaces m_jobs
    // with the resolved closure; surfaces any resolution warning to the UI log.
    QStringList resolve_warnings;
    const QVector<BatchInternalizationJob> closure = resolveDependencyClosure(resolve_warnings);
    for (const QString& warning : resolve_warnings) {
        emitLog(QStringLiteral("[Dependencies] %1").arg(warning));
    }

    // Surface (never silently ship) any dependency the resolved closure declares
    // but could not pack. Previously these were ignored, so a bundle looked
    // self-contained when it was not. Such a package is NOT a build error for a
    // normal Bundle: it is fetched from the feed at install time (will-fetch). Only
    // an air-gap (packed_only) install cannot fetch it, and that is enforced
    // per-entry at install time -- so warn + record here rather than abort.
    const QStringList unmet = unmetClosureDependencies(closure);
    if (!unmet.isEmpty()) {
        emitLog(QStringLiteral("[Dependencies] Not packed -- will be fetched from the feed at "
                               "install (unavailable for air-gap/packed-only install): %1")
                    .arg(unmet.join(QStringLiteral(", "))));
    }
    {
        QMutexLocker jobs_lock(&m_mutex);
        m_jobs = closure;
    }

    DeploymentManifest manifest = makeManifestHeader(output_dir, description, PayloadMode::Bundle);

    QMutexLocker lock(&m_mutex);
    ctx.total_jobs = m_jobs.size();
    lock.unlock();

    if (ctx.total_jobs > 0) {
        emitLog(
            QStringLiteral("Resolved %1 package(s) including dependencies").arg(ctx.total_jobs));
    }

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

void OfflineDeploymentWorker::executeBuildListManifest(const QString& output_dir,
                                                       const QString& description) {
    // List payload: metadata only. No closure resolution, no download -- the
    // target's bundled choco fetches + resolves each package from the feed at
    // install time. Fast and network-free to build.
    QVector<BatchInternalizationJob> requested;
    {
        QMutexLocker lock(&m_mutex);
        requested = m_jobs;
    }

    DeploymentManifest manifest = makeManifestHeader(output_dir, description, PayloadMode::List);

    for (const BatchInternalizationJob& job : requested) {
        if (isChocolateyFrameworkId(job.package_id)) {
            continue;  // never deploy the Chocolatey framework onto a target
        }
        DeploymentManifestEntry entry;
        entry.package_id = job.package_id;
        entry.version = job.version;
        entry.internalized = false;
        entry.offline_ready = false;  // a List entry always fetches at install time
        entry.offline_note =
            QStringLiteral("metadata-only; downloaded from the Chocolatey feed at install");
        manifest.packages.append(entry);
    }

    QDir(output_dir).mkpath(QStringLiteral("."));
    if (manifest.packages.isEmpty() || !writeManifest(manifest, output_dir)) {
        m_running = false;
        QMetaObject::invokeMethod(
            this,
            [this]() { Q_EMIT operationError("Failed to write the List payload manifest"); },
            Qt::QueuedConnection);
        return;
    }
    writeReadme(manifest, output_dir);

    BatchStats stats;
    stats.total = manifest.packages.size();
    stats.completed = manifest.packages.size();
    stats.requires_network = manifest.packages.size();

    m_running = false;
    QMetaObject::invokeMethod(
        this,
        [this, stats, output_dir]() {
            Q_EMIT manifestWritten(output_dir + "/" + offline::kManifestFilename);
            Q_EMIT logMessage(
                QString("Thin Bundle written: %1 package(s), fetched at install").arg(stats.total));
            Q_EMIT operationCompleted(stats);
        },
        Qt::QueuedConnection);
}

// ============================================================================
// Transitive dependency resolution (the offline bundle's completeness guarantee)
// ============================================================================

QVector<FeedPackageVersion> OfflineDeploymentWorker::fetchFeedVersions(const QString& package_id,
                                                                       bool& ok) {
    ok = false;
    // A crafted id is embedded into the feed query; keep it a single safe segment.
    if (!PackageInternalizationEngine::isSafePackageComponent(package_id)) {
        emitLog(QStringLiteral("[Dependencies] Rejected unsafe package id: %1").arg(package_id));
        return {};
    }

    QUrl url(QString("%1%2").arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), QStringLiteral("'%1'").arg(package_id));
    url.setQuery(query);

    NetworkTransferRequest request;
    request.url = url;
    request.timeout_ms = offline::kApiRequestTimeoutMs;
    // Bound the feed document BEFORE it is buffered and DOM-parsed, so a hostile or
    // oversized endpoint cannot exhaust memory here.
    request.max_response_bytes = offline::kMaxFeedResponseBytes;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("Accept"),
                                                             QByteArrayLiteral("application/xml")});

    const auto transfer = runNetworkTransfer(request, [this]() { return m_cancelled.load(); });
    if (transfer.cancelled) {
        return {};  // ok stays false; a cancelled fetch is not a package-missing answer
    }
    if (!transfer.success) {
        emitLog(QStringLiteral("[Dependencies] Feed fetch HTTP %1 for %2")
                    .arg(transfer.http_status)
                    .arg(package_id));
        return {};
    }

    // A successful transfer is an authoritative answer even if it lists no
    // versions (the resolver then records "no satisfying version").
    ok = true;
    return NuGetDependencyResolver::parseODataFeedVersions(transfer.body);
}

QVector<BatchInternalizationJob> OfflineDeploymentWorker::resolveDependencyClosure(
    QStringList& warnings) {
    QVector<BatchInternalizationJob> requested;
    {
        QMutexLocker lock(&m_mutex);
        requested = m_jobs;
    }

    NuGetDependencyResolver resolver(offline::kMaxDependencyDepth, offline::kMaxPackagesPerBuild);
    bool seeded = false;
    for (const BatchInternalizationJob& job : requested) {
        if (!seeded) {
            resolver.start(job.package_id, job.version);
            seeded = true;
        } else {
            resolver.addRoot(job.package_id, job.version);
        }
    }

    while (!resolver.isComplete()) {
        if (m_cancelled) {
            resolver.cancel();
            break;
        }
        const QString id = resolver.nextFetchId();
        bool ok = false;
        const QVector<FeedPackageVersion> versions = fetchFeedVersions(id, ok);
        if (m_cancelled) {
            // A cancel that arrived DURING the fetch is not a feed failure -- do
            // not record a bogus "failed to fetch" for it; just stop.
            resolver.cancel();
            break;
        }
        if (ok) {
            resolver.provideFeed(id, versions);
        } else {
            resolver.provideFeedFailure(id);
        }
    }

    warnings = resolver.errors();
    return assembleClosureJobs(resolver.resolved(), requested, warnings);
}

QVector<BatchInternalizationJob> OfflineDeploymentWorker::assembleClosureJobs(
    const QVector<ResolvedPackage>& resolved,
    const QVector<BatchInternalizationJob>& requested,
    QStringList& warnings) {
    QVector<BatchInternalizationJob> jobs;
    QSet<QString> resolved_ids;
    bool excluded_framework = false;
    for (const ResolvedPackage& pkg : resolved) {
        // Drop the 'chocolatey' framework package: bundling/force-installing it
        // would bootstrap Chocolatey onto the clean target, which this portable
        // tool must never do. The bundled portable choco supplies it at deploy.
        if (isChocolateyFrameworkId(pkg.package_id)) {
            excluded_framework = true;
            continue;
        }
        BatchInternalizationJob job;
        job.package_id = pkg.package_id;
        job.version = pkg.version;
        job.dependencies = pkg.dependencies;  // direct deps -> manifest provenance
        jobs.append(job);
        resolved_ids.insert(pkg.package_id.toLower());
    }

    // NEVER drop an explicitly-requested package. If its feed fetch failed (or it
    // resolved to no version), it is still appended so internalizeOnePackage
    // attempts it directly -- preserving the pre-closure behavior where every
    // requested package was built. Closure resolution only ADDS dependencies.
    for (const BatchInternalizationJob& req : requested) {
        if (isChocolateyFrameworkId(req.package_id)) {
            excluded_framework = true;
            continue;
        }
        if (!resolved_ids.contains(req.package_id.toLower())) {
            jobs.append(req);
            resolved_ids.insert(req.package_id.toLower());
            warnings.append(
                QStringLiteral("Requested package %1 could not be resolved from the feed; "
                               "attempting it directly")
                    .arg(req.package_id));
        }
    }

    if (excluded_framework) {
        warnings.append(
            QStringLiteral("Excluded the 'chocolatey' framework package from the payload; the "
                           "bundled portable Chocolatey provides it at deploy time (it is never "
                           "installed onto the target machine)."));
    }

    return jobs;
}

QStringList OfflineDeploymentWorker::unmetClosureDependencies(
    const QVector<BatchInternalizationJob>& jobs) {
    QSet<QString> present;
    for (const BatchInternalizationJob& job : jobs) {
        present.insert(job.package_id.toLower());
    }
    QStringList missing;
    QSet<QString> reported;
    for (const BatchInternalizationJob& job : jobs) {
        for (const QString& dep : job.dependencies) {
            const QString lower = dep.toLower();
            // The 'chocolatey' framework is deliberately excluded (supplied at
            // deploy by the bundled portable choco), so it is never "missing".
            if (isChocolateyFrameworkId(dep) || present.contains(lower) ||
                reported.contains(lower)) {
                continue;
            }
            reported.insert(lower);
            missing.append(dep);
        }
    }
    return missing;
}

bool OfflineDeploymentWorker::internalizeOnePackage(int idx,
                                                    const BuildBundleContext& ctx,
                                                    DeploymentManifest& manifest) {
    const BatchInternalizationJob job = beginInternalizationJob(idx);
    emitInternalizationStarted(ctx, job.package_id);

    const InternalizationResult result = runInternalizationJob(job, ctx);
    applyInternalizationResult(idx, result, manifest);
    emitInternalizationResult(job.package_id, result);

    return result.success;
}

BatchInternalizationJob OfflineDeploymentWorker::beginInternalizationJob(int idx) {
    QMutexLocker lock(&m_mutex);
    BatchInternalizationJob job = m_jobs[idx];
    m_jobs[idx].status = InternalizationStatus::DownloadingNupkg;
    return job;
}

void OfflineDeploymentWorker::emitInternalizationStarted(const BuildBundleContext& ctx,
                                                         const QString& pkg_id) {
    QMetaObject::invokeMethod(
        this,
        [this, completed_count = ctx.completed_count, total_jobs = ctx.total_jobs, pkg_id]() {
            Q_EMIT batchProgress(completed_count, total_jobs, pkg_id);
            Q_EMIT logMessage(QString("Internalizing: %1").arg(pkg_id));
        },
        Qt::QueuedConnection);
}

InternalizationResult OfflineDeploymentWorker::runInternalizationJob(
    const BatchInternalizationJob& job, const BuildBundleContext& ctx) {
    InternalizationResult result;
    bool got_result = false;

    PackageInternalizationEngine engine;
    connect(&engine,
            &PackageInternalizationEngine::packageComplete,
            [&](const InternalizationResult& res) {
                result = res;
                got_result = true;
            });

    connect(&engine,
            &PackageInternalizationEngine::progressChanged,
            this,
            [this, pkg_id = job.package_id](const InternalizationProgress& progress) {
                QMetaObject::invokeMethod(
                    this,
                    [this, pkg_id, progress]() {
                        Q_EMIT logMessage(QString("[%1] %2").arg(pkg_id, progress.status_message));
                    },
                    Qt::QueuedConnection);
            });

    // Publish the active engine so cancel() (UI thread) can abort THIS engine's
    // in-flight download; clear it before the local engine goes out of scope.
    {
        QMutexLocker lock(&m_mutex);
        m_active_engine = &engine;
    }
    if (m_cancelled) {
        engine.cancel();  // a cancel that landed before we published the pointer
    }
    engine.internalizePackage(job.package_id, job.version, ctx.packages_dir, ctx.work_dir);
    {
        QMutexLocker lock(&m_mutex);
        m_active_engine = nullptr;
    }

    if (!got_result) {
        result.package_id = job.package_id;
        result.version = job.version;
        result.error_message = QStringLiteral("Internalization finished without a result");
    }
    return result;
}

void OfflineDeploymentWorker::applyInternalizationResult(int idx,
                                                         const InternalizationResult& result,
                                                         DeploymentManifest& manifest) {
    QMutexLocker lock(&m_mutex);
    if (result.success) {
        m_jobs[idx].status = InternalizationStatus::Complete;
        m_jobs[idx].output_path = result.output_nupkg_path;
        m_jobs[idx].checksum = result.checksum;

        DeploymentManifestEntry entry;
        entry.package_id = result.package_id;
        entry.version = result.version;
        entry.nupkg_filename = QFileInfo(result.output_nupkg_path).fileName();
        entry.checksum = result.checksum;
        entry.size_bytes = result.internalized_size;
        // Record the HONEST offline classification: internalized (binaries
        // embedded), self-contained (no download needed), or requires-network
        // (repacked but its script still fetches at install time). The manifest
        // must not claim a package is offline-ready when it is not.
        entry.internalized = result.binaries_internalized;
        const OfflineReadinessInfo info = readinessInfo(result.offline_readiness);
        entry.offline_ready = info.ready;
        entry.offline_note = info.note;
        entry.dependencies = m_jobs[idx].dependencies;  // direct deps from the resolved closure
        manifest.packages.append(entry);
        manifest.total_size_bytes += result.internalized_size;
    } else {
        m_jobs[idx].status = InternalizationStatus::Failed;
        m_jobs[idx].error_message = result.error_message;
    }
}

void OfflineDeploymentWorker::emitInternalizationResult(const QString& pkg_id,
                                                        const InternalizationResult& result) {
    QMetaObject::invokeMethod(
        this,
        [this, pkg_id, result]() {
            Q_EMIT packageProgress(pkg_id,
                                   result.success,
                                   result.success ? "Complete" : result.error_message);
        },
        Qt::QueuedConnection);
}

void OfflineDeploymentWorker::finalizeBundle(const DeploymentManifest& manifest,
                                             const BuildBundleContext& ctx) {
    // Write manifest. A failed/short manifest write must NOT be reported as a completed bundle:
    // installFromBundle would later fail with "Manifest is empty or unreadable".
    if (!manifest.packages.isEmpty()) {
        if (!writeManifest(manifest, ctx.output_dir)) {
            QDir(ctx.work_dir).removeRecursively();
            m_running = false;
            QMetaObject::invokeMethod(
                this,
                [this, dir = ctx.output_dir]() {
                    Q_EMIT operationError("Failed to write manifest to " + dir);
                },
                Qt::QueuedConnection);
            return;
        }
        writeReadme(manifest, ctx.output_dir);
        QMetaObject::invokeMethod(
            this,
            [this, output_dir = ctx.output_dir]() {
                Q_EMIT manifestWritten(output_dir + "/" + offline::kManifestFilename);
            },
            Qt::QueuedConnection);
    }

    // Clean up work directory
    QDir(ctx.work_dir).removeRecursively();

    // Build final stats
    BatchStats stats;
    stats.total = ctx.total_jobs;
    stats.completed = ctx.completed_count;
    stats.failed = ctx.failed_count;
    stats.total_bytes = manifest.total_size_bytes;

    // Distinguish truly-offline packages from ones that will still need the
    // internet at install time, so the completion is honest about the bundle's
    // real offline coverage instead of conflating the two into "N succeeded".
    for (const DeploymentManifestEntry& entry : manifest.packages) {
        if (entry.offline_ready) {
            stats.offline_capable++;
        } else {
            stats.requires_network++;
        }
    }

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
            Q_EMIT logMessage(bundleCompletionMessage(stats));
            Q_EMIT operationCompleted(stats);
        },
        Qt::QueuedConnection);
}

// ============================================================================
// Install From Bundle
// ============================================================================

OfflineDeploymentWorker::InstallDisposition OfflineDeploymentWorker::installDispositionFor(
    const DeploymentManifestEntry& entry, bool packed_only) {
    // Default: install everything. Only the air-gap switch (packed_only) skips a
    // not-fully-packed (will-fetch) package -- on a deliberately disconnected
    // target it could only fail, so skip it with a clear reason instead.
    if (packed_only && !entry.offline_ready) {
        return InstallDisposition::SkipNotPacked;
    }
    return InstallDisposition::Install;
}

// Fill the dependency edges among @p packages: enables[d] gets each package that
// depends on d, and in_degree[i] counts i's dependencies that are present in the
// set. Only intra-set edges count (a dependency not in the payload is ignored).
static void buildInstallEdges(const QVector<DeploymentManifestEntry>& packages,
                              const QHash<QString, int>& index_by_id,
                              QVector<QVector<int>>& enables,
                              QVector<int>& in_degree) {
    for (int i = 0; i < packages.size(); ++i) {
        for (const QString& dep : packages[i].dependencies) {
            const auto it = index_by_id.constFind(dep.toLower());
            if (it != index_by_id.constEnd() && *it != i) {
                enables[*it].append(i);
                ++in_degree[i];
            }
        }
    }
}

QVector<DeploymentManifestEntry> OfflineDeploymentWorker::topologicalInstallOrder(
    const QVector<DeploymentManifestEntry>& packages, QStringList* cyclic_ids) {
    QHash<QString, int> index_by_id;
    for (int i = 0; i < packages.size(); ++i) {
        index_by_id.insert(packages[i].package_id.toLower(), i);
    }
    QVector<QVector<int>> enables(packages.size());
    QVector<int> in_degree(packages.size(), 0);
    buildInstallEdges(packages, index_by_id, enables, in_degree);

    QVector<int> queue;
    for (int i = 0; i < packages.size(); ++i) {
        if (in_degree[i] == 0) {
            queue.append(i);  // deps-free packages first, in original order (stable)
        }
    }
    QVector<DeploymentManifestEntry> ordered;
    QVector<bool> emitted(packages.size(), false);
    for (int head = 0; head < queue.size(); ++head) {
        const int n = queue[head];
        ordered.append(packages[n]);
        emitted[n] = true;
        for (const int m : enables[n]) {
            if (--in_degree[m] == 0) {
                queue.append(m);
            }
        }
    }
    // A dependency cycle leaves some packages unemitted: append them in original
    // order so nothing is ever dropped from the install set, and surface their ids
    // so the caller can warn (the cycle is NOT dependency-ordered).
    for (int i = 0; i < packages.size(); ++i) {
        if (!emitted[i]) {
            ordered.append(packages[i]);
            if (cyclic_ids != nullptr) {
                cyclic_ids->append(packages[i].package_id);
            }
        }
    }
    return ordered;
}

BundleInstallContext OfflineDeploymentWorker::installContextForMode(
    PayloadMode mode, const QString& local_source_dir) {
    if (mode == PayloadMode::List) {
        // Metadata-only payload: install straight from the Chocolatey feed and let
        // choco resolve + download dependencies. No local .nupkg to verify.
        return {offline::kNuGetBaseUrl, /*ignore_dependencies=*/false, /*verify_local=*/false};
    }
    // Self-contained bundle: install from the local packages dir. We install every
    // closure member ourselves (in topological order), so choco must NOT re-resolve
    // dependencies -- otherwise it would demand the excluded 'chocolatey' framework
    // from a local-only source and fail. Verify each local .nupkg first.
    return {local_source_dir, /*ignore_dependencies=*/true, /*verify_local=*/true};
}

void OfflineDeploymentWorker::installFromBundle(const QString& manifest_path,
                                                const QString& choco_source_dir,
                                                bool packed_only) {
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

    const bool is_list = manifest.payload_mode == PayloadMode::List;
    Q_EMIT operationStarted(manifest.packages.size());
    Q_EMIT logMessage(
        QString("Installing %1: %2 package(s)%3")
            .arg(is_list ? QStringLiteral("Thin Bundle") : QStringLiteral("Full Bundle"))
            .arg(manifest.packages.size())
            .arg(packed_only && !is_list ? QStringLiteral(" (air-gap: packed only)") : QString()));

    m_operation_future = QtConcurrent::run([this, manifest, choco_source_dir, packed_only]() {
        executeInstallFromBundle(manifest, choco_source_dir, packed_only);
    });
}

bool OfflineDeploymentWorker::installOneManifestEntry(const DeploymentManifestEntry& entry,
                                                      bool packed_only,
                                                      const BundleInstallContext& install_ctx,
                                                      BatchStats& stats) {
    // Defense in depth against an older/tampered bundle: never force-install the
    // Chocolatey framework onto a target (the build side already excludes it).
    if (isChocolateyFrameworkId(entry.package_id)) {
        ++stats.skipped;
        emitLog(QString("[%1] Skipped: the Chocolatey framework is never installed onto a target "
                        "machine")
                    .arg(entry.package_id));
        return false;
    }
    if (installDispositionFor(entry, packed_only) == InstallDisposition::SkipNotPacked) {
        ++stats.skipped;
        // A skip is NOT a failure: surface it as a neutral log line so the UI does
        // not render a skipped package as a failed one.
        emitLog(QString("[%1] Skipped: not fully packed, air-gap install requested -- %2")
                    .arg(entry.package_id, entry.offline_note));
        return false;
    }
    const bool ok = installBundlePackage(entry, stats.completed, stats.total, install_ctx);
    if (ok) {
        ++stats.completed;
    } else if (m_cancelled) {
        ++stats.cancelled;  // interrupted mid-install -- not a hard failure
    } else {
        ++stats.failed;
    }
    return m_cancelled.load();
}

void OfflineDeploymentWorker::executeInstallFromBundle(DeploymentManifest manifest,
                                                       QString choco_source_dir,
                                                       bool packed_only) {
    const BundleInstallContext install_ctx = installContextForMode(manifest.payload_mode,
                                                                   choco_source_dir);
    // Air-gap (packed-only) applies only to a self-contained Bundle; a List always
    // fetches at install. A Bundle installs with --ignore-dependencies, so order
    // its packages dependencies-first.
    const bool is_bundle = manifest.payload_mode == PayloadMode::Bundle;
    const bool effective_packed_only = packed_only && is_bundle;
    QStringList cyclic_ids;
    const QVector<DeploymentManifestEntry> ordered =
        is_bundle ? topologicalInstallOrder(manifest.packages, &cyclic_ids) : manifest.packages;
    if (!cyclic_ids.isEmpty()) {
        emitLog(QStringLiteral("[Dependencies] Cycle detected among %1 package(s); installed in "
                               "manifest order (dependency ordering not guaranteed): %2")
                    .arg(cyclic_ids.size())
                    .arg(cyclic_ids.join(QStringLiteral(", "))));
    }

    BatchStats stats;
    stats.total = ordered.size();

    for (const auto& entry : ordered) {
        if (m_cancelled) {
            break;
        }
        if (installOneManifestEntry(entry, effective_packed_only, install_ctx, stats)) {
            break;  // cancelled mid-install; remaining packages become "pending"
        }
    }

    // Every package must be accounted for so the totals sum: any never reached
    // (because a cancel broke the loop) are pending.
    const int accounted = stats.completed + stats.failed + stats.skipped + stats.cancelled;
    stats.pending = (stats.total > accounted) ? (stats.total - accounted) : 0;

    m_running = false;
    QMetaObject::invokeMethod(
        this,
        [this, stats]() {
            Q_EMIT logMessage(installCompletionMessage(stats));
            Q_EMIT operationCompleted(stats);
        },
        Qt::QueuedConnection);
}

bool OfflineDeploymentWorker::isSuccessInstallExitCode(int exit_code) {
    // choco/MSI treat 1641 (reboot initiated) and 3010 (reboot required) as
    // successful installs, not failures -- accept them alongside 0.
    return exit_code == offline::kExitSuccess || exit_code == offline::kExitRebootInitiated ||
           exit_code == offline::kExitRebootRequired;
}

QString OfflineDeploymentWorker::resolveChocoExecutable() {
    // Portable technician tool: ONLY the choco.exe bundled with the app is ever
    // run. Never a system-installed choco -- a target machine must not need one,
    // and must not have Chocolatey installed onto it -- and never a bare "choco"
    // (the OS would resolve it via cwd/PATH, so a planted choco.exe could run
    // with the app's rights). A missing bundled copy is a packaging fault: fail
    // closed rather than silently borrow the host's tooling.
    const auto& tools = BundledToolsManager::instance();
    if (tools.toolExists(QStringLiteral("chocolatey"), QStringLiteral("choco.exe"))) {
        return tools.toolPath(QStringLiteral("chocolatey"), QStringLiteral("choco.exe"));
    }
    return QString();
}

bool OfflineDeploymentWorker::verifyBundledPackage(const DeploymentManifestEntry& entry,
                                                   const QString& source_dir,
                                                   QString& error_out) {
    const QString safe_name = sanitizeManifestFilename(entry.nupkg_filename);
    if (safe_name.isEmpty()) {
        error_out = "Manifest entry has no valid package filename";
        return false;
    }

    // Fail closed: a Bundle entry MUST declare both a size and a checksum. An entry
    // lacking either cannot be integrity-verified, so it is rejected before install
    // rather than accepted on the mere existence of a file at the path.
    if (entry.size_bytes <= 0 || entry.checksum.isEmpty()) {
        error_out = "Bundle entry lacks a size/checksum to verify: " + safe_name;
        return false;
    }

    QFile file(QDir(source_dir).filePath(safe_name));
    if (!file.open(QIODevice::ReadOnly)) {
        error_out = "Bundled package missing: " + safe_name;
        return false;
    }

    if (entry.size_bytes > 0 && file.size() != entry.size_bytes) {
        error_out = QString("Size mismatch for %1 (%2 vs manifest %3)")
                        .arg(safe_name)
                        .arg(file.size())
                        .arg(entry.size_bytes);
        return false;
    }

    if (!entry.checksum.isEmpty()) {
        QCryptographicHash hash(QCryptographicHash::Sha256);  // as written by the build side
        if (!hash.addData(&file)) {
            error_out = "Cannot read bundled package for checksum: " + safe_name;
            return false;
        }
        const QString actual = QString::fromLatin1(hash.result().toHex());
        if (actual.compare(entry.checksum, Qt::CaseInsensitive) != 0) {
            error_out = "Checksum mismatch for " + safe_name;
            return false;
        }
    }
    return true;
}

void OfflineDeploymentWorker::emitPackageOutcome(const DeploymentManifestEntry& entry,
                                                 bool success,
                                                 const QString& message) {
    QMetaObject::invokeMethod(
        this,
        [this, entry, success, message]() {
            Q_EMIT packageProgress(entry.package_id, success, message);
        },
        Qt::QueuedConnection);
}

// Build the choco install argv for a manifest entry. A Bundle passes
// --ignore-dependencies (we install every closure member ourselves in dependency
// order), so choco does not re-resolve and demand the excluded 'chocolatey'
// framework from a local-only source.
static QStringList chocoInstallArgs(const DeploymentManifestEntry& entry,
                                    const BundleInstallContext& install_ctx) {
    QStringList args{QStringLiteral("install"),
                     entry.package_id,
                     QStringLiteral("--version"),
                     entry.version,
                     QStringLiteral("--source"),
                     install_ctx.source,
                     QStringLiteral("--yes"),
                     QStringLiteral("--no-progress"),
                     QStringLiteral("--force")};
    if (install_ctx.ignore_dependencies) {
        args.append(QStringLiteral("--ignore-dependencies"));
    }
    return args;
}

bool OfflineDeploymentWorker::installBundlePackage(const DeploymentManifestEntry& entry,
                                                   int completed,
                                                   int total,
                                                   const BundleInstallContext& install_ctx) {
    QMetaObject::invokeMethod(
        this,
        [this, completed, total, entry]() {
            Q_EMIT batchProgress(completed, total, entry.package_id);
            Q_EMIT logMessage(QString("Installing: %1 v%2").arg(entry.package_id, entry.version));
        },
        Qt::QueuedConnection);

    const QString choco_exe = resolveChocoExecutable();
    if (choco_exe.isEmpty()) {
        emitPackageOutcome(entry,
                           false,
                           QStringLiteral("Bundled Chocolatey (choco.exe) missing from app"));
        return false;
    }

    // Re-validate the id/version straight from the manifest BEFORE they become
    // choco argv elements. A tampered manifest could otherwise smuggle an
    // option-like token (leading '-') that choco parses as a flag.
    if (!entryInstallTokensValid(entry)) {
        emitPackageOutcome(entry,
                           false,
                           QStringLiteral("Rejected unsafe package id/version from manifest"));
        return false;
    }

    // Verify the bundled artifact against the manifest before install (Bundle mode
    // only -- a List payload has no local .nupkg; choco downloads it from the feed).
    if (install_ctx.verify_local) {
        QString verify_error;
        if (!verifyBundledPackage(entry, install_ctx.source, verify_error)) {
            emitPackageOutcome(entry, false, verify_error);
            return false;
        }
    }

    // Point the child at the bundled portable Chocolatey root (the dir holding
    // choco.exe), matching the search side. Without ChocolateyInstall a portable
    // choco would fall back to %ProgramData%\chocolatey -- i.e. try to install
    // onto the target machine's system location, which a portable tool must not
    // do (and which does not exist on a clean target).
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("ChocolateyInstall"), QFileInfo(choco_exe).absolutePath());

    const auto process = runProcessWithEnvironment(choco_exe,
                                                   chocoInstallArgs(entry, install_ctx),
                                                   offline::kInstallTimeoutPerPackageMs,
                                                   env,
                                                   [this]() { return m_cancelled.load(); });

    const bool success = !process.timed_out && !process.cancelled &&
                         isSuccessInstallExitCode(process.exit_code);
    emitPackageOutcome(entry, success, installOutcomeText(process, success));
    return success;
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

    PackageInternalizationEngine resolver;
    int completed = 0;
    int failed = 0;
    int total = packages.size();
    // Shared across EVERY package in the run: two packages emitting the same
    // installer basename must not overwrite each other in the one output dir.
    QSet<QString> used_names;

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

        int files = downloadOnePackageInstallers(pkg_id, resolved_version, output_dir, used_names);

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
                                                          QSet<QString>& used_names) {
    // A crafted package id must not escape the per-package temp dir -- reject
    // traversal/separators up front.
    if (!PackageInternalizationEngine::isSafePackageComponent(pkg_id)) {
        emitLog(QString("[%1] Rejected: unsafe package id").arg(pkg_id));
        return 0;
    }

    // Unique, owned scratch dir (random suffix) auto-removed on scope exit. This
    // replaces a PREDICTABLE path that was recursively deleted -- a foreign dir or
    // symlink can no longer pre-occupy it, and there is no manual recursive delete
    // of a guessable path to be tricked into wiping unrelated data.
    QTemporaryDir temp(output_dir + "/_sak_temp_XXXXXX");
    if (!temp.isValid()) {
        emitLog(QString("[%1] Could not create a temp working directory").arg(pkg_id));
        return 0;
    }
    temp.setAutoRemove(true);
    const QString temp_dir = temp.path();

    // Steps 1-2: Download and extract the nupkg
    const QString extract_dir = downloadAndExtractNupkg(pkg_id, resolved_version, temp_dir);
    if (extract_dir.isEmpty()) {
        return 0;
    }

    // Step 3: Find install script (or resolve meta-package dependency)
    PackageInternalizationEngine engine;
    QString script_path = engine.findInstallScript(extract_dir);
    QString pkg_extract_dir = extract_dir;
    if (script_path.isEmpty()) {
        auto [dep_script,
              dep_extract] = resolveMetaPackageDependency(pkg_id, extract_dir, temp_dir);
        if (dep_script.isEmpty()) {
            return 0;
        }
        script_path = dep_script;
        pkg_extract_dir = dep_extract;
    }

    // Parse for installer downloads (all resources, 32- and 64-bit, with checksums).
    const QVector<InstallerDownload> downloads =
        collectInstallerDownloads(InstallScriptParser().parseFile(script_path));
    if (downloads.isEmpty()) {
        const int copied = copyEmbeddedInstallers(pkg_id, pkg_extract_dir, output_dir, used_names);
        if (copied == 0) {
            emitLog(QString("[%1] No download URLs or embedded files").arg(pkg_id));
        }
        return copied;
    }
    return downloadInstallersToDir(pkg_id, downloads, output_dir, used_names);
}

QString OfflineDeploymentWorker::downloadAndExtractNupkg(const QString& pkg_id,
                                                         const QString& resolved_version,
                                                         const QString& temp_dir) {
    QString nupkg_url =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, pkg_id, resolved_version);
    QString nupkg_path = temp_dir + "/" + pkg_id + ".nupkg";

    if (!downloadFileFromUrl(nupkg_url, nupkg_path)) {
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
    const QString& pkg_id, const QString& extract_dir, const QString& temp_dir) {
    QString dep_id = findNuspecDependencyId(extract_dir);
    if (dep_id.isEmpty()) {
        emitLog(QString("[%1] No chocolateyInstall.ps1 found").arg(pkg_id));
        return {};
    }
    // dep_id comes from a downloaded .nuspec (attacker-influenced): it feeds a
    // temp path below, so it must be a single safe path segment.
    if (!PackageInternalizationEngine::isSafePackageComponent(dep_id)) {
        emitLog(QString("[%1] Rejected unsafe dependency id: %2").arg(pkg_id, dep_id));
        return {};
    }

    emitLog(QString("[%1] Meta-package -> resolving %2").arg(pkg_id, dep_id));

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
    if (!downloadFileFromUrl(dep_nupkg_url, dep_nupkg_path)) {
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
                                                    const QString& output_dir,
                                                    QSet<QString>& used_names) {
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
        // Disambiguate against every name already written this run so a second
        // package's identically-named installer never overwrites the first.
        const QString dest_name = uniqueFilename(safeInstallerFilename(name, name), used_names);
        const QString src = tools_dir.filePath(name);
        const QString dest = output_dir + "/" + dest_name;
        if (QFile::copy(src, dest)) {
            ++copied;
            sak::logInfo("[DirectDownload] Embedded: {}", dest_name.toStdString());
        } else {
            emitLog(QString("[%1] Copy failed: %2").arg(pkg_id, name));
        }
    }
    return copied;
}

QString OfflineDeploymentWorker::uniqueFilename(const QString& desired, QSet<QString>& used) {
    if (!used.contains(desired)) {
        used.insert(desired);
        return desired;
    }
    const QFileInfo info(desired);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    int counter = 1;
    QString candidate;
    do {
        candidate = suffix.isEmpty() ? QString("%1_%2").arg(stem).arg(counter)
                                     : QString("%1_%2.%3").arg(stem).arg(counter).arg(suffix);
        ++counter;
    } while (used.contains(candidate));
    used.insert(candidate);
    return candidate;
}

QVector<InstallerDownload> OfflineDeploymentWorker::collectInstallerDownloads(
    const ParsedInstallScript& parsed) {
    // Collect the 32- and 64-bit installers from EVERY resource, each carrying its
    // declared checksum, de-duplicated by URL. A package can declare several
    // installers (e.g. an app + a prerequisite); keeping only the first resource
    // would ship an incomplete set that then fails at install time.
    QVector<InstallerDownload> downloads;
    QSet<QString> seen;
    auto add = [&](const QString& url, const QString& sum, const QString& type) {
        if (url.isEmpty() || seen.contains(url)) {
            return;
        }
        seen.insert(url);
        downloads.append(InstallerDownload{url, sum, type});
    };
    for (const auto& resource : parsed.resources) {
        add(resource.url, resource.checksum, resource.checksum_type);
        add(resource.url_64bit, resource.checksum_64bit, resource.checksum_type_64bit);
    }
    return downloads;
}

int OfflineDeploymentWorker::downloadInstallersToDir(const QString& pkg_id,
                                                     const QVector<InstallerDownload>& downloads,
                                                     const QString& output_dir,
                                                     QSet<QString>& used_names) {
    int downloaded = 0;
    int index = 0;
    for (const auto& item : downloads) {
        if (m_cancelled) {
            break;
        }
        ++index;
        // Two distinct installer URLs can share a basename (x86 vs x64 setup.exe).
        // Disambiguate so the second never overwrites the first on disk (which
        // would ship one architecture while counting two downloads). used_names is
        // shared across packages, so a cross-package collision is caught too.
        QString filename = safeInstallerFilename(QUrl(item.url).fileName(),
                                                 QString("%1_installer_%2").arg(pkg_id).arg(index));
        filename = uniqueFilename(filename, used_names);
        const QString dest = output_dir + "/" + filename;
        // Verify each installer against its declared checksum before counting it.
        if (downloadFileFromUrl(item.url, dest, item.checksum, item.checksum_type)) {
            ++downloaded;
            sak::logInfo("[DirectDownload] Saved: {}", filename.toStdString());
        } else {
            emitLog(QString("[%1] Download failed: %2").arg(pkg_id, item.url));
        }
    }

    if (downloaded == 0 && !downloads.isEmpty()) {
        emitLog(QString("[%1] Found %2 URL(s) but all downloads failed")
                    .arg(pkg_id)
                    .arg(downloads.size()));
    }
    return downloaded;
}

bool OfflineDeploymentWorker::downloadFileFromUrl(const QString& url,
                                                  const QString& output_path,
                                                  const QString& expected_checksum,
                                                  const QString& checksum_type) {
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.timeout_ms = offline::kDownloadTimeoutMs;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});

    const auto transfer = runNetworkTransfer(request, [this]() { return m_cancelled.load(); });
    if (!transfer.success || transfer.body.isEmpty()) {
        sak::logError("[DirectDownload] HTTP {} for {}: {}",
                      transfer.http_status,
                      url.toStdString(),
                      transfer.error_message.toStdString());
        return false;
    }

    // Integrity-gate BEFORE anything is committed to disk: a declared checksum that
    // does not match (or is declared but unresolvable) fails closed, so a tampered
    // or corrupt installer is never written or counted. An empty declared checksum
    // (nothing to verify against) passes through unchanged.
    if (!PackageInternalizationEngine::binaryChecksumMatches(
            transfer.body, expected_checksum, checksum_type)) {
        sak::logError("[DirectDownload] Checksum mismatch for {}", url.toStdString());
        return false;
    }

    // QSaveFile: fail closed on a short write / flush failure so a truncated installer is never
    // counted as a downloaded package; the temp file is discarded on any failure.
    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly)) {
        sak::logError("[DirectDownload] Cannot write {}", output_path.toStdString());
        return false;
    }
    const qint64 written = file.write(transfer.body);
    if (written != transfer.body.size() || !file.commit()) {
        sak::logError("[DirectDownload] Short write for {}: {} of {} bytes",
                      output_path.toStdString(),
                      written,
                      static_cast<qint64>(transfer.body.size()));
        return false;
    }
    return true;
}

// ============================================================================
// Cancel / Status
// ============================================================================

void OfflineDeploymentWorker::cancel() {
    m_cancelled = true;
    // Abort the engine actively internalizing a package, if any, so a large
    // in-progress download stops promptly instead of running to completion.
    QMutexLocker lock(&m_mutex);
    if (m_active_engine != nullptr) {
        m_active_engine->cancel();
    }
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
    root["payload_mode"] = manifest.payload_mode == PayloadMode::List ? QStringLiteral("list")
                                                                      : QStringLiteral("bundle");

    QJsonArray packages_arr;
    for (const auto& entry : manifest.packages) {
        QJsonObject pkg;
        pkg["package_id"] = entry.package_id;
        pkg["version"] = entry.version;
        pkg["filename"] = entry.nupkg_filename;
        pkg["checksum"] = entry.checksum;
        pkg["size_bytes"] = entry.size_bytes;
        pkg["internalized"] = entry.internalized;
        pkg["offline_ready"] = entry.offline_ready;
        pkg["offline_note"] = entry.offline_note;

        QJsonArray deps;
        for (const auto& dep : entry.dependencies) {
            deps.append(dep);
        }
        pkg["dependencies"] = deps;

        packages_arr.append(pkg);
    }
    root["packages"] = packages_arr;

    QString manifest_path = output_dir + "/" + offline::kManifestFilename;
    // QSaveFile: write to a temp file and atomically rename on commit(), so a
    // crash or short write never truncates a previously-good manifest (which
    // installFromBundle would then reject as "empty or unreadable").
    QSaveFile file(manifest_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logError("[OfflineDeploymentWorker] Cannot write manifest: {}",
                      manifest_path.toStdString());
        return false;
    }

    QJsonDocument doc(root);
    const QByteArray json = doc.toJson(QJsonDocument::Indented);
    if (file.write(json) != json.size() || !file.commit()) {
        sak::logError("[OfflineDeploymentWorker] Failed to write manifest (original intact): {}",
                      manifest_path.toStdString());
        return false;
    }

    sak::logInfo("[OfflineDeploymentWorker] Manifest written: {}", manifest_path.toStdString());
    return true;
}

// Write the per-package section of a payload README. Bundle entries are tagged
// FULLY PACKED / WILL FETCH; a List has no such tag (everything downloads).
static void writeReadmePackageList(QTextStream& stream,
                                   const DeploymentManifest& manifest,
                                   bool is_list) {
    stream << "Included Packages:\n";
    for (const auto& entry : manifest.packages) {
        stream << "  - " << entry.package_id << " v" << entry.version;
        if (!is_list) {
            stream << " -- " << (entry.offline_ready ? "FULLY PACKED" : "WILL FETCH");
        }
        if (!entry.offline_note.isEmpty()) {
            stream << " (" << entry.offline_note << ")";
        }
        stream << "\n";
    }
}

void OfflineDeploymentWorker::writeReadme(const DeploymentManifest& manifest,
                                          const QString& output_dir) const {
    QString readme_path = output_dir + "/" + offline::kReadmeFilename;
    QFile file(readme_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return;
    }

    const bool is_list = manifest.payload_mode == PayloadMode::List;
    QTextStream stream(&file);
    stream << "S.A.K. Utility - Deployment Payload\n";
    stream << "===================================\n\n";
    stream << "Type: "
           << (is_list ? "Thin Bundle (metadata-only; installers downloaded at install time)"
                       : "Full Bundle (self-contained; installers packed in)")
           << "\n";
    stream << "Created: " << manifest.created_date << "\n";
    if (!manifest.description.isEmpty()) {
        stream << "Description: " << manifest.description << "\n";
    }

    int packed = 0;
    for (const auto& entry : manifest.packages) {
        if (entry.offline_ready) {
            ++packed;
        }
    }
    if (is_list) {
        stream << "Packages: " << manifest.packages.size()
               << " (all fetched from the Chocolatey feed at install)\n\n";
    } else {
        stream << "Packages: " << manifest.packages.size() << " (" << packed << " fully packed, "
               << (manifest.packages.size() - packed) << " fetch a remainder at install)\n\n";
    }

    writeReadmePackageList(stream, manifest, is_list);

    stream << "\nInstallation:\n";
    stream << "  1. Copy this folder to the target machine\n";
    stream << "  2. Open S.A.K. Utility on the target machine\n";
    stream << "  3. Go to App Management > Offline Deploy tab\n";
    stream << "  4. Click 'Install from Bundle' and select manifest.json\n";
    stream << "     (installs via the app's OWN bundled Chocolatey; nothing is installed onto the "
              "machine itself)\n";
    if (is_list) {
        stream << "  Note: the target needs internet -- each installer is downloaded, then "
                  "installed.\n";
    }

    file.close();
}

static DeploymentManifestEntry parseManifestEntry(const QJsonObject& pkg) {
    DeploymentManifestEntry entry;
    entry.package_id = pkg["package_id"].toString();
    entry.version = pkg["version"].toString();
    entry.nupkg_filename = pkg["filename"].toString();
    entry.checksum = pkg["checksum"].toString();
    entry.size_bytes = pkg["size_bytes"].toInteger();
    entry.internalized = pkg["internalized"].toBool();
    // Fail closed: an entry that does not positively declare offline_ready is
    // treated as NOT offline-ready. Under the air-gap (packed_only) switch it is
    // then skipped rather than attempted against a disconnected target where it
    // could only fail. (Default installs are unaffected -- they install regardless.)
    entry.offline_ready = pkg.contains("offline_ready") && pkg["offline_ready"].toBool();
    entry.offline_note = pkg["offline_note"].toString();

    const QJsonArray deps = pkg["dependencies"].toArray();
    for (const auto& dep : deps) {
        entry.dependencies.append(dep.toString());
    }
    return entry;
}

DeploymentManifest OfflineDeploymentWorker::readManifest(const QString& path) const {
    DeploymentManifest manifest;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logError("[OfflineDeploymentWorker] Cannot read manifest: {}", path.toStdString());
        return manifest;
    }

    // Bound a hostile/corrupt manifest BEFORE reading it wholesale into memory
    // (the install side otherwise had no size cap, unlike the build side).
    if (file.size() > offline::kMaxManifestBytes) {
        sak::logError("[OfflineDeploymentWorker] Manifest too large: {} bytes (cap {})",
                      static_cast<long long>(file.size()),
                      static_cast<long long>(offline::kMaxManifestBytes));
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
    // Absent (pre-field manifest) -> Bundle, so an older bundle installs from its
    // local packages exactly as before.
    manifest.payload_mode = root["payload_mode"].toString() == QLatin1String("list")
                                ? PayloadMode::List
                                : PayloadMode::Bundle;

    QJsonArray packages_arr = root["packages"].toArray();
    // Parity with the build-side kMaxPackagesPerBuild gate: refuse a manifest that
    // lists more packages than a build could ever have produced, failing closed
    // (an empty manifest -> installFromBundle reports "empty or unreadable").
    if (packages_arr.size() > offline::kMaxPackagesPerBuild) {
        sak::logError("[OfflineDeploymentWorker] Manifest lists {} packages; exceeds cap {}",
                      static_cast<long long>(packages_arr.size()),
                      static_cast<long long>(offline::kMaxPackagesPerBuild));
        return DeploymentManifest{};
    }
    for (const auto& val : packages_arr) {
        manifest.packages.append(parseManifestEntry(val.toObject()));
    }

    return manifest;
}

}  // namespace sak
