// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file media_read_back.h
/// @brief Reads a file back from the drive that holds it, never from Windows' file cache.
///
/// A check that reads back what was just written proves nothing about the drive when Windows
/// answers the read from the pages it still holds from the write. On testpc1 the Windows USB's
/// final check read its second install image part, 2.8 GB, in 17.5 s against 101 s for the first:
/// the cache had served it, so the check proved what was written and not what the stick held.
///
/// The handle here is opened with FILE_FLAG_NO_BUFFERING. The file system then writes out any of
/// the file's pages it still holds and reads the device itself. Such a read has to start on a
/// sector boundary, be a whole number of sectors long and land in a buffer aligned to the sector,
/// so this device reads the file in whole chunks that start on a chunk boundary, into a
/// page-aligned buffer, which meets all three for every sector size Windows supports, and hands the
/// bytes on as an ordinary read-only QIODevice. A seek starts the next read at the chunk that holds
/// the place. The last read may come back short: that is the end of the file.

#pragma once

#include <QByteArray>
#include <QIODevice>
#include <QString>

#include <functional>

namespace sak {

class MediaReadBackFile final : public QIODevice {
public:
    /// What one read from the drive asks for. A whole number of sectors of any size Windows
    /// supports, and large enough that a USB stick streams at its own speed.
    static constexpr qint64 kChunkBytes = 4LL * 1024 * 1024;

    explicit MediaReadBackFile(QString path, QObject* parent = nullptr);
    ~MediaReadBackFile() override;

    MediaReadBackFile(const MediaReadBackFile&) = delete;
    MediaReadBackFile& operator=(const MediaReadBackFile&) = delete;
    MediaReadBackFile(MediaReadBackFile&&) = delete;
    MediaReadBackFile& operator=(MediaReadBackFile&&) = delete;

    /// Opens the file for reading. Only QIODevice::ReadOnly is accepted.
    bool open(OpenMode mode) override;
    void close() override;
    bool seek(qint64 pos) override;
    /// The file's size when it was opened.
    [[nodiscard]] qint64 size() const override { return m_size; }
    [[nodiscard]] qint64 bytesAvailable() const override;
    /// True once every byte of the file has been read. A read that failed is not the end.
    [[nodiscard]] bool atEnd() const override;

    /// Windows' error code for the failed open, seek or read, 0 when none failed.
    [[nodiscard]] unsigned long windowsError() const { return m_error; }

protected:
    qint64 readData(char* data, qint64 max_size) override;
    qint64 writeData(const char* data, qint64 max_size) override;

private:
    [[nodiscard]] bool fillChunk();
    [[nodiscard]] qint64 position() const {
        return m_read_from_file - (m_chunk_fill - m_chunk_pos);
    }
    void release();

    QString m_path;
    void* m_handle{nullptr};
    char* m_chunk{nullptr};
    qint64 m_chunk_fill{0};
    qint64 m_chunk_pos{0};
    qint64 m_size{0};
    qint64 m_read_from_file{0};
    unsigned long m_error{0};
};

/// SHA-256 of @p path as the drive holds it, read through MediaReadBackFile. Empty when the file
/// cannot be opened or read, or when @p cancelled says to stop.
[[nodiscard]] QByteArray sha256FromMedia(const QString& path,
                                         const std::function<bool()>& cancelled = {});

}  // namespace sak
