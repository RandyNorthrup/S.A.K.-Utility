// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/xz_decompressor.h"

namespace sak {

XzDecompressor::XzDecompressor(QObject* parent)
    : StreamingDecompressor(parent), m_lzmaStream(LZMA_STREAM_INIT) {}

XzDecompressor::~XzDecompressor() {
    close();
}

bool XzDecompressor::initStream() {
    // Automatic format detection (XZ or LZMA), no memory limit
    lzma_ret ret = lzma_stream_decoder(&m_lzmaStream, UINT64_MAX, 0);
    if (ret != LZMA_OK) {
        m_lastError =
            QString("Failed to initialize lzma: error code %1").arg(static_cast<int>(ret));
        return false;
    }
    m_lzmaStream.avail_in = 0;
    m_lzmaStream.next_in = nullptr;
    return true;
}

void XzDecompressor::cleanupStream() {
    lzma_end(&m_lzmaStream);
}

void XzDecompressor::setInputFromBuffer(size_t bytes) {
    m_lzmaStream.next_in = m_inputBuffer;
    m_lzmaStream.avail_in = bytes;
}

void XzDecompressor::setOutput(char* data, size_t maxSize) {
    m_lzmaStream.next_out = reinterpret_cast<uint8_t*>(data);
    m_lzmaStream.avail_out = maxSize;
}

size_t XzDecompressor::outputRemaining() const {
    return m_lzmaStream.avail_out;
}

bool XzDecompressor::inputEmpty() const {
    return m_lzmaStream.avail_in == 0;
}

XzDecompressor::StepResult XzDecompressor::decompressStep() {
    lzma_ret ret = lzma_code(&m_lzmaStream, LZMA_RUN);
    if (ret == LZMA_STREAM_END) {
        return StepResult::stream_end;
    }
    if (ret != LZMA_OK) {
        m_lastError = QString("Decompression error: lzma error code %1").arg(static_cast<int>(ret));
        return StepResult::error;
    }
    return StepResult::ok;
}

}  // namespace sak
