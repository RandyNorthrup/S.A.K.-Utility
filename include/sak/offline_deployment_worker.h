// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file offline_deployment_worker.h
/// @brief Background worker for batch offline deployment operations
///
/// Manages the batch internalization of multiple Chocolatey packages,
/// creating deployment packages (directories with internalized .nupkg files
/// and a manifest), and installing from offline packages.

#pragma once

#include "sak/nuget_dependency_resolver.h"
#include "sak/package_internalization_engine.h"

#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVector>

#include <atomic>

namespace sak {

/// @brief What kind of deployment payload to build / install from.
///
/// Two payloads, both deployed on the target by the app's OWN bundled portable
/// choco (never a system choco, never installed onto the target):
///   - Bundle: self-contained -- the installers are downloaded + internalized on
///     the staging machine and packed in, so the target installs from local
///     .nupkg files (bundle once, deploy many, minimal bandwidth at deploy).
///   - List:   metadata-only -- just the package ids/versions; nothing is
///     downloaded at build time and the target fetches each installer from the
///     Chocolatey feed at install time (smallest payload).
/// (Provisional internal names; the user-facing labels are workshopped separately.)
enum class PayloadMode {
    Bundle,
    List
};

/// @brief Manifest entry for a single package in a deployment bundle
struct DeploymentManifestEntry {
    QString package_id;
    QString version;
    QString nupkg_filename;
    QString checksum;
    qint64 size_bytes{0};
    QStringList dependencies;
    bool internalized{false};   ///< external installer binaries were embedded
    bool offline_ready{false};  ///< installs with NO internet (internalized or self-contained)
    QString offline_note;       ///< human-readable reason for offline_ready
};

/// @brief Full deployment manifest describing a bundled offline package set
struct DeploymentManifest {
    QString manifest_version;
    QString created_date;
    QString creator;
    QString description;
    QString output_dir;
    QVector<DeploymentManifestEntry> packages;
    qint64 total_size_bytes{0};
    /// @brief Bundle (installers packed in) or List (metadata-only, target fetches).
    ///        Drives the install source strategy. Defaults to Bundle so a manifest
    ///        written before this field existed installs from its local packages.
    PayloadMode payload_mode{PayloadMode::Bundle};
};

/// @brief Per-package install strategy, derived from the manifest's payload mode.
struct BundleInstallContext {
    QString source;            ///< local packages dir (Bundle) or feed URL (List)
    bool ignore_dependencies;  ///< Bundle: true -- we install every closure member ourselves
    bool verify_local;         ///< Bundle: verify the local .nupkg before handing it to choco
};

/// @brief A package to be internalized as part of a batch operation
struct BatchInternalizationJob {
    QString package_id;
    QString version;
    InternalizationStatus status{InternalizationStatus::Pending};
    QString error_message;
    QString output_path;
    QString checksum;
    QStringList dependencies;  ///< direct dependency ids (from the resolved closure)
};

/// @brief Statistics for a batch internalization run
struct BatchStats {
    int total{0};
    int completed{0};
    int failed{0};
    int cancelled{0};
    int pending{0};
    int offline_capable{0};   ///< packages that install with NO internet
    int requires_network{0};  ///< packages that still need internet at install time
    int skipped{0};           ///< packages skipped (e.g. requires-network under an offline install)
    qint64 total_bytes{0};
};

/// @brief Context for building a deployment bundle (used internally)
struct BuildBundleContext {
    QString packages_dir;
    QString work_dir;
    QString output_dir;
    int total_jobs{0};
    int completed_count{0};
    int failed_count{0};
};

/// @brief Background worker for offline deployment operations
///
/// Supports three modes of operation:
///   1. **Build**: Internalize multiple packages into a deployment bundle
///   2. **Install**: Install packages from a local deployment bundle
///   3. **Direct Download**: Download installer binaries for manual use
///
/// Runs long-running operations on a background thread via QtConcurrent.
/// Communicates progress and results to the UI thread via signals.
class OfflineDeploymentWorker : public QObject {
    Q_OBJECT

public:
    explicit OfflineDeploymentWorker(QObject* parent = nullptr);
    ~OfflineDeploymentWorker() override;

    OfflineDeploymentWorker(const OfflineDeploymentWorker&) = delete;
    OfflineDeploymentWorker& operator=(const OfflineDeploymentWorker&) = delete;

    /// @brief Build an offline deployment bundle
    /// @param packages List of (package_id, version) pairs
    /// @param output_dir Directory to write the deployment bundle
    /// @param description Optional user description for the manifest
    void buildDeploymentBundle(const QVector<QPair<QString, QString>>& packages,
                               const QString& output_dir,
                               const QString& description = QString(),
                               PayloadMode mode = PayloadMode::Bundle);

