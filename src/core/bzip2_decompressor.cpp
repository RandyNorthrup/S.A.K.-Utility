// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/bzip2_decompressor.h"
#include "sak/logger.h"
#include <cstring>

namespace sak {

Bzip2Decompressor::Bzip2Decompressor(QObject* parent)
    : StreamingDecompressor(parent)
    , m_bzipInitialized(false)
    , m_eof(false)
    , m_compressedBytesRead(0)
    , m_decompressedBytesProduced(0)
{
    memset(&m_bzstream, 0, sizeof(m_bzstream));
}

Bzip2Decompressor::~Bzip2Decompressor() {
    close();
}

bool Bzip2Decompressor::open(const QString& filePath) {
    close();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_lastError = QString("Failed to open file: %1").arg(m_file.errorString());
        return false;
    }

    if (!initBzip2Stream()) {
        m_file.close();
        return false;
    }

    m_compressedBytesRead = 0;
    m_decompressedBytesProduced = 0;
    m_eof = false;

    sak::logInfo(QString("Opened bzip2 file: %1").arg(filePath).toStdString());
    return true;
}

void Bzip2Decompressor::close() {
    if (m_bzipInitialized) {
        BZ2_bzDecompressEnd(&m_bzstream);
        m_bzipInitialized = false;
    }

    if (m_file.isOpen()) {
        m_file.close();
    }

    m_eof = false;
}

bool Bzip2Decompressor::isOpen() const {
    return m_file.isOpen() && m_bzipInitialized;
}

qint64 Bzip2Decompressor::read(char* data, qint64 maxSize) {
    if (!isOpen()) {
        m_lastError = "Decompressor not open";
        return -1;
    }

    if (m_eof) {
        return 0;
    }

    m_bzstream.next_out = data;
    m_bzstream.avail_out = static_cast<unsigned int>(maxSize);

    while (m_bzstream.avail_out > 0 && !m_eof) {
        // Refill input buffer if needed
        if (m_bzstream.avail_in == 0) {
            if (!fillInputBuffer()) {
                if (m_file.atEnd()) {
                    m_eof = true;
                    break;
                }
                return -1;
            }
        }

        // Decompress
        int ret = BZ2_bzDecompress(&m_bzstream);

        if (ret == BZ_STREAM_END) {
            m_eof = true;
            break;
        } else if (ret != BZ_OK) {
            m_lastError = QString("Decompression error: bzip2 error code %1").arg(ret);
            sak::logError(m_lastError.toStdString());
            return -1;
        }
    }

    qint64 bytesProduced = maxSize - m_bzstream.avail_out;
    m_decompressedBytesProduced += bytesProduced;

    // Emit progress periodically (every 1MB)
    if (m_decompressedBytesProduced % (1024 * 1024) < bytesProduced) {
        Q_EMIT progressUpdated(m_compressedBytesRead, m_decompressedBytesProduced);
    }

    return bytesProduced;
}

bool Bzip2Decompressor::atEnd() const {
    return m_eof;
}

qint64 Bzip2Decompressor::compressedBytesRead() const {
    return m_compressedBytesRead;
}

qint64 Bzip2Decompressor::decompressedBytesProduced() const {
    return m_decompressedBytesProduced;
}

qint64 Bzip2Decompressor::uncompressedSize() const {
    // Bzip2 doesn't store uncompressed size in the format
    return -1;
}

bool Bzip2Decompressor::initBzip2Stream() {
    // verbosity = 0 (quiet), small = 0 (use normal memory)
    int ret = BZ2_bzDecompressInit(&m_bzstream, 0, 0);
    
    if (ret != BZ_OK) {
        m_lastError = QString("Failed to initialize bzip2: error code %1").arg(ret);
        sak::logError(m_lastError.toStdString());
        return false;
    }

    m_bzipInitialized = true;
    m_bzstream.avail_in = 0;
    m_bzstream.next_in = nullptr;

    return true;
}

bool Bzip2Decompressor::fillInputBuffer() {
    qint64 bytesRead = m_file.read(m_inputBuffer, CHUNK_SIZE);
    
    if (bytesRead < 0) {
        m_lastError = QString("File read error: %1").arg(m_file.errorString());
        sak::logError(m_lastError.toStdString());
        return false;
    }

    if (bytesRead == 0) {
        return false;  // End of file
    }

    m_bzstream.next_in = m_inputBuffer;
    m_bzstream.avail_in = static_cast<unsigned int>(bytesRead);
    m_compressedBytesRead += bytesRead;

    return true;
}

} // namespace sak
