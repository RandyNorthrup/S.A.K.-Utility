// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_internalization_engine.h
/// @brief Engine for internalizing Chocolatey packages for offline deployment
///
/// Orchestrates the full internalization pipeline: download nupkg → extract →
/// parse install script → download embedded binaries → rewrite script →
/// repack into self-contained nupkg. The output package installs without
/// internet access.

#pragma once

#include "sak/install_script_parser.h"
#include "sak/nuget_api_client.h"
#include "sak/script_rewriter.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <functional>

class QNetworkAccessManager;

namespace sak {

/// @brief Status of a single package internalization job
enum class InternalizationStatus {
    Pending,
    DownloadingNupkg,
    Extracting,
    ParsingScript,
    DownloadingBinaries,
    RewritingScript,
    Repacking,
    Checksumming,
    Complete,
    Failed,
    Cancelled
};

/// @brief Progress info for a single package being internalized
struct InternalizationProgress {
    QString package_id;
    QString version;
    InternalizationStatus status{InternalizationStatus::Pending};
    QString status_message;
    int binary_index{0};
    int binary_total{0};
    qint64 bytes_downloaded{0};
    qint64 bytes_total{0};
    QString error_message;
};

/// @brief Result of a completed internalization
struct InternalizationResult {
    QString package_id;
    QString version;
    bool success{false};
    QString output_nupkg_path;
    QString checksum;
    QStringList internalized_files;
    QString error_message;
    qint64 original_size{0};
    qint64 internalized_size{0};
};

/// @brief Engine that internalizes Chocolatey packages for offline use
///
/// Pipeline per package:
///   1. Download .nupkg from Chocolatey community repo
///   2. Extract as ZIP archive
///   3. Parse chocolateyInstall.ps1 for download URLs
///   4. Download all referenced binaries
///   5. Rewrite install script to use local paths
///   6. Clean NuGet metadata artifacts
///   7. Repack as .nupkg
///   8. Compute checksum of output package
///
/// All I/O is async. Progress and completion reported via signals.
class PackageInternalizationEngine : public QObject {
    Q_OBJECT

public:
    explicit PackageInternalizationEngine(QObject* parent = nullptr);
    ~PackageInternalizationEngine() override;

    PackageInternalizationEngine(const PackageInternalizationEngine&) = delete;
    PackageInternalizationEngine& operator=(const PackageInternalizationEngine&) = delete;

    /// @brief Internalize a single package
    /// @param package_id Chocolatey package ID
    /// @param version Package version to internalize
    /// @param output_dir Directory for the output .nupkg
    /// @param work_dir Temporary working directory for extraction
    void internalizePackage(const QString& package_id,
                            const QString& version,
                            const QString& output_dir,
                            const QString& work_dir);

    /// @brief Cancel the current internalization
    void cancel();

    /// @brief Check if an internalization is in progress
    [[nodiscard]] bool isBusy() const;

Q_SIGNALS:
    /// @brief Progress update for the current package
    void progressChanged(const InternalizationProgress& progress);

    /// @brief A single package internalization completed
    void packageComplete(const InternalizationResult& result);

    /// @brief An error occurred during internalization
    void errorOccurred(const QString& package_id, const QString& error);

private:
    /// @brief Download the .nupkg file from the NuGet feed
    [[nodiscard]] bool downloadNupkg(const QString& package_id,
                                     const QString& version,
                                     const QString& nupkg_path,
                                     InternalizationResult& result);

    /// @brief Clean, repack, checksum, and emit completion
    void repackAndFinish(InternalizationResult& result,
                         const QString& extract_dir,
                         const QString& output_dir,
                         const QString& status_message);

    /// @brief Emit error, set result, and mark not busy
    void finishWithError(InternalizationResult& result, const QString& error);

    /// @brief Download all binary URLs referenced by the install script
    [[nodiscard]] bool downloadAllBinaries(const ParsedInstallScript& parsed,
                                           const QString& tools_dir,
                                           InternalizationResult& result);

    /// @brief Collect unique binary URLs from parsed install script resources
    [[nodiscard]] QStringList collectBinaryUrls(const ParsedInstallScript& parsed) const;

    /// @brief Build URL-to-local-filename map from parsed resources
    [[nodiscard]] QHash<QString, QString> buildLocalFilenameMap(
        const ParsedInstallScript& parsed) const;

    /// @brief Extract a .nupkg (ZIP) to a directory
    [[nodiscard]] bool extractNupkg(const QString& nupkg_path, const QString& extract_dir);

    /// @brief Find the chocolateyInstall.ps1 within extracted package
    [[nodiscard]] QString findInstallScript(const QString& extract_dir) const;

    /// @brief Download a single binary file from a URL
    void downloadBinary(const QString& url,
                        const QString& output_path,
                        std::function<void(bool success, const QString& error)> callback);

    /// @brief Remove NuGet-specific metadata files from extracted package
    void cleanNugetArtifacts(const QString& extract_dir) const;

    /// @brief Repack an extracted directory into a .nupkg
    [[nodiscard]] bool repackNupkg(const QString& source_dir, const QString& output_path);

    /// @brief Compute SHA-256 checksum of a file
    [[nodiscard]] QString computeChecksum(const QString& file_path) const;

    /// @brief Update and emit progress
    void emitProgress(InternalizationStatus status, const QString& message);

    QNetworkAccessManager* m_network_manager{nullptr};
    InstallScriptParser m_parser;
    ScriptRewriter m_rewriter;

    InternalizationProgress m_current_progress;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_busy{false};
};

}  // namespace sak
