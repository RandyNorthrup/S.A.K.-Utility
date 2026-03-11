// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/image_source.h"

#include "sak/decompressor_factory.h"
#include "sak/file_hash.h"
#include "sak/logger.h"
#include "sak/streaming_decompressor.h"

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
    close();
}

bool FileImageSource::open() {
    Q_ASSERT(m_device);
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
    Q_ASSERT(m_device);
    Q_ASSERT(data);
    if (!isOpen()) {
        return -1;
    }

    qint64 bytesRead = m_device->read(data, maxSize);
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
    Q_ASSERT(m_device);
    if (!isOpen()) {
        if (!open()) {
            return QString();
        }
    }

    // Save current position
    qint64 oldPos = position();
    m_device->seek(0);

    // Calculate SHA-512
    QCryptographicHash hash(QCryptographicHash::Sha512);

    const qint64 bufferSize = 64 * 1024 * 1024;  // 64MB
    QByteArray buffer(bufferSize, 0);
    qint64 totalRead = 0;

    while (!atEnd()) {
        qint64 bytesRead = read(buffer.data(), bufferSize);
        if (bytesRead < 0) {
            sak::logError("Error reading file for checksum");
            return QString();
        }

        hash.addData(buffer.data(), bytesRead);
        totalRead += bytesRead;

        int percentage = static_cast<int>((totalRead * 100) / size());
        Q_EMIT checksumProgress(percentage);
    }

    // Restore position
    m_device->seek(oldPos);

    QString checksum = hash.result().toHex();
    m_metadata.checksum = checksum;

    sak::logInfo(QString("Calculated checksum: %1").arg(checksum).toStdString());
    return checksum;
}

sak::ImageFormat FileImageSource::detectFormat(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    QString ext = QFileInfo(filePath).suffix().toLower();

    struct FormatEntry {
        const char* ext;
        sak::ImageFormat format;
    };
    static constexpr FormatEntry kFormats[] = {
        {"iso", sak::ImageFormat::ISO},
        {"img", sak::ImageFormat::IMG},
        {"wic", sak::ImageFormat::WIC},
        {"zip", sak::ImageFormat::ZIP},
        {"gz", sak::ImageFormat::GZIP},
        {"bz2", sak::ImageFormat::BZIP2},
        {"xz", sak::ImageFormat::XZ},
        {"dmg", sak::ImageFormat::DMG},
        {"dsk", sak::ImageFormat::DSK},
    };
    const auto it =
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
        {".gz", sak::ImageFormat::GZIP},
        {".bz2", sak::ImageFormat::BZIP2},
        {".xz", sak::ImageFormat::XZ},
    };
    QString fullExt = QFileInfo(filePath).completeSuffix().toLower();
    const auto cit =
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
    QString ext = QFileInfo(filePath).suffix().toLower();
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
    // so this is safe — but we call the qualified name explicitly.
    CompressedImageSource::close();
}

bool CompressedImageSource::open() {
    Q_ASSERT(m_decompressor);
    if (m_decompressor) {
        sak::logWarning("CompressedImageSource already open");
        return true;
    }

    // Create decompressor using factory
    m_decompressor = sak::DecompressorFactory::create(m_filePath);
    if (!m_decompressor) {
        QString error = QString("Unsupported or undetected compression format: %1").arg(m_filePath);
        sak::logError(error.toStdString());
        Q_EMIT readError(error);
        return false;
    }

    // Open the decompressor
    if (!m_decompressor->open(m_filePath)) {
        QString error = QString("Failed to open compressed file: %1").arg(m_filePath);
        sak::logError(error.toStdString());
        Q_EMIT readError(error);
        m_decompressor.reset();
        return false;
    }

    // Connect progress signals
    connect(m_decompressor.get(),
            &sak::StreamingDecompressor::progressUpdated,
            this,
            [this](qint64 compressedBytes, qint64 decompressedBytes) {
                Q_UNUSED(decompressedBytes);

                // Calculate percentage based on compressed file size
                if (m_metadata.size > 0) {
                    int percentage = static_cast<int>((compressedBytes * 100) / m_metadata.size);
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
    Q_ASSERT(m_decompressor);
    Q_ASSERT(data);
    if (!isOpen()) {
        sak::logError("Cannot read from closed CompressedImageSource");
        return -1;
    }

    qint64 bytesRead = m_decompressor->read(data, maxSize);
    if (bytesRead > 0) {
        m_totalDecompressed += bytesRead;
    }

    return bytesRead;
}

qint64 CompressedImageSource::size() const {
    return m_metadata.uncompressedSize;
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
    qint64 savedPos = m_totalDecompressed;
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
        qint64 bytesRead = read(buffer.data(), bufferSize);
        if (bytesRead < 0) {
            sak::logError("Error reading data during checksum calculation");
            return QString();
        }
        if (bytesRead > 0) {
            hash.addData(buffer.data(), bytesRead);
        }
    }

    // Close and reopen to reset
    close();
    open();

    // Cannot restore position for compressed streams, user must re-read from start
    sak::logWarning("Checksum calculation reset decompression stream to beginning");

    return QString::fromLatin1(hash.result().toHex());
}

bool CompressedImageSource::isCompressed(const QString& filePath) {
    QString ext = QFileInfo(filePath).suffix().toLower();
    return (ext == "gz" || ext == "bz2" || ext == "xz" || ext == "zip");
}
