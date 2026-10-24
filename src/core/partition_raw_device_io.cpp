// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_raw_device_io.cpp
/// @brief File/raw-device openers for Partition Manager parsers and certified writers.

#include "sak/partition_raw_device_io.h"

#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QIODevice>

#include <algorithm>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <io.h>
#include <winioctl.h>

namespace sak {

namespace {

void setError(QString* errorMessage, const QString& value) {
    if (errorMessage != nullptr) {
        *errorMessage = value;
    }
}

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
        // A writable commit takes the device with write access UNshared so a concurrent
        // third-party writer cannot open it for writing and corrupt an APFS/HFS commit
        // mid-flight; readers are still permitted. A read-only opener shares fully. The
        // app's own post-commit read-back opens only after the write handle is closed, so
        // the tighter share mode never blocks it.
        const DWORD shareMode = writable_ ? FILE_SHARE_READ : (FILE_SHARE_READ | FILE_SHARE_WRITE);
        handle_ = CreateFileW(reinterpret_cast<LPCWSTR>(path_.utf16()),
                              desiredAccess,
                              shareMode,
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
            // Best-effort final flush on teardown; QIODevice::close() cannot report a
            // failure. Callers that must surface a durable-flush failure call
            // syncToDevice() (via flushDeviceBuffers) BEFORE close while the handle lives.
            if (writable_) {
                FlushFileBuffers(handle_);
            }
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        position_ = 0;
        QIODevice::close();
    }

    // Flush written blocks to durable storage, returning false (with the Win32 error
    // recorded) when FlushFileBuffers fails, so a caller never reports a durable write it
    // could not flush. A read-only or closed device has nothing to flush and succeeds.
    [[nodiscard]] bool syncToDevice() {
        if (handle_ == INVALID_HANDLE_VALUE || !writable_) {
            return true;
        }
        if (FlushFileBuffers(handle_) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return false;
        }
        return true;
    }

    bool isSequential() const override { return false; }

    qint64 pos() const override { return position_; }

