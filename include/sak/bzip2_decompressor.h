// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/streaming_decompressor.h"
#include <QFile>
#include <bzlib.h>

namespace sak {

/**
 * @brief Bzip2 decompressor using libbz2
 * 
 * Handles .bz2 compressed files with streaming decompression.
 * Uses libbz2 library for actual decompression.
 * 
 * Features:
 * - Streaming decompression (no temp files)
 * - Progress tracking
 * - Memory-efficient processing
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 */
class Bzip2Decompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit Bzip2Decompressor(QObject* parent = nullptr);
    ~Bzip2Decompressor() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;
    qint64 read(char* data, qint64 maxSize) override;
    bool atEnd() const override;
    qint64 compressedBytesRead() const override;
    qint64 decompressedBytesProduced() const override;
    qint64 uncompressedSize() const override;
    QString formatName() const override { return "bzip2"; }

private:
    /**
     * @brief Initialize bzip2 stream
     * @return true if successful
     */
    bool initBzip2Stream();

    /**
     * @brief Read more compressed data from file
     * @return true if data was read
     */
    bool fillInputBuffer();

    QFile m_file;                      // Input file
    bz_stream m_bzstream;              // bzip2 stream state
    bool m_bzipInitialized;            // bzip2 initialized flag
    bool m_eof;                        // End of file flag
    
    static constexpr int CHUNK_SIZE = 128 * 1024;  // 128KB input buffer
    char m_inputBuffer[CHUNK_SIZE];                // Input buffer
    
    qint64 m_compressedBytesRead;      // Total compressed bytes read
    qint64 m_decompressedBytesProduced; // Total decompressed bytes produced
};

} // namespace sak
