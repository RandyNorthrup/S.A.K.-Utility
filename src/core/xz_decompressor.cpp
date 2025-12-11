// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/xz_decompressor.h"
#include "sak/logger.h"
#include <cstring>

namespace sak {

XzDecompressor::XzDecompressor(QObject* parent)
    : StreamingDecompressor(parent)
    , m_lzmaInitialized(false)
    , m_eof(false)
    , m_compressedBytesRead(0)
    , m_decompressedBytesProduced(0)
{
    m_lzmaStream = LZMA_STREAM_INIT;
}

XzDecompressor::~XzDecompressor() {
    close();
}

bool XzDecompressor::open(const QString& filePath) {
    close();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_lastError = QString("Failed to open file: %1").arg(m_file.errorString());
        return false;
    }

    if (!initLzmaStream()) {
        m_file.close();
        return false;
    }

    m_compressedBytesRead = 0;
    m_decompressedBytesProduced = 0;
    m_eof = false;

    sak::log_info(QString("Opened xz file: %1").arg(filePath).toStdString());
    return true;
}

void XzDecompressor::close() {
    if (m_lzmaInitialized) {
        lzma_end(&m_lzmaStream);
        m_lzmaInitialized = false;
    }

    if (m_file.isOpen()) {
        m_file.close();
    }

    m_eof = false;
}

bool XzDecompressor::isOpen() const {
    return m_file.isOpen() && m_lzmaInitialized;
}

qint64 XzDecompressor::read(char* data, qint64 maxSize) {
    if (!isOpen()) {
        m_lastError = "Decompressor not open";
        return -1;
    }

    if (m_eof) {
        return 0;
    }

    m_lzmaStream.next_out = reinterpret_cast<uint8_t*>(data);
    m_lzmaStream.avail_out = static_cast<size_t>(maxSize);

    while (m_lzmaStream.avail_out > 0 && !m_eof) {
        // Refill input buffer if needed
        if (m_lzmaStream.avail_in == 0) {
            if (!fillInputBuffer()) {
                if (m_file.atEnd()) {
                    m_eof = true;
                    break;
                }
                return -1;
            }
        }

        // Decompress
        lzma_ret ret = lzma_code(&m_lzmaStream, LZMA_RUN);

        if (ret == LZMA_STREAM_END) {
            m_eof = true;
            break;
        } else if (ret != LZMA_OK) {
            m_lastError = QString("Decompression error: lzma error code %1").arg(static_cast<int>(ret));
            sak::log_error(m_lastError.toStdString());
            return -1;
        }
    }

    qint64 bytesProduced = maxSize - m_lzmaStream.avail_out;
    m_decompressedBytesProduced += bytesProduced;

    // Emit progress periodically (every 1MB)
    if (m_decompressedBytesProduced % (1024 * 1024) < bytesProduced) {
        Q_EMIT progressUpdated(m_compressedBytesRead, m_decompressedBytesProduced);
    }

    return bytesProduced;
}

bool XzDecompressor::atEnd() const {
    return m_eof;
}

qint64 XzDecompressor::compressedBytesRead() const {
    return m_compressedBytesRead;
}

qint64 XzDecompressor::decompressedBytesProduced() const {
    return m_decompressedBytesProduced;
}

qint64 XzDecompressor::uncompressedSize() const {
    // XZ format can store uncompressed size, but it's optional
    // and would require parsing the file structure
    return -1;
}

bool XzDecompressor::initLzmaStream() {
    // Initialize with automatic format detection (XZ or LZMA)
    // UINT64_MAX = no memory limit
    uint64_t memlimit = UINT64_MAX;
    uint32_t flags = 0;
    
    lzma_ret ret = lzma_stream_decoder(&m_lzmaStream, memlimit, flags);
    
    if (ret != LZMA_OK) {
        m_lastError = QString("Failed to initialize lzma: error code %1").arg(static_cast<int>(ret));
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    m_lzmaInitialized = true;
    m_lzmaStream.avail_in = 0;
    m_lzmaStream.next_in = nullptr;

    return true;
}

bool XzDecompressor::fillInputBuffer() {
    qint64 bytesRead = m_file.read(reinterpret_cast<char*>(m_inputBuffer), CHUNK_SIZE);
    
    if (bytesRead < 0) {
        m_lastError = QString("File read error: %1").arg(m_file.errorString());
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    if (bytesRead == 0) {
        return false;  // End of file
    }

    m_lzmaStream.next_in = m_inputBuffer;
    m_lzmaStream.avail_in = static_cast<size_t>(bytesRead);
    m_compressedBytesRead += bytesRead;

    return true;
}

} // namespace sak