    /// @brief Install packages from a deployment payload, using the app's OWN
    ///        bundled portable choco. The source strategy follows the manifest's
    ///        PayloadMode: a Bundle installs from @p choco_source_dir (local
    ///        .nupkg files, dependency install handled by us); a List installs
    ///        from the Chocolatey feed (choco resolves + downloads).
    /// @param manifest_path Path to the deployment manifest.json
    /// @param choco_source_dir Path to the local package source directory (Bundle)
    /// @param packed_only Air-gap switch (Bundle only): when true, install ONLY
    ///        fully-packed packages and skip will-fetch ones with a clear reason.
    ///        The default (false) installs everything -- packed content first,
    ///        fetching any will-fetch remainder at install time.
    void installFromBundle(const QString& manifest_path,
                           const QString& choco_source_dir,
                           bool packed_only = false);

    /// @brief Download .nupkg files directly (no internalization)
    /// @param packages List of (package_id, version) pairs
    /// @param output_dir Directory to save downloaded installer files
    void directDownload(const QVector<QPair<QString, QString>>& packages,
                        const QString& output_dir);

    /// @brief Cancel the current operation
    void cancel();

    /// @brief Check if an operation is running
    [[nodiscard]] bool isRunning() const;

    /// @brief Get current batch statistics
    [[nodiscard]] BatchStats getStats() const;

    /// @brief How a build's reusable <output>/_work directory should be treated.
    enum class WorkDirDisposition {
        CreateFresh,    ///< Absent, or an empty unstamped dir we may adopt.
        ReuseOwned,     ///< Bears our ownership marker: a leftover we may reuse+delete.
        RefuseForeign,  ///< Non-empty and unstamped: never create/reuse or delete it.
    };

    /// @brief Decide how to treat a build work directory from its state, so a
    ///        pre-existing FOREIGN <output>/_work is never recursively deleted.
    ///        Pure; unit-testable.
    [[nodiscard]] static WorkDirDisposition classifyWorkDir(bool exists,
                                                            bool has_ownership_marker,
                                                            bool is_empty);

    /// @brief Reduce a URL-derived name to a single safe path segment, returning
    ///        @p fallback when it would escape the output dir (empty, ".", "..",
    ///        or containing a path separator after basename reduction). Pure;
    ///        unit-testable.
    [[nodiscard]] static QString safeInstallerFilename(const QString& raw_name,
                                                       const QString& fallback);

    /// @brief Confine a manifest-declared filename to a bare basename (defense in
    ///        depth against a hostile manifest). Empty/"."/".."/separator -> empty.
    ///        Pure; unit-testable.
    [[nodiscard]] static QString sanitizeManifestFilename(const QString& raw_name);

    /// @brief Verify the bundled .nupkg named by @p entry exists in @p source_dir
    ///        and matches the manifest size and SHA-256 checksum before it is
    ///        handed to choco. Returns false + @p error_out on any mismatch; a
    ///        declared checksum that does not match fails closed. Unit-testable.
    [[nodiscard]] static bool verifyBundledPackage(const DeploymentManifestEntry& entry,
                                                   const QString& source_dir,
                                                   QString& error_out);

Q_SIGNALS:
    /// @brief Batch operation started
    void operationStarted(int total_packages);

    /// @brief Progress update for the current batch
    void batchProgress(int completed, int total, const QString& current_package);

    /// @brief A single package completed (success or failure)
    void packageProgress(const QString& package_id, bool success, const QString& message);

    /// @brief Batch operation fully completed
    void operationCompleted(const BatchStats& stats);

    /// @brief Manifest written successfully
    void manifestWritten(const QString& manifest_path);

    /// @brief Error during batch operation
    void operationError(const QString& error_message);

    /// @brief Log message for the UI log panel
    void logMessage(const QString& message);

private:
    /// @brief Write the deployment manifest to disk
    [[nodiscard]] bool writeManifest(const DeploymentManifest& manifest, const QString& output_dir);

    /// @brief Write a README.txt explaining the deployment bundle
    void writeReadme(const DeploymentManifest& manifest, const QString& output_dir) const;

    /// @brief Read a deployment manifest from disk
    [[nodiscard]] DeploymentManifest readManifest(const QString& path) const;

    /// @brief Ensure the build work dir is ours to create/reuse and later delete.
    ///        Refuses (returns false + @p error_out) a pre-existing foreign dir so
    ///        a recursive cleanup can never wipe unrelated user data (B10-13).
    [[nodiscard]] bool prepareOwnedWorkDir(const QString& work_dir, QString& error_out);

    /// @brief Execute a Bundle (self-contained) build on a background thread:
    ///        resolve the closure, internalize + pack every installer.
    void executeBuildBundle(const QString& output_dir, const QString& description);

