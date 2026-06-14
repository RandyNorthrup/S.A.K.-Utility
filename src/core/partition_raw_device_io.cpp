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
