// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/streaming_decompressor.h"
#include <QFile>
#include <lzma.h>

namespace sak {

/**
 * @brief XZ decompressor using liblzma
 * 
 * Handles .xz compressed files with streaming decompression.
 * Uses liblzma library for actual decompression.
 * 
 * Features:
 * - Streaming decompression (no temp files)
 * - Progress tracking
 * - Memory-efficient processing
 * - Supports XZ and LZMA formats
 * 
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 */
class XzDecompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit XzDecompressor(QObject* parent = nullptr);
    ~XzDecompressor() override;

    bool open(const QString& filePath) override;
    void close() override;
    bool isOpen() const override;
    qint64 read(char* data, qint64 maxSize) override;
    bool atEnd() const override;
    qint64 compressedBytesRead() const override;
    qint64 decompressedBytesProduced() const override;
    qint64 uncompressedSize() const override;
    QString formatName() const override { return "xz"; }

private:
    /**
     * @brief Initialize lzma stream
     * @return true if successful
     */
    bool initLzmaStream();

    /**
     * @brief Read more compressed data from file
     * @return true if data was read
     */
    bool fillInputBuffer();

    QFile m_file;                      // Input file
    lzma_stream m_lzmaStream;          // lzma stream state
    bool m_lzmaInitialized;            // lzma initialized flag
    bool m_eof;                        // End of file flag
    
    static constexpr int CHUNK_SIZE = 128 * 1024;  // 128KB input buffer
    uint8_t m_inputBuffer[CHUNK_SIZE];             // Input buffer
    
    qint64 m_compressedBytesRead;      // Total compressed bytes read
    qint64 m_decompressedBytesProduced; // Total decompressed bytes produced
};

} // namespace sak