    /// @brief Execute a List (metadata-only) build on a background thread: write a
    ///        manifest of the requested packages with NO download/internalization.
    ///        Fast and network-free; the target fetches each installer at install.
    void executeBuildListManifest(const QString& output_dir, const QString& description);

    /// @brief Expand the requested package list (m_jobs) into the FULL transitive
    ///        dependency closure so the bundle actually contains everything an
    ///        offline install needs. Honors each dependency's declared NuGet
    ///        version range (picks the highest satisfying version) instead of
    ///        grabbing latest. Runs synchronously on the build thread. Any
    ///        unresolved dependency / fetch failure is appended to @p warnings so
    ///        an incomplete bundle is surfaced, not silently reported as success.
    ///        On total resolution failure, falls back to the requested list.
    [[nodiscard]] QVector<BatchInternalizationJob> resolveDependencyClosure(QStringList& warnings);

    /// @brief Turn the resolver's output into build jobs: drop the 'chocolatey'
    ///        framework package, keep every resolved dependency, and re-append any
    ///        explicitly-requested package the feed could not resolve (attempted
    ///        directly). Surfaces the framework exclusion + unresolved requests via
    ///        @p warnings. Pure (no instance state).
    [[nodiscard]] static QVector<BatchInternalizationJob> assembleClosureJobs(
        const QVector<ResolvedPackage>& resolved,
        const QVector<BatchInternalizationJob>& requested,
        QStringList& warnings);

    /// @brief Fetch a package's available versions (with declared dependencies)
    ///        from the NuGet feed (FindPackagesById). @p ok is set false on a
    ///        transport failure (so resolution records a fetch error); an empty
    ///        result with @p ok true means the feed legitimately returned no
    ///        matching package.
    [[nodiscard]] QVector<FeedPackageVersion> fetchFeedVersions(const QString& package_id,
                                                                bool& ok);

    /// @brief Execute direct download on a background thread
    void executeDirectDownload(const QVector<QPair<QString, QString>>& packages,
                               const QString& output_dir);

    /// @brief Internalize a single package within the batch loop
    [[nodiscard]] bool internalizeOnePackage(int idx,
                                             const BuildBundleContext& ctx,
                                             DeploymentManifest& manifest);
    [[nodiscard]] BatchInternalizationJob beginInternalizationJob(int idx);
    void emitInternalizationStarted(const BuildBundleContext& ctx, const QString& pkg_id);
    [[nodiscard]] InternalizationResult runInternalizationJob(const BatchInternalizationJob& job,
                                                              const BuildBundleContext& ctx);
    void applyInternalizationResult(int idx,
                                    const InternalizationResult& result,
                                    DeploymentManifest& manifest);
    void emitInternalizationResult(const QString& pkg_id, const InternalizationResult& result);

    /// @brief Finalize the bundle: write manifest, clean up, emit completion
    void finalizeBundle(const DeploymentManifest& manifest, const BuildBundleContext& ctx);

    /// @brief Execute bundle installation on a background thread
    void executeInstallFromBundle(DeploymentManifest manifest,
                                  QString choco_source_dir,
                                  bool packed_only);

public:
    /// @brief What to do with a bundle entry at install time.
    enum class InstallDisposition {
        Install,        ///< install it (the default: install everything)
        SkipNotPacked,  ///< air-gap (packed_only) + not fully packed -> skip, don't fail
    };

    /// @brief Decide whether a manifest entry should be installed or skipped.
    ///        The default installs everything; under the air-gap switch
    ///        (@p packed_only) a not-fully-packed (will-fetch) entry is skipped so
    ///        it does not fail against a deliberately disconnected target. Pure.
    [[nodiscard]] static InstallDisposition installDispositionFor(
        const DeploymentManifestEntry& entry, bool packed_only);

    /// @brief Order @p packages so every package's bundled dependencies install
    ///        BEFORE it (a stable topological sort over the manifest's own
    ///        dependency edges). Required because a Bundle install passes
    ///        --ignore-dependencies (we install each closure member ourselves), so
    ///        choco does not order them. A dependency cycle falls back to the
    ///        original order. Pure; unit-testable.
    [[nodiscard]] static QVector<DeploymentManifestEntry> topologicalInstallOrder(
        const QVector<DeploymentManifestEntry>& packages);

    /// @brief Build the per-package install strategy for a payload mode. Bundle
    ///        installs from the local @p local_source_dir with --ignore-dependencies
    ///        and local verification; List installs from the Chocolatey feed and
    ///        lets choco resolve + download. Pure; unit-testable.
    [[nodiscard]] static BundleInstallContext installContextForMode(
        PayloadMode mode, const QString& local_source_dir);

