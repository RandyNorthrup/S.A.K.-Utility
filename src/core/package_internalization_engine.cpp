// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_internalization_engine.cpp
/// @brief Engine implementation for internalizing Chocolatey packages

#include "sak/package_internalization_engine.h"

#include "sak/file_explorer_archive_service.h"
#include "sak/logger.h"
#include "sak/network_transfer_runner.h"
#include "sak/nuget_version_range.h"
#include "sak/offline_deployment_constants.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>
#include <QXmlStreamReader>

#include <algorithm>
#include <cstring>
#include <optional>

namespace sak {

namespace {
// The retry wait between feed attempts is slept in slices this long, so a Stop ends it promptly.
constexpr int kFeedRetrySleepSliceMs = 100;
// Hex-string lengths of the supported digest algorithms (two hex chars per byte).
constexpr int kMd5HexLength = 32;
constexpr int kSha1HexLength = 40;
constexpr int kSha256HexLength = 64;
constexpr int kSha512HexLength = 128;
// Code units below this are C0 control characters and are rejected in a path
// component (U+0000..U+001F).
constexpr char16_t kFirstNonControlCodeUnit = 0x20;

NetworkTransferRequest makeFeedRequest(const QUrl& url) {
    NetworkTransferRequest request;
    request.url = url;
    request.timeout_ms = offline::kApiRequestTimeoutMs;
    // A feed page is a small metadata document; cap the buffer before it is parsed so a hostile
    // endpoint cannot exhaust memory here (B9-19).
    request.max_response_bytes = offline::kMaxFeedResponseBytes;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});
    request.raw_headers.append(QPair<QByteArray, QByteArray>{
        QByteArrayLiteral("Accept"), QByteArrayLiteral("application/atom+xml")});
    return request;
}

// Wait @p ms in short slices; false as soon as @p cancel reports a Stop.
bool sleepUnlessCancelled(int ms, const NetworkCancelCheck& cancel) {
    for (int waited = 0; waited < ms; waited += kFeedRetrySleepSliceMs) {
        if (cancel && cancel()) {
            return false;
        }
        QThread::msleep(static_cast<unsigned long>(kFeedRetrySleepSliceMs));
    }
    return !(cancel && cancel());
}

// One pass over every page of the listing that starts at @p first_url. An empty body is the
// feed's "no further versions" answer; anything short of a final page with no rel="next" is not
// authoritative and returns no versions.
PackageInternalizationEngine::FeedListing fetchFeedListingOnce(const QUrl& first_url,
                                                               const NetworkCancelCheck& cancel) {
    using Status = PackageInternalizationEngine::FeedListingStatus;
    PackageInternalizationEngine::FeedListing listing;
    QUrl url = first_url;
    for (int page = 0; page < PackageInternalizationEngine::kMaxFeedPages; ++page) {
        const auto transfer = runNetworkTransfer(makeFeedRequest(url), cancel);
        listing.http_status = transfer.http_status;
        if (transfer.cancelled || !transfer.success) {
            listing.status = transfer.cancelled ? Status::Cancelled : Status::TransportError;
            listing.versions.clear();
            return listing;
        }
        if (transfer.body.isEmpty()) {
            listing.status = Status::Ok;
            return listing;
        }
        const auto scanned = PackageInternalizationEngine::parseFeedPage(transfer.body);
        if (!scanned.well_formed) {
            listing.status = Status::MalformedBody;
            listing.versions.clear();
            return listing;
        }
        listing.versions.append(scanned.versions);
        if (scanned.next_link.isEmpty()) {
            listing.status = Status::Ok;
            return listing;
        }
        url = QUrl(scanned.next_link);
        if (!PackageInternalizationEngine::sameFeedOrigin(first_url, url)) {
            listing.status = Status::OffOriginContinuation;
            listing.versions.clear();
            return listing;
        }
    }
    listing.status = Status::TooManyPages;
    listing.versions.clear();
    return listing;
}

QString uniqueBinaryFilename(const QUrl& url, int index, QSet<QString>& used_names) {
    // Reduce the URL-derived name to a safe in-tree basename first: a crafted URL
    // segment can decode to traversal / separators and must never escape the dir.
    const QString base = PackageInternalizationEngine::sanitizedBinaryBasename(url.fileName(),
                                                                               index);
    // Two distinct URLs can share a basename (x86 vs x64 setup.exe); disambiguate so the second
    // never overwrites the first on disk and the rewritten script maps each URL to its own file.
    // used_names holds LOWERCASED keys: on a case-insensitive filesystem 'Setup.exe' and
    // 'setup.exe' collide, so compare case-insensitively while returning the original-cased name.
    QString candidate = base;
    const QFileInfo info(base);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    int counter = 1;
    while (used_names.contains(candidate.toLower())) {
        candidate = suffix.isEmpty() ? QString("%1_%2").arg(stem).arg(counter)
                                     : QString("%1_%2.%3").arg(stem).arg(counter).arg(suffix);
        ++counter;
    }
    used_names.insert(candidate.toLower());
    return candidate;
}

NetworkTransferRequest makeBinaryDownloadRequest(const QUrl& url) {
    NetworkTransferRequest request;
    request.url = url;
    request.timeout_ms = offline::kDownloadTimeoutMs;
    // Cap the buffered body: an installer response is hashed whole in memory, so a
    // hostile URL returning an unbounded body would otherwise exhaust memory (B9-19
    // parity for binary downloads).
    request.max_response_bytes = offline::kMaxBinarySizeBytes;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});
    return request;
}

std::optional<QCryptographicHash::Algorithm> hashAlgorithmByName(const QString& lower_hint) {
    if (lower_hint == QLatin1String("md5")) {
        return QCryptographicHash::Md5;
    }
    if (lower_hint == QLatin1String("sha1")) {
        return QCryptographicHash::Sha1;
    }
    if (lower_hint == QLatin1String("sha256")) {
        return QCryptographicHash::Sha256;
    }
    if (lower_hint == QLatin1String("sha512")) {
        return QCryptographicHash::Sha512;
    }
    return std::nullopt;
}

