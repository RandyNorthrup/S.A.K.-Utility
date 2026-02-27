// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/gzip_decompressor.h"
#include <cstring>

namespace sak {

GzipDecompressor::GzipDecompressor(QObject* parent)
    : StreamingDecompressor(parent)
{
    memset(&m_zstream, 0, sizeof(m_zstream));
}

GzipDecompressor::~GzipDecompressor()
{
    close();
}

bool GzipDecompressor::initStream()
{
    // windowBits = 15 (max) + 16 (gzip format detection)
    int ret = inflateInit2(&m_zstream, 15 + 16);
    if (ret != Z_OK) {
        m_lastError = QString("Failed to initialize zlib: %1")
            .arg(m_zstream.msg ? m_zstream.msg : "unknown error");
        return false;
    }
    m_zstream.avail_in = 0;
    m_zstream.next_in = nullptr;
    return true;
}

void GzipDecompressor::cleanupStream()
{
    inflateEnd(&m_zstream);
}

void GzipDecompressor::setInputFromBuffer(size_t bytes)
{
    m_zstream.next_in = m_inputBuffer;
    m_zstream.avail_in = static_cast<unsigned int>(bytes);
}

void GzipDecompressor::setOutput(char* data, size_t maxSize)
{
    m_zstream.next_out = reinterpret_cast<unsigned char*>(data);
    m_zstream.avail_out = static_cast<unsigned int>(maxSize);
}

size_t GzipDecompressor::outputRemaining() const
{
    return m_zstream.avail_out;
}

bool GzipDecompressor::inputEmpty() const
{
    return m_zstream.avail_in == 0;
}

GzipDecompressor::StepResult GzipDecompressor::decompressStep()
{
    int ret = inflate(&m_zstream, Z_NO_FLUSH);
    if (ret == Z_STREAM_END) return StepResult::stream_end;
    if (ret != Z_OK) {
        m_lastError = QString("Decompression error: %1 (%2)")
            .arg(m_zstream.msg ? m_zstream.msg : "unknown error")
            .arg(ret);
        return StepResult::error;
    }
    return StepResult::ok;
}

} // namespace sak
