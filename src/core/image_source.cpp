// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/image_source.h"

#include "sak/decompressor_factory.h"
#include "sak/file_hash.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/streaming_decompressor.h"

#include <QByteArrayView>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>

#include <algorithm>

// ============================================================================
// ImageSource Base Class
// ============================================================================

ImageSource::ImageSource(QObject* parent) : QObject(parent) {}

// ============================================================================
// FileImageSource Implementation
// ============================================================================

FileImageSource::FileImageSource(const QString& filePath, QObject* parent)
    : ImageSource(parent), m_filePath(filePath), m_device(nullptr) {
    m_metadata.name = QFileInfo(filePath).fileName();
    m_metadata.path = filePath;
    m_metadata.format = detectFormat(filePath);
    m_metadata.size = QFileInfo(filePath).size();
    m_metadata.isCompressed = false;
    m_metadata.uncompressedSize = 0;
}

FileImageSource::~FileImageSource() {
    FileImageSource::close();
}

bool FileImageSource::open() {
    // m_device is null until created below on the first open(); the null-guard is the real logic.
    if (m_device && m_device->isOpen()) {
        return true;
    }

    auto file = std::make_unique<QFile>(m_filePath);
    if (!file->open(QIODevice::ReadOnly)) {
        sak::logError(QString("Failed to open file: %1").arg(m_filePath).toStdString());
        Q_EMIT readError(QString("Failed to open file: %1").arg(file->errorString()));
        return false;
    }

    m_device = std::move(file);
    // Bind the reported size to the handle we actually opened, not to the pre-open stat taken in
    // the constructor: a reparse-point or file swap between construction and open() would leave
    // size() reporting file A while every read comes from file B. The open handle is the
    // authority the capacity gate must trust.
    const qint64 openedSize = m_device->size();
    if (openedSize >= 0) {
        m_metadata.size = openedSize;
    }
    sak::logInfo(QString("Opened image: %1 (%2 bytes)")
                     .arg(m_metadata.name)
                     .arg(m_metadata.size)
                     .toStdString());

    return true;
}

void FileImageSource::close() {
    if (m_device) {
        m_device->close();
        m_device.reset();
    }
}

bool FileImageSource::isOpen() const {
    return m_device && m_device->isOpen();
}

qint64 FileImageSource::read(char* data, qint64 maxSize) {
    // read() is public on the ImageSource interface, so the buffer arrives from outside this
    // class: reject a null one with the documented -1 error instead of handing it to
    // QIODevice::read. m_device is covered by the isOpen() check below.
    if (data == nullptr) {
        sak::logError("FileImageSource::read called with a null buffer");
        return -1;
    }
    if (!isOpen()) {
        return -1;
    }

    const qint64 bytesRead = m_device->read(data, maxSize);
    if (bytesRead < 0) {
        sak::logError(QString("Read error: %1")
                          .arg(static_cast<QFile*>(m_device.get())->errorString())
                          .toStdString());
        Q_EMIT readError("Read error");
    }

    return bytesRead;
}

qint64 FileImageSource::size() const {
    return m_metadata.size;
}

qint64 FileImageSource::position() const {
    if (!m_device) {
        return 0;
    }
    return m_device->pos();
}

bool FileImageSource::seek(qint64 pos) {
    if (!m_device) {
        return false;
    }
    return m_device->seek(pos);
}

bool FileImageSource::atEnd() const {
    if (!m_device) {
        return true;
    }
    return m_device->atEnd();
}

sak::ImageMetadata FileImageSource::metadata() const {
    return m_metadata;
}

