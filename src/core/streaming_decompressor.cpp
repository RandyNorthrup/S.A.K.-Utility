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
    // An empty path makes QFile::open() below fail, which is the rejection.
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
    m_input_exhausted = false;
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
    m_input_exhausted = false;
}

bool StreamingDecompressor::isOpen() const {
    return m_file.isOpen() && m_initialized;
}

qint64 StreamingDecompressor::read(char* data, qint64 maxSize) {
    // setOutput() hands the buffer straight to the decoder, so a null buffer or a
    // negative size (which would widen to an enormous size_t) has to be refused
    // here; there is no later check between this point and the decoder writing.
    if (data == nullptr || maxSize < 0) {
        m_lastError = QStringLiteral("Invalid read buffer");
        logError(m_lastError.toStdString());
        return -1;
    }
    if (!isOpen()) {
        m_lastError = "Decompressor not open";
        return -1;
    }
    if (m_eof) {
        return 0;
    }

    setOutput(data, static_cast<size_t>(maxSize));

    if (!pumpDecoder()) {
        return -1;
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

StreamingDecompressor::MemberBoundary StreamingDecompressor::onMemberEnd() {
    // A compressed member ended. Concatenated formats put more members after it
    // (pigz emits multi-member gzip; cat'd .xz/.bz2 streams), so only a true end of
    // input is EOF. When input remains (buffered leftover or more on disk), reset the
    // decoder and keep decoding so members are never dropped and trailing bytes are
    // not silently accepted -- if what follows is not a valid next member, the
    // continued decode fails closed (B8-12).
    if (inputEmpty() && m_file.atEnd()) {
        m_eof = true;
        return MemberBoundary::finished;
    }
    if (!resetStreamForNextMember()) {
        logError(m_lastError.toStdString());
        return MemberBoundary::failed;
    }
    return MemberBoundary::continued;
}

bool StreamingDecompressor::isTruncatedStream(size_t output_before) {
    // The decoder reported ok but made no progress and there is no more compressed
    // input to give it: the stream ended before the decoder reached end-of-stream,
    // i.e. it is truncated. Fail closed rather than returning the partial output as a
    // clean EOF (which would, e.g., flash a truncated disk image with no warning).
    if (!(m_input_exhausted && inputEmpty() && outputRemaining() == output_before)) {
        return false;
    }
    m_lastError = QStringLiteral(
        "Compressed stream is truncated: the input ended before the decoder "
        "reached end-of-stream");
    logError(m_lastError.toStdString());
    return true;
}

bool StreamingDecompressor::pumpDecoder() {
    while (outputRemaining() > 0 && !m_eof) {
        if (!tryRefillInput()) {
            return false;
        }

        const size_t output_before = outputRemaining();
        const StepResult result = decompressStep();
        if (result == StepResult::stream_end) {
            const MemberBoundary boundary = onMemberEnd();
            if (boundary == MemberBoundary::failed) {
                return false;
            }
            if (boundary == MemberBoundary::finished) {
                return true;
            }
            continue;
        }
        if (result == StepResult::error) {
            logError(m_lastError.toStdString());
            return false;
        }
        if (isTruncatedStream(output_before)) {
            return false;
        }
    }
    return true;
}

bool StreamingDecompressor::tryRefillInput() {
    if (!inputEmpty()) {
        return true;
    }
    if (fillInputBuffer()) {
        return true;
    }
    if (m_file.atEnd()) {
        // No more compressed bytes on disk. This is NOT end-of-stream: only the
        // decoder confirming stream_end sets m_eof. Marking the input exhausted
        // lets read() detect a truncated stream (input gone, decoder not done).
        m_input_exhausted = true;
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
