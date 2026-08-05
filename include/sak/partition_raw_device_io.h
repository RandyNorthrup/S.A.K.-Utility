// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_raw_device_io.h
/// @brief File/raw-device openers for Partition Manager parsers and certified writers.

#pragma once

#include <QString>

#include <cstdint>
#include <memory>
#include <optional>

class QIODevice;

namespace sak {

/// @brief Single raw-device classification authority for every caller (plan-time safety
///        validator, raw I/O, clone/image script builder). Recognizes the \\.\ device
///        namespace, \\?\PhysicalDriveN, the \\?\GLOBALROOT\ device namespace, a BARE
///        \\?\Volume{GUID} handle, and POSIX /dev/ nodes. A \\?\Volume{GUID}\dir\file form
///        has a separator after the closing brace and is an ordinary extended-length FILE
///        path; the generic \\?\ long-path prefix is deliberately NOT matched.
[[nodiscard]] bool isWindowsRawDevicePath(const QString& path);

/// @brief Canonical \\.\ device-namespace spelling of @p path, or an empty string when it is
///        not a Windows raw device. \\?\PhysicalDriveN, \\?\GLOBALROOT\... and a bare
///        \\?\Volume{GUID} name the SAME device objects as their \\.\ spellings, but the
///        emitted executor PowerShell recognizes only the \\.\ form (that is what selects the
///        take-offline / IsBoot re-check and raw open semantics). Callers normalize through
///        this one authority instead of each re-deriving the rule.
[[nodiscard]] QString canonicalRawDevicePath(const QString& path);

/// @brief Physical drive number a raw-device path addresses, accepting both the \\.\ and the
///        \\?\ PhysicalDriveN spellings. Returns nullopt when @p path is not a physical-drive
///        device path, so an unresolvable target fails closed at the call site.
[[nodiscard]] std::optional<uint32_t> rawDevicePhysicalDriveNumber(const QString& path);

/// @brief Best-effort mark an open file (by descriptor) sparse so a large logical size
///        materializes only written regions. No-op where unsupported; POSIX files are
///        already sparse via ftruncate. @p fileDescriptor is QFile::handle().
void markFileSparse(int fileDescriptor);

/// @brief Copy @p source to @p destination preserving sparseness, so a multi-TiB
///        container with mostly-hole content (only its metadata written) copies in the
///        size of its allocated data, not its logical size. On Windows this reads only
///        the source's allocated ranges into a sparse destination; elsewhere it falls
///        back to a hole-preserving file copy. @p destination must not already exist.
[[nodiscard]] bool copyFileSparse(const QString& source,
                                  const QString& destination,
                                  QString* errorMessage = nullptr);

/// @brief Flush a writable file/raw-device's buffers to durable storage. Returns false (with
///        @p errorMessage set) when the flush fails, so a caller can refuse to report a
///        durable write on an unflushed target rather than swallowing the failure. Handles
///        both a QFileDevice (QFile::flush) and the Windows raw-device helper
///        (FlushFileBuffers). A null device is treated as a failure.
[[nodiscard]] bool flushDeviceBuffers(QIODevice* device, QString* errorMessage = nullptr);

[[nodiscard]] std::unique_ptr<QIODevice> openFileOrRawDeviceReadOnly(
    const QString& path, QString* errorMessage = nullptr);

[[nodiscard]] std::unique_ptr<QIODevice> openFileOrRawDeviceReadWrite(
    const QString& path, QString* errorMessage = nullptr);

}  // namespace sak