QString FileImageSource::calculateChecksum() {
    if (!isOpen()) {
        if (!open()) {
            return QString();
        }
    }

    const qint64 oldPos = position();
    // A seek that failed leaves the cursor wherever it was, so the hash would cover a SUFFIX of
    // the image and be returned as the checksum of the whole thing -- a wrong digest is worse
    // than no digest, because the caller compares it and believes the answer.
    if (!m_device->seek(0)) {
        sak::logError("Could not rewind the image to checksum it");
        return QString();
    }

    // Calculate SHA-512
    QCryptographicHash hash(QCryptographicHash::Sha512);

    const qint64 bufferSize = 64 * 1024 * 1024;  // 64MB
    QByteArray buffer(bufferSize, 0);
    qint64 totalRead = 0;
    // size() is metadata; the device is the thing being read. A stale or zero size would divide
    // by zero below, and progress is a report about the read, so it is derived from what the
    // device says it holds and simply omitted when that is not known.
    const qint64 totalBytes = m_device->size();

    while (!atEnd()) {
        const qint64 bytesRead = read(buffer.data(), bufferSize);
        if (bytesRead < 0) {
            sak::logError("Error reading file for checksum");
            // Restore before returning: this function is a read-only observation, and leaving
            // the cursor parked mid-image silently changes what every later read of this source
            // returns. The failure is reported by the empty checksum, not by moving the file.
            static_cast<void>(m_device->seek(oldPos));
            return QString();
        }

        hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(bytesRead)));
        totalRead += bytesRead;

        if (totalBytes > 0) {
            const int percentage = static_cast<int>((totalRead * sak::kPercentMax) / totalBytes);
            Q_EMIT checksumProgress(percentage);
        }
    }

    // The image must not change length under us mid-hash: if the bytes we read do not match what
    // the handle reported before the loop, a concurrent truncation or growth raced the read and
    // the digest would certify a prefix (or a short read) as the whole image. A wrong digest the
    // caller compares and trusts is worse than none, so fail closed.
    if (totalBytes >= 0 && totalRead != totalBytes) {
        sak::logError("Image length changed during checksum; refusing to certify a partial read");
        static_cast<void>(m_device->seek(oldPos));
        return QString();
    }

    // Restore position
    static_cast<void>(m_device->seek(oldPos));

    QString checksum = hash.result().toHex();
    m_metadata.checksum = checksum;

    sak::logInfo(QString("Calculated checksum: %1").arg(checksum).toStdString());
    return checksum;
}

sak::ImageFormat FileImageSource::detectFormat(const QString& filePath) {
    // A total function over any path: an empty or unrecognised extension is
    // ImageFormat::Unknown, which callers must handle anyway. No assert.
    QString ext = QFileInfo(filePath).suffix().toLower();

    struct FormatEntry {
        const char* ext;
        sak::ImageFormat format;
    };
    static constexpr FormatEntry kFormats[] = {
        {.ext = "iso", .format = sak::ImageFormat::ISO},
        {.ext = "img", .format = sak::ImageFormat::IMG},
        {.ext = "wic", .format = sak::ImageFormat::WIC},
        {.ext = "zip", .format = sak::ImageFormat::ZIP},
        {.ext = "gz", .format = sak::ImageFormat::GZIP},
        {.ext = "bz2", .format = sak::ImageFormat::BZIP2},
        {.ext = "xz", .format = sak::ImageFormat::XZ},
        {.ext = "dmg", .format = sak::ImageFormat::DMG},
        {.ext = "dsk", .format = sak::ImageFormat::DSK},
    };
    const auto* const it =
        std::find_if(std::begin(kFormats), std::end(kFormats), [&ext](const auto& entry) {
            return ext == QLatin1String(entry.ext);
        });
    if (it != std::end(kFormats)) {
        return it->format;
    }

    // Check for double extensions like .img.gz
    struct CompoundEntry {
        const char* suffix;
        sak::ImageFormat format;
    };
    static constexpr CompoundEntry kCompound[] = {
        {.suffix = ".gz", .format = sak::ImageFormat::GZIP},
        {.suffix = ".bz2", .format = sak::ImageFormat::BZIP2},
        {.suffix = ".xz", .format = sak::ImageFormat::XZ},
    };
    QString fullExt = QFileInfo(filePath).completeSuffix().toLower();
    const auto* const cit =
        std::find_if(std::begin(kCompound), std::end(kCompound), [&fullExt](const auto& entry) {
            return fullExt.endsWith(QLatin1String(entry.suffix));
        });
    if (cit != std::end(kCompound)) {
        return cit->format;
    }

    return sak::ImageFormat::Unknown;
}

// ============================================================================
// CompressedImageSource Implementation
// ============================================================================