    bool seek(qint64 pos) override {
        if (handle_ == INVALID_HANDLE_VALUE || pos < 0) {
            return false;
        }
        LARGE_INTEGER target{};
        target.QuadPart = pos;
        if (SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) == 0) {
            if (!writable_ || (pos % kRawAlignment) == 0) {
                setErrorString(win32ErrorMessage(GetLastError()));
                return false;
            }
            target.QuadPart = alignedStartFor(pos);
            if (SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) == 0) {
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
        if (GetFileSizeEx(handle_, &fileSize) != 0) {
            return fileSize.QuadPart;
        }
        // A physical-drive / partition / volume handle rejects GetFileSizeEx; ask the
        // device for its true length so a caller can fail closed on an oversized request
        // instead of treating an unknown size as sufficient.
        return rawDeviceLength();
    }

protected:
    qint64 readData(char* data, qint64 maxSize) override {
        if (handle_ == INVALID_HANDLE_VALUE || (data == nullptr) || maxSize < 0) {
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
        if (handle_ == INVALID_HANDLE_VALUE || (data == nullptr) || maxSize < 0) {
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
    // The largest alignment-multiple byte count that still fits in a DWORD. A raw
    // volume ReadFile/WriteFile count must be an alignment multiple, and DWORD_MAX
    // itself is not one, so every clamped request is rounded down to this bound and a
    // larger transfer is satisfied as a short read/write that QIODevice loops over.
    static constexpr qint64 kMaxAlignedRequest =
        (static_cast<qint64>(std::numeric_limits<DWORD>::max()) / kRawAlignment) * kRawAlignment;

    // The device's true byte length via IOCTL_DISK_GET_LENGTH_INFO (works on both disk and
    // volume/partition handles), or -1 if it cannot be determined -- callers fail closed on -1.
    [[nodiscard]] qint64 rawDeviceLength() const {
        GET_LENGTH_INFORMATION info{};
        DWORD returned = 0;
        if (DeviceIoControl(handle_,
                            IOCTL_DISK_GET_LENGTH_INFO,
                            nullptr,
                            0,
                            &info,
                            sizeof(info),
                            &returned,
                            nullptr) != 0) {
            return info.Length.QuadPart;
        }
        return -1;
    }

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
        if (ReadFile(handle_, data, bytesRequested, &bytesRead, nullptr) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        position_ += static_cast<qint64>(bytesRead);
        return static_cast<qint64>(bytesRead);
    }

    [[nodiscard]] qint64 readUnaligned(char* data, qint64 maxSize) {
        const qint64 start = alignedStart();
        const qint64 prefixBytes = position_ - start;
        // Bound the request before adding prefixBytes so prefix + span can never overflow
        // qint64 (an absurd maxSize would wrap negative in alignedReadSize). A larger read is
        // satisfied as a short read the caller loops over; the single scratch ReadFile is
        // already capped to kMaxAlignedRequest via clampedDword.
        const qint64 span = std::min<qint64>(maxSize, kMaxAlignedRequest);
        const qint64 alignedBytes = alignedReadSize(prefixBytes + span);
        const DWORD bytesRequested = clampedDword(alignedBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        std::vector<char> scratch(bytesRequested);
        DWORD bytesRead = 0;
        if (ReadFile(handle_, scratch.data(), bytesRequested, &bytesRead, nullptr) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (static_cast<qint64>(bytesRead) <= prefixBytes) {
            return 0;
        }

        const qint64 copied = std::min<qint64>(span, static_cast<qint64>(bytesRead) - prefixBytes);
        std::copy_n(scratch.data() + prefixBytes, copied, data);
        position_ += copied;
        return copied;
    }

    [[nodiscard]] static qint64 alignedReadSize(qint64 wantedBytes) {
        return ((wantedBytes + kRawAlignment - 1) / kRawAlignment) * kRawAlignment;
    }

    [[nodiscard]] static DWORD clampedDword(qint64 size) {
        return static_cast<DWORD>(std::min<qint64>(size, kMaxAlignedRequest));
    }

    [[nodiscard]] bool seekHandle(qint64 offset) {
        LARGE_INTEGER target{};
        target.QuadPart = offset;
        if (SetFilePointerEx(handle_, target, nullptr, FILE_BEGIN) != 0) {
            return true;
        }
        setErrorString(win32ErrorMessage(GetLastError()));
        return false;
    }

    [[nodiscard]] qint64 writeAligned(const char* data, qint64 maxSize) {
        const DWORD bytesRequested = clampedDword(maxSize);
        DWORD bytesWritten = 0;
        if (WriteFile(handle_, data, bytesRequested, &bytesWritten, nullptr) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        position_ += static_cast<qint64>(bytesWritten);
        return static_cast<qint64>(bytesWritten);
    }

    [[nodiscard]] qint64 writeUnaligned(const char* data, qint64 maxSize) {
        const qint64 start = alignedStart();
        const qint64 prefixBytes = position_ - start;
        // The scratch buffer is read/written in one ReadFile/WriteFile whose byte count
        // must fit in a DWORD and be an alignment multiple (kMaxAlignedRequest). Cap the
        // payload so prefix + payload never rounds past it; a larger request returns a
        // short write and QIODevice::write() loops for the remainder. Without this cap a
        // clamped scratch would be smaller than prefix + maxSize and std::copy_n below
        // would write past the buffer (heap overflow).
        const qint64 payload = std::min<qint64>(maxSize, kMaxAlignedRequest - prefixBytes);
        if (payload <= 0) {
            setErrorString(QStringLiteral("Raw device unaligned write prefix exceeds request cap"));
            return -1;
        }
        const qint64 alignedBytes = alignedReadSize(prefixBytes + payload);
        const auto bytesRequested = static_cast<DWORD>(alignedBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        std::vector<char> scratch(bytesRequested);
        DWORD bytesRead = 0;
        if (ReadFile(handle_, scratch.data(), bytesRequested, &bytesRead, nullptr) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (bytesRead != bytesRequested) {
            setErrorString(QStringLiteral("Raw device short read before unaligned write"));
            return -1;
        }
        std::copy_n(data, payload, scratch.data() + prefixBytes);
        if (!seekHandle(start)) {
            return -1;
        }

        DWORD bytesWritten = 0;
        if (WriteFile(handle_, scratch.data(), bytesRequested, &bytesWritten, nullptr) == 0) {
            setErrorString(win32ErrorMessage(GetLastError()));
            return -1;
        }
        if (bytesWritten != bytesRequested) {
            setErrorString(QStringLiteral("Raw device short unaligned write"));
            return -1;
        }
        position_ += payload;
        return payload;
    }

    QString path_;
    bool writable_{false};
    HANDLE handle_{INVALID_HANDLE_VALUE};
    qint64 position_{0};
};

}  // namespace

namespace {

// Copy a byte range from src to dst in chunks. Sparse mode calls this only for allocated
// ranges; dense mode calls it for the whole source. Returns false on any short read/write.
bool copyHandleRange(HANDLE src, HANDLE dst, qint64 offset, qint64 length) {
    constexpr DWORD kChunk = 4u * 1024u * 1024u;
    std::vector<char> buffer(kChunk);
    qint64 remaining = length;
    qint64 position = offset;
    while (remaining > 0) {
        LARGE_INTEGER seek{};
        seek.QuadPart = position;
        if ((SetFilePointerEx(src, seek, nullptr, FILE_BEGIN) == 0) ||
            (SetFilePointerEx(dst, seek, nullptr, FILE_BEGIN) == 0)) {
            return false;
        }
        const DWORD want =
            static_cast<DWORD>(std::min<qint64>(remaining, static_cast<qint64>(kChunk)));
        DWORD got = 0;
        if (ReadFile(src, buffer.data(), want, &got, nullptr) == 0) {
            return false;
        }
        if (got == 0) {
            SetLastError(ERROR_HANDLE_EOF);
            return false;
        }
        DWORD wrote = 0;
        if (WriteFile(dst, buffer.data(), got, &wrote, nullptr) == 0) {
            return false;
        }
        if (wrote != got) {
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        position += got;
        remaining -= got;
    }
    return true;
}

enum class ImageCopyMode {
    Sparse,
    Dense
};

std::optional<ImageCopyMode> chooseImageCopyMode(
    HANDLE src, HANDLE dst, const QString& destination, qint64 size, QString* errorMessage) {
    DWORD sourceFlags = 0;
    DWORD destinationFlags = 0;
    if ((GetVolumeInformationByHandleW(
             src, nullptr, 0, nullptr, nullptr, &sourceFlags, nullptr, 0) == 0) ||
        (GetVolumeInformationByHandleW(
             dst, nullptr, 0, nullptr, nullptr, &destinationFlags, nullptr, 0) == 0)) {
        setError(errorMessage,
                 QStringLiteral("Unable to inspect image-copy volume: %1")
                     .arg(win32ErrorMessage(GetLastError())));
        return std::nullopt;
    }
    if ((sourceFlags & FILE_SUPPORTS_SPARSE_FILES) != 0 &&
        (destinationFlags & FILE_SUPPORTS_SPARSE_FILES) != 0) {
        DWORD returned = 0;
        if (DeviceIoControl(dst, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) ==
            0) {
            setError(errorMessage,
                     QStringLiteral("Unable to mark image-copy destination sparse: %1")
                         .arg(win32ErrorMessage(GetLastError())));
            return std::nullopt;
        }
        return ImageCopyMode::Sparse;
    }
    const QString destinationDirectory =
        QDir::toNativeSeparators(QFileInfo(destination).absolutePath());
    ULARGE_INTEGER available{};
    if (GetDiskFreeSpaceExW(reinterpret_cast<LPCWSTR>(destinationDirectory.utf16()),
                            &available,
                            nullptr,
                            nullptr) == 0) {
        setError(errorMessage,
                 QStringLiteral("Unable to inspect dense image-copy capacity: %1")
                     .arg(win32ErrorMessage(GetLastError())));
        return std::nullopt;
    }
    if (size < 0 || static_cast<quint64>(size) > available.QuadPart) {
        setError(errorMessage,
                 QStringLiteral("Dense image copy requires %1 bytes but only %2 bytes are "
                                "available on the destination volume")
                     .arg(size)
                     .arg(available.QuadPart));
        return std::nullopt;
    }
    return ImageCopyMode::Dense;
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
        if ((ok == 0) && error != ERROR_MORE_DATA) {
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
        if (ok != 0) {
            break;
        }
    }
    return true;
}

bool copyImageContent(HANDLE src, HANDLE dst, qint64 size, ImageCopyMode mode) {
    LARGE_INTEGER end{};
    end.QuadPart = size;
    if (mode == ImageCopyMode::Sparse) {
        return (SetFilePointerEx(dst, end, nullptr, FILE_BEGIN) != 0) && (SetEndOfFile(dst) != 0) &&
               copyAllocatedRanges(src, dst, size);
    }
    return copyHandleRange(src, dst, 0, size) &&
           (SetFilePointerEx(dst, end, nullptr, FILE_BEGIN) != 0) && (SetEndOfFile(dst) != 0);
}

void disposeFailedImageCopy(HANDLE dst, QString* errorMessage) {
    FILE_DISPOSITION_INFO disposition{};
    disposition.DeleteFile = TRUE;
    if (SetFileInformationByHandle(dst, FileDispositionInfo, &disposition, sizeof(disposition)) !=
        0) {
        return;
    }
    const QString cleanupError = win32ErrorMessage(GetLastError());
    const QString copyError = errorMessage == nullptr ? QStringLiteral("Image copy failed")
                                                      : *errorMessage;
    setError(errorMessage,
             QStringLiteral("%1; partial destination could not be disposed: %2")
                 .arg(copyError, cleanupError));
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
    if (src == INVALID_HANDLE_VALUE || (GetFileSizeEx(src, &size) == 0)) {
        setError(errorMessage, win32ErrorMessage(GetLastError()));
        if (src != INVALID_HANDLE_VALUE) {
            CloseHandle(src);
        }
        return false;
    }
    const HANDLE dst = CreateFileW(reinterpret_cast<LPCWSTR>(destination.utf16()),
                                   GENERIC_READ | GENERIC_WRITE | DELETE,
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
    const auto mode = chooseImageCopyMode(src, dst, destination, size.QuadPart, errorMessage);
    const bool ok = mode.has_value() && copyImageContent(src, dst, size.QuadPart, *mode) &&
                    (FlushFileBuffers(dst) != 0);
    if (mode.has_value() && !ok) {
        setError(errorMessage, win32ErrorMessage(GetLastError()));
    }
    if (!ok) {
        disposeFailedImageCopy(dst, errorMessage);
    }
    CloseHandle(src);
    CloseHandle(dst);
    return ok;
}

[[nodiscard]] HANDLE nativeFileHandle(int fileDescriptor) {
    return fileDescriptor < 0 ? INVALID_HANDLE_VALUE
                              : reinterpret_cast<HANDLE>(_get_osfhandle(fileDescriptor));
}

}  // namespace

bool copyFileSparse(const QString& source, const QString& destination, QString* errorMessage) {
    return copyFileSparseWindows(source, destination, errorMessage);
}

void markFileSparse(int fileDescriptor) {
    HANDLE handle = nativeFileHandle(fileDescriptor);
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD returned = 0;
    DeviceIoControl(handle, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr);
}

namespace {
// Every raw-device rule below tests the same normalized text, so a target spelled with
// forward slashes or surrounding whitespace cannot be classified differently by two callers.
[[nodiscard]] QString normalizedDevicePath(const QString& path) {
    QString normalized = path.trimmed();
    normalized.replace(QLatin1Char('/'), QLatin1Char('\\'));
    return normalized;
}

// A BARE \\?\Volume{GUID} is a device handle. A \\?\Volume{GUID}\path\file form is an
// extended-length FILE path (it has a separator after the closing brace), not a raw device,
// and must stay classified as a file so it is not routed through the aligned raw writer.
[[nodiscard]] bool isBareVolumeGuidDevice(const QString& path) {
    if (!path.startsWith(QStringLiteral("\\\\?\\Volume{"), Qt::CaseInsensitive)) {
        return false;
    }
    const qsizetype closing = path.indexOf(QLatin1Char('}'));
    return closing >= 0 && path.indexOf(QLatin1Char('\\'), closing) < 0;
}

// Raw-device spellings the \\.\ prefix misses: \\?\PhysicalDriveN, the \\?\GLOBALROOT\
// device namespace, and a bare \\?\Volume{GUID} handle. The generic \\?\ prefix is
// deliberately NOT matched (it is also the long-path prefix for ordinary files, e.g.
// \\?\C:\dir\x.img).
[[nodiscard]] bool isExtendedWindowsRawDevice(const QString& path) {
    return path.startsWith(QStringLiteral("\\\\?\\PhysicalDrive"), Qt::CaseInsensitive) ||
           path.startsWith(QStringLiteral("\\\\?\\GLOBALROOT\\"), Qt::CaseInsensitive) ||
           isBareVolumeGuidDevice(path);
}

constexpr qsizetype kExtendedDevicePrefixLength = 4;  // "\\?\"
}  // namespace

bool isWindowsRawDevicePath(const QString& path) {
    // Windows device paths are case-insensitive. The validator and executor share their raw
    // classification. A root-relative dev path (any case or slash dialect) is also raw-looking
    // hostile input on Windows: image-only gates refuse it, and the raw openers reject it.
    const QString normalized = normalizedDevicePath(path);
    return normalized.startsWith(QStringLiteral("\\\\.\\"), Qt::CaseInsensitive) ||
           isExtendedWindowsRawDevice(normalized) ||
           normalized.startsWith(QStringLiteral("\\dev\\"), Qt::CaseInsensitive);
}

QString canonicalRawDevicePath(const QString& path) {
    const QString normalized = normalizedDevicePath(path);
    if (normalized.startsWith(QStringLiteral("\\\\.\\"), Qt::CaseInsensitive)) {
        return normalized;
    }
    if (isExtendedWindowsRawDevice(normalized)) {
        // The same device object addressed through the \\.\ namespace the executor guards test.
        return QStringLiteral("\\\\.\\") + normalized.mid(kExtendedDevicePrefixLength);
    }
    return {};
}

std::optional<uint32_t> rawDevicePhysicalDriveNumber(const QString& path) {
    const QString canonical = canonicalRawDevicePath(path);
    const QString prefix = QStringLiteral("\\\\.\\PhysicalDrive");
    if (!canonical.startsWith(prefix, Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    bool ok = false;
    const uint value = canonical.mid(prefix.size()).toUInt(&ok);
    return ok ? std::optional<uint32_t>(static_cast<uint32_t>(value)) : std::nullopt;
}

namespace {
[[nodiscard]] bool requireCanonicalRawOpenPath(const QString& path, QString* errorMessage) {
    if (!canonicalRawDevicePath(path).isEmpty()) {
        return true;
    }
    setError(errorMessage, QStringLiteral("Unsupported raw device path on Windows"));
    return false;
}
}  // namespace

bool flushDeviceBuffers(QIODevice* device, QString* errorMessage) {
    if (device == nullptr) {
        setError(errorMessage, QStringLiteral("No open device to flush"));
        return false;
    }
    if (auto* raw = dynamic_cast<WindowsRawDevice*>(device)) {
        if (!raw->syncToDevice()) {
            setError(errorMessage, raw->errorString());
            return false;
        }
        return true;
    }
    if (auto* file = qobject_cast<QFileDevice*>(device)) {
        if (!file->flush()) {
            setError(errorMessage, file->errorString());
            return false;
        }
        // QFileDevice::flush drains Qt buffers but does not force the Windows file cache
        // to storage. The APFS/HFS+ commit callers require a checked OS flush as well.
        HANDLE handle = nativeFileHandle(file->handle());
        if (handle == INVALID_HANDLE_VALUE) {
            setError(errorMessage, QStringLiteral("No native file handle for durable flush"));
            return false;
        }
        if (FlushFileBuffers(handle) == 0) {
            setError(errorMessage,
                     QStringLiteral("Windows file flush failed: %1")
                         .arg(win32ErrorMessage(GetLastError())));
            return false;
        }
        return true;
    }
    // openFileOrRawDevice* only ever hands back a WindowsRawDevice or a QFile, both handled
    // above. An unrecognized QIODevice subtype cannot be proven durably flushed, so fail
    // closed rather than reporting a success we did not perform.
    setError(errorMessage, QStringLiteral("Cannot flush unrecognized device type"));
    return false;
}

std::unique_ptr<QIODevice> openFileOrRawDeviceReadOnly(const QString& path, QString* errorMessage) {
    if (path.trimmed().isEmpty()) {
        setError(errorMessage, QStringLiteral("Path is required"));
        return {};
    }

    if (isWindowsRawDevicePath(path)) {
        if (!requireCanonicalRawOpenPath(path, errorMessage)) {
            return {};
        }
        auto device = std::make_unique<WindowsRawDevice>(path, false);
        if (!device->open(QIODevice::ReadOnly)) {
            setError(errorMessage, device->errorString());
            return {};
        }
        return device;
    }

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

    if (isWindowsRawDevicePath(path)) {
        if (!requireCanonicalRawOpenPath(path, errorMessage)) {
            return {};
        }
        auto device = std::make_unique<WindowsRawDevice>(path, true);
        if (!device->open(QIODevice::ReadWrite)) {
            setError(errorMessage, device->errorString());
            return {};
        }
        return device;
    }

    auto file = std::make_unique<QFile>(path);
    if (!file->open(QIODevice::ReadWrite)) {
        setError(errorMessage, file->errorString());
        return {};
    }
    return file;
}

}  // namespace sak
