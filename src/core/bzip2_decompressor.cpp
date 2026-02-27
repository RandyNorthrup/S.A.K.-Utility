// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/bzip2_decompressor.h"
#include <cstring>

namespace sak {

Bzip2Decompressor::Bzip2Decompressor(QObject* parent)
    : StreamingDecompressor(parent)
{
    memset(&m_bzstream, 0, sizeof(m_bzstream));
}

Bzip2Decompressor::~Bzip2Decompressor()
{
    close();
}

bool Bzip2Decompressor::initStream()
{
    // verbosity = 0 (quiet), small = 0 (use normal memory)
    int ret = BZ2_bzDecompressInit(&m_bzstream, 0, 0);
    if (ret != BZ_OK) {
        m_lastError = QString("Failed to initialize bzip2: error code %1").arg(ret);
        return false;
    }
    m_bzstream.avail_in = 0;
    m_bzstream.next_in = nullptr;
    return true;
}

void Bzip2Decompressor::cleanupStream()
{
    BZ2_bzDecompressEnd(&m_bzstream);
}

void Bzip2Decompressor::setInputFromBuffer(size_t bytes)
{
    m_bzstream.next_in = reinterpret_cast<char*>(m_inputBuffer);
    m_bzstream.avail_in = static_cast<unsigned int>(bytes);
}

void Bzip2Decompressor::setOutput(char* data, size_t maxSize)
{
    m_bzstream.next_out = data;
    m_bzstream.avail_out = static_cast<unsigned int>(maxSize);
}

size_t Bzip2Decompressor::outputRemaining() const
{
    return m_bzstream.avail_out;
}

bool Bzip2Decompressor::inputEmpty() const
{
    return m_bzstream.avail_in == 0;
}

Bzip2Decompressor::StepResult Bzip2Decompressor::decompressStep()
{
    int ret = BZ2_bzDecompress(&m_bzstream);
    if (ret == BZ_STREAM_END) return StepResult::stream_end;
    if (ret != BZ_OK) {
        m_lastError = QString("Decompression error: bzip2 error code %1").arg(ret);
        return StepResult::error;
    }
    return StepResult::ok;
}

} // namespace sak