std::optional<QCryptographicHash::Algorithm> hashAlgorithmByHexLength(int hex_len) {
    switch (hex_len) {
    case kMd5HexLength:
        return QCryptographicHash::Md5;
    case kSha1HexLength:
        return QCryptographicHash::Sha1;
    case kSha256HexLength:
        return QCryptographicHash::Sha256;
    case kSha512HexLength:
        return QCryptographicHash::Sha512;
    default:
        return std::nullopt;
    }
}

// Resolve the digest algorithm from an explicit type hint, or -- when the hint
// is absent -- infer it from the checksum's hex length. A present-but-unknown
// hint stays unresolved (nullopt) so the caller fails closed.
std::optional<QCryptographicHash::Algorithm> resolveChecksumAlgorithm(const QString& type_hint,
                                                                      int hex_len) {
    const QString hint = type_hint.trimmed().toLower();
    if (!hint.isEmpty()) {
        return hashAlgorithmByName(hint);
    }
    return hashAlgorithmByHexLength(hex_len);
}

// Read a script for offline-readiness classification. Returns nullopt when the
// file cannot be opened, so an unreadable (locked/denied) script is NOT read as
// an empty "no-network" script and mis-classified self-contained -- the caller
// fails closed to requires-network instead.
std::optional<QString> readTextFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return std::nullopt;
    }
    return QString::fromUtf8(file.readAll());
}

// A located install script whose contents force offline-readiness to false: it
// either performs a network download, or could not be read (fail closed -- an
// unverifiable script must never be presented as fully offline).
bool scriptForcesNetwork(const QString& script_path) {
    const auto text = readTextFile(script_path);
    return !text || PackageInternalizationEngine::scriptHasNetworkDownload(*text);
}

bool writeBinaryBody(const QString& output_path, const QByteArray& body, QString& error) {
    // QSaveFile: verify the full byte count and atomically commit, so a partial write (disk full)
    // leaves no truncated installer that would be repacked as a "successfully internalized" file.
    QSaveFile file(output_path);
    if (!file.open(QIODevice::WriteOnly)) {
        error = "Cannot write file: " + output_path;
        return false;
    }
    if (file.write(body) != body.size() || !file.commit()) {
        error = "Failed to write file: " + output_path;
        return false;
    }
    return true;
}

// --- scriptHasNetworkDownload line predicates (kept small for CCN) -----------

bool lineHasDownloadUrl(const QString& lower) {
    return lower.contains(QLatin1String("http://")) || lower.contains(QLatin1String("https://")) ||
           lower.contains(QLatin1String("ftp://"));
}

bool lineHasRawDownloadToken(const QString& lower) {
    // Raw download primitives (NOT the recognized Chocolatey helpers, which are
    // rewritten to LOCAL paths before this runs). curl/wget/iwr/irm are handled
    // by the command regex instead, so a rewritten local literal ('curl-8.zip')
    // or the word "confirm" (contains "irm") never substring-trips this.
    static const QStringList kDownloadTokens = {QStringLiteral("invoke-webrequest"),
                                                QStringLiteral("invoke-restmethod"),
                                                QStringLiteral("start-bitstransfer"),
                                                QStringLiteral("bitsadmin"),
                                                QStringLiteral("downloadfile"),
                                                QStringLiteral("downloadstring"),
                                                QStringLiteral("downloaddata"),
                                                QStringLiteral("webclient"),
                                                QStringLiteral("net.http")};
    return std::ranges::any_of(kDownloadTokens,
                               [&lower](const QString& token) { return lower.contains(token); });
}

bool lineHasDownloadCommand(const QString& lower) {
    // A nested Chocolatey invocation pulls another package from the feed at
    // install time and cannot be constrained to the bundle -> honestly will-fetch.
    static const QRegularExpression kNestedChoco(
        QStringLiteral("\\b(?:cinst|(?:choco(?:\\.exe)?|chocolatey)\\s+(?:install|upgrade))\\b"));
    // curl/wget/iwr/irm as a COMMAND (line start or after |;&`({, optionally via
    // Start-Process) followed by an argument -- catches `curl $dynamicUrl` where
    // the URL is variable-built, without matching a local filename like
    // 'curl-8.0.0.zip' (no whitespace after the token there).
    static const QRegularExpression kCurlWget(QStringLiteral(
        "(?:^|[|;&`({])\\s*(?:start-process\\s+)?(?:curl|wget|iwr|irm)(?:\\.exe)?\\s"));
    // Scheme concatenation: `'http' + '://'` or `+ '://'`, i.e. a URL assembled
    // from string fragments to dodge a literal-URL scan.
    static const QRegularExpression kSchemeConcat(
        QStringLiteral("[\"'](?:https?|ftp)[\"']\\s*\\+|\\+\\s*[\"']:?//"));
    return kNestedChoco.match(lower).hasMatch() || kCurlWget.match(lower).hasMatch() ||
           kSchemeConcat.match(lower).hasMatch();
}

bool nupkgHasZipMagic(const QString& nupkg_path, QString& error_out) {
    // Validate the file starts with the ZIP magic bytes (PK\x03\x04).
    constexpr int kZipMagicSize = 4;
    constexpr char kZipMagic[] = {'\x50', '\x4B', '\x03', '\x04'};
    QFile nupkg_file(nupkg_path);
    if (!nupkg_file.open(QIODevice::ReadOnly)) {
        error_out = "Cannot open nupkg file: " + nupkg_path;
        sak::logError("[InternalizationEngine] {}", error_out.toStdString());
        return false;
    }
    const QByteArray header = nupkg_file.read(kZipMagicSize);
    const qint64 file_size = nupkg_file.size();
    nupkg_file.close();
    if (header.size() < kZipMagicSize ||
        std::memcmp(header.constData(), kZipMagic, kZipMagicSize) != 0) {
        error_out = QString("Not a valid ZIP archive (%1 bytes)").arg(file_size);
        sak::logError("[InternalizationEngine] {}: {}",
                      error_out.toStdString(),
                      nupkg_path.toStdString());
        return false;
    }
    return true;
}

