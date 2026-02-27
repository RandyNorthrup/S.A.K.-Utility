// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/streaming_decompressor.h"
#include <zlib.h>

namespace sak {

/// @brief Gzip decompressor using zlib
///
/// Library-specific hooks for zlib inflate. All common logic
/// (open, close, read, progress) lives in StreamingDecompressor.
class GzipDecompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit GzipDecompressor(QObject* parent = nullptr);
    ~GzipDecompressor() override;

    qint64 uncompressedSize() const override { return -1; }
    QString formatName() const override { return "gzip"; }

protected:
    bool initStream() override;
    void cleanupStream() override;
    void setInputFromBuffer(size_t bytes) override;
    void setOutput(char* data, size_t maxSize) override;
    [[nodiscard]] size_t outputRemaining() const override;
    [[nodiscard]] bool inputEmpty() const override;
    StepResult decompressStep() override;

private:
    z_stream m_zstream{};
};

} // namespace sak
