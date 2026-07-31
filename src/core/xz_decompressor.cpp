// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/xz_decompressor.h"

namespace sak {

namespace {
// Decoder memory ceiling. lzma_auto_decoder allocates the dictionary declared in
// the stream/block header BEFORE decoding any data, so an unlimited (UINT64_MAX)
// limit lets a crafted header (dictionary up to xz's 1.5 GiB maximum) allocate
// gigabytes from a tiny file. 512 MiB comfortably covers every standard preset
// (xz -9/-9e use a 64 MiB dictionary) plus large custom dictionaries, while a
// hostile oversized dictionary trips LZMA_MEMLIMIT_ERROR and fails closed (B8-11).
constexpr uint64_t kXzDecoderMemLimitBytes = 512ULL * 1024 * 1024;
}  // namespace

XzDecompressor::XzDecompressor(QObject* parent)
    : StreamingDecompressor(parent), m_lzmaStream(LZMA_STREAM_INIT) {}

XzDecompressor::~XzDecompressor() {
    close();
}

bool XzDecompressor::initStream() {
    // Automatic format detection (XZ or legacy .lzma-alone), bounded decoder
    // memory (kXzDecoderMemLimitBytes) so a crafted oversized-dictionary header
    // fails closed with LZMA_MEMLIMIT_ERROR instead of allocating gigabytes.
    // lzma_stream_decoder only decodes .xz containers and fails with
    // LZMA_FORMAT_ERROR on a valid legacy .lzma file; lzma_auto_decoder handles
    // both, matching what the decompressor factory already advertises.
    lzma_ret ret = lzma_auto_decoder(&m_lzmaStream, kXzDecoderMemLimitBytes, 0);
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
