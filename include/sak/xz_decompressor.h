// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/streaming_decompressor.h"

#include <lzma.h>

namespace sak {

/// @brief XZ decompressor using liblzma
///
/// Library-specific hooks for liblzma (XZ and LZMA formats).
/// All common logic (open, close, read, progress) lives in
/// StreamingDecompressor.
class XzDecompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit XzDecompressor(QObject* parent = nullptr);
    ~XzDecompressor() override;

    qint64 uncompressedSize() const override { return -1; }
    QString formatName() const override { return "xz"; }

protected:
    bool initStream() override;
    void cleanupStream() override;
    void setInputFromBuffer(size_t bytes) override;
    void setOutput(char* data, size_t maxSize) override;
    [[nodiscard]] size_t outputRemaining() const override;
    [[nodiscard]] bool inputEmpty() const override;
    StepResult decompressStep() override;

private:
    lzma_stream m_lzmaStream{};
};

}  // namespace sak
