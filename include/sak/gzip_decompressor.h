// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/streaming_decompressor.h"
#include <QFile>
#include <zlib.h>

namespace sak {

/**
 * @brief Gzip decompressor using zlib
 * 
 * Handles .gz compressed files with streaming decompression.
 * Uses zlib library for actual decompression.
 * 
 * Features:
 * - Streaming decompression (no temp files)
 * - Supports both gzip and raw deflate formats
 * - Automatic format detection
 * - Progress tracking
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 */
class GzipDecompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit GzipDecompressor(QObject* parent = nullptr);
    ~GzipDecompressor() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;
    qint64 read(char* data, qint64 maxSize) override;
    bool atEnd() const override;
    qint64 compressedBytesRead() const override;
    qint64 decompressedBytesProduced() const override;
    qint64 uncompressedSize() const override;
    QString formatName() const override { return "gzip"; }

private:
    /**
     * @brief Initialize zlib stream
     * @return true if successful
     */
    bool initZlibStream();

    /**
     * @brief Read more compressed data from file
     * @return true if data was read
     */
    bool fillInputBuffer();

    QFile m_file;                      // Input file
    z_stream m_zstream;                // zlib stream state
    bool m_zlibInitialized;            // zlib initialized flag
    bool m_eof;                        // End of file flag
    
    static constexpr int CHUNK_SIZE = 128 * 1024;  // 128KB input buffer
    unsigned char m_inputBuffer[CHUNK_SIZE];       // Input buffer
    
    qint64 m_compressedBytesRead;      // Total compressed bytes read
    qint64 m_decompressedBytesProduced; // Total decompressed bytes produced
};

} // namespace sak
