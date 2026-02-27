// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/streaming_decompressor.h"
#include <bzlib.h>

namespace sak {

/// @brief Bzip2 decompressor using libbz2
///
/// Library-specific hooks for libbz2. All common logic
/// (open, close, read, progress) lives in StreamingDecompressor.
class Bzip2Decompressor : public StreamingDecompressor {
    Q_OBJECT

public:
    explicit Bzip2Decompressor(QObject* parent = nullptr);
    ~Bzip2Decompressor() override;

    qint64 uncompressedSize() const override { return -1; }
    QString formatName() const override { return "bzip2"; }

protected:
    bool initStream() override;
    void cleanupStream() override;
    void setInputFromBuffer(size_t bytes) override;
    void setOutput(char* data, size_t maxSize) override;
    [[nodiscard]] size_t outputRemaining() const override;
    [[nodiscard]] bool inputEmpty() const override;
    StepResult decompressStep() override;

private:
    bz_stream m_bzstream{};
};

} // namespace sak
