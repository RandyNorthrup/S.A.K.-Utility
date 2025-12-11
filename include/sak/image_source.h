// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/streaming_decompressor.h"
#include <QObject>
#include <QString>
#include <QIODevice>
#include <memory>

namespace sak {

/**
 * @brief Supported image formats
 */
enum class ImageFormat {
    Unknown,
    ISO,      // ISO 9660 CD/DVD image
    IMG,      // Raw disk image
    WIC,      // Windows Imaging Format
    ZIP,      // ZIP archive containing image
    GZIP,     // GZIP compressed image
    BZIP2,    // BZIP2 compressed image
    XZ,       // XZ compressed image
    DMG,      // Apple Disk Image
    DSK       // Generic disk image
};

/**
 * @brief Metadata about an image file
 */
struct ImageMetadata {
    QString name;              // Filename
    QString path;              // Full path
    ImageFormat format;        // Detected format
    qint64 size;              // File size in bytes
    qint64 uncompressedSize;  // Size after decompression (0 if not compressed)
    bool isCompressed;         // True if compressed format
    QString checksum;          // SHA-512 hash (if calculated)
    QString compressionType;   // "gzip", "bzip2", "xz", etc.
    
    bool isValid() const { return !path.isEmpty() && size > 0 && format != ImageFormat::Unknown; }
};

} // namespace sak

/**
 * @brief Abstract base class for image sources
 * 
 * Provides a unified interface for reading disk images, with support for
 * compressed formats. Based on Etcher SDK's SourceDestination abstraction.
 * 
 * Implementations:
 * - FileImageSource: Regular file on disk
 * - CompressedImageSource: Compressed file with streaming decompression
 * - HTTPImageSource: Downloaded file (future)
 * 
 * Thread-Safety: Not thread-safe. Create separate instances per thread.
 */
class ImageSource : public QObject {
    Q_OBJECT

public:
    explicit ImageSource(QObject* parent = nullptr);
    ~ImageSource() override = default;

    /**
     * @brief Open the image source
     * @return true if opened successfully
     */
    virtual bool open() = 0;

    /**
     * @brief Close the image source
     */
    virtual void close() = 0;

    /**
     * @brief Check if source is open
     * @return true if open and ready to read
     */
    virtual bool isOpen() const = 0;

    /**
     * @brief Read data from the image
     * @param data Buffer to read into
     * @param maxSize Maximum bytes to read
     * @return Number of bytes actually read, -1 on error
     */
    virtual qint64 read(char* data, qint64 maxSize) = 0;

    /**
     * @brief Get total size of uncompressed image
     * @return Size in bytes
     */
    virtual qint64 size() const = 0;

    /**
     * @brief Get current read position
     * @return Position in bytes
     */
    virtual qint64 position() const = 0;
    
    /**
     * @brief Seek to position
     * @param pos Position to seek to
     * @return true on success
     */
    virtual bool seek(qint64 pos) = 0;

    /**
     * @brief Check if at end of data
     * @return true if no more data to read
     */
    virtual bool atEnd() const = 0;

    /**
     * @brief Get image metadata
     * @return Metadata structure
     */
    virtual sak::ImageMetadata metadata() const = 0;

    /**
     * @brief Calculate SHA-512 checksum
     * Reads entire image and calculates hash. Resets position after.
     * @return Hex-encoded checksum, empty on error
     */
    virtual QString calculateChecksum() = 0;

Q_SIGNALS:
    /**
     * @brief Emitted during checksum calculation
     * @param percentage Progress 0-100
     */
    void checksumProgress(int percentage);

    /**
     * @brief Emitted on read error
     * @param error Error message
     */
    void readError(const QString& error);
};

/**
 * @brief Image source from a regular file
 */
class FileImageSource : public ImageSource {
    Q_OBJECT

public:
    explicit FileImageSource(const QString& filePath, QObject* parent = nullptr);
    ~FileImageSource() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    qint64 read(char* data, qint64 maxSize) override;
    qint64 size() const override;
    qint64 position() const override;
    bool seek(qint64 pos) override;
    bool atEnd() const override;
    sak::ImageMetadata metadata() const override;
    QString calculateChecksum() override;

    /**
     * @brief Detect image format from file
     * @param filePath Path to image file
     * @return Detected format
     */
    static sak::ImageFormat detectFormat(const QString& filePath);

private:
    QString m_filePath;
    std::unique_ptr<QIODevice> m_device;
    sak::ImageMetadata m_metadata;
};

/**
 * @brief Image source with automatic decompression
 * 
 * Supports streaming decompression of gzip, bzip2, and xz formats.
 * Uses streaming decompression without temporary files.
 */
class CompressedImageSource : public ImageSource {
    Q_OBJECT

public:
    explicit CompressedImageSource(const QString& filePath, QObject* parent = nullptr);
    ~CompressedImageSource() override;

    bool open() override;
    void close() override;
    bool isOpen() const override;
    qint64 read(char* data, qint64 maxSize) override;
    qint64 size() const override;
    qint64 position() const override;
    bool seek(qint64 pos) override;
    bool atEnd() const override;
    sak::ImageMetadata metadata() const override;
    QString calculateChecksum() override;

    /**
     * @brief Check if file is compressed
     * @param filePath Path to check
     * @return true if supported compression format detected
     */
    static bool isCompressed(const QString& filePath);

Q_SIGNALS:
    /**
     * @brief Emitted during decompression
     * @param percentage Progress 0-100
     */
    void decompressionProgress(int percentage);

private:
    QString m_filePath;
    std::unique_ptr<sak::StreamingDecompressor> m_decompressor;
    sak::ImageMetadata m_metadata;
    qint64 m_totalDecompressed;
};
