// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_raw_device_io.cpp
/// @brief File/raw-device openers for Partition Manager parsers and certified writers.

#include "sak/partition_raw_device_io.h"

#include <QFile>
#include <QIODevice>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>
#include <winioctl.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace sak {

namespace {

void setError(QString* errorMessage, const QString& value) {
    if (errorMessage) {
        *errorMessage = value;
    }
}

#ifdef Q_OS_WIN
QString win32ErrorMessage(DWORD errorCode) {
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                        FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = FormatMessageW(flags,
                                        nullptr,
                                        errorCode,
                                        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                        reinterpret_cast<LPWSTR>(&buffer),
                                        0,
                                        nullptr);
    QString message;
    if (length > 0 && buffer != nullptr) {
        message = QString::fromWCharArray(buffer, static_cast<int>(length)).trimmed();
    }
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message.isEmpty() ? QStringLiteral("Win32 error %1").arg(errorCode) : message;
}

class WindowsRawDevice final : public QIODevice {
public:
    WindowsRawDevice(QString path, bool writable) : path_(std::move(path)), writable_(writable) {}

    ~WindowsRawDevice() override { close(); }

    bool open(OpenMode mode) override {
        const bool wantsWrite = (mode & QIODevice::WriteOnly) != 0;
        if (wantsWrite && !writable_) {
            setErrorString(QStringLiteral("Raw device helper is read-only"));
            return false;
        }
        const DWORD desiredAccess = writable_ ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_READ;
        handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(path_.utf16()),
                              desiredAccess,
                              FILE_SHARE_READ | FILE_SHARE_WRITE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return false;
        }
        position_ = 0;
        return QIODevice::open(writable_ ? QIODevice::ReadWrite : QIODevice::ReadOnly);
    }

    void close() override {
        if (handle_ != INVALID_HANDLE_VALUE) {
            if (writable_) {
                FlushFileBuffers(handle_);
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        position_ = 0;
        QIODevice::close();
    }

    bool isSequential() const override { return false; }

    qint64 pos() const override { return position_; }

    bool seek(qint64 pos) override {
        if (handle_ == INVALID_HANDLE_VALUE || pos < 0) {
            return false;
        }
        LARGE_INTEGER target{};
        target.QuadPart = pos;
        if (!SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN)) {
            if (!writable_ || (pos % kRawAlignment) == 0) {
                setErrorString(win32ErrorMessage(GetLastError()));
                return false;
            }
            target.QuadPart = alignedStartFor(pos);
            if (!SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN)) {
                setErrorString(win32ErrorMessage(GetLastError()));
                return false;
            }
            setErrorString(QString());
        }
        QIODevice::seek(pos);
        position_ = pos;
        return true;
    }

    qint64 size() const override {
        if (handle_ == INVALID_HANDLE_VALUE) {
            return -1;
        }
        LARGE_INTEGER fileSize{};
        if (!GetFileSizeEx(handle_, &fileSize)) {
            return -1;
        }
        return fileSize.QuadPart;
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (handle_ == INVALID_HANDLE_VALUE || !data || maxSize < 0) {
            return -1;
        }
        if (maxSize == 0) {
            return 0;
        }

        return isAlignedRawRequest(maxSize) ? readAligned(data, maxSize)
                                            : readUnaligned(data, maxSize);
    }

    qint64 writeData(const char* data, qint64 maxSize) override {
        if (!writable_) {
            setErrorString(QStringLiteral("Raw device helper is read-only"));
            return -1;
        }
        if (handle_ == INVALID_HANDLE_VALUE || !data || maxSize < 0) {
            return -1;
        }
        if (maxSize == 0) {
            return 0;
        }

        return isAlignedRawRequest(maxSize) ? writeAligned(data, maxSize)
                                            : writeUnaligned(data, maxSize);
    }

private:
    static constexpr qint64 kRawAlignment = 4096;

    [[nodiscard]] bool isAlignedRawRequest(qint64 maxSize) const {
        const qint64 prefixBytes = position_ - alignedStart();
        return prefixBytes == 0 && (maxSize % kRawAlignment) == 0;
    }

    [[nodiscard]] qint64 alignedStart() const { return alignedStartFor(position_); }

    [[nodiscard]] static qint64 alignedStartFor(qint64 position) {
        return (position / kRawAlignment) * kRawAlignment;
    }