QString semverMaxVersion(const QStringList& versions) {
    QString best;
    std::optional<NuGetVersion> best_ver;
    for (const QString& raw : versions) {
        const auto ver = NuGetVersion::parse(raw);
        if (!ver || !ver->isValid()) {
            continue;
        }
        if (!best_ver || ver->compare(*best_ver) > 0) {
            best_ver = ver;
            best = raw;
        }
    }
    return best;
}
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

PackageInternalizationEngine::PackageInternalizationEngine(QObject* parent) : QObject(parent) {}

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
    QString resolved_version;
    InternalizationResult result;
    if (!beginInternalization(package_id, version, resolved_version, result)) {
        return;
    }

    const InternalizationPaths paths =
        prepareInternalizationPaths(package_id, resolved_version, output_dir, work_dir);
    if (!downloadAndExtractNupkg(paths, result)) {
        return;
    }

    emitProgress(InternalizationStatus::ParsingScript, "Parsing install script...");
    const QString script_path = findInstallScript(paths.extract_dir);
    if (script_path.isEmpty()) {
        // No install script at all: nothing to download -- the nupkg is the whole
        // package (a metapackage or content package), so it installs offline.
        sak::logInfo(
            "[InternalizationEngine] No install script for {}; self-contained (no download needed)",
            package_id.toStdString());
        result.offline_readiness = OfflineReadiness::SelfContained;
        repackAndFinish(result, paths.extract_dir, output_dir, "Repacking (self-contained)...");
        return;
    }

    const auto parsed = m_parser.parseFile(script_path);
    if (parsed.resources.isEmpty()) {
        // Script present but no recognized download URL. It is offline-ready ONLY
        // if the script performs NO network download at all (a config-only or
        // embedded-installer package); if it fetches via a method the parser does
        // not recognize (raw Invoke-WebRequest, WebClient, ...), flag it honestly.
        const bool needs_net = scriptForcesNetwork(script_path);
        result.offline_readiness = needs_net ? OfflineReadiness::RequiresNetwork
                                             : OfflineReadiness::SelfContained;
        sak::logInfo("[InternalizationEngine] {} has no recognized download URL; classified {}",
                     package_id.toStdString(),
                     needs_net ? "requires-network" : "self-contained");
        repackAndFinish(result, paths.extract_dir, output_dir, "Repacking (no binaries)...");
        return;
    }

    if (!downloadAndRewriteInstallScript(parsed, script_path, result)) {
        return;
    }

    // The recognized URLs are now local files. If the REWRITTEN script STILL
    // references a network download (a URL the parser did not recognize, e.g. a
    // raw Invoke-WebRequest for a prerequisite), the package is NOT fully offline
    // -- report RequiresNetwork honestly instead of over-claiming Internalized.
    const bool residual = scriptForcesNetwork(script_path);
    if (residual) {
        result.offline_readiness = OfflineReadiness::RequiresNetwork;
        sak::logWarning(
            "[InternalizationEngine] {} internalized its recognized installers but its script "
            "still "
            "fetches from the network; classified requires-network",
            package_id.toStdString());
    } else {
        result.offline_readiness = OfflineReadiness::Internalized;
    }
    repackAndFinish(result, paths.extract_dir, output_dir, "Repacking .nupkg...");
}

bool PackageInternalizationEngine::scriptHasNetworkDownload(const QString& script_text) {
    // Scan line by line so a full-line comment (an internalized package's homepage
    // banner, `# https://project.example`) is not read as a live download; only
    // executable lines count. A line is a network fetch if it carries a literal
    // remote URL, a raw download primitive, or a download COMMAND (nested choco,
    // curl/wget/iwr/irm with an argument, or a scheme assembled by concatenation).
    const QStringList lines = script_text.split(QLatin1Char('\n'));
    for (const QString& raw_line : lines) {
        const QString trimmed = raw_line.trimmed();
        if (trimmed.startsWith(QLatin1Char('#'))) {
            continue;
        }
        const QString lower = trimmed.toLower();
        if (lineHasDownloadUrl(lower) || lineHasRawDownloadToken(lower) ||
            lineHasDownloadCommand(lower)) {
            return true;
        }
    }
    return false;
}

namespace {
// A Windows reserved device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9), with or
// without an extension, is not a usable file-name component and can alias a device.
bool isReservedWindowsDeviceName(const QString& component) {
    static const QSet<QString> kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    const QString base = component.section(QLatin1Char('.'), 0, 0).trimmed().toUpper();
    return kReserved.contains(base);
}
}  // namespace

bool PackageInternalizationEngine::isSafePackageComponent(const QString& component) {
    if (component.isEmpty() || component == QLatin1String(".") ||
        component == QLatin1String("..")) {
        return false;
    }
    // Path separators, the drive colon, and the Win32-illegal filename characters
    // (< > " | ? *) plus control characters are all rejected.
    static const QString kForbidden = QStringLiteral("/\\:<>\"|?*");
    const bool has_forbidden_char = std::ranges::any_of(component, [](const QChar c) {
        return kForbidden.contains(c) || c.unicode() < kFirstNonControlCodeUnit;
    });
    if (has_forbidden_char) {
        return false;
    }
    // A trailing dot or space is stripped by Win32 path resolution and can alias a
    // different name; a reserved device name is not a usable component either.
    if (component.endsWith(QLatin1Char('.')) || component.endsWith(QLatin1Char(' '))) {
        return false;
    }
    return !isReservedWindowsDeviceName(component);
}