CompressedImageSource::CompressedImageSource(const QString& filePath, QObject* parent)
    : ImageSource(parent), m_filePath(filePath), m_decompressor(nullptr), m_totalDecompressed(0) {
    m_metadata.name = QFileInfo(filePath).fileName();
    m_metadata.path = filePath;
    m_metadata.format = FileImageSource::detectFormat(filePath);
    m_metadata.size = QFileInfo(filePath).size();
    m_metadata.isCompressed = true;

    // Determine compression type
    const QString ext = QFileInfo(filePath).suffix().toLower();
    if (ext == "gz") {
        m_metadata.compressionType = "gzip";
    } else if (ext == "bz2") {
        m_metadata.compressionType = "bzip2";
    } else if (ext == "xz") {
        m_metadata.compressionType = "xz";
    } else if (ext == "zip") {
        m_metadata.compressionType = "zip";
    }
}

CompressedImageSource::~CompressedImageSource() {
    // Call close() directly to avoid virtual dispatch warning.
    // At destruction time, the vtable is CompressedImageSource's own,
    // so this is safe -- but we call the qualified name explicitly.
    CompressedImageSource::close();
}

bool CompressedImageSource::open() {
    // m_decompressor is null until created below on the first open(); the null-guard is the logic.
    if (m_decompressor) {
        sak::logWarning("CompressedImageSource already open");
        return true;
    }

    // Create decompressor using factory
    m_decompressor = sak::DecompressorFactory::create(m_filePath);
    if (!m_decompressor) {
        const QString error =
            QString("Unsupported or undetected compression format: %1").arg(m_filePath);
        sak::logError(error.toStdString());
        Q_EMIT readError(error);
        return false;
    }

    // Open the decompressor
    if (!m_decompressor->open(m_filePath)) {
        const QString error = QString("Failed to open compressed file: %1 (%2)")
                                  .arg(m_filePath, m_decompressor->lastError());
        sak::logError(error.toStdString());
        Q_EMIT readError(error);
        m_decompressor.reset();
        return false;
    }

    // Record the true uncompressed size when the decompressor can report it. It
    // feeds the flash capacity gate; a compressed stream must NEVER be gated on
    // its on-disk (compressed) size. When it stays unknown, size() returns -1 so
    // the gate fails closed instead of approving an oversized decompressed image.
    const qint64 uncompressed = m_decompressor->uncompressedSize();
    if (uncompressed >= 0) {
        m_metadata.uncompressedSize = uncompressed;
    }

    // Connect progress signals
    connect(m_decompressor.get(),
            &sak::StreamingDecompressor::progressUpdated,
            this,
            [this](qint64 compressedBytes, qint64 decompressedBytes) {
                Q_UNUSED(decompressedBytes);

                // Calculate percentage based on compressed file size
                if (m_metadata.size > 0) {
                    const int percentage =
                        static_cast<int>((compressedBytes * sak::kPercentMax) / m_metadata.size);
                    Q_EMIT decompressionProgress(percentage);
                }
            });

    sak::logInfo(QString("Opened compressed image: %1 (format: %2)")
                     .arg(m_filePath)
                     .arg(m_decompressor->formatName())
                     .toStdString());

    return true;
}

void CompressedImageSource::close() {
    if (m_decompressor) {
        m_decompressor->close();
        m_decompressor.reset();
        m_totalDecompressed = 0;
    }
}

bool CompressedImageSource::isOpen() const {
    return m_decompressor && m_decompressor->isOpen();
}

qint64 CompressedImageSource::read(char* data, qint64 maxSize) {
    // Public interface entry point: reject a null buffer with the documented -1 error.
    // m_decompressor is covered by the isOpen() check below.
    if (data == nullptr) {
        sak::logError("CompressedImageSource::read called with a null buffer");
        return -1;
    }
    // A negative length is not a short read to tolerate: it is a malformed request that would
    // reach the backend as an unbounded/garbage width. Reject it with the documented -1.
    if (maxSize < 0) {
        sak::logError("CompressedImageSource::read called with a negative length");
        return -1;
    }
    if (!isOpen()) {
        sak::logError("Cannot read from closed CompressedImageSource");
        return -1;
    }

    const qint64 bytesRead = m_decompressor->read(data, maxSize);
    if (bytesRead < 0) {
        // Surface the decompressor's real failure instead of a bare -1: malformed compressed
        // input must reach the caller as the actual error, not a silent short read.
        const QString err = m_decompressor->lastError();
        sak::logError(QString("Decompressor read error: %1").arg(err).toStdString());
        Q_EMIT readError(err.isEmpty() ? QStringLiteral("Decompressor read error") : err);
    } else if (bytesRead > 0) {
        m_totalDecompressed += bytesRead;
    }

    return bytesRead;
}