    [[nodiscard]] qint64 readAligned(char* data, qint64 maxSize) {
        const DWORD bytesRequested = clampedDword(maxSize);
        DWORD bytesRead = 0;
        if (!ReadFile(handle_, data, bytesRequested, &bytesRead, nullptr)) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        position_ += static_cast<qint64>(bytesRead);
        return static_cast<qint64>(bytesRead);
    }

    [[nodiscard]] qint64 readUnaligned(char* data, qint64 maxSize) {
        const qint64 start = alignedStart();
        const qint64 prefixBytes = position_ - start;
        const qint64 alignedBytes = alignedReadSize(prefixBytes + maxSize);
        const DWORD bytesRequested = clampedDword(alignedBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        std::vector<char> scratch(bytesRequested);
        DWORD bytesRead = 0;
        if (!ReadFile(handle_, scratch.data(), bytesRequested, &bytesRead, nullptr)) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (static_cast<qint64>(bytesRead) <= prefixBytes) {
            return 0;
        }

        const qint64 copied = std::min<qint64>(maxSize,
                                               static_cast<qint64>(bytesRead) - prefixBytes);
        std::copy_n(scratch.data() + prefixBytes, copied, data);
        position_ += copied;
        return copied;
    }

    [[nodiscard]] static qint64 alignedReadSize(qint64 wantedBytes) {
        return ((wantedBytes + kRawAlignment - 1) / kRawAlignment) * kRawAlignment;
    }

    [[nodiscard]] static DWORD clampedDword(qint64 size) {
        return static_cast<DWORD>(
            std::min<qint64>(size, static_cast<qint64>(std::numeric_limits<DWORD>::max())));
    }

    [[nodiscard]] bool seekHandle(qint64 offset) {
        LARGE_INTEGER target{};
        target.QuadPart = offset;
        if (SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN)) {
            return true;
        }
        setErrorString(win32ErrorMessage(GetLastError()));
        return false;
    }

    [[nodiscard]] qint64 writeAligned(const char* data, qint64 maxSize) {
        const DWORD bytesRequested = clampedDword(maxSize);
        DWORD bytesWritten = 0;
        if (!WriteFile(handle_, data, bytesRequested, &bytesWritten, nullptr)) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        position_ += static_cast<qint64>(bytesWritten);
        return static_cast<qint64>(bytesWritten);
    }

    [[nodiscard]] qint64 writeUnaligned(const char* data, qint64 maxSize) {
        const qint64 start = alignedStart();
        const qint64 prefixBytes = position_ - start;
        const qint64 alignedBytes = alignedReadSize(prefixBytes + maxSize);
        const DWORD bytesRequested = clampedDword(alignedBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        std::vector<char> scratch(bytesRequested);
        DWORD bytesRead = 0;
        if (!ReadFile(handle_, scratch.data(), bytesRequested, &bytesRead, nullptr)) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (bytesRead != bytesRequested) {
            setErrorString(QStringLiteral("Raw device short read before unaligned write"));
            return -1;
        }
        std::copy_n(data, maxSize, scratch.data() + prefixBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(handle_, scratch.data(), bytesRequested, &bytesWritten, nullptr)) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (bytesWritten != bytesRequested) {
            setErrorString(QStringLiteral("Raw device short unaligned write"));
            return -1;
        }
        position_ += maxSize;
        return maxSize;
    }

    QString path_;
    bool writable_{false};
    HANDLE handle_{INVALID_HANDLE_VALUE};
    qint64 position_{0};
};
#endif

}  // namespace