bool PackageInternalizationEngine::beginInternalization(const QString& package_id,
                                                        const QString& version,
                                                        QString& resolved_version,
                                                        InternalizationResult& result) {
    // Atomic check-and-set: two threads racing into the same engine must not both
    // acquire it. compare_exchange publishes the busy flag in one step (the prior
    // read-then-set left a window where both saw not-busy).
    bool expected = false;
    if (!m_busy.compare_exchange_strong(expected, true)) {
        Q_EMIT errorOccurred(package_id, "Engine is already busy");
        return false;
    }

    // Reject a package id that would escape the work dir (path separators, "..", drive
    // colon) BEFORE it is composed into extract/nupkg/output paths (B10-11). Release
    // the busy flag we just acquired so a rejected id does not wedge the engine.
    if (!isSafePackageComponent(package_id)) {
        m_busy = false;
        Q_EMIT errorOccurred(package_id,
                             "Invalid package id: path separators or traversal not allowed");
        return false;
    }

    m_cancelled = false;
    m_current_progress = {};
    m_current_progress.package_id = package_id;
    m_current_progress.version = version;

    // Resolve latest version if none was specified
    resolved_version = resolveAndLogVersion(package_id, version);
    result.package_id = package_id;
    result.version = resolved_version;
    if (resolved_version.isEmpty()) {
        finishWithError(result, "Could not resolve latest version for " + package_id);
        return false;
    }
    // The resolved version is likewise concatenated into paths; reject an unsafe one
    // (a hostile feed could return a traversal-bearing version string).
    if (!isSafePackageComponent(resolved_version)) {
        finishWithError(result, "Resolved version contains unsafe path characters");
        return false;
    }
    m_current_progress.version = resolved_version;

    sak::logInfo("[InternalizationEngine] Starting: {} v{}",
                 package_id.toStdString(),
                 resolved_version.toStdString());
    return true;
}

PackageInternalizationEngine::InternalizationPaths
PackageInternalizationEngine::prepareInternalizationPaths(const QString& package_id,
                                                          const QString& version,
                                                          const QString& output_dir,
                                                          const QString& work_dir) const {
    QDir(work_dir).mkpath(".");
    QDir(output_dir).mkpath(".");

    InternalizationPaths paths;
    paths.extract_dir = work_dir + "/" + package_id + "." + version;
    paths.nupkg_path = work_dir + "/" + package_id + "." + version + ".nupkg";
    // Clear any leftovers from a crashed prior build in this exact-named dir before
    // reuse: .NET ExtractToDirectory throws if a file already exists, and stray
    // unrelated files would otherwise be repacked into the output nupkg. The dir is
    // our own work tree (validated id/version components), never a user path.
    QDir(paths.extract_dir).removeRecursively();
    QDir(paths.extract_dir).mkpath(".");
    return paths;
}

bool PackageInternalizationEngine::downloadAndExtractNupkg(const InternalizationPaths& paths,
                                                           InternalizationResult& result) {
    if (!downloadNupkg(result.package_id, result.version, paths.nupkg_path, result)) {
        m_busy = false;
        return false;
    }

    result.original_size = QFileInfo(paths.nupkg_path).size();
    sak::logInfo("[InternalizationEngine] Downloaded {} bytes: {}",
                 result.original_size,
                 paths.nupkg_path.toStdString());

    emitProgress(InternalizationStatus::Extracting, "Extracting package...");
    QString extract_error;
    if (!extractNupkg(paths.nupkg_path, paths.extract_dir, extract_error)) {
        finishWithError(result, "Extraction failed: " + extract_error);
        return false;
    }

    if (m_cancelled) {
        m_busy = false;
        return false;
    }

    return true;
}

bool PackageInternalizationEngine::downloadAndRewriteInstallScript(
    const ParsedInstallScript& parsed, const QString& script_path, InternalizationResult& result) {
    const QString tools_dir = QFileInfo(script_path).dir().absolutePath();
    if (!downloadAllBinaries(parsed, tools_dir, result)) {
        m_busy = false;
        return false;
    }

    emitProgress(InternalizationStatus::RewritingScript, "Rewriting install script...");
    auto rewrite_result =
        m_rewriter.rewriteToFile(parsed, buildLocalFilenameMap(parsed), script_path);
    if (!rewrite_result.success) {
        finishWithError(result, "Script rewrite failed: " + rewrite_result.error_message);
        return false;
    }
    return true;
}

QString PackageInternalizationEngine::resolveAndLogVersion(const QString& package_id,
                                                           const QString& version) {
    if (!version.isEmpty()) {
        return version;
    }
    emitProgress(InternalizationStatus::DownloadingNupkg, "Resolving latest version...");
    QString resolved = resolveLatestVersion(package_id);
    if (!resolved.isEmpty()) {
        sak::logInfo("[InternalizationEngine] Resolved {} -> v{}",
                     package_id.toStdString(),
                     resolved.toStdString());
    }
    return resolved;
}

// ============================================================================
// Internalization Helpers
// ============================================================================

bool PackageInternalizationEngine::downloadNupkg(const QString& package_id,
                                                 const QString& version,
                                                 const QString& nupkg_path,
                                                 InternalizationResult& result) {
    emitProgress(InternalizationStatus::DownloadingNupkg, "Downloading .nupkg...");
    if (m_cancelled) {
        return false;
    }
    const NupkgDownload download = downloadVerifiedNupkg(package_id, version, nupkg_path, [this]() {
        return m_cancelled.load();
    });
    if (download.cancelled) {
        return false;
    }
    if (!download.ok) {
        finishWithError(result, download.error);
        return false;
    }
    sak::logInfo("[InternalizationEngine] Verified .nupkg hash for {} v{}",
                 package_id.toStdString(),
                 version.toStdString());
    return true;
}

// ============================================================================
// NuGet feed client
// ============================================================================

QString PackageInternalizationEngine::FeedListing::failureReason() const {
    switch (status) {
    case FeedListingStatus::Ok:
        return {};
    case FeedListingStatus::Cancelled:
        return QStringLiteral("cancelled");
    case FeedListingStatus::UnsafePackageId:
        return QStringLiteral("unsafe package id");
    case FeedListingStatus::TransportError:
        return QStringLiteral("HTTP %1").arg(http_status);
    case FeedListingStatus::MalformedBody:
        return QStringLiteral("malformed feed body");
    case FeedListingStatus::OffOriginContinuation:
        return QStringLiteral("off-origin feed continuation refused");
    case FeedListingStatus::TooManyPages:
        return QStringLiteral("feed exceeded %1 pages; version list truncated").arg(kMaxFeedPages);
    }
    return QStringLiteral("unknown feed status");
}