    /// @brief Collect ALL installer URLs (32- and 64-bit, every resource) from a
    ///        parsed install script -- direct download must not silently drop the
    ///        secondary installers a multi-resource package declares. Pure.
    [[nodiscard]] static QStringList collectInstallerUrls(const ParsedInstallScript& parsed);

    /// @brief Return @p desired if unused, else a de-duplicated variant ("x_1.exe"),
    ///        inserting the chosen name into @p used. Two installer URLs that share
    ///        a basename must not write to the same file. Pure; unit-testable.
    [[nodiscard]] static QString uniqueFilename(const QString& desired, QSet<QString>& used);

    /// @brief True if @p id is the Chocolatey FRAMEWORK package ('chocolatey',
    ///        case-insensitive) -- which must NEVER be bundled or force-installed
    ///        onto a target (it bootstraps Chocolatey there). Helper/extension
    ///        packages (chocolatey.extension, ...) are NOT the framework. Pure.
    [[nodiscard]] static bool isChocolateyFrameworkId(const QString& id);

    /// @brief Reject a manifest-supplied token that is empty, option-like (a
    ///        leading '-'), or carries whitespace/control chars, before it becomes
    ///        a choco argv element -- defense in depth against a tampered manifest
    ///        injecting a Chocolatey flag. Pure; unit-testable.
    [[nodiscard]] static bool isSafeInstallToken(const QString& token);

private:
    /// @brief Install one package from a deployment payload, per @p install_ctx
    ///        (local source + verify for Bundle, feed source for List).
    [[nodiscard]] bool installBundlePackage(const DeploymentManifestEntry& entry,
                                            int completed,
                                            int total,
                                            const BundleInstallContext& install_ctx);

    /// @brief Install one manifest entry or skip it (Chocolatey framework, or an
    ///        air-gap install of a not-fully-packed package), updating @p stats.
    ///        Returns true when a mid-install cancel means the loop should stop.
    [[nodiscard]] bool installOneManifestEntry(const DeploymentManifestEntry& entry,
                                               bool packed_only,
                                               const BundleInstallContext& install_ctx,
                                               BatchStats& stats);

    /// @brief Resolve an absolute path to choco.exe (bundled portable first,
    ///        then PATH). Empty if not found -- callers must NOT fall back to a
    ///        bare "choco" (PATH/cwd binary hijack).
    [[nodiscard]] static QString resolveChocoExecutable();

    /// @brief Emit a terminal packageProgress signal to the UI thread.
    void emitPackageOutcome(const DeploymentManifestEntry& entry,
                            bool success,
                            const QString& message);

    /// @brief Download installer binaries for a single package
    /// @return Number of files successfully downloaded (0 on failure)
    [[nodiscard]] int downloadOnePackageInstallers(const QString& pkg_id,
                                                   const QString& resolved_version,
                                                   const QString& output_dir);

    /// @brief Download and extract a .nupkg into a temp directory
    /// @return Path to the extracted directory, empty on failure
    [[nodiscard]] QString downloadAndExtractNupkg(const QString& pkg_id,
                                                  const QString& resolved_version,
                                                  const QString& temp_dir);

    /// @brief Resolve a meta-package's dependency and extract it
    /// @return Pair of (script_path, extract_dir), both empty on failure
    [[nodiscard]] QPair<QString, QString> resolveMetaPackageDependency(const QString& pkg_id,
                                                                       const QString& extract_dir,
                                                                       const QString& temp_dir);

    /// @brief Copy embedded installer files from the nupkg tools/ directory
    /// @return Number of files successfully copied
    [[nodiscard]] int copyEmbeddedInstallers(const QString& pkg_id,
                                             const QString& pkg_extract_dir,
                                             const QString& output_dir);

    /// @brief Download a list of URLs to a directory
    /// @return Number of files successfully downloaded
    [[nodiscard]] int downloadUrlsToDir(const QString& pkg_id,
                                        const QStringList& urls,
                                        const QString& output_dir);

    /// @brief Download a single file from a URL to disk
    [[nodiscard]] bool downloadFileFromUrl(const QString& url, const QString& output_path);

    /// @brief Emit a log message to the UI from a background thread
    void emitLog(const QString& message);

    /// @brief The internalization engine running the CURRENT package, or nullptr
    ///        between jobs. Guarded by m_mutex so cancel() (UI thread) can abort
    ///        the in-flight download the build thread is performing. (The build
    ///        creates a fresh local engine per job; without this, cancel() had no
    ///        way to reach it and an in-progress download ran to completion.)
    PackageInternalizationEngine* m_active_engine{nullptr};
    QVector<BatchInternalizationJob> m_jobs;
    mutable QMutex m_mutex;
    QFuture<void> m_operation_future;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelled{false};
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::BatchStats)
Q_DECLARE_METATYPE(sak::DeploymentManifest)
