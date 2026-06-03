// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_recovery_engine.cpp
/// @brief Offline file-level recovery scanner for Partition Manager Data Recovery.

#include "sak/file_recovery_engine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

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
                                  uint64_t maxCandidateBytes) {
    if (!hasBytesAt(data, offset, QByteArrayView(kPngSignature, kPngSignatureSize))) {
        return std::nullopt;
    }
    qsizetype cursor = offset + kPngSignatureSize;
    while (cursor + kPngChunkHeaderSize <= data.size()) {
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
                                   uint64_t maxCandidateBytes) {
    const QByteArrayView start("\xff\xd8\xff", kJpegSignatureSize);
    const QByteArrayView end("\xff\xd9", kJpegMarkerSize);
    if (!hasBytesAt(data, offset, start)) {
        return std::nullopt;
    }
    const qsizetype limit = std::min<qsizetype>(data.size(),
                                                offset + static_cast<qsizetype>(maxCandidateBytes));
    for (qsizetype cursor = offset + start.size(); cursor + end.size() <= limit; ++cursor) {
        if (hasBytesAt(data, cursor, end)) {
            return static_cast<uint64_t>(cursor + end.size() - offset);
        }
    }
    return std::nullopt;
}

std::optional<uint64_t> pdfSizeAt(const QByteArray& data,
                                  qsizetype offset,
                                  uint64_t maxCandidateBytes) {
    const QByteArrayView start("%PDF-", kPdfSignatureSize);
    const QByteArrayView end("%%EOF", kPdfEofMarkerSize);
    if (!hasBytesAt(data, offset, start)) {
        return std::nullopt;
    }
    const qsizetype limit = std::min<qsizetype>(data.size(),
                                                offset + static_cast<qsizetype>(maxCandidateBytes));
    for (qsizetype cursor = offset + start.size(); cursor + end.size() <= limit; ++cursor) {
        if (hasBytesAt(data, cursor, end)) {
            return static_cast<uint64_t>(cursor + end.size() - offset);
        }
    }
    return std::nullopt;
}

std::optional<CandidateMatch> matchAt(const QByteArray& data,
                                      qsizetype offset,
                                      uint64_t maxCandidateBytes) {
    if (const auto size = pngSizeAt(data, offset, maxCandidateBytes)) {
        return CandidateMatch{QStringLiteral("PNG image"),
                              QStringLiteral("png"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    if (const auto size = jpegSizeAt(data, offset, maxCandidateBytes)) {
        return CandidateMatch{QStringLiteral("JPEG image"),
                              QStringLiteral("jpg"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    if (const auto size = pdfSizeAt(data, offset, maxCandidateBytes)) {
        return CandidateMatch{QStringLiteral("PDF document"),
                              QStringLiteral("pdf"),
                              static_cast<uint64_t>(offset),
                              *size};
    }
    return std::nullopt;
}

QString restoredFilePath(const QString& destinationDirectory,
                         const FileRecoveryCandidate& candidate) {
    return QDir(destinationDirectory).filePath(candidate.id);
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
    bytes.reserve(static_cast<qsizetype>(
        std::min<uint64_t>(size, static_cast<uint64_t>(std::numeric_limits<qsizetype>::max()))));
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
    if (static_cast<uint64_t>(candidate.recovered_bytes.size()) == candidate.size_bytes) {
        return candidate.recovered_bytes;
    }
    if (isWindowsRawDevicePath(image->fileName())) {
        QFile rawImage(image->fileName());
        if (rawImage.open(QIODevice::ReadOnly)) {
            return readSequentialBytes(&rawImage, candidate.offset_bytes, candidate.size_bytes);
        }
        return {};
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
    QFile output(outputPath);
    if (!output.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        warnings->append(QStringLiteral("Could not write recovered file: %1").arg(outputPath));
        return false;
    }
    output.write(bytes);
    output.close();
    return true;
}

void restoreCandidate(const RestoreContext& context, const FileRecoveryCandidate& candidate) {
    const QString outputPath = restoredFilePath(context.destination.absolutePath(), candidate);
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
        result->warnings.append(
            QStringLiteral("Scan limited to first %1 byte(s)").arg(QString::number(scanBytes)));
    }

    const QByteArray data = image->read(static_cast<qint64>(scanBytes));
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
                          FileRecoveryScanResult* result) {
    for (qsizetype offset = 0;
         offset < data.size() && result->candidates.size() < options.max_candidates;
         ++offset) {
        const auto match = matchAt(data, offset, options.max_candidate_bytes);
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

FileRecoveryScanResult FileRecoveryEngine::scanOfflineImage(
    const FileRecoveryScanOptions& options) {
    FileRecoveryScanResult result;
    QFile image(options.image_path);
    if (!image.open(QIODevice::ReadOnly)) {
        result.warnings.append(QStringLiteral("Could not open recovery source image read-only"));
        return result;
    }
    result.source_opened_read_only = true;

    const uint64_t imageSize = static_cast<uint64_t>(std::max<qint64>(0, image.size()));
    const QByteArray data = readScanData(&image, imageSize, options, &result);
    appendScanCandidates(data, options, &result);
    if (result.candidates.size() >= options.max_candidates) {
        result.warnings.append(QStringLiteral("Candidate limit reached"));
    }
    return result;
}

FileRecoveryRestoreResult FileRecoveryEngine::restoreCandidates(
    const FileRecoveryRestoreOptions& options) {
    FileRecoveryRestoreResult result;
    QFileInfo imageInfo(options.image_path);
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

    const RestoreContext context{&image, *destination, options.overwrite_existing, &result};
    for (const auto& candidate : options.candidates) {
        restoreCandidate(context, candidate);
    }

    result.source_not_mutated = !beforeHash.isEmpty() &&
                                beforeHash == hashOpenFile(&image, options.source_hash_bytes);
    return result;
}

}  // namespace sak