#ifdef Q_OS_WIN
namespace {

// Copy a byte range from src to dst in chunks; the destination is sparse, so ranges we
// never write stay holes. Returns false on any short read/write.
bool copyHandleRange(HANDLE src, HANDLE dst, qint64 offset, qint64 length) {
    constexpr DWORD kChunk = 4u * 1024u * 1024u;
    std::vector<char> buffer(kChunk);
    qint64 remaining = length;
    qint64 position = offset;
    while (remaining > 0) {
        LARGE_INTEGER seek{};
        seek.QuadPart = position;
        if (!SetFilePointerEx(src, seek, nullptr, FILE_BEGIN) ||
            !SetFilePointerEx(dst, seek, nullptr, FILE_BEGIN)) {
            return false;
        }
        const DWORD want =
            static_cast<DWORD>(std::min<qint64>(remaining, static_cast<qint64>(kChunk)));
        DWORD got = 0;
        if (!ReadFile(src, buffer.data(), want, &got, nullptr) || got == 0) {
            return false;
        }
        DWORD wrote = 0;
        if (!WriteFile(dst, buffer.data(), got, &wrote, nullptr) || wrote != got) {
            return false;
        }
        position += got;
        remaining -= got;
    }
    return true;
}

bool copyAllocatedRanges(HANDLE src, HANDLE dst, qint64 size) {
    FILE_ALLOCATED_RANGE_BUFFER query{};
    query.FileOffset.QuadPart = 0;
    query.Length.QuadPart = size;
    constexpr int kInitialAllocatedRangeCount = 1024;
    std::vector<FILE_ALLOCATED_RANGE_BUFFER> ranges(kInitialAllocatedRangeCount);
    qint64 scanStart = 0;
    while (scanStart < size) {
        query.FileOffset.QuadPart = scanStart;
        query.Length.QuadPart = size - scanStart;
        DWORD returned = 0;
        const BOOL ok = DeviceIoControl(src,
                                        FSCTL_QUERY_ALLOCATED_RANGES,
                                        &query,
                                        sizeof(query),
                                        ranges.data(),
                                        static_cast<DWORD>(ranges.size() * sizeof(ranges[0])),
                                        &returned,
                                        nullptr);
        const DWORD error = GetLastError();
        if (!ok && error != ERROR_MORE_DATA) {
            return false;
        }
        const size_t count = returned / sizeof(FILE_ALLOCATED_RANGE_BUFFER);
        if (count == 0) {
            break;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!copyHandleRange(
                    src, dst, ranges[i].FileOffset.QuadPart, ranges[i].Length.QuadPart)) {
                return false;
            }
        }
        const auto& last = ranges[count - 1];
        scanStart = last.FileOffset.QuadPart + last.Length.QuadPart;
        if (ok) {
            break;
        }
    }
    return true;
}

