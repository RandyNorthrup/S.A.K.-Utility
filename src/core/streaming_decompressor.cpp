// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/streaming_decompressor.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include <QtGlobal>

namespace sak {

StreamingDecompressor::StreamingDecompressor(QObject* parent) : QObject(parent) {}

StreamingDecompressor::~StreamingDecompressor() {
    close();
}

bool StreamingDecompressor::open(const QString& filePath) {
    Q_ASSERT(!filePath.isEmpty());
    close();

    m_file.setFileName(filePath);
    if (!m_file.open(QIODevice::ReadOnly)) {
        m_lastError = QString("Failed to open file: %1").arg(m_file.errorString());
        return false;
    }

    if (!initStream()) {
        m_file.close();
        return false;
    }

    m_compressedBytesRead = 0;
    m_decompressedBytesProduced = 0;
    m_eof = false;
    m_initialized = true;

    logInfo("Opened {} file: {}", formatName().toStdString(), filePath.toStdString());
    return true;
}

void StreamingDecompressor::close() {
    if (m_initialized) {
        cleanupStream();
        m_initialized = false;
    }
    if (m_file.isOpen()) {
        m_file.close();
    }
    m_eof = false;
}

bool StreamingDecompressor::isOpen() const {
    return m_file.isOpen() && m_initialized;
}

qint64 StreamingDecompressor::read(char* data, qint64 maxSize) {
    Q_ASSERT(data);
    Q_ASSERT(maxSize >= 0);
    if (!isOpen()) {
        m_lastError = "Decompressor not open";
        return -1;
    }
    if (m_eof) {
        return 0;
    }

    setOutput(data, static_cast<size_t>(maxSize));

    while (outputRemaining() > 0 && !m_eof) {
        if (!tryRefillInput()) {
            return -1;
        }
        if (m_eof) {
            break;
        }

        StepResult result = decompressStep();
        if (result == StepResult::stream_end) {
            m_eof = true;
            break;
        }
        if (result == StepResult::error) {
            logError(m_lastError.toStdString());
            return -1;
        }
    }

    qint64 bytesProduced = maxSize - static_cast<qint64>(outputRemaining());
    m_decompressedBytesProduced += bytesProduced;

    // Emit progress every ~1 MB
    if (m_decompressedBytesProduced % sak::kBytesPerMB < bytesProduced) {
        Q_EMIT progressUpdated(m_compressedBytesRead, m_decompressedBytesProduced);
    }

    return bytesProduced;
}

bool StreamingDecompressor::atEnd() const {
    return m_eof;
}

qint64 StreamingDecompressor::compressedBytesRead() const {
    return m_compressedBytesRead;
}

qint64 StreamingDecompressor::decompressedBytesProduced() const {
    return m_decompressedBytesProduced;
}

bool StreamingDecompressor::tryRefillInput() {
    if (!inputEmpty()) {
        return true;
    }
    if (fillInputBuffer()) {
        return true;
    }
    if (m_file.atEnd()) {
        m_eof = true;
        return true;
    }
    return false;
}

bool StreamingDecompressor::fillInputBuffer() {
    qint64 bytesRead = m_file.read(reinterpret_cast<char*>(m_inputBuffer), CHUNK_SIZE);
    if (bytesRead < 0) {
        m_lastError = QString("File read error: %1").arg(m_file.errorString());
        logError(m_lastError.toStdString());
        return false;
    }
    if (bytesRead == 0) {
        return false;
    }
    setInputFromBuffer(static_cast<size_t>(bytesRead));
    m_compressedBytesRead += bytesRead;
    return true;
}

}  // namespace sak
