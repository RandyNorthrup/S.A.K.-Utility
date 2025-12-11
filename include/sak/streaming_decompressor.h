// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QString>
#include <QObject>
#include <cstdint>

namespace sak {

/**
 * @brief Base class for streaming decompression
 * 
 * Provides a uniform interface for different compression formats.
 * Based on Etcher SDK CompressedSource pattern with streaming support.
 * 
 * Features:
 * - Stream-based decompression (no temp files)
 * - Progress tracking
 * - Memory-efficient processing
 * - Format-agnostic interface
 * 
 * Supported Formats (via subclasses):
 * - Gzip (.gz) - via zlib
 * - Bzip2 (.bz2) - via libbz2
 * - XZ (.xz) - via liblzma
 * - ZIP (.zip) - via QuaZip
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 * 
 * Example:
 * @code
 * auto decompressor = DecompressorFactory::create("image.gz");
 * decompressor->open();
 * 
 * char buffer[4096];
 * while (!decompressor->atEnd()) {
 *     qint64 bytes = decompressor->read(buffer, sizeof(buffer));
 *     // Process decompressed data
 * }
 * 
 * decompressor->close();
 * @endcode
 */
class StreamingDecompressor : public QObject {
    Q_OBJECT

public:
    explicit StreamingDecompressor(QObject* parent = nullptr);
    virtual ~StreamingDecompressor() = default;

    /**
     * @brief Open the compressed file for reading
     * @param filePath Path to compressed file
     * @return true if successful, false on error
     */
    virtual bool open(const QString& filePath) = 0;

    /**
     * @brief Close the decompressor
     */
    virtual void close() = 0;

    /**
     * @brief Check if decompressor is open
     * @return true if open and ready to read
     */
    virtual bool isOpen() const = 0;

    /**
     * @brief Read decompressed data into buffer
     * @param data Buffer to read into
     * @param maxSize Maximum bytes to read
     * @return Number of decompressed bytes read, -1 on error, 0 at end
     */
    virtual qint64 read(char* data, qint64 maxSize) = 0;

    /**
     * @brief Check if at end of decompressed data
     * @return true if no more data available
     */
    virtual bool atEnd() const = 0;

    /**
     * @brief Get total compressed bytes read so far
     * @return Bytes read from compressed file
     */
    virtual qint64 compressedBytesRead() const = 0;

    /**
     * @brief Get total decompressed bytes produced so far
     * @return Bytes of decompressed data produced
     */
    virtual qint64 decompressedBytesProduced() const = 0;

    /**
     * @brief Get uncompressed size (if known)
     * @return Uncompressed size in bytes, -1 if unknown
     */
    virtual qint64 uncompressedSize() const = 0;

    /**
     * @brief Get last error message
     * @return Human-readable error description
     */
    virtual QString lastError() const { return m_lastError; }

    /**
     * @brief Get compression format name
     * @return Format name (e.g., "gzip", "bzip2", "xz")
     */
    virtual QString formatName() const = 0;

Q_SIGNALS:
    /**
     * @brief Emitted periodically during decompression
     * @param compressedBytes Bytes read from compressed file
     * @param decompressedBytes Bytes of decompressed data produced
     */
    void progressUpdated(qint64 compressedBytes, qint64 decompressedBytes);

protected:
    QString m_lastError;  // Last error message
};

} // namespace sak
