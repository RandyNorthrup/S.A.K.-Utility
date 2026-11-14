// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file junction_fixture.h
/// @brief Junctions and canary folders for tests that prove a write or delete never follows a link.
///
/// A junction needs no privilege (a file symbolic link needs Developer Mode or administrator), so
/// these tests run anywhere. The junction is made in-process with FSCTL_SET_REPARSE_POINT rather
/// than by starting cmd.exe's mklink.

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>

#include <cstddef>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>

#include <winioctl.h>

namespace sak::test {

/// Make @p link (a new folder) a junction to @p target, which need not exist.
inline bool createJunction(const QString& link, const QString& target) {
    if (CreateDirectoryW(reinterpret_cast<LPCWSTR>(link.utf16()), nullptr) == 0) {
        return false;
    }
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(link.utf16()),
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    const std::wstring print = QDir::toNativeSeparators(QDir::cleanPath(target)).toStdWString();
    const std::wstring substitute = L"\\??\\" + print;
    const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print.size() * sizeof(wchar_t);
    // REPARSE_DATA_BUFFER (ntifs.h) for a mount point: an 8-byte header, four USHORT offsets and
    // lengths, then both names, each followed by a NUL.
    constexpr std::size_t kHeaderBytes = 8;
    constexpr std::size_t kMountPointFieldsBytes = 8;
    const std::size_t path_buffer_bytes = substitute_bytes + sizeof(wchar_t) + print_bytes +
                                          sizeof(wchar_t);
    std::vector<std::byte> buffer(kHeaderBytes + kMountPointFieldsBytes + path_buffer_bytes);
    auto put16 = [&buffer](std::size_t offset, std::size_t value) {
        const auto narrow = static_cast<unsigned short>(value);
        std::memcpy(buffer.data() + offset, &narrow, sizeof(narrow));
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, sizeof(tag));
    put16(4, kMountPointFieldsBytes + path_buffer_bytes);         // ReparseDataLength
    put16(kHeaderBytes, 0);                                       // SubstituteNameOffset
    put16(kHeaderBytes + 2, substitute_bytes);                    // SubstituteNameLength
    put16(kHeaderBytes + 4, substitute_bytes + sizeof(wchar_t));  // PrintNameOffset
    put16(kHeaderBytes + 6, print_bytes);                         // PrintNameLength
    std::byte* names = buffer.data() + kHeaderBytes + kMountPointFieldsBytes;
    std::memcpy(names, substitute.c_str(), substitute_bytes + sizeof(wchar_t));
    std::memcpy(names + substitute_bytes + sizeof(wchar_t),
                print.c_str(),
                print_bytes + sizeof(wchar_t));
    DWORD returned = 0;
    const BOOL ok = DeviceIoControl(handle,
                                    FSCTL_SET_REPARSE_POINT,
                                    buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    nullptr,
                                    0,
                                    &returned,
                                    nullptr);
    CloseHandle(handle);
    return ok != 0;
}

/// A folder holding one file whose bytes a test checks are still there afterwards.
inline bool createCanary(const QString& folder, const QString& name, const QByteArray& bytes) {
    if (!QDir().mkpath(folder)) {
        return false;
    }
    QFile file(QDir(folder).filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/// True when the canary file still holds exactly @p bytes.
inline bool canaryIntact(const QString& folder, const QString& name, const QByteArray& bytes) {
    QFile file(QDir(folder).filePath(name));
    return file.open(QIODevice::ReadOnly) && file.readAll() == bytes;
}

}  // namespace sak::test
