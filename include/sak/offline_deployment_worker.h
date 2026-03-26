// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file offline_deployment_worker.h
/// @brief Background worker for batch offline deployment operations
///
/// Manages the batch internalization of multiple Chocolatey packages,
/// creating deployment packages (directories with internalized .nupkg files
/// and a manifest), and installing from offline packages.

#pragma once

#include "sak/package_internalization_engine.h"

#include <QFuture>
#include <QMutex>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

namespace sak {

/// @brief Manifest entry for a single package in a deployment bundle
struct DeploymentManifestEntry {
    QString package_id;
    QString version;
    QString nupkg_filename;
    QString checksum;
    qint64 size_bytes{0};
    QStringList dependencies;
    bool internalized{false};
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
};

/// @brief A package to be internalized as part of a batch operation
struct BatchInternalizationJob {
    QString package_id;
    QString version;
    InternalizationStatus status{InternalizationStatus::Pending};
    QString error_message;
    QString output_path;
    QString checksum;
};

/// @brief Statistics for a batch internalization run
struct BatchStats {
    int total{0};
    int completed{0};
    int failed{0};
    int cancelled{0};
    int pending{0};
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
                               const QString& description = QString());

    /// @brief Install packages from a local deployment bundle
    /// @param manifest_path Path to the deployment manifest.json
    /// @param choco_source_dir Path to the local package source directory
    void installFromBundle(const QString& manifest_path, const QString& choco_source_dir);

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

    /// @brief Execute the build bundle operation on a background thread
    void executeBuildBundle(const QString& output_dir, const QString& description);

    /// @brief Execute direct download on a background thread
    void executeDirectDownload(const QVector<QPair<QString, QString>>& packages,
                               const QString& output_dir);

    /// @brief Internalize a single package within the batch loop
    [[nodiscard]] bool internalizeOnePackage(int idx,
                                             const BuildBundleContext& ctx,
                                             DeploymentManifest& manifest);

    /// @brief Finalize the bundle: write manifest, clean up, emit completion
    void finalizeBundle(const DeploymentManifest& manifest, const BuildBundleContext& ctx);

    /// @brief Download installer binaries for a single package
    /// @return Number of files successfully downloaded (0 on failure)
    [[nodiscard]] int downloadOnePackageInstallers(const QString& pkg_id,
                                                   const QString& resolved_version,
                                                   const QString& output_dir,
                                                   QNetworkAccessManager& nam);

    /// @brief Download and extract a .nupkg into a temp directory
    /// @return Path to the extracted directory, empty on failure
    [[nodiscard]] QString downloadAndExtractNupkg(const QString& pkg_id,
                                                  const QString& resolved_version,
                                                  const QString& temp_dir,
                                                  QNetworkAccessManager& nam);

    /// @brief Resolve a meta-package's dependency and extract it
    /// @return Pair of (script_path, extract_dir), both empty on failure
    [[nodiscard]] QPair<QString, QString> resolveMetaPackageDependency(const QString& pkg_id,
                                                                       const QString& extract_dir,
                                                                       const QString& temp_dir,
                                                                       QNetworkAccessManager& nam);

    /// @brief Copy embedded installer files from the nupkg tools/ directory
    /// @return Number of files successfully copied
    [[nodiscard]] int copyEmbeddedInstallers(const QString& pkg_id,
                                             const QString& pkg_extract_dir,
                                             const QString& output_dir);

    /// @brief Collect primary installer URLs from parsed install script
    [[nodiscard]] static QStringList collectPrimaryUrls(const ParsedInstallScript& parsed);

    /// @brief Download a list of URLs to a directory
    /// @return Number of files successfully downloaded
    [[nodiscard]] int downloadUrlsToDir(const QString& pkg_id,
                                        const QStringList& urls,
                                        const QString& output_dir,
                                        QNetworkAccessManager& nam);

    /// @brief Download a single file from a URL to disk
    [[nodiscard]] bool downloadFileFromUrl(const QString& url,
                                           const QString& output_path,
                                           QNetworkAccessManager& nam);

    /// @brief Emit a log message to the UI from a background thread
    void emitLog(const QString& message);

    PackageInternalizationEngine m_engine;
    QVector<BatchInternalizationJob> m_jobs;
    mutable QMutex m_mutex;
    QFuture<void> m_operation_future;

    std::atomic<bool> m_running{false};
    std::atomic<bool> m_cancelled{false};
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::BatchStats)
Q_DECLARE_METATYPE(sak::DeploymentManifest)
