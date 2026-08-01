// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_internalization_engine.cpp
/// @brief Engine implementation for internalizing Chocolatey packages

#include "sak/package_internalization_engine.h"

#include "sak/logger.h"
#include "sak/network_transfer_runner.h"
#include "sak/offline_deployment_constants.h"
#include "sak/process_runner.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>
#include <QThread>
#include <QUrl>
#include <QUrlQuery>

#include <cstring>
#include <optional>

namespace sak {

namespace {
NetworkTransferRequest makeVersionRequest(const QUrl& url) {
    NetworkTransferRequest request;
    request.url = url;
    request.timeout_ms = offline::kApiRequestTimeoutMs;
    // Version metadata is a small feed document; cap the buffer so a hostile endpoint
    // cannot exhaust memory here (B9-19). Binary downloads are left uncapped.
    request.max_response_bytes = 32LL * 1024 * 1024;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});
    request.raw_headers.append(QPair<QByteArray, QByteArray>{
        QByteArrayLiteral("Accept"), QByteArrayLiteral("application/atom+xml")});
    return request;
}

QString uniqueBinaryFilename(const QUrl& url, int index, QSet<QString>& used_names) {
    QString base = url.fileName();
    if (base.isEmpty()) {
        base = QString("binary_%1").arg(index + 1);
    }
    // Two distinct URLs can share a basename (x86 vs x64 setup.exe); disambiguate so the second
    // never overwrites the first on disk and the rewritten script maps each URL to its own file.
    QString candidate = base;
    const QFileInfo info(base);
    const QString stem = info.completeBaseName();
    const QString suffix = info.suffix();
    int counter = 1;
    while (used_names.contains(candidate)) {
        candidate = suffix.isEmpty() ? QString("%1_%2").arg(stem).arg(counter)
                                     : QString("%1_%2.%3").arg(stem).arg(counter).arg(suffix);
        ++counter;
    }
    used_names.insert(candidate);
    return candidate;
}

NetworkTransferRequest makeBinaryDownloadRequest(const QUrl& url) {
    NetworkTransferRequest request;
    request.url = url;
    request.timeout_ms = offline::kDownloadTimeoutMs;
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
    case 32:
        return QCryptographicHash::Md5;
    case 40:
        return QCryptographicHash::Sha1;
    case 64:
        return QCryptographicHash::Sha256;
    case 128:
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
        // No script means nothing was internalized: repack, but do NOT present it
        // as a fully offline package (binaries_internalized stays false).
        sak::logWarning(
            "[InternalizationEngine] No install script found for {}; nothing internalized "
            "(package may still require internet at install time)",
            package_id.toStdString());
        repackAndFinish(
            result, paths.extract_dir, output_dir, "Repacking (no binaries internalized)...");
        return;
    }

    const auto parsed = m_parser.parseFile(script_path);
    if (parsed.resources.isEmpty()) {
        sak::logWarning(
            "[InternalizationEngine] No recognized download URLs in script for {}; nothing "
            "internalized (package may still require internet at install time)",
            package_id.toStdString());
        repackAndFinish(
            result, paths.extract_dir, output_dir, "Repacking (no binaries internalized)...");
        return;
    }

    if (!downloadAndRewriteInstallScript(parsed, script_path, result)) {
        return;
    }

    repackAndFinish(result, paths.extract_dir, output_dir, "Repacking .nupkg...");
}

bool PackageInternalizationEngine::isSafePackageComponent(const QString& component) {
    if (component.isEmpty() || component == QLatin1String(".") ||
        component == QLatin1String("..")) {
        return false;
    }
    for (const QChar c : component) {
        if (c == QLatin1Char('/') || c == QLatin1Char('\\') || c == QLatin1Char(':') ||
            c.unicode() < 0x20) {
            return false;
        }
    }
    return true;
}

