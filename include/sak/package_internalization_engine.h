// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_internalization_engine.h
/// @brief Engine for internalizing Chocolatey packages for offline deployment
///
/// Orchestrates the full internalization pipeline: download nupkg -> extract ->
/// parse install script -> download embedded binaries -> rewrite script ->
/// repack into self-contained nupkg. The output package installs without
/// internet access.

#pragma once

#include "sak/install_script_parser.h"
#include "sak/script_rewriter.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>

#include <atomic>
#include <optional>
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

/// @brief How confidently a repacked package will install with NO internet.
enum class OfflineReadiness {
    Internalized,     ///< external installer binaries were downloaded + embedded
    SelfContained,    ///< no external download needed (no script, or installer ships in the nupkg)
    RequiresNetwork,  ///< an install script is present but its installer could not be internalized
};

/// @brief Result of a completed internalization
struct InternalizationResult {
    QString package_id;
    QString version;
    bool success{false};
    /// @brief True only if at least one external binary was actually downloaded
    ///        and internalized. False means the package was merely repacked as-is
    ///        (no recognized download URLs) -- it may STILL fetch from the
    ///        internet at install time, so callers must surface this to the user
    ///        rather than presenting it as a fully offline package.
    bool binaries_internalized{false};
    /// @brief Honest offline-install classification (drives the manifest's
    ///        offline_ready flag and the install-time offline gate). Defaults to
    ///        the conservative RequiresNetwork until a branch proves otherwise.
    OfflineReadiness offline_readiness{OfflineReadiness::RequiresNetwork};
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

public:
    /// @brief Resolve the latest version from the NuGet v2 OData API
    [[nodiscard]] QString resolveLatestVersion(const QString& package_id);

    /// @brief Extract a .nupkg (ZIP) to a directory
    [[nodiscard]] bool extractNupkg(const QString& nupkg_path,
                                    const QString& extract_dir,
                                    QString& error_out);

    /// @brief Find the chocolateyInstall.ps1 within extracted package
    [[nodiscard]] QString findInstallScript(const QString& extract_dir) const;

    /// @brief True if @p component is safe to use as a single path segment when
    ///        composing work/extract/output paths from a package id or version.
    ///        Rejects empty, ".", "..", and any path-separator / drive-colon /
    ///        control character, so a crafted id/version cannot escape the work dir
    ///        (traversal, overwrite, or recursive-cleanup outside the work tree).
    ///        Pure; unit-testable.
    [[nodiscard]] static bool isSafePackageComponent(const QString& component);

    /// @brief True iff @p data matches @p expected_checksum under the algorithm
    ///        named by @p checksum_type (md5/sha1/sha256/sha512), or, when the
    ///        type is empty, inferred from the checksum's hex length. An empty
    ///        expected checksum returns true (nothing declared); a declared but
    ///        unresolvable checksum returns false (fail closed). Pure;
    ///        unit-testable.
    [[nodiscard]] static bool binaryChecksumMatches(const QByteArray& data,
                                                    const QString& expected_checksum,
                                                    const QString& checksum_type);

    /// @brief Supply-chain verification decision for a downloaded installer.
    ///        Unlike binaryChecksumMatches (a pure matcher where an EMPTY expected
    ///        means "no constraint"), this FAILS CLOSED on a missing checksum: an
    ///        installer whose script declared no checksum cannot be verified and
    ///        must never ship in an offline bundle. Returns true only when a
    ///        resolvable, declared checksum matches @p data. Pure; unit-testable.
    [[nodiscard]] static bool installerVerified(const QByteArray& data,
                                                const QString& expected_checksum,
                                                const QString& checksum_type);

    /// @brief Reduce a URL-derived filename to a safe single-segment basename.
    ///        An empty name or one bearing traversal / path-separators / control
    ///        characters (per isSafePackageComponent) collapses to a deterministic
    ///        in-tree fallback ("binary_<index+1>") so a crafted download URL can
    ///        never write outside the tools dir. Pure; unit-testable.
    [[nodiscard]] static QString sanitizedBinaryBasename(const QString& url_filename, int index);

    /// @brief True iff @p url is an acceptable installer/package source: a valid,
    ///        host-bearing https URL. Rejects http/ftp/file/UNC and any hostless
    ///        or malformed URL so a hostile script cannot pull an installer off
    ///        the local disk or an insecure origin. Pure; unit-testable.
    [[nodiscard]] static bool isAllowedInstallerUrl(const QString& url);

    /// @brief True iff the SHA of @p data (under @p algorithm, e.g. "SHA512"),
    ///        BASE64-encoded, equals @p expected_base64 (the NuGet feed's
    ///        PackageHash form). An empty expected hash or an unknown algorithm
    ///        returns false (fail closed). Pure; unit-testable.
    [[nodiscard]] static bool nupkgHashMatches(const QByteArray& data,
                                               const QString& expected_base64,
                                               const QString& algorithm);