PackageInternalizationEngine::FeedListing PackageInternalizationEngine::fetchFeedVersions(
    const QString& package_id, int attempts, const NetworkCancelCheck& cancel) {
    FeedListing listing;
    // The id is embedded into the feed query; keep it a single safe segment.
    if (!isSafePackageComponent(package_id)) {
        listing.status = FeedListingStatus::UnsafePackageId;
        return listing;
    }
    QUrl url(QString("%1%2").arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("id"), QStringLiteral("'%1'").arg(package_id));
    url.setQuery(query);

    for (int attempt = 0; attempt < std::max(attempts, 1); ++attempt) {
        if (attempt > 0) {
            const int delay = offline::kApiRetryDelayBaseMs * attempt;
            sak::logInfo("[NuGetFeed] Retry {}/{} for {} (wait {} ms)",
                         attempt + 1,
                         attempts,
                         package_id.toStdString(),
                         delay);
            if (!sleepUnlessCancelled(delay, cancel)) {
                listing = {};
                listing.status = FeedListingStatus::Cancelled;
                return listing;
            }
        }
        listing = fetchFeedListingOnce(url, cancel);
        if (listing.status != FeedListingStatus::TransportError) {
            return listing;
        }
        sak::logWarning("[NuGetFeed] {} for {} (attempt {})",
                        listing.failureReason().toStdString(),
                        package_id.toStdString(),
                        attempt + 1);
    }
    return listing;
}

PackageInternalizationEngine::FeedPage PackageInternalizationEngine::parseFeedPage(
    const QByteArray& body) {
    // A non-empty body that is NOT a well-formed feed (an error document, HTML, truncated XML)
    // is refused rather than read as an authoritative empty version list. A genuinely
    // empty-but-valid <feed> still parses, so a package with no versions is unaffected.
    FeedPage page;
    bool saw_feed_root = false;
    QXmlStreamReader reader(body);
    while (!reader.atEnd()) {
        reader.readNext();
        if (!reader.isStartElement()) {
            continue;
        }
        const QStringView name = reader.name();
        if (name == QLatin1String("feed")) {
            saw_feed_root = true;
        } else if (name == QLatin1String("link") &&
                   reader.attributes().value(QLatin1String("rel")) == QLatin1String("next")) {
            page.next_link = reader.attributes().value(QLatin1String("href")).toString();
        }
    }
    page.well_formed = saw_feed_root && !reader.hasError();
    if (!page.well_formed) {
        page.next_link.clear();
        return page;
    }
    page.versions = NuGetDependencyResolver::parseODataFeedVersions(body);
    return page;
}

bool PackageInternalizationEngine::sameFeedOrigin(const QUrl& origin, const QUrl& candidate) {
    return candidate.scheme() == origin.scheme() && candidate.host() == origin.host() &&
           candidate.port() == origin.port();
}

QString PackageInternalizationEngine::latestFeedVersion(
    const QVector<FeedPackageVersion>& versions) {
    QStringList all_versions;
    for (const FeedPackageVersion& entry : versions) {
        if (entry.version.isEmpty()) {
            continue;
        }
        // The feed's explicit IsLatestVersion flag wins outright when present.
        if (entry.is_latest) {
            return entry.version;
        }
        all_versions.append(entry.version);
    }
    // No entry flagged latest: pick the SemVer-max rather than trusting feed order (a
    // hostile/unsorted feed could otherwise push a downgrade via last-entry). When NO entry
    // parses as a valid SemVer, fail closed (empty) rather than fall back to the feed's raw
    // last version.
    return semverMaxVersion(all_versions);
}

std::optional<FeedPackageVersion> PackageInternalizationEngine::feedEntryForVersion(
    const QVector<FeedPackageVersion>& versions, const QString& version) {
    // EXACT compare: a near-miss request ("2.0" for "2.0.0") must never resolve to a different
    // entry's PackageHash, which the download then trusts as the expected digest.
    const auto found = std::ranges::find_if(versions, [&version](const FeedPackageVersion& entry) {
        return entry.version == version;
    });
    if (found == versions.end()) {
        return std::nullopt;
    }
    return *found;
}

namespace {

// The feed entry whose PackageHash authenticates @p version's .nupkg, or nullopt with
// @p outcome saying why (a Stop, a failed lookup, or no hash published for that version).
std::optional<FeedPackageVersion> trustedFeedEntry(
    const QString& package_id,
    const QString& version,
    const NetworkCancelCheck& cancel,
    PackageInternalizationEngine::NupkgDownload& outcome) {
    using Engine = PackageInternalizationEngine;
    // Both become segments of the package URL.
    if (!Engine::isSafePackageComponent(package_id) || !Engine::isSafePackageComponent(version)) {
        outcome.error = QStringLiteral("Refusing an unsafe package id or version: %1 %2")
                            .arg(package_id, version);
        return std::nullopt;
    }
    const Engine::FeedListing listing = Engine::fetchFeedVersions(package_id, 1, cancel);
    if (listing.status == Engine::FeedListingStatus::Cancelled) {
        outcome.cancelled = true;
        return std::nullopt;
    }
    if (!listing.ok()) {
        outcome.error = QStringLiteral(
                            "Cannot verify package integrity: feed lookup for %1 "
                            "failed (%2)")
                            .arg(package_id, listing.failureReason());
        return std::nullopt;
    }
    auto entry = Engine::feedEntryForVersion(listing.versions, version);
    if (!entry || entry->package_hash.trimmed().isEmpty()) {
        outcome.error = "Cannot verify package integrity: feed published no hash for " + package_id;
        return std::nullopt;
    }
    return entry;
}

}  // namespace