qint64 CompressedImageSource::size() const {
    // Report the true UNCOMPRESSED size, never the compressed on-disk size: the
    // flash capacity gate compares this against the target device, and gating a
    // stream that decompresses to 50 GB on its 100 MB compressed size would let
    // it clobber the whole device before failing at end-of-media. When the size
    // is unknown (the decompressor could not determine it), return -1 so the gate
    // fails closed rather than approving an unbounded write.
    return m_metadata.uncompressedSize > 0 ? m_metadata.uncompressedSize : -1;
}

qint64 CompressedImageSource::position() const {
    return m_totalDecompressed;
}

bool CompressedImageSource::seek(qint64 pos) {
    // Seeking in compressed streams is not supported
    // Would require decompressing from beginning to reach position
    Q_UNUSED(pos);
    sak::logWarning("Seek not supported for compressed streams");
    return false;
}

bool CompressedImageSource::atEnd() const {
    if (!m_decompressor) {
        return true;
    }
    return m_decompressor->atEnd();
}

sak::ImageMetadata CompressedImageSource::metadata() const {
    return m_metadata;
}

QString CompressedImageSource::calculateChecksum() {
    if (!isOpen()) {
        sak::logError("Cannot calculate checksum on closed CompressedImageSource");
        return QString();
    }

    // Save current position
    const qint64 savedPos = m_totalDecompressed;
    // Note: Position restoration not possible for compressed streams
    // Must reopen to reset decompression state
    Q_UNUSED(savedPos);

    // Close and reopen to reset decompression stream
    close();
    if (!open()) {
        sak::logError("Failed to reopen CompressedImageSource for checksum calculation");
        return QString();
    }

    // Calculate checksum while reading
    QCryptographicHash hash(QCryptographicHash::Sha512);
    constexpr qint64 bufferSize = 1024 * 1024;  // 1MB buffer
    std::vector<char> buffer(bufferSize);

    while (!atEnd()) {
        const qint64 bytesRead = read(buffer.data(), bufferSize);
        if (bytesRead < 0) {
            sak::logError(QString("Error reading data during checksum calculation: %1")
                              .arg(m_decompressor ? m_decompressor->lastError() : QString())
                              .toStdString());
            return QString();
        }
        if (bytesRead > 0) {
            hash.addData(QByteArrayView(buffer.data(), static_cast<qsizetype>(bytesRead)));
        }
    }

    // Close and reopen to reset the stream to the beginning so the caller can re-read from the
    // start. The digest above is already complete and valid; a reopen failure here does not
    // change it, but must not pass silently -- a later read would otherwise fail with no clue.
    close();
    if (!open()) {
        sak::logError("Failed to reopen CompressedImageSource after checksum calculation");
    }

    // Cannot restore position for compressed streams, user must re-read from start
    sak::logWarning("Checksum calculation reset decompression stream to beginning");

    return QString::fromLatin1(hash.result().toHex());
}

bool CompressedImageSource::isCompressed(const QString& filePath) {
    // Single source of truth: a file is "compressed" for flashing purposes ONLY
    // when DecompressorFactory can actually stream-decompress it (gz/gzip, bz2/
    // bzip2, xz/lzma; NOT zip -- a multi-member archive it cannot produce a raw
    // image from). The old hand-rolled {gz,bz2,xz,zip} set both missed .gzip/
    // .bzip2/.lzma (written raw) and wrongly accepted .zip.
    if (filePath.isEmpty()) {
        return false;
    }
    return sak::DecompressorFactory::isCompressed(filePath);
}
