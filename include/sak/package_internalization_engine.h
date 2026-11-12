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
#include "sak/network_transfer_runner.h"
#include "sak/nuget_dependency_resolver.h"
#include "sak/script_rewriter.h"

#include <QByteArray>
#include <QCryptographicHash>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <atomic>
#include <functional>
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

    /// @brief Extract a .nupkg (ZIP) into @p extract_dir through the app's zip engine
    ///        (FileExplorerArchiveService). Fails closed, with the reason in @p error_out, on a
    ///        file that is not a zip, any refused entry, and any entry the engine would skip.
    [[nodiscard]] static bool extractNupkg(const QString& nupkg_path,
                                           const QString& extract_dir,
                                           QString& error_out);

    /// @brief Repack @p source_dir into a new .nupkg at @p output_path (which must not exist),
    ///        its contents at the archive root. Fails closed, with the reason in @p error_out,
    ///        when the engine refuses or would skip any source; no archive is left behind.
    [[nodiscard]] static bool repackNupkg(const QString& source_dir,
                                          const QString& output_path,
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

    /// @brief Why an installer that failed installerVerified() was not bundled, naming the
    ///        upstream package as the one at fault. Pure; unit-testable.
    [[nodiscard]] static QString installerRefusalMessage(const QString& url,
                                                         const QString& declared_checksum);

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

    // --- Verified downloads, streamed to disk --------------------------------------------------

    /// @brief What a download's bytes must hash to: the algorithm, and the test its digest must
    ///        pass (a hex checksum from an install script, a base64 PackageHash from the feed).
    struct DownloadDigest {
        QCryptographicHash::Algorithm algorithm{QCryptographicHash::Sha256};
        std::function<bool(const QByteArray& digest)> matches;
    };

    /// @brief The digest an installer's declared checksum names, compared as hex without regard
    ///        to case; the algorithm comes from @p checksum_type, or the checksum's hex length when
    ///        the type is empty. Nullopt when nothing was declared or the algorithm cannot be
    ///        resolved: such an installer cannot be verified (see installerVerified). Pure.
    [[nodiscard]] static std::optional<DownloadDigest> installerDigest(
        const QString& expected_checksum, const QString& checksum_type);

    /// @brief The digest a feed PackageHash names, compared as exact base64. Nullopt when the
    ///        hash is empty or the algorithm unknown (see nupkgHashMatches). Pure.
    [[nodiscard]] static std::optional<DownloadDigest> nupkgDigest(const QString& expected_base64,
                                                                   const QString& algorithm);

    /// @brief Outcome of downloadVerifiedFile.
    struct VerifiedDownload {
        bool ok{false};         ///< downloaded, verified and in place
        bool cancelled{false};  ///< a Stop was observed
        bool mismatch{false};   ///< downloaded whole, but the bytes do not match the digest
        int http_status{0};
        QString error;          ///< why it failed, when none of the above
    };

    /// @brief Free bytes on the drive holding a folder, or -1 when Windows cannot say.
    using FreeSpaceProbe = std::function<qint64(const QString& folder)>;

    /// @brief Replace how free space is read (QStorageInfo), so a test can stand a drive of any
    ///        size in; returns the probe it replaced. An empty probe restores QStorageInfo.
    static FreeSpaceProbe setFreeSpaceProbeForTesting(FreeSpaceProbe probe);

    /// @brief Why work files cannot go under @p work_root, or empty when they can: the drive
    ///        holding it needs @p needed_bytes free. Names the folder and both sizes.
    [[nodiscard]] static QString workSpaceRefusal(const QString& work_root, qint64 needed_bytes);

    /// @brief What a download of @p declared_bytes needs on the drive it is staged on: the staged
    ///        file, a second copy when its target is on that drive too, and
    ///        offline::kWorkSpaceMarginBytes.
    [[nodiscard]] static qint64 downloadWorkBytes(qint64 declared_bytes, bool target_on_same_drive);

    /// @brief Download @p url and put it at @p output_path only when its bytes match @p digest.
    ///
    /// Memory stays bounded at any size up to offline::kMaxBinarySizeBytes: the body streams to a
    /// file in the process's temp folder (QDir::tempPath, the per-run folder the app points TEMP
    /// at), and is then copied into place through a QSaveFile while being hashed, so the bytes
    /// that land are exactly the bytes that were checked and nothing is committed on a mismatch.
    /// When the server declares the size, the drive the file is staged on must have
    /// downloadWorkBytes free before the body is taken, or the download stops with
    /// workSpaceRefusal's message. Callers decide which URLs are acceptable
    /// (isAllowedInstallerUrl) before calling.
    [[nodiscard]] static VerifiedDownload downloadVerifiedFile(
        const QUrl& url,
        const QString& output_path,
        const DownloadDigest& digest,
        const NetworkCancelCheck& cancel,
        const NetworkProgressCallback& progress = {});

    // --- The app's one NuGet feed client (FindPackagesById) -----------------------------------

    /// @brief Why a feed listing is, or is not, the authoritative version list.
    enum class FeedListingStatus {
        Ok,                     ///< every page fetched through to the last one
        Cancelled,              ///< a Stop was observed
        UnsafePackageId,        ///< the id is not a single safe path segment
        TransportError,         ///< HTTP or network failure
        MalformedBody,          ///< a non-empty page that is not a well-formed Atom feed
        OffOriginContinuation,  ///< a rel="next" link leaving the feed's scheme/host/port
        TooManyPages            ///< more pages than kMaxFeedPages: truncated, not authoritative
    };

    /// @brief Every version the feed lists for one package id, across all of its pages.
    struct FeedListing {
        FeedListingStatus status{FeedListingStatus::TransportError};
        int http_status{0};
        QVector<FeedPackageVersion> versions;  ///< empty unless status is Ok
        [[nodiscard]] bool ok() const { return status == FeedListingStatus::Ok; }
        /// @brief Short reason for a status other than Ok ("HTTP 503", "malformed feed body").
        [[nodiscard]] QString failureReason() const;
    };

    /// @brief One FindPackagesById page, scanned in one pass. Pure; unit-testable.
    struct FeedPage {
        bool well_formed{false};               ///< a well-formed Atom <feed> document
        QVector<FeedPackageVersion> versions;  ///< empty unless well_formed
        QString next_link;                     ///< href of <link rel="next">, empty when none
    };

    /// @brief Pages followed for one package before the list counts as truncated, so a huge
    ///        or hostile feed cannot keep the fetch going forever.
    static constexpr int kMaxFeedPages = 128;

    /// @brief List @p package_id's versions from the community feed, following every
    ///        rel="next" page (same origin only, at most kMaxFeedPages). A transport failure is
    ///        retried up to @p attempts times in all, waiting kApiRetryDelayBaseMs * attempt
    ///        between tries in slices that @p cancel interrupts. Responses are capped at
    ///        kMaxFeedResponseBytes.
    [[nodiscard]] static FeedListing fetchFeedVersions(const QString& package_id,
                                                       int attempts,
                                                       const NetworkCancelCheck& cancel);

    /// @brief Scan one feed page: well-formedness (QXmlStreamReader), the rel="next" link, and
    ///        every entry's version, dependencies, IsLatestVersion and PackageHash.
    [[nodiscard]] static FeedPage parseFeedPage(const QByteArray& body);

    /// @brief True when @p candidate shares @p origin's scheme, host and port, so a crafted
    ///        rel="next" link cannot move the fetch to another host. Pure.
    [[nodiscard]] static bool sameFeedOrigin(const QUrl& origin, const QUrl& candidate);

    /// @brief The latest version among @p versions: the first entry flagged IsLatestVersion
    ///        wins; otherwise the SemVer-max (never the feed's raw last entry). Empty when no
    ///        entry parses as SemVer (fail closed). Pure.
    [[nodiscard]] static QString latestFeedVersion(const QVector<FeedPackageVersion>& versions);

    /// @brief The entry whose version string is exactly @p version, if any. Pure.
    [[nodiscard]] static std::optional<FeedPackageVersion> feedEntryForVersion(
        const QVector<FeedPackageVersion>& versions, const QString& version);

    /// @brief Outcome of downloadVerifiedNupkg.
    struct NupkgDownload {
        bool ok{false};
        bool cancelled{false};
        QString error;  ///< why it failed, when neither ok nor cancelled
    };

    /// @brief Download @p package_id @p version's .nupkg to @p output_path, authenticated
    ///        against the PackageHash the feed publishes for that version. The hash is looked
    ///        up FIRST (no trusted hash, no download), and the file goes through
    ///        downloadVerifiedFile, so nothing reaches @p output_path unless it matches.
    [[nodiscard]] static NupkgDownload downloadVerifiedNupkg(const QString& package_id,
                                                             const QString& version,
                                                             const QString& output_path,
                                                             const NetworkCancelCheck& cancel);

    /// @brief True if @p script_text references a NETWORK download -- a literal
    ///        http/https/ftp URL, or a raw download method (Invoke-WebRequest,
    ///        Start-BitsTransfer, WebClient/DownloadFile, wget, curl, ...). This is
    ///        the honest offline-readiness test: a package installs offline only if
    ///        its (post-internalization) script has no remaining network fetch.
    ///        Note: the recognized Chocolatey download helpers are rewritten to
    ///        LOCAL paths before this runs, so their cmdlet names are intentionally
    ///        NOT treated as download indicators. Pure; unit-testable.
    [[nodiscard]] static bool scriptHasNetworkDownload(const QString& script_text);

    /// @brief Remove NuGet-specific metadata files from the extracted package.
    ///        Returns false if any removal failed, so the caller can fail closed
    ///        rather than repack leftover NuGet metadata into the output nupkg. A folder is
    ///        removed without following a link inside it (sak::no_follow::removeTree).
    [[nodiscard]] static bool cleanNugetArtifacts(const QString& extract_dir);

    /// @brief Make @p dir an empty folder: whatever a crashed earlier build left there is removed
    ///        without following a link inside it (sak::no_follow::removeTree), then the folder is
    ///        made again. False if either step failed.
    [[nodiscard]] static bool resetWorkFolder(const QString& dir);

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

    /// @brief Download one binary to @p output_path, verified against its declared checksum
    ///        (no usable checksum, no download) and streamed to disk. False on cancellation
    ///        (silent; the caller's gate reports it) or a failure, emitted via @p result.
    [[nodiscard]] bool fetchVerifiedBinary(const QString& url,
                                           const QString& output_path,
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

    /// @brief Compute SHA-256 checksum of a file
    [[nodiscard]] QString computeChecksum(const QString& file_path) const;

    /// @brief Update and emit progress
    void emitProgress(InternalizationStatus status, const QString& message);

    InstallScriptParser m_parser;
    ScriptRewriter m_rewriter;

    InternalizationProgress m_current_progress;
    std::atomic<bool> m_cancelled{false};
    std::atomic<bool> m_busy{false};
};

}  // namespace sak
