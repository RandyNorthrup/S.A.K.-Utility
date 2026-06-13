// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_detector.h
/// @brief Read-only raw file-system signature detector for Partition Manager.

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

#include <cstdint>
#include <optional>

class QIODevice;

namespace sak {

struct PartitionFileSystemDetection {
    QString file_system;
    QString source;
    uint64_t total_bytes{0};
    uint64_t free_bytes{0};
    QStringList details;
};

class PartitionFileSystemDetector {
public:
    [[nodiscard]] static qsizetype maxProbeBytes() noexcept;
    [[nodiscard]] static QString rawSignatureSource();
    [[nodiscard]] static QString windowsVolumeSource();
    [[nodiscard]] static QString inferredProtectedSource();
    [[nodiscard]] static QString sourceDisplayName(const QString& source);

    [[nodiscard]] static std::optional<PartitionFileSystemDetection> detectBytes(
        const QByteArray& bytes, uint64_t partition_size_bytes = 0);
    [[nodiscard]] static qsizetype probeReadLimit(uint64_t partition_size_bytes) noexcept;
    [[nodiscard]] static std::optional<QByteArray> readProbeBytesFromDevicePath(
        const QString& device_path,
        uint64_t partition_offset_bytes,
        uint64_t partition_size_bytes,
        QString* error_message = nullptr);
    [[nodiscard]] static std::optional<PartitionFileSystemDetection> detectFromDevice(
        QIODevice* device,
        uint64_t partition_offset_bytes,
        uint64_t partition_size_bytes,
        QString* error_message = nullptr);
    [[nodiscard]] static std::optional<PartitionFileSystemDetection> detectFromDevicePath(
        const QString& device_path,
        uint64_t partition_offset_bytes,
        uint64_t partition_size_bytes,
        QString* error_message = nullptr);
};

}  // namespace sak
