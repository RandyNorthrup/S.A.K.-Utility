// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/gzip_decompressor.h"
#include "sak/logger.h"
#include <cstring>

namespace sak {

GzipDecompressor::GzipDecompressor(QObject* parent)
    : StreamingDecompressor(parent)
    , m_zlibInitialized(false)
    , m_eof(false)
    , m_compressedBytesRead(0)
    , m_decompressedBytesProduced(0)
{
    memset(&m_zstream, 0, sizeof(m_zstream));
}

GzipDecompressor::~GzipDecompressor() {
    close();
}

bool GzipDecompressor::open(const QString& filePath) {
    close();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_lastError = QString("Failed to open file: %1").arg(m_file.errorString());
        return false;
    }

    if (!initZlibStream()) {
        m_file.close();
        return false;
    }

    m_compressedBytesRead = 0;
    m_decompressedBytesProduced = 0;
    m_eof = false;

    sak::log_info(QString("Opened gzip file: %1").arg(filePath).toStdString());
    return true;
}

void GzipDecompressor::close() {
    if (m_zlibInitialized) {
        inflateEnd(&m_zstream);
        m_zlibInitialized = false;
    }

    if (m_file.isOpen()) {
        m_file.close();
    }

    m_eof = false;
}

bool GzipDecompressor::isOpen() const {
    return m_file.isOpen() && m_zlibInitialized;
}

qint64 GzipDecompressor::read(char* data, qint64 maxSize) {
    if (!isOpen()) {
        m_lastError = "Decompressor not open";
        return -1;
    }

    if (m_eof) {
        return 0;
    }

    m_zstream.next_out = reinterpret_cast<unsigned char*>(data);
    m_zstream.avail_out = static_cast<unsigned int>(maxSize);

    while (m_zstream.avail_out > 0 && !m_eof) {
        // Refill input buffer if needed
        if (m_zstream.avail_in == 0) {
            if (!fillInputBuffer()) {
                if (m_file.atEnd()) {
                    m_eof = true;
                    break;
                }
                return -1;
            }
        }

        // Decompress
        int ret = inflate(&m_zstream, Z_NO_FLUSH);

        if (ret == Z_STREAM_END) {
            m_eof = true;
            break;
        } else if (ret != Z_OK) {
            m_lastError = QString("Decompression error: %1 (%2)")
                .arg(m_zstream.msg ? m_zstream.msg : "unknown error")
                .arg(ret);
            sak::log_error(m_lastError.toStdString());
            return -1;
        }
    }

    qint64 bytesProduced = maxSize - m_zstream.avail_out;
    m_decompressedBytesProduced += bytesProduced;

    // Emit progress periodically (every 1MB)
    if (m_decompressedBytesProduced % (1024 * 1024) < bytesProduced) {
        Q_EMIT progressUpdated(m_compressedBytesRead, m_decompressedBytesProduced);
    }

    return bytesProduced;
}

bool GzipDecompressor::atEnd() const {
    return m_eof;
}

qint64 GzipDecompressor::compressedBytesRead() const {
    return m_compressedBytesRead;
}

qint64 GzipDecompressor::decompressedBytesProduced() const {
    return m_decompressedBytesProduced;
}

qint64 GzipDecompressor::uncompressedSize() const {
    // Gzip format stores uncompressed size in last 4 bytes (modulo 2^32)
    // This would require seeking to end, which we don't support in streaming mode
    return -1;
}

bool GzipDecompressor::initZlibStream() {
    // windowBits = 15 (max) + 16 (gzip format detection)
    // This allows automatic detection of gzip vs raw deflate
    int ret = inflateInit2(&m_zstream, 15 + 16);
    
    if (ret != Z_OK) {
        m_lastError = QString("Failed to initialize zlib: %1")
            .arg(m_zstream.msg ? m_zstream.msg : "unknown error");
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    m_zlibInitialized = true;
    m_zstream.avail_in = 0;
    m_zstream.next_in = nullptr;

    return true;
}

bool GzipDecompressor::fillInputBuffer() {
    qint64 bytesRead = m_file.read(reinterpret_cast<char*>(m_inputBuffer), CHUNK_SIZE);
    
    if (bytesRead < 0) {
        m_lastError = QString("File read error: %1").arg(m_file.errorString());
        sak::log_error(m_lastError.toStdString());
        return false;
    }

    if (bytesRead == 0) {
        return false;  // End of file
    }

    m_zstream.next_in = m_inputBuffer;
    m_zstream.avail_in = static_cast<unsigned int>(bytesRead);
    m_compressedBytesRead += bytesRead;

    return true;
}

} // namespace sak
