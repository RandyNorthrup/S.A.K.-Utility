// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_recovery_engine.cpp
/// @brief Offline file-level recovery scanner for Partition Manager Data Recovery.

#include "sak/file_recovery_engine.h"

#include "sak/io_write_utils.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <limits>
#include <optional>

namespace sak {

namespace {

constexpr int kPngSignatureSize = 8;
constexpr int kPngChunkHeaderSize = 8;
constexpr int kPngChunkCrcSize = 4;
constexpr int kPngChunkTypeOffset = 4;
constexpr int kPngChunkLengthBytes = 4;
constexpr int kJpegMarkerSize = 2;
constexpr int kJpegSignatureSize = 3;
constexpr int kPdfSignatureSize = 5;
constexpr int kPdfEofMarkerSize = 5;
constexpr int kByteBits = 8;
constexpr int kOffsetBaseHex = 16;
constexpr int kRecoveredOffsetDigits = 12;
constexpr int kWindowsDevicePrefixLength = 4;
constexpr int kDriveLetterPathLength = 2;
constexpr int kDriveLetterSeparatorIndex = 1;
constexpr uint64_t kMinimumCandidateAdvance = 1;
constexpr qint64 kHashChunkBytes = 1024 * 1024;
// Forward-scan work budget (B8-14): a real image scans ~= its data size in total
// (sparse, terminated signatures); allow a small multiple of that plus a floor so a
// hostile image of repeated unterminated signatures fails closed to a partial result
// instead of pinning the CPU on O(n * max_candidate_bytes) work.
constexpr uint64_t kScanWorkBudgetFactor = 4;
constexpr uint64_t kScanWorkBudgetFloorBytes = 64ULL * 1024 * 1024;
constexpr char kPngSignature[kPngSignatureSize] = {'\x89', 'P', 'N', 'G', '\r', '\n', '\x1a', '\n'};

struct CandidateMatch {
    QString format;
    QString extension;
    uint64_t offset{0};
    uint64_t size{0};
};

struct RestoreContext {
    QFile* image{nullptr};
    QDir destination;
    bool overwrite_existing{false};
    FileRecoveryRestoreResult* result{nullptr};
};

uint32_t readBigEndianUInt32(const QByteArray& data, qsizetype offset) {
    uint32_t value = 0;
    for (int index = 0; index < kPngChunkLengthBytes; ++index) {
        value = (value << kByteBits) | static_cast<unsigned char>(data.at(offset + index));
    }
    return value;
}

bool hasBytesAt(const QByteArray& data, qsizetype offset, QByteArrayView bytes) {
    return offset >= 0 && offset + bytes.size() <= data.size() &&
           QByteArrayView(data).sliced(offset, bytes.size()) == bytes;
}

QByteArray candidateHash(const QByteArray& data, uint64_t offset, uint64_t size) {
    return QCryptographicHash::hash(QByteArrayView(data).sliced(static_cast<qsizetype>(offset),
                                                                static_cast<qsizetype>(size)),
                                    QCryptographicHash::Sha256);
}

QString candidateId(uint64_t offset, const QString& extension) {
    return QStringLiteral("recovered_%1.%2")
        .arg(QString::number(offset, kOffsetBaseHex)
                 .rightJustified(kRecoveredOffsetDigits, QLatin1Char('0')),
             extension);
}

std::optional<uint64_t> pngSizeAt(const QByteArray& data,
                                  qsizetype offset,
                                  uint64_t maxCandidateBytes,
                                  uint64_t* work) {
    if (!hasBytesAt(data, offset, QByteArrayView(kPngSignature, kPngSignatureSize))) {
        return std::nullopt;
    }
    qsizetype cursor = offset + kPngSignatureSize;
    while (cursor + kPngChunkHeaderSize <= data.size()) {
        ++(*work);  // count forward-scan work so the caller can bound it (B8-14)
        const uint32_t length = readBigEndianUInt32(data, cursor);
        const qsizetype chunkEnd = cursor + kPngChunkHeaderSize + length + kPngChunkCrcSize;
        if (chunkEnd < cursor || chunkEnd > data.size()) {
            return std::nullopt;
        }
        if (static_cast<uint64_t>(chunkEnd - offset) > maxCandidateBytes) {
            return std::nullopt;
        }
        const auto type = QByteArrayView(data).sliced(cursor + kPngChunkTypeOffset,
                                                      kPngChunkLengthBytes);
        if (type == QByteArrayView("IEND", kPngChunkLengthBytes)) {
            return static_cast<uint64_t>(chunkEnd - offset);
        }
        cursor = chunkEnd;
    }
    return std::nullopt;
}

std::optional<uint64_t> jpegSizeAt(const QByteArray& data,
                                   qsizetype offset,
                                   uint64_t maxCandidateBytes,
                                   uint64_t* work) {
    const QByteArrayView start("\xff\xd8\xff", kJpegSignatureSize);
    const QByteArrayView end("\xff\xd9", kJpegMarkerSize);
    if (!hasBytesAt(data, offset, start)) {
        return std::nullopt;
    }
    // Bound the forward search to maxCandidateBytes without signed overflow: compute the span in
    // uint64 and clamp it to the bytes actually available (offset <= data.size(), guaranteed by
    // hasBytesAt above), so offset + span never exceeds data.size() (F2).
    const qsizetype available = data.size() - offset;
    const qsizetype span = maxCandidateBytes >= static_cast<uint64_t>(available)
                               ? available
                               : static_cast<qsizetype>(maxCandidateBytes);
    const qsizetype limit = offset + span;
    for (qsizetype cursor = offset + start.size(); cursor + end.size() <= limit; ++cursor) {
        ++(*work);  // count forward-scan work so the caller can bound it (B8-14)
        if (hasBytesAt(data, cursor, end)) {
            return static_cast<uint64_t>(cursor + end.size() - offset);
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> pdfSizeAt(const QByteArray& data,
                                  qsizetype offset,
                                  uint64_t maxCandidateBytes,
                                  uint64_t* work) {
    const QByteArrayView start("%PDF-", kPdfSignatureSize);
    const QByteArrayView end("%%EOF", kPdfEofMarkerSize);
    if (!hasBytesAt(data, offset, start)) {
        return std::nullopt;
    }
    // Bound the forward search to maxCandidateBytes without signed overflow: compute the span in
    // uint64 and clamp it to the bytes actually available (offset <= data.size(), guaranteed by
    // hasBytesAt above), so offset + span never exceeds data.size() (F2).
    const qsizetype available = data.size() - offset;
    const qsizetype span = maxCandidateBytes >= static_cast<uint64_t>(available)
                               ? available
                               : static_cast<qsizetype>(maxCandidateBytes);
    const qsizetype limit = offset + span;
    for (qsizetype cursor = offset + start.size(); cursor + end.size() <= limit; ++cursor) {
        ++(*work);  // count forward-scan work so the caller can bound it (B8-14)
        if (hasBytesAt(data, cursor, end)) {
            return static_cast<uint64_t>(cursor + end.size() - offset);
        }
    }
    return std::nullopt;
}

std::optional<CandidateMatch> matchAt(const QByteArray& data,
                                      qsizetype offset,
                                      uint64_t maxCandidateBytes,
                                      uint64_t* work) {
    if (const auto size = pngSizeAt(data, offset, maxCandidateBytes, work)) {
        return CandidateMatch{QStringLiteral("PNG image"),
                              QStringLiteral("png"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    if (const auto size = jpegSizeAt(data, offset, maxCandidateBytes, work)) {
        return CandidateMatch{QStringLiteral("JPEG image"),
                              QStringLiteral("jpg"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    if (const auto size = pdfSizeAt(data, offset, maxCandidateBytes, work)) {
        return CandidateMatch{QStringLiteral("PDF document"),
                              QStringLiteral("pdf"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    return std::nullopt;
}

QString restoredFilePath(const QString& destinationDirectory,
                         const FileRecoveryCandidate& candidate) {
    // Confine the candidate id to a bare filename so a crafted id ("../evil", "a/b")
    // in a caller-supplied candidate cannot write outside the restore directory
    // (B8-15). QFileInfo::fileName drops any path components; "."/".."/empty are
    // rejected (empty return => the caller skips the candidate).
    const QString name = QFileInfo(candidate.id).fileName();
    if (name.isEmpty() || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return {};
    }
    return QDir(destinationDirectory).filePath(name);
}

bool isWindowsRawDevicePath(const QString& path) {
#ifdef Q_OS_WIN
    const QString sourcePath = QDir::toNativeSeparators(path);
    return sourcePath.startsWith(QStringLiteral("\\\\.\\")) ||
           sourcePath.startsWith(QStringLiteral("\\\\?\\"));
#else
    Q_UNUSED(path);
    return false;
#endif
}

QString windowsRawDeviceDriveRoot(const QString& path) {
#ifdef Q_OS_WIN
    QString sourcePath = QDir::toNativeSeparators(path);
    const bool devicePath = sourcePath.startsWith(QStringLiteral("\\\\.\\")) ||
                            sourcePath.startsWith(QStringLiteral("\\\\?\\"));
    if (!devicePath) {
        return {};
    }
    if (sourcePath.startsWith(QStringLiteral("\\\\.\\"))) {
        sourcePath = sourcePath.mid(kWindowsDevicePrefixLength);
    }
    if (sourcePath.startsWith(QStringLiteral("\\\\?\\"))) {
        sourcePath = sourcePath.mid(kWindowsDevicePrefixLength);
    }
    if (sourcePath.size() >= kDriveLetterPathLength &&
        sourcePath.at(kDriveLetterSeparatorIndex) == QLatin1Char(':')) {
        return QDir::toNativeSeparators(
            QStringLiteral("%1/").arg(sourcePath.left(kDriveLetterPathLength)));
    }
#else
    Q_UNUSED(path);
#endif
    return {};
}

bool destinationIsSeparate(const QFileInfo& imageInfo, const QDir& destination) {
    const QString destinationPath = QFileInfo(destination.absolutePath()).canonicalFilePath();
    if (destinationPath.isEmpty()) {
        return false;
    }

    const QString sourceRoot = windowsRawDeviceDriveRoot(imageInfo.filePath());
    if (!sourceRoot.isEmpty()) {
        return !QDir::toNativeSeparators(destinationPath)
                    .startsWith(sourceRoot, Qt::CaseInsensitive);
    }

    const QString imageDir = imageInfo.absoluteDir().canonicalPath();
    return !imageDir.isEmpty() && imageDir != destinationPath;
}

QByteArray readSequentialBytes(QFile* image, uint64_t offset, uint64_t size) {
    if (!image->seek(0)) {
        return {};
    }
    uint64_t skipped = 0;
    while (skipped < offset) {
        const qint64 chunkSize = static_cast<qint64>(
            std::min<uint64_t>(static_cast<uint64_t>(kHashChunkBytes), offset - skipped));
        const QByteArray chunk = image->read(chunkSize);
        if (chunk.isEmpty()) {
            return {};
        }
        skipped += static_cast<uint64_t>(chunk.size());
    }

    QByteArray bytes;
    // Reserve at most the per-candidate ceiling: a hostile candidate.size_bytes (narrowed from an
    // untrusted uint64) could otherwise ask QByteArray to preallocate an unsatisfiable buffer and
    // abort. Anything larger grows through append instead, still bounded by the source (F3).
    bytes.reserve(
        static_cast<qsizetype>(std::min<uint64_t>(size, kFileRecoveryDefaultMaxCandidateBytes)));
    while (static_cast<uint64_t>(bytes.size()) < size) {
        const qint64 chunkSize = static_cast<qint64>(std::min<uint64_t>(
            static_cast<uint64_t>(kHashChunkBytes), size - static_cast<uint64_t>(bytes.size())));
        const QByteArray chunk = image->read(chunkSize);
        if (chunk.isEmpty()) {
            return {};
        }
        bytes.append(chunk);
    }
    return bytes;
}

QByteArray readCandidateBytes(QFile* image, const FileRecoveryCandidate& candidate) {
    // Caller-supplied recovered_bytes may stand in for a source read ONLY when a hash pins them to
    // the carve (candidateBytesMatch then verifies it). Without a hash they are unverifiable,
    // caller-influenced bytes, so fall through and re-read from the read-only source rather than
    // writing them verbatim as "recovered" (F4).
    if (!candidate.sha256.isEmpty() &&
        static_cast<uint64_t>(candidate.recovered_bytes.size()) == candidate.size_bytes) {
        return candidate.recovered_bytes;
    }
    if (isWindowsRawDevicePath(image->fileName())) {
        // Read through the ALREADY-OPEN, validated handle -- the same handle the before/after
        // integrity hash covers -- instead of reopening the path by name, which a swapped
        // device or junction could redirect between validation, hashing, and this read (F5).
        return readSequentialBytes(image, candidate.offset_bytes, candidate.size_bytes);
    }
    if (image->seek(static_cast<qint64>(candidate.offset_bytes))) {
        const QByteArray bytes = image->read(static_cast<qint64>(candidate.size_bytes));
        if (static_cast<uint64_t>(bytes.size()) == candidate.size_bytes) {
            return bytes;
        }
    }
    return {};
}

std::optional<QDir> prepareRestoreDestination(const QFileInfo& imageInfo,
                                              const QString& destinationDirectory,
                                              QStringList* warnings) {
    if (destinationDirectory.trimmed().isEmpty()) {
        // A blank destination would make QDir("") resolve to the process working directory and
        // write recovered files there. Refuse an absent target rather than pick one (F6).
        warnings->append(QStringLiteral("No restore destination directory was given"));
        return std::nullopt;
    }
    QDir destination(destinationDirectory);
    if (!destination.exists() && !destination.mkpath(QStringLiteral("."))) {
        warnings->append(QStringLiteral("Could not create restore destination"));
        return std::nullopt;
    }
    if (!destinationIsSeparate(imageInfo, destination)) {
        warnings->append(QStringLiteral("Restore destination must be separate from source"));
        return std::nullopt;
    }
    return destination;
}

QByteArray hashOpenFile(QFile* file, uint64_t maxBytes = 0) {
    if (!file->seek(0)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    uint64_t bytesRead = 0;
    while (maxBytes > 0 || !file->atEnd()) {
        qint64 bytesToRead = kHashChunkBytes;
        if (maxBytes > 0) {
            if (bytesRead >= maxBytes) {
                break;
            }
            bytesToRead = static_cast<qint64>(
                std::min<uint64_t>(static_cast<uint64_t>(kHashChunkBytes), maxBytes - bytesRead));
        }
        const QByteArray chunk = file->read(bytesToRead);
        if (chunk.isEmpty() && file->error() != QFileDevice::NoError) {
            return {};
        }
        if (chunk.isEmpty()) {
            break;
        }
        bytesRead += static_cast<uint64_t>(chunk.size());
        hash.addData(chunk);
    }
    return hash.result();
}

bool skippedExistingRestoreFile(const QString& outputPath,
                                bool overwriteExisting,
                                QStringList* warnings) {
    if (!QFileInfo::exists(outputPath) || overwriteExisting) {
        return false;
    }
    warnings->append(QStringLiteral("Skipped existing restore file: %1").arg(outputPath));
    return true;
}

bool candidateBytesMatch(const QByteArray& bytes,
                         const FileRecoveryCandidate& candidate,
                         QStringList* warnings) {
    if (candidate.size_bytes == 0) {
        // A zero-byte candidate carves nothing to recover; refuse it rather than write an empty
        // file and report it restored (F4).
        warnings->append(QStringLiteral("Skipped zero-size candidate: %1").arg(candidate.id));
        return false;
    }
    if (static_cast<uint64_t>(bytes.size()) != candidate.size_bytes) {
        warnings->append(
            QStringLiteral("Skipped candidate with unreadable byte range: %1").arg(candidate.id));
        return false;
    }
    if (!candidate.sha256.isEmpty() &&
        QCryptographicHash::hash(bytes, QCryptographicHash::Sha256) != candidate.sha256) {
        warnings->append(
            QStringLiteral("Skipped candidate with hash mismatch: %1").arg(candidate.id));
        return false;
    }
    return true;
}

bool writeRecoveredFile(const QString& outputPath, const QByteArray& bytes, QStringList* warnings) {
    // Atomic write: QSaveFile stages to a temporary and only renames into place on
    // commit(), so a short write (disk full) or a flush failure never leaves a
    // truncated recovered file reported as a success. writeFully loops over partial
    // writes; commit() flushes+closes and fails closed on any I/O error (B8-13).
    QSaveFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly)) {
        warnings->append(QStringLiteral("Could not write recovered file: %1").arg(outputPath));
        return false;
    }
    if (!sak::writeFully(output, bytes)) {
        output.cancelWriting();
        warnings->append(
            QStringLiteral("Short write recovering file (disk full?): %1").arg(outputPath));
        return false;
    }
    if (!output.commit()) {
        warnings->append(
            QStringLiteral("Could not finalize recovered file (disk full?): %1").arg(outputPath));
        return false;
    }
    return true;
}

void restoreCandidate(const RestoreContext& context, const FileRecoveryCandidate& candidate) {
    const QString outputPath = restoredFilePath(context.destination.absolutePath(), candidate);
    if (outputPath.isEmpty()) {
        context.result->warnings.append(
            QStringLiteral("Skipped candidate with an unsafe name: %1").arg(candidate.id));
        return;
    }
    if (skippedExistingRestoreFile(
            outputPath, context.overwrite_existing, &context.result->warnings)) {
        return;
    }

    const QByteArray bytes = readCandidateBytes(context.image, candidate);
    if (!candidateBytesMatch(bytes, candidate, &context.result->warnings)) {
        return;
    }
    if (!writeRecoveredFile(outputPath, bytes, &context.result->warnings)) {
        return;
    }
    context.result->restored_paths.append(outputPath);
}

// Whether an integrity hash bounded by hash_cap_bytes over a source of
// source_size_bytes covered the WHOLE source, so a before/after match can be read
// as a whole-source (not merely prefix) guarantee. A cap of 0 means "no cap" ->
// whole. A known size no larger than the cap is fully covered. An unknown size
// (0, e.g. a raw device that reports no length) under a cap cannot be proven whole
// -- the hash only covered a bounded prefix (B8-23).
bool hashCoveredWholeSource(uint64_t hash_cap_bytes, uint64_t source_size_bytes) {
    // An uncapped hash covers the whole source only if the source has a KNOWN, non-zero size to
    // cover. A size of 0 -- an unknown-length raw device that reports no size, whose hash therefore
    // covered nothing provable -- cannot be read as a whole-source guarantee (F7).
    if (hash_cap_bytes == 0) {
        return source_size_bytes > 0;
    }
    return source_size_bytes > 0 && source_size_bytes <= hash_cap_bytes;
}

uint64_t scanByteLimit(uint64_t imageSize, const FileRecoveryScanOptions& options) {
    if (imageSize == 0 && isWindowsRawDevicePath(options.image_path)) {
        return options.max_scan_bytes;
    }
    return std::min(imageSize, options.max_scan_bytes);
}

QByteArray readScanData(QFile* image,
                        uint64_t imageSize,
                        const FileRecoveryScanOptions& options,
                        FileRecoveryScanResult* result) {
    const uint64_t scanBytes = scanByteLimit(imageSize, options);
    if (scanBytes < imageSize) {
        // The byte cap stopped the read before EOF, so any candidate list carved from it is a
        // partial view of the source. Flag it (like the candidate-limit and work-budget bounds)
        // so a caller that only checks scan_cancelled never reports a capped scan as complete.
        result->warnings.append(
            QStringLiteral("Scan limited to first %1 byte(s)").arg(QString::number(scanBytes)));
        result->scan_cancelled = true;
    }

    // Fill up to scanBytes over repeated reads: a single QFile::read can short-read a device, and
    // casting a huge scanBytes straight to qint64 could go negative. Loop in bounded chunks and
    // surface a genuine read error as a partial result rather than a clean-looking empty scan
    // (F2/F10/F11).
    QByteArray data;
    while (static_cast<uint64_t>(data.size()) < scanBytes) {
        const qint64 want =
            static_cast<qint64>(std::min<uint64_t>(static_cast<uint64_t>(kHashChunkBytes),
                                                   scanBytes - static_cast<uint64_t>(data.size())));
        const QByteArray chunk = image->read(want);
        if (chunk.isEmpty()) {
            if (image->error() != QFileDevice::NoError) {
                // A genuine read error truncated the scan; the carve is partial. Flag it so the
                // shortfall is never read as a complete, authoritative scan of the source.
                result->warnings.append(
                    QStringLiteral("Read error during scan; results are partial"));
                result->scan_cancelled = true;
            }
            break;
        }
        data.append(chunk);
    }
    result->bytes_read = static_cast<uint64_t>(std::max<qsizetype>(0, data.size()));
    if (scanBytes > 0 && data.isEmpty()) {
        result->warnings.append(QStringLiteral("No bytes read from recovery source"));
    }
    return data;
}

FileRecoveryCandidate scanCandidateFromMatch(const QByteArray& data,
                                             const CandidateMatch& match,
                                             bool captureCandidateBytes) {
    FileRecoveryCandidate candidate;
    candidate.id = candidateId(match.offset, match.extension);
    candidate.format = match.format;
    candidate.extension = match.extension;
    candidate.offset_bytes = match.offset;
    candidate.size_bytes = match.size;
    candidate.sha256 = candidateHash(data, match.offset, match.size);
    if (captureCandidateBytes) {
        candidate.recovered_bytes = QByteArrayView(data)
                                        .sliced(static_cast<qsizetype>(match.offset),
                                                static_cast<qsizetype>(match.size))
                                        .toByteArray();
    }
    return candidate;
}

void appendScanCandidates(const QByteArray& data,
                          const FileRecoveryScanOptions& options,
                          FileRecoveryScanResult* result,
                          const std::atomic<bool>* cancel) {
    // Total forward-scan work bound: matchAt scans forward up to max_candidate_bytes
    // per signature prefix, so a crafted image of repeated unterminated signatures is
    // O(n * max_candidate_bytes). A real image (sparse, terminated signatures) scans
    // ~= its data size in total; bound the cumulative forward-scan to a small multiple
    // of that so a hostile image fails closed to a partial result instead of pinning
    // the CPU -- independent of whether a cancel deadline was supplied (B8-14).
    const uint64_t work_budget = (static_cast<uint64_t>(data.size()) * kScanWorkBudgetFactor) +
                                 kScanWorkBudgetFloorBytes;
    uint64_t work = 0;
    for (qsizetype offset = 0;
         offset < data.size() && result->candidates.size() < options.max_candidates;
         ++offset) {
        // Poll the cancel flag once per start offset (a supplied deadline can still
        // interrupt), and also stop when the forward-scan work budget is exhausted.
        if (cancel != nullptr && cancel->load()) {
            result->scan_cancelled = true;
            result->warnings.append(
                QStringLiteral("Scan cancelled before completing (time limit reached)"));
            return;
        }
        if (work > work_budget) {
            result->scan_cancelled = true;
            result->warnings.append(
                QStringLiteral("Scan bounded before completing: excessive unterminated signatures "
                               "(possible hostile image); results are partial"));
            return;
        }
        const auto match = matchAt(data, offset, options.max_candidate_bytes, &work);
        if (!match) {
            continue;
        }
        result->candidates.append(
            scanCandidateFromMatch(data, *match, options.capture_candidate_bytes));
        offset += static_cast<qsizetype>(std::max<uint64_t>(match->size, kMinimumCandidateAdvance) -
                                         kMinimumCandidateAdvance);
    }
}

}  // namespace

FileRecoveryScanResult FileRecoveryEngine::scanOfflineImage(const FileRecoveryScanOptions& options,
                                                            const std::atomic<bool>* cancel) {
    FileRecoveryScanResult result;
    QFile image(options.image_path);
    if (!image.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("Could not open recovery source image read-only"));
        return result;
    }
    result.source_opened_read_only = true;

    const qint64 reportedSize = image.size();
    if (reportedSize < 0 && !isWindowsRawDevicePath(options.image_path)) {
        // A regular readable file that cannot report its size is an I/O error, not an empty scan:
        // fail closed with a warning instead of silently carving zero bytes (F10).
        result.warnings.append(QStringLiteral("Could not determine recovery source size"));
        return result;
    }
    const uint64_t imageSize = static_cast<uint64_t>(std::max<qint64>(0, reportedSize));
    const QByteArray data = readScanData(&image, imageSize, options, &result);
    appendScanCandidates(data, options, &result, cancel);
    if (result.candidates.size() >= options.max_candidates) {
        // The cap cut enumeration short -- more candidates may exist beyond it. Flag the result as
        // partial (like the work-budget bound) so callers do not report the scan as complete (F12).
        result.warnings.append(QStringLiteral("Candidate limit reached"));
        result.scan_cancelled = true;
    }
    return result;
}

FileRecoveryRestoreResult FileRecoveryEngine::restoreCandidates(
    const FileRecoveryRestoreOptions& options) {
    FileRecoveryRestoreResult result;
    const QFileInfo imageInfo(options.image_path);
    const auto destination =
        prepareRestoreDestination(imageInfo, options.destination_directory, &result.warnings);
    if (!destination) {
        return result;
    }

    QFile image(options.image_path);
    if (!image.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("Could not open recovery source image read-only"));
        return result;
    }
    result.source_opened_read_only = true;
    const QByteArray beforeHash = hashOpenFile(&image, options.source_hash_bytes);
    if (beforeHash.isEmpty()) {
        // Could not establish the pre-restore source baseline (a seek/read failure -- a readable
        // image, even an empty one, hashes to a non-empty digest). Without it the source-not-
        // mutated proof is impossible, so refuse to write anything rather than mutate the
        // destination first and discover the failure afterward (F13).
        result.warnings.append(
            QStringLiteral("Could not hash the source before restore; no files were restored"));
        return result;
    }

    const RestoreContext context{&image, *destination, options.overwrite_existing, &result};
    for (const auto& candidate : options.candidates) {
        restoreCandidate(context, candidate);
    }

    result.source_not_mutated = !beforeHash.isEmpty() &&
                                beforeHash == hashOpenFile(&image, options.source_hash_bytes);
    // Distinguish a whole-source guarantee from a prefix-only one: a capped hash
    // that stops short of the source proves only the hashed window is unchanged.
    result.source_hash_covered_whole = hashCoveredWholeSource(
        options.source_hash_bytes, static_cast<uint64_t>(std::max<qint64>(0, image.size())));
    return result;
}

}  // namespace sak
