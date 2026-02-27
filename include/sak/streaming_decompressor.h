// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QFile>
#include <QObject>
#include <QString>
#include <cstdint>

namespace sak {

/**
 * @brief Base class for streaming decompression (Template Method pattern)
 *
 * Implements the common open/close/read/progress logic once. Derived
 * classes override only the library-specific hooks:
 *   - initStream() / cleanupStream()
 *   - setInputFromBuffer() / setOutput()
 *   - outputRemaining() / inputEmpty()
 *   - decompressStep()
 *
 * Supported Formats (via subclasses):
 * - Gzip (.gz) - via zlib
 * - Bzip2 (.bz2) - via libbz2
 * - XZ (.xz) - via liblzma
 *
 * Thread-Safety: NOT thread-safe. Use one instance per thread.
 */
class StreamingDecompressor : public QObject {
    Q_OBJECT

public:
    explicit StreamingDecompressor(QObject* parent = nullptr);
    ~StreamingDecompressor() override;

    /// @brief Open the compressed file for reading
    bool open(const QString& filePath);

    /// @brief Close the decompressor and release resources
    void close();

    /// @brief Check if decompressor is open
    [[nodiscard]] bool isOpen() const;

    /// @brief Read decompressed data into buffer
    /// @return Bytes produced, 0 at end, -1 on error
    qint64 read(char* data, qint64 maxSize);

    /// @brief Check if at end of decompressed data
    [[nodiscard]] bool atEnd() const;

    /// @brief Get total compressed bytes read
    [[nodiscard]] qint64 compressedBytesRead() const;

    /// @brief Get total decompressed bytes produced
    [[nodiscard]] qint64 decompressedBytesProduced() const;

    /// @brief Get uncompressed size (if known)
    /// @return -1 if unknown (most formats)
    [[nodiscard]] virtual qint64 uncompressedSize() const = 0;

    /// @brief Last error message
    [[nodiscard]] QString lastError() const { return m_lastError; }

    /// @brief Format name (e.g. "gzip", "bzip2", "xz")
    [[nodiscard]] virtual QString formatName() const = 0;

Q_SIGNALS:
    void progressUpdated(qint64 compressedBytes, qint64 decompressedBytes);

protected:
    /// @brief Result from one decompression step
    enum class StepResult { ok, stream_end, error };

    // ---- Library-specific hooks (derived classes implement) ----

    /// @brief Initialize the library stream (inflate, bz2, lzma)
    virtual bool initStream() = 0;

    /// @brief Free the library stream resources
    virtual void cleanupStream() = 0;

    /// @brief Point the stream's input at m_inputBuffer[0..bytes)
    virtual void setInputFromBuffer(size_t bytes) = 0;

    /// @brief Point the stream's output at the caller's buffer
    virtual void setOutput(char* data, size_t maxSize) = 0;

    /// @brief How many output bytes the stream can still produce
    [[nodiscard]] virtual size_t outputRemaining() const = 0;

    /// @brief True when the stream has consumed all input
    [[nodiscard]] virtual bool inputEmpty() const = 0;

    /// @brief Run one decompression step; update stream pointers
    virtual StepResult decompressStep() = 0;

    // ---- Common state (available to derived classes) ----

    static constexpr int CHUNK_SIZE = 128 * 1024;  ///< 128 KB input buffer
    uint8_t m_inputBuffer[CHUNK_SIZE]{};
    QString m_lastError;

private:
    /// @brief Fill m_inputBuffer from m_file and call setInputFromBuffer()
    bool fillInputBuffer();

    QFile m_file;
    bool m_initialized{false};
    bool m_eof{false};
    qint64 m_compressedBytesRead{0};
    qint64 m_decompressedBytesProduced{0};
};

} // namespace sak