PackageInternalizationEngine::NupkgDownload PackageInternalizationEngine::downloadVerifiedNupkg(
    const QString& package_id,
    const QString& version,
    const QString& output_path,
    const NetworkCancelCheck& cancel) {
    NupkgDownload outcome;
    // The feed's published hash FIRST: with no trusted hash there is nothing to authenticate
    // the package against, so it is not downloaded at all.
    const auto entry = trustedFeedEntry(package_id, version, cancel, outcome);
    if (!entry) {
        return outcome;
    }
    const QString nupkg_url =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, package_id, version);
    // https only (no file://, ftp://, http://, UNC), so a tampered base URL cannot pull the
    // package off disk or an insecure origin.
    if (!isAllowedInstallerUrl(nupkg_url)) {
        outcome.error = "Refusing non-https package URL: " + nupkg_url;
        return outcome;
    }
    const auto transfer = runNetworkTransfer(makeBinaryDownloadRequest(QUrl(nupkg_url)), cancel);
    if (transfer.cancelled) {
        outcome.cancelled = true;
        return outcome;
    }
    if (!transfer.success || transfer.body.isEmpty()) {
        outcome.error = "Nupkg download failed: " +
                        (transfer.error_message.isEmpty()
                             ? QStringLiteral("HTTP %1").arg(transfer.http_status)
                             : transfer.error_message);
        return outcome;
    }
    // Authenticate BEFORE anything reaches disk: a tampered .nupkg would otherwise have its
    // install script parsed and its embedded binaries harvested into the bundle.
    if (!nupkgHashMatches(transfer.body, entry->package_hash, entry->package_hash_algorithm)) {
        outcome.error = "Package integrity verification failed for " + package_id;
        return outcome;
    }
    // QSaveFile + full-size + commit, so a disk-full partial write never leaves a truncated
    // .nupkg that would then be extracted and shipped.
    QString write_error;
    if (!writeBinaryBody(output_path, transfer.body, write_error)) {
        outcome.error = "Nupkg download failed: " + write_error;
        return outcome;
    }
    outcome.ok = true;
    return outcome;
}

// ============================================================================
// Version Resolution
// ============================================================================

QString PackageInternalizationEngine::resolveLatestVersion(const QString& package_id) {
    // A transport failure is retried (kApiMaxRetries attempts in all), as version resolution
    // always was; a malformed or truncated feed is not.
    const FeedListing listing = fetchFeedVersions(package_id, offline::kApiMaxRetries, [this]() {
        return m_cancelled.load();
    });
    if (!listing.ok()) {
        if (listing.status != FeedListingStatus::Cancelled) {
            sak::logWarning("[InternalizationEngine] Version lookup failed for {}: {}",
                            package_id.toStdString(),
                            listing.failureReason().toStdString());
        }
        return {};
    }
    const QString resolved = latestFeedVersion(listing.versions);
    if (resolved.isEmpty()) {
        sak::logWarning("[InternalizationEngine] No version found for {} ({} feed entries)",
                        package_id.toStdString(),
                        static_cast<int>(listing.versions.size()));
    } else {
        sak::logInfo("[InternalizationEngine] Resolved {} -> v{}",
                     package_id.toStdString(),
                     resolved.toStdString());
    }
    return resolved;
}

void PackageInternalizationEngine::repackAndFinish(InternalizationResult& result,
                                                   const QString& extract_dir,
                                                   const QString& output_dir,
                                                   const QString& status_message) {
    emitProgress(InternalizationStatus::Repacking, status_message);
    // Fail closed if metadata cleanup did not fully succeed: repacking leftover
    // NuGet artifacts would ship a malformed offline nupkg.
    if (!cleanNugetArtifacts(extract_dir)) {
        finishWithError(result, "Failed to remove NuGet metadata before repack");
        return;
    }

    // Repacking runs in-process and cannot be interrupted, so a Stop is honoured before it,
    // the same way as after extraction (a cancelled run ends without success or an error).
    if (m_cancelled) {
        m_busy = false;
        return;
    }
    const QString output_nupkg = output_dir + "/" + result.package_id + "." + result.version +
                                 ".nupkg";
    QString repack_error;
    if (!repackNupkg(extract_dir, output_nupkg, repack_error)) {
        finishWithError(result, "Failed to repack .nupkg: " + repack_error);
        return;
    }

    result.checksum = computeChecksum(output_nupkg);
    result.internalized_size = QFileInfo(output_nupkg).size();
    // A repack that yielded no readable checksum or a zero-size file cannot be
    // integrity-verified at install time, so it is a failure -- never reported as a
    // successful internalization on the strength of the repack step alone.
    if (result.checksum.isEmpty() || result.internalized_size <= 0) {
        finishWithError(result, "Repacked package failed checksum/size verification");
        return;
    }

    result.success = true;
    // True only if binaries were actually downloaded; a no-URL repack leaves this
    // false so consumers can warn the package may not be fully offline.
    result.binaries_internalized = !result.internalized_files.isEmpty();
    result.output_nupkg_path = output_nupkg;
    sak::logInfo("[InternalizationEngine] Internalized {} v{}: {} -> {} bytes, {} files",
                 result.package_id.toStdString(),
                 result.version.toStdString(),
                 result.original_size,
                 result.internalized_size,
                 static_cast<int>(result.internalized_files.size()));
    emitProgress(InternalizationStatus::Complete, "Internalization complete");
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

bool PackageInternalizationEngine::binaryChecksumMatches(const QByteArray& data,
                                                         const QString& expected_checksum,
                                                         const QString& checksum_type) {
    const QString expected = expected_checksum.trimmed();
    if (expected.isEmpty()) {
        return true;  // no checksum was declared in the script -> nothing to verify
    }
    const auto algo = resolveChecksumAlgorithm(checksum_type, static_cast<int>(expected.size()));
    if (!algo) {
        return false;  // a checksum was declared but we cannot resolve its algorithm
    }
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(data, *algo).toHex());
    return actual.compare(expected, Qt::CaseInsensitive) == 0;
}

