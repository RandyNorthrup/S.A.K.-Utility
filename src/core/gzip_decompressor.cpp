// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/gzip_decompressor.h"

#include <cstring>

namespace sak {

namespace {
constexpr int kZlibMaxWindowBits = 15;
constexpr int kZlibGzipDetectionWindowBits = 16;
}  // namespace

GzipDecompressor::GzipDecompressor(QObject* parent) : StreamingDecompressor(parent) {
    memset(&m_zstream, 0, sizeof(m_zstream));
}

GzipDecompressor::~GzipDecompressor() {
    close();
}

bool GzipDecompressor::initStream() {
    const int ret = inflateInit2(&m_zstream, kZlibMaxWindowBits + kZlibGzipDetectionWindowBits);
    if (ret != Z_OK) {
        m_lastError = QString("Failed to initialize zlib: %1")
                          .arg((m_zstream.msg != nullptr) ? m_zstream.msg : "zlib gave no message");
        return false;
    }
    m_zstream.avail_in = 0;
    m_zstream.next_in = nullptr;
    return true;
}

void GzipDecompressor::cleanupStream() {
    inflateEnd(&m_zstream);
}

void GzipDecompressor::setInputFromBuffer(size_t bytes) {
    m_zstream.next_in = m_inputBuffer;
    m_zstream.avail_in = static_cast<unsigned int>(bytes);
}

void GzipDecompressor::setOutput(char* data, size_t maxSize) {
    m_zstream.next_out = reinterpret_cast<unsigned char*>(data);
    m_zstream.avail_out = static_cast<unsigned int>(maxSize);
}

size_t GzipDecompressor::outputRemaining() const {
    return m_zstream.avail_out;
}

bool GzipDecompressor::inputEmpty() const {
    return m_zstream.avail_in == 0;
}

GzipDecompressor::StepResult GzipDecompressor::decompressStep() {
    const int ret = inflate(&m_zstream, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) {
        return StepResult::stream_end;
    }
    if (ret != Z_OK) {
        m_lastError = QString("Decompression error: %1 (%2)")
                          .arg((m_zstream.msg != nullptr) ? m_zstream.msg : "zlib gave no message")
                          .arg(ret);
        return StepResult::error;
    }
    return StepResult::ok;
}

bool GzipDecompressor::resetStreamForNextMember() {
    // inflateReset restarts the inflate state for a new gzip member without freeing
    // the allocated window, and leaves next_in/avail_in/next_out/avail_out untouched
    // so the bytes after the previous member feed the next one (B8-12).
    const int ret = inflateReset(&m_zstream);
    if (ret != Z_OK) {
        m_lastError = QString("Failed to reset zlib for the next member: %1")
                          .arg((m_zstream.msg != nullptr) ? m_zstream.msg : "zlib gave no message");
        return false;
    }
    return true;
}

}  // namespace sak