    /// @brief Extract the (PackageHash, PackageHashAlgorithm) pair for @p version
    ///        from a NuGet OData feed document. Returns an empty-first pair when
    ///        the version/hash is absent. Pure; unit-testable.
    [[nodiscard]] static QPair<QString, QString> parsePackageHashFromOData(const QByteArray& data,
                                                                           const QString& version);

    /// @brief Parse OData XML to the latest version string: an entry flagged
    ///        IsLatestVersion wins; otherwise the SemVer-max is chosen (never the
    ///        feed's raw last entry). Pure; unit-testable.
    [[nodiscard]] static QString parseLatestVersionFromOData(const QByteArray& data);

    /// @brief True if @p script_text references a NETWORK download -- a literal
    ///        http/https/ftp URL, or a raw download method (Invoke-WebRequest,
    ///        Start-BitsTransfer, WebClient/DownloadFile, wget, curl, ...). This is
    ///        the honest offline-readiness test: a package installs offline only if
    ///        its (post-internalization) script has no remaining network fetch.
    ///        Note: the recognized Chocolatey download helpers are rewritten to
    ///        LOCAL paths before this runs, so their cmdlet names are intentionally
    ///        NOT treated as download indicators. Pure; unit-testable.
    [[nodiscard]] static bool scriptHasNetworkDownload(const QString& script_text);

private:
    struct InternalizationPaths {
        QString extract_dir;
        QString nupkg_path;
    };

    [[nodiscard]] bool beginInternalization(const QString& package_id,
                                            const QString& version,
                                            QString& resolved_version,
                                            InternalizationResult& result);

    [[nodiscard]] InternalizationPaths prepareInternalizationPaths(const QString& package_id,
                                                                   const QString& version,
                                                                   const QString& output_dir,
                                                                   const QString& work_dir) const;

    [[nodiscard]] bool downloadAndExtractNupkg(const InternalizationPaths& paths,
                                               InternalizationResult& result);

    [[nodiscard]] bool downloadAndRewriteInstallScript(const ParsedInstallScript& parsed,
                                                       const QString& script_path,
                                                       InternalizationResult& result);

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

    /// @brief Fetch one binary to @p output_path (with progress + fail-closed
    ///        write). Returns the bytes on success; std::nullopt on cancellation
    ///        (silent) or a real failure (which emits the error via the result).
    [[nodiscard]] std::optional<QByteArray> fetchBinary(const QString& url,
                                                        const QString& output_path,
                                                        InternalizationResult& result);

    /// @brief True iff @p body passes its declared checksum (fail-closed on a
    ///        missing checksum). Emits the error via @p result on failure.
    [[nodiscard]] bool binaryChecksumOk(const QByteArray& body,
                                        const QString& url,
                                        const QHash<QString, QPair<QString, QString>>& checksums,
                                        InternalizationResult& result);

    /// @brief Map each primary URL that declares a checksum to its (checksum,
    ///        type) so a downloaded binary can be verified before repack.
    [[nodiscard]] static QHash<QString, QPair<QString, QString>> binaryChecksumMap(
        const ParsedInstallScript& parsed);

    /// @brief Resolve version, using latest from NuGet API if empty
    /// @return Resolved version string, or empty on failure
    [[nodiscard]] QString resolveAndLogVersion(const QString& package_id, const QString& version);

    /// @brief Collect unique binary URLs from parsed install script resources
    [[nodiscard]] QStringList collectBinaryUrls(const ParsedInstallScript& parsed) const;

    /// @brief Build URL-to-local-filename map from parsed resources
    [[nodiscard]] QHash<QString, QString> buildLocalFilenameMap(
        const ParsedInstallScript& parsed) const;

    /// @brief Remove NuGet-specific metadata files from the extracted package.
    ///        Returns false if any removal failed, so the caller can fail closed
    ///        rather than repack leftover NuGet metadata into the output nupkg.
    [[nodiscard]] bool cleanNugetArtifacts(const QString& extract_dir) const;

    /// @brief Repack an extracted directory into a .nupkg
    [[nodiscard]] bool repackNupkg(const QString& source_dir, const QString& output_path);

    /// @brief Compute SHA-256 checksum of a file
    [[nodiscard]] QString computeChecksum(const QString& file_path) const;

    /// @brief Verify the downloaded .nupkg bytes against the feed's published
    ///        PackageHash BEFORE extraction/repack. Fetches the feed hash for the
    ///        version and fails closed (emitting the error) when no trusted hash
    ///        is available or the bytes do not match.
    [[nodiscard]] bool verifyNupkgIntegrity(const QString& package_id,
                                            const QString& version,
                                            const QByteArray& body,
                                            InternalizationResult& result);

    /// @brief Fetch (PackageHash, PackageHashAlgorithm) for @p version from the
    ///        NuGet FindPackagesById feed. Empty-first pair on any failure.
    [[nodiscard]] QPair<QString, QString> fetchPackageHashMeta(const QString& package_id,
                                                               const QString& version);

    /// @brief Update and emit progress
    void emitProgress(InternalizationStatus status, const QString& message);

    InstallScriptParser m_parser;
    ScriptRewriter m_rewriter;

    InternalizationProgress m_current_progress;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_busy{false};
};

}  // namespace sak
