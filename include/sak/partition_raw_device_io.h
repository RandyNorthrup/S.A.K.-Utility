// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_raw_device_io.h
/// @brief File/raw-device openers for Partition Manager parsers and certified writers.

#pragma once

#include <QString>

#include <memory>

class QIODevice;

namespace sak {

[[nodiscard]] bool isWindowsRawDevicePath(const QString& path);

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

[[nodiscard]] std::unique_ptr<QIODevice> openFileOrRawDeviceReadOnly(
    const QString& path, QString* errorMessage = nullptr);

[[nodiscard]] std::unique_ptr<QIODevice> openFileOrRawDeviceReadWrite(
    const QString& path, QString* errorMessage = nullptr);

}  // namespace sak