QString PackageInternalizationEngine::installerRefusalMessage(const QString& url,
                                                              const QString& declared_checksum) {
    // Review 2: adobereader failed every bundle with "Installer checksum verification failed",
    // which read like a fault of the bundle. The package's own declaration is what disagrees.
    if (declared_checksum.trimmed().isEmpty()) {
        return QStringLiteral(
                   "Not bundled: the package declares no checksum for %1, so its installer "
                   "cannot be verified. The package needs fixing upstream.")
            .arg(url);
    }
    // A checksum still naming a variable is one this app could not read from the script: not the
    // vendor's change, and not an altered file.
    if (declared_checksum.contains(QLatin1Char('$'))) {
        return QStringLiteral(
                   "Not bundled: the checksum for %1 is set by %2 in the package's script, which "
                   "S.A.K. could not resolve, so the installer cannot be verified.")
            .arg(url, declared_checksum.trimmed());
    }
    return QStringLiteral(
               "Not bundled: the installer at %1 does not match the checksum its package "
               "declares. The vendor has changed the file and the upstream package has not "
               "caught up, or the file was altered; try again once the package is updated.")
        .arg(url);
}

bool PackageInternalizationEngine::installerVerified(const QByteArray& data,
                                                     const QString& expected_checksum,
                                                     const QString& checksum_type) {
    // Fail closed: an installer whose script declared no checksum cannot be
    // verified against a trusted value, so it must not ship in an offline bundle.
    if (expected_checksum.trimmed().isEmpty()) {
        return false;
    }
    return binaryChecksumMatches(data, expected_checksum, checksum_type);
}

QString PackageInternalizationEngine::sanitizedBinaryBasename(const QString& url_filename,
                                                              int index) {
    if (url_filename.isEmpty() || !isSafePackageComponent(url_filename)) {
        return QStringLiteral("binary_%1").arg(index + 1);
    }
    return url_filename;
}

bool PackageInternalizationEngine::isAllowedInstallerUrl(const QString& url) {
    const QUrl parsed(url, QUrl::StrictMode);
    if (!parsed.isValid() || parsed.host().isEmpty()) {
        return false;  // malformed, hostless (file:///, bare path), or UNC -> refuse
    }
    return parsed.scheme().compare(QLatin1String("https"), Qt::CaseInsensitive) == 0;
}

bool PackageInternalizationEngine::nupkgHashMatches(const QByteArray& data,
                                                    const QString& expected_base64,
                                                    const QString& algorithm) {
    const QString expected = expected_base64.trimmed();
    if (expected.isEmpty()) {
        return false;  // no trusted hash available -> cannot verify -> fail closed
    }
    const auto algo = hashAlgorithmByName(algorithm.trimmed().toLower());
    if (!algo) {
        return false;  // unknown algorithm -> fail closed
    }
    const QString actual = QString::fromLatin1(QCryptographicHash::hash(data, *algo).toBase64());
    return actual == expected;
}

QHash<QString, QPair<QString, QString>> PackageInternalizationEngine::binaryChecksumMap(
    const ParsedInstallScript& parsed) {
    QHash<QString, QPair<QString, QString>> map;
    for (const auto& resource : parsed.resources) {
        // The primary URL carries checksum/checksumType; the 64-bit URL carries
        // its own checksum64/checksumType64. Verify BOTH at build time -- a 64-bit
        // installer that shipped unverified (only the 32-bit was checked) was the
        // asymmetric integrity gap.
        if (!resource.url.isEmpty() && !resource.checksum.isEmpty()) {
            map.insert(resource.url, {resource.checksum, resource.checksum_type});
        }
        if (!resource.url_64bit.isEmpty() && !resource.checksum_64bit.isEmpty()) {
            map.insert(resource.url_64bit, {resource.checksum_64bit, resource.checksum_type_64bit});
        }
    }
    return map;
}

bool PackageInternalizationEngine::downloadAllBinaries(const ParsedInstallScript& parsed,
                                                       const QString& tools_dir,
                                                       InternalizationResult& result) {
    emitProgress(InternalizationStatus::DownloadingBinaries, "Downloading binaries...");

    QStringList urls_to_download = collectBinaryUrls(parsed);
    m_current_progress.binary_total = static_cast<int>(urls_to_download.size());

    // Same iteration order + shared used_names as buildLocalFilenameMap, so the on-disk name and
    // the rewritten-script name for each URL always agree.
    const QHash<QString, QPair<QString, QString>> checksums = binaryChecksumMap(parsed);
    QSet<QString> used_names;
    for (int idx = 0; idx < urls_to_download.size(); ++idx) {
        if (m_cancelled) {
            return false;
        }

        m_current_progress.binary_index = idx + 1;
        emitProgress(
            InternalizationStatus::DownloadingBinaries,
            QString("Downloading binary %1 of %2...").arg(idx + 1).arg(urls_to_download.size()));

        const QString& url = urls_to_download[idx];
        // Restrict installer sources to https before any fetch (no file://, ftp://,
        // http://, UNC) so a hostile script cannot pull a binary off disk / insecurely.
        if (!isAllowedInstallerUrl(url)) {
            finishWithError(result, "Refusing non-https installer URL: " + url);
            return false;
        }
        const QString filename = uniqueBinaryFilename(QUrl(url), idx, used_names);
        const QString output_path = tools_dir + "/" + filename;

        const auto body = fetchBinary(url, output_path, result);
        if (!body || !binaryChecksumOk(*body, url, checksums, result)) {
            return false;
        }
        result.internalized_files.append(filename);
    }

    return !m_cancelled;
}

std::optional<QByteArray> PackageInternalizationEngine::fetchBinary(const QString& url,
                                                                    const QString& output_path,
                                                                    InternalizationResult& result) {
    const NetworkTransferRequest request = makeBinaryDownloadRequest(QUrl(url));
    const auto transfer = runNetworkTransfer(
        request,
        [this]() { return m_cancelled.load(); },
        [this](qint64 received, qint64 total) {
            m_current_progress.bytes_downloaded = received;
            m_current_progress.bytes_total = total;
            Q_EMIT progressChanged(m_current_progress);
        });
    if (transfer.cancelled) {
        return std::nullopt;  // cancellation is reported by the caller's own gate
    }

    QString error = transfer.error_message;
    bool ok = transfer.success && !transfer.body.isEmpty();
    if (ok) {
        ok = writeBinaryBody(output_path, transfer.body, error);
    }
    if (!ok) {
        sak::logWarning("[InternalizationEngine] Binary download failed: {} - {}",
                        url.toStdString(),
                        error.toStdString());
        finishWithError(result, "Binary download failed: " + error);
        return std::nullopt;
    }
    return transfer.body;
}

