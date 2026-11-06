// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file media_read_back.cpp
/// @brief A file read back from its drive with Windows' file cache bypassed.

#include "sak/media_read_back.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFileInfo>

#include <algorithm>
#include <cstring>
#include <utility>

#include <windows.h>

namespace sak {

namespace {

/// What one hashing pass takes from the device at a time.
constexpr qint64 kHashReadBytes = MediaReadBackFile::kChunkBytes;

/// The name CreateFileW is given: absolute, with the \\?\ prefix Qt's own QFile uses, so a path
/// longer than MAX_PATH (a deep AppData folder in a backup) opens here exactly as it did there.
std::wstring createFileName(const QString& path) {
    const QString absolute = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath());
    const QString long_prefix = QStringLiteral("\\\\?\\");
    const QString unc_start = QStringLiteral("\\\\");
    if (absolute.startsWith(long_prefix) || absolute.startsWith(QStringLiteral("\\\\.\\"))) {
        return absolute.toStdWString();
    }
    if (absolute.startsWith(unc_start)) {
        return (long_prefix + QStringLiteral("UNC\\") + absolute.mid(unc_start.size()))
            .toStdWString();
    }
    return (long_prefix + absolute).toStdWString();
}

}  // namespace

MediaReadBackFile::MediaReadBackFile(QString path, QObject* parent)
    : QIODevice(parent), m_path(std::move(path)) {}

MediaReadBackFile::~MediaReadBackFile() {
    release();
}

bool MediaReadBackFile::open(OpenMode mode) {
    const OpenMode allowed = QIODevice::ReadOnly | QIODevice::Unbuffered;
    if (isOpen() || !mode.testFlag(QIODevice::ReadOnly) || (mode & ~allowed).toInt() != 0) {
        m_error = ERROR_INVALID_PARAMETER;
        return false;
    }
    m_error = 0;
    const std::wstring name = createFileName(m_path);
    HANDLE handle = CreateFileW(name.c_str(),
                                GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_NO_BUFFERING | FILE_FLAG_SEQUENTIAL_SCAN,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        m_error = GetLastError();
        return false;
    }
    LARGE_INTEGER size{};
    // VirtualAlloc hands back whole pages, so the chunk starts on a page and therefore on a
    // sector boundary, which an unbuffered read needs and a heap buffer does not promise.
    void* chunk = VirtualAlloc(
        nullptr, static_cast<SIZE_T>(kChunkBytes), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (GetFileSizeEx(handle, &size) == 0 || chunk == nullptr) {
        m_error = GetLastError();
        if (chunk != nullptr) {
            VirtualFree(chunk, 0, MEM_RELEASE);
        }
        CloseHandle(handle);
        return false;
    }
    m_handle = handle;
    m_chunk = static_cast<char*>(chunk);
    m_size = size.QuadPart;
    m_chunk_fill = 0;
    m_chunk_pos = 0;
    m_read_from_file = 0;
    return QIODevice::open(QIODevice::ReadOnly | QIODevice::Unbuffered);
}

void MediaReadBackFile::close() {
    QIODevice::close();
    release();
}

void MediaReadBackFile::release() {
    if (m_handle != nullptr) {
        CloseHandle(static_cast<HANDLE>(m_handle));
        m_handle = nullptr;
    }
    if (m_chunk != nullptr) {
        VirtualFree(m_chunk, 0, MEM_RELEASE);
        m_chunk = nullptr;
    }
    m_chunk_fill = 0;
    m_chunk_pos = 0;
}

qint64 MediaReadBackFile::bytesAvailable() const {
    return std::max<qint64>(0, m_size - position());
}

bool MediaReadBackFile::atEnd() const {
    return isOpen() && m_error == 0 && position() >= m_size;
}

bool MediaReadBackFile::seek(qint64 pos) {
    if (m_handle == nullptr || pos < 0 || pos > m_size || !QIODevice::seek(pos)) {
        return false;
    }
    // The next read has to start on a sector boundary, so it starts at the chunk holding pos
    // and the bytes before pos are passed over.
    const qint64 chunk_start = pos - (pos % kChunkBytes);
    LARGE_INTEGER to{};
    to.QuadPart = chunk_start;
    if (SetFilePointerEx(static_cast<HANDLE>(m_handle), to, nullptr, FILE_BEGIN) == 0) {
        m_error = GetLastError();
        return false;
    }
    m_error = 0;
    m_read_from_file = chunk_start;
    m_chunk_fill = 0;
    m_chunk_pos = 0;
    if (pos > chunk_start) {
        if (!fillChunk() || pos - chunk_start > m_chunk_fill) {
            return false;
        }
        m_chunk_pos = pos - chunk_start;
    }
    return true;
}

bool MediaReadBackFile::fillChunk() {
    DWORD got = 0;
    if (ReadFile(static_cast<HANDLE>(m_handle),
                 m_chunk,
                 static_cast<DWORD>(kChunkBytes),
                 &got,
                 nullptr) == 0) {
        m_error = GetLastError();
        return false;
    }
    // Nothing came back before the size the file had when it was opened: it was cut short
    // while being checked, which is a failed check, not the end of one.
    if (got == 0) {
        m_error = ERROR_HANDLE_EOF;
        return false;
    }
    m_chunk_fill = got;
    m_chunk_pos = 0;
    m_read_from_file += got;
    return true;
}

qint64 MediaReadBackFile::readData(char* data, qint64 max_size) {
    if (m_handle == nullptr || m_error != 0) {
        return -1;
    }
    qint64 copied = 0;
    while (copied < max_size) {
        if (m_chunk_pos >= m_chunk_fill) {
            if (position() >= m_size) {
                break;
            }
            if (!fillChunk()) {
                return copied > 0 ? copied : -1;
            }
        }
        const qint64 take = std::min(max_size - copied, m_chunk_fill - m_chunk_pos);
        std::memcpy(data + copied, m_chunk + m_chunk_pos, static_cast<size_t>(take));
        m_chunk_pos += take;
        copied += take;
    }
    return copied;
}

qint64 MediaReadBackFile::writeData(const char* /*data*/, qint64 /*max_size*/) {
    return -1;
}

QByteArray sha256FromMedia(const QString& path, const std::function<bool()>& cancelled) {
    MediaReadBackFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray block(static_cast<qsizetype>(kHashReadBytes), Qt::Uninitialized);
    while (!file.atEnd()) {
        if (cancelled && cancelled()) {
            return {};
        }
        const qint64 read = file.read(block.data(), block.size());
        if (read <= 0) {
            return {};
        }
        hash.addData(QByteArrayView(block.constData(), static_cast<qsizetype>(read)));
    }
    return hash.result();
}

}  // namespace sak