bool PackageInternalizationEngine::beginInternalization(const QString& package_id,
                                                        const QString& version,
                                                        QString& resolved_version,
                                                        InternalizationResult& result) {
    if (m_busy) {
        Q_EMIT errorOccurred(package_id, "Engine is already busy");
        return false;
    }

    // Reject a package id that would escape the work dir (path separators, "..", drive
    // colon) BEFORE it is composed into extract/nupkg/output paths (B10-11).
    if (!isSafePackageComponent(package_id)) {
        Q_EMIT errorOccurred(package_id,
                             "Invalid package id: path separators or traversal not allowed");
        return false;
    }

    m_busy = true;
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

    QString nupkg_url;
    if (version.isEmpty()) {
        nupkg_url =
            QString("%1%2%3").arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, package_id);
    } else {
        nupkg_url =
            QString("%1%2%3/%4")
                .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, package_id, version);
    }

    sak::logInfo("[InternalizationEngine] Download URL: {}", nupkg_url.toStdString());

    if (m_cancelled) {
        return false;
    }

    NetworkTransferRequest request;
    request.url = QUrl(nupkg_url);
    request.timeout_ms = offline::kDownloadTimeoutMs;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{QByteArrayLiteral("User-Agent"),
                                                             QByteArrayLiteral("SAK-Utility/1.0")});

    const auto transfer = runNetworkTransfer(request, [this]() { return m_cancelled.load(); });
    if (transfer.cancelled) {
        return false;
    }
    if (!transfer.success || transfer.body.isEmpty()) {
        const auto error = transfer.error_message.isEmpty()
                               ? QStringLiteral("HTTP %1").arg(transfer.http_status)
                               : transfer.error_message;
        sak::logError("[InternalizationEngine] Download HTTP {}: {}",
                      transfer.http_status,
                      error.toStdString());
        finishWithError(result, "Nupkg download failed: " + error);
        return false;
    }

    QFile file(nupkg_path);
    if (!file.open(QIODevice::WriteOnly)) {
        finishWithError(result, "Nupkg download failed: Cannot write nupkg file");
        return false;
    }
    file.write(transfer.body);
    file.close();

    return true;
}

// ============================================================================
// Version Resolution
// ============================================================================

QString PackageInternalizationEngine::resolveLatestVersion(const QString& package_id) {
    QString url_str = QString("%1%2").arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath);
    QUrl url(url_str);
    QUrlQuery query;
    query.addQueryItem("id", QString("'%1'").arg(package_id));
    url.setQuery(query);

    sak::logInfo("[InternalizationEngine] Resolving version: {}", url.toString().toStdString());

    QString resolved_version;

    for (int attempt = 0; attempt < offline::kApiMaxRetries; ++attempt) {
        if (m_cancelled) {
            return {};
        }
        if (attempt > 0) {
            int delay = offline::kApiRetryDelayBaseMs * attempt;
            sak::logInfo("[InternalizationEngine] Retry {}/{} for {} (wait {}ms)",
                         attempt + 1,
                         offline::kApiMaxRetries,
                         package_id.toStdString(),
                         delay);
            QThread::msleep(static_cast<unsigned long>(delay));
        }

        const NetworkTransferRequest request = makeVersionRequest(url);
        bool request_ok = false;
        const auto transfer = runNetworkTransfer(request, [this]() { return m_cancelled.load(); });
        if (transfer.cancelled) {
            return {};
        }
        if (!transfer.success) {
            sak::logWarning(
                "[InternalizationEngine] HTTP {} for "
                "{} (attempt {}): {}",
                transfer.http_status,
                package_id.toStdString(),
                attempt + 1,
                transfer.error_message.toStdString());
        } else {
            resolved_version = parseLatestVersionFromOData(transfer.body);

            if (resolved_version.isEmpty()) {
                sak::logWarning(
                    "[InternalizationEngine] No version "
                    "found for {} ({} bytes)",
                    package_id.toStdString(),
                    static_cast<int>(transfer.body.size()));
            } else {
                sak::logInfo(
                    "[InternalizationEngine] Resolved "
                    "{} -> v{}",
                    package_id.toStdString(),
                    resolved_version.toStdString());
            }
            request_ok = true;
        }

        if (!resolved_version.isEmpty()) {
            return resolved_version;
        }
        if (request_ok) {
            break;
        }
    }

    return resolved_version;
}