bool PackageInternalizationEngine::binaryChecksumOk(
    const QByteArray& body,
    const QString& url,
    const QHash<QString, QPair<QString, QString>>& checksums,
    InternalizationResult& result) {
    // Verify the downloaded bytes against the script-declared checksum BEFORE this
    // file is repacked into the offline nupkg. Fails CLOSED when the script declared
    // no checksum: an unverifiable installer is refused, not shipped as "verified".
    const auto expected = checksums.constFind(url);
    const QString cs = expected == checksums.constEnd() ? QString() : expected->first;
    const QString cs_type = expected == checksums.constEnd() ? QString() : expected->second;
    if (!installerVerified(body, cs, cs_type)) {
        sak::logError(
            "[InternalizationEngine] Unverified installer (missing/mismatched "
            "checksum): {}",
            url.toStdString());
        finishWithError(result, installerRefusalMessage(url, cs));
        return false;
    }
    return true;
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
    QSet<QString> used_names;
    const QStringList urls = collectBinaryUrls(parsed);
    for (int idx = 0; idx < urls.size(); ++idx) {
        // Mirror downloadAllBinaries exactly (same order, index base, and disambiguation) so each
        // URL maps to the identical local filename that was written to disk.
        const QString filename = uniqueBinaryFilename(QUrl(urls[idx]), idx, used_names);
        local_filenames.insert(urls[idx], filename);
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
                                                const QString& extract_dir,
                                                QString& error_out) {
    if (!nupkgHasZipMagic(nupkg_path, error_out)) {
        return false;
    }
    // The app's one zip engine: bounded central directory, entry and expansion caps, zip-slip
    // and junction refusal, exclusive file creation and rollback of a partial extraction.
    const FileExplorerArchiveResult extracted = FileExplorerArchiveService::extractZip(nupkg_path,
                                                                                       extract_dir);
    // A skipped entry (a symlink, or a corrupt central-directory record) would be missing from
    // the repacked package, so for a package it is a failure rather than a warning.
    const QStringList problems = extracted.blockers + extracted.warnings;
    if (!extracted.ok || !problems.isEmpty()) {
        error_out = problems.isEmpty() ? QStringLiteral("the archive could not be extracted")
                                       : problems.join(QStringLiteral(" "));
        sak::logError("[InternalizationEngine] Extract failed: {}", error_out.toStdString());
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
// NuGet Artifact Cleanup
// ============================================================================

bool PackageInternalizationEngine::cleanNugetArtifacts(const QString& extract_dir) const {
    // Remove NuGet metadata not needed in internalized packages, tracking every
    // removal so a failure is surfaced instead of silently repacking the leftover.
    bool ok = true;

    QDir rels_dir(extract_dir + "/_rels");
    if (rels_dir.exists()) {
        ok = rels_dir.removeRecursively();
    }

    const QString content_types = extract_dir + "/[Content_Types].xml";
    if (QFile::exists(content_types)) {
        ok = QFile::remove(content_types) && ok;
    }

    QDir pkg_dir(extract_dir + "/package");  // NuGet services metadata
    if (pkg_dir.exists()) {
        ok = pkg_dir.removeRecursively() && ok;
    }

    QDirIterator iter(extract_dir, {"*.psmdcp"}, QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        ok = QFile::remove(iter.next()) && ok;
    }

    if (!ok) {
        sak::logError("[InternalizationEngine] Failed to remove NuGet metadata artifacts in {}",
                      extract_dir.toStdString());
    }
    return ok;
}

// ============================================================================
// Repacking
// ============================================================================

bool PackageInternalizationEngine::repackNupkg(const QString& source_dir,
                                               const QString& output_path,
                                               QString& error_out) {
    // The package's files belong at the archive root, so the sources are the extracted tree's
    // top-level entries rather than the folder itself (which would add a wrapper folder).
    const QFileInfoList top_level =
        QDir(source_dir)
            .entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                           QDir::Name);
    if (top_level.isEmpty()) {
        error_out = QStringLiteral("%1 holds nothing to repack").arg(source_dir);
        sak::logError("[InternalizationEngine] Nothing to repack: {}", error_out.toStdString());
        return false;
    }
    QStringList sources;
    sources.reserve(top_level.size());
    for (const QFileInfo& entry : top_level) {
        sources.append(entry.absoluteFilePath());
    }
    // compressToZip creates the archive exclusively and removes it when it reports a failure.
    const FileExplorerArchiveResult packed = FileExplorerArchiveService::compressToZip(output_path,
                                                                                       sources);
    // A skipped source (a symlink or special file) would be missing from the package.
    const QStringList problems = packed.blockers + packed.warnings;
    if (!packed.ok || !problems.isEmpty()) {
        if (packed.ok) {
            QFile::remove(output_path);
        }
        error_out = problems.isEmpty() ? QStringLiteral("the archive could not be written")
                                       : problems.join(QStringLiteral(" "));
        sak::logError("[InternalizationEngine] Repack failed: {}", error_out.toStdString());
        return false;
    }
    return true;
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
        const QByteArray block = file.read(kBlockSize);
        // read() returns an empty block on a mid-stream I/O error while atEnd() is
        // still false. Hashing on regardless would yield a checksum over a partial
        // file; fail closed (empty) so the caller rejects it rather than shipping a
        // wrong digest.
        if (block.isEmpty() && file.error() != QFileDevice::NoError) {
            sak::logWarning("[InternalizationEngine] Read error during checksum: {}",
                            file_path.toStdString());
            return {};
        }
        hasher.addData(block);
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