bool copyFileSparseWindows(const QString& source,
                           const QString& destination,
                           QString* errorMessage) {
    const HANDLE src = CreateFileW(reinterpret_cast<LPCWSTR>(source.utf16()),
                                   GENERIC_READ,
                                   FILE_SHARE_READ,
                                   nullptr,
                                   OPEN_EXISTING,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    LARGE_INTEGER size{};
    if (src == INVALID_HANDLE_VALUE || !GetFileSizeEx(src, &size)) {
        setError(errorMessage, win32ErrorMessage(GetLastError()));
        if (src != INVALID_HANDLE_VALUE) {
            CloseHandle(src);
        }
        return false;
    }
    const HANDLE dst = CreateFileW(reinterpret_cast<LPCWSTR>(destination.utf16()),
                                   GENERIC_READ | GENERIC_WRITE,
                                   0,
                                   nullptr,
                                   CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL,
                                   nullptr);
    if (dst == INVALID_HANDLE_VALUE) {
        setError(errorMessage, win32ErrorMessage(GetLastError()));
        CloseHandle(src);
        return false;
    }
    DWORD returned = 0;
    DeviceIoControl(dst, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
    const bool ok = SetFilePointerEx(dst, size, nullptr, FILE_BEGIN) && SetEndOfFile(dst) &&
                    copyAllocatedRanges(src, dst, size.QuadPart);
    if (!ok) {
        setError(errorMessage, win32ErrorMessage(GetLastError()));
    }
    CloseHandle(src);
    CloseHandle(dst);
    if (!ok) {
        DeleteFileW(reinterpret_cast<LPCWSTR>(destination.utf16()));
    }
    return ok;
}

}  // namespace
#elif defined(SEEK_DATA) && defined(SEEK_HOLE)
namespace {

// Copy one source data region [offset, offset+length) to the destination at the same
// offset (the rest of the destination stays a hole).
bool copyDataRegionPosix(int in, int out, off_t offset, off_t length) {
    constexpr size_t kSparseCopyChunkBytes = 4u * 1024u * 1024u;
    std::vector<char> buffer(kSparseCopyChunkBytes);
    off_t done = 0;
    while (done < length) {
        const size_t want =
            static_cast<size_t>(std::min<off_t>(length - done, static_cast<off_t>(buffer.size())));
        const ssize_t got = ::pread(in, buffer.data(), want, offset + done);
        if (got <= 0 ||
            ::pwrite(out, buffer.data(), static_cast<size_t>(got), offset + done) != got) {
            return false;
        }
        done += got;
    }
    return true;
}

bool copyFileSparsePosix(const QString& source, const QString& destination, QString* errorMessage) {
    const QByteArray sourceName = QFile::encodeName(source);
    const QByteArray destName = QFile::encodeName(destination);
    const int in = ::open(sourceName.constData(), O_RDONLY);
    struct stat info{};
    if (in < 0 || ::fstat(in, &info) != 0) {
        setError(errorMessage, QStringLiteral("Unable to read %1").arg(source));
        if (in >= 0) {
            ::close(in);
        }
        return false;
    }
    const int out = ::open(destName.constData(), O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (out < 0) {
        ::close(in);
        setError(errorMessage, QStringLiteral("Unable to create %1").arg(destination));
        return false;
    }
    bool ok = ::ftruncate(out, info.st_size) == 0;
    off_t position = 0;
    while (ok && position < info.st_size) {
        const off_t dataStart = ::lseek(in, position, SEEK_DATA);
        if (dataStart < 0) {
            break;  // ENXIO: no more data before EOF
        }
        off_t dataEnd = ::lseek(in, dataStart, SEEK_HOLE);
        if (dataEnd < 0) {
            dataEnd = info.st_size;
        }
        ok = copyDataRegionPosix(in, out, dataStart, dataEnd - dataStart);
        position = dataEnd;
    }
    ::close(in);
    ::close(out);
    if (!ok) {
        ::unlink(destName.constData());
        setError(errorMessage, QStringLiteral("Unable to copy %1 to %2").arg(source, destination));
    }
    return ok;
}

}  // namespace
#endif

bool copyFileSparse(const QString& source, const QString& destination, QString* errorMessage) {
#ifdef Q_OS_WIN
    return copyFileSparseWindows(source, destination, errorMessage);
#elif defined(SEEK_DATA) && defined(SEEK_HOLE)
    return copyFileSparsePosix(source, destination, errorMessage);
#else
    if (QFile::copy(source, destination)) {
        return true;
    }
    setError(errorMessage, QStringLiteral("Unable to copy %1 to %2").arg(source, destination));
    return false;
#endif
}

void markFileSparse(int fileDescriptor) {
#ifdef Q_OS_WIN
    if (fileDescriptor < 0) {
        return;
    }
    const auto handle = reinterpret_cast<HANDLE>(_get_osfhandle(fileDescriptor));
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
#else
    Q_UNUSED(fileDescriptor);
#endif
}

bool isWindowsRawDevicePath(const QString& path) {
#ifdef Q_OS_WIN
    return path.startsWith(QStringLiteral("\\\\.\\")) ||
           path.startsWith(QStringLiteral("\\\\?\\GLOBALROOT\\"));
#else
    Q_UNUSED(path);
    return false;
#endif
}

std::unique_ptr<QIODevice> openFileOrRawDeviceReadOnly(const QString& path, QString* errorMessage) {
    if (path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("Path is required"));
        return {};
    }

#ifdef Q_OS_WIN
    if (isWindowsRawDevicePath(path)) {
        auto device = std::make_unique<WindowsRawDevice>(path, false);
        if (!device->open(QIODevice::ReadOnly)) {
            setError(errorMessage, device->errorString());
            return {};
        }
        return device;
    }
#endif

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadOnly)) {
        setError(errorMessage, file->errorString());
        return {};
    }
    return file;
}

std::unique_ptr<QIODevice> openFileOrRawDeviceReadWrite(const QString& path,
                                                        QString* errorMessage) {
    if (path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("Path is required"));
        return {};
    }

#ifdef Q_OS_WIN
    if (isWindowsRawDevicePath(path)) {
        auto device = std::make_unique<WindowsRawDevice>(path, true);
        if (!device->open(QIODevice::ReadWrite)) {
            setError(errorMessage, device->errorString());
            return {};
        }
        return device;
    }
#endif

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadWrite)) {
        setError(errorMessage, file->errorString());
        return {};
    }
    return file;
}

}  // namespace sak