QString PackageInternalizationEngine::parseLatestVersionFromOData(const QByteArray& data) {
    QDomDocument doc;
    if (!doc.setContent(data)) {
        sak::logError(
            "[InternalizationEngine] XML parse failed "
            "({} bytes)",
            static_cast<int>(data.size()));
        return {};
    }

    QDomElement root = doc.documentElement();
    QDomNodeList entries = root.elementsByTagName("entry");
    QString fallback_version;

    for (int idx = 0; idx < entries.count(); ++idx) {
        QDomElement entry = entries.at(idx).toElement();

        QDomNodeList prop_nodes = entry.elementsByTagName("m:properties");
        if (prop_nodes.isEmpty()) {
            prop_nodes = entry.elementsByTagName("properties");
        }
        if (prop_nodes.isEmpty()) {
            continue;
        }

        QDomElement props = prop_nodes.at(0).toElement();
        QDomElement version_elem = props.firstChildElement("d:Version");
        if (version_elem.isNull()) {
            version_elem = props.firstChildElement("Version");
        }
        if (version_elem.isNull()) {
            continue;
        }

        QString ver = version_elem.text().trimmed();
        fallback_version = ver;

        QDomElement latest_elem = props.firstChildElement("d:IsLatestVersion");
        if (latest_elem.isNull()) {
            latest_elem = props.firstChildElement("IsLatestVersion");
        }
        if (!latest_elem.isNull() && latest_elem.text().trimmed().toLower() == "true") {
            return ver;
        }
    }

    return fallback_version;
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
        // True only if binaries were actually downloaded; a no-URL repack leaves
        // this false so consumers can warn the package may not be fully offline.
        result.binaries_internalized = !result.internalized_files.isEmpty();
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

QHash<QString, QPair<QString, QString>> PackageInternalizationEngine::binaryChecksumMap(
    const ParsedInstallScript& parsed) {
    QHash<QString, QPair<QString, QString>> map;
    for (const auto& resource : parsed.resources) {
        // The parser captures a single checksum, which applies to the primary URL.
        if (!resource.url.isEmpty() && !resource.checksum.isEmpty()) {
            map.insert(resource.url, {resource.checksum, resource.checksum_type});
        }
    }
    return map;
}

bool PackageInternalizationEngine::downloadAllBinaries(const ParsedInstallScript& parsed,
                                                       const QString& tools_dir,
                                                       InternalizationResult& result) {
    emitProgress(InternalizationStatus::DownloadingBinaries, "Downloading binaries...");

    QStringList urls_to_download = collectBinaryUrls(parsed);
    m_current_progress.binary_total = urls_to_download.size();

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
        const QUrl parsed_url(url);
        const QString filename = uniqueBinaryFilename(parsed_url, idx, used_names);
        const QString output_path = tools_dir + "/" + filename;
        const NetworkTransferRequest request = makeBinaryDownloadRequest(parsed_url);

        const auto transfer = runNetworkTransfer(
            request,
            [this]() { return m_cancelled.load(); },
            [this](qint64 received, qint64 total) {
                m_current_progress.bytes_downloaded = received;
                m_current_progress.bytes_total = total;
                Q_EMIT progressChanged(m_current_progress);
            });
        if (transfer.cancelled) {
            return false;
        }

        bool ok = transfer.success && !transfer.body.isEmpty();
        QString error = transfer.error_message;
        if (ok) {
            ok = writeBinaryBody(output_path, transfer.body, error);
        }

        if (!ok) {
            sak::logWarning("[InternalizationEngine] Binary download failed: {} - {}",
                            url.toStdString(),
                            error.toStdString());
            finishWithError(result, "Binary download failed: " + error);
            return false;
        }

        // Verify the downloaded bytes against the script-declared checksum (when the
        // parser captured one) BEFORE this file is repacked into the offline nupkg --
        // a corrupted or substituted installer must never ship in the bundle.
        const auto expected = checksums.constFind(url);
        if (expected != checksums.constEnd() &&
            !binaryChecksumMatches(transfer.body, expected->first, expected->second)) {
            sak::logError("[InternalizationEngine] Checksum mismatch for {}", url.toStdString());
            finishWithError(result, "Checksum verification failed for " + url);
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
    // Validate the file starts with the ZIP magic bytes (PK\x03\x04)
    constexpr int kZipMagicSize = 4;
    constexpr char kZipMagic[] = {'\x50', '\x4B', '\x03', '\x04'};
    QFile nupkg_file(nupkg_path);
    if (!nupkg_file.open(QIODevice::ReadOnly)) {
        error_out = "Cannot open nupkg file: " + nupkg_path;
        sak::logError("[InternalizationEngine] {}", error_out.toStdString());
        return false;
    }
    QByteArray header = nupkg_file.read(kZipMagicSize);
    qint64 file_size = nupkg_file.size();
    nupkg_file.close();
    if (header.size() < kZipMagicSize ||
        std::memcmp(header.constData(), kZipMagic, kZipMagicSize) != 0) {
        error_out = QString("Not a valid ZIP archive (%1 bytes)").arg(file_size);
        sak::logError("[InternalizationEngine] {}: {}",
                      error_out.toStdString(),
                      nupkg_path.toStdString());
        return false;
    }

    // Use .NET ZipFile for reliable extraction with native path separators
    QString native_nupkg = QDir::toNativeSeparators(nupkg_path);
    QString native_extract = QDir::toNativeSeparators(extract_dir);
    // Escape apostrophes for the single-quoted PowerShell string literals below;
    // a path containing ' would otherwise close the literal and inject commands.
    native_nupkg.replace(QLatin1Char('\''), QStringLiteral("''"));
    native_extract.replace(QLatin1Char('\''), QStringLiteral("''"));
    QString ps_command = QString(
                             "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                             "[System.IO.Compression.ZipFile]::ExtractToDirectory('%1', '%2')")
                             .arg(native_nupkg, native_extract);

    const auto process = runPowerShell(ps_command, offline::kPackTimeoutMs, true, false, [this]() {
        return m_cancelled.load();
    });
    if (process.timed_out) {
        error_out = "Extraction timed out";
        sak::logError("[InternalizationEngine] {}", error_out.toStdString());
        return false;
    }
    if (process.cancelled) {
        error_out = "Extraction cancelled";
        return false;
    }

    if (process.exit_code != 0) {
        QString stderr_text = process.std_err.trimmed();
        error_out = stderr_text.isEmpty()
                        ? QString("PowerShell exit code %1").arg(process.exit_code)
                        : stderr_text;
        sak::logError("[InternalizationEngine] Extract failed (exit {}): {}",
                      process.exit_code,
                      error_out.toStdString());
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
    // Use .NET ZipFile to create the archive (Compress-Archive rejects .nupkg)
    QString native_src = QDir::toNativeSeparators(source_dir);
    QString native_out = QDir::toNativeSeparators(output_path);
    // Escape apostrophes for the single-quoted PowerShell string literals below;
    // a path containing ' would otherwise close the literal and inject commands.
    native_src.replace(QLatin1Char('\''), QStringLiteral("''"));
    native_out.replace(QLatin1Char('\''), QStringLiteral("''"));
    QString ps_command = QString(
                             "Add-Type -AssemblyName System.IO.Compression.FileSystem; "
                             "[System.IO.Compression.ZipFile]::CreateFromDirectory('%1', '%2')")
                             .arg(native_src, native_out);

    const auto process = runPowerShell(ps_command, offline::kPackTimeoutMs, true, false, [this]() {
        return m_cancelled.load();
    });
    if (process.timed_out) {
        sak::logError("[InternalizationEngine] Repack timed out");
        return false;
    }
    if (process.cancelled) {
        sak::logError("[InternalizationEngine] Repack cancelled");
        return false;
    }

    if (process.exit_code != 0) {
        QString err = process.std_err;
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
