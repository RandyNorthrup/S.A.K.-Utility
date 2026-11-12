// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_registry.h
/// @brief File-system support capability registry for Partition Manager.

#pragma once

#include <QString>
#include <QStringList>

namespace sak {

struct PartitionVolumeInfo;

struct PartitionFileSystemCapability {
    QString id;
    QString display_name;
    QString support_level;
    QStringList aliases;
    QStringList available_actions;
    QStringList blocked_actions;
    QStringList required_tools;
    bool non_native{false};
};

class PartitionFileSystemRegistry {
public:
    [[nodiscard]] static PartitionFileSystemCapability capabilityFor(const QString& file_system);
    [[nodiscard]] static QString actionSummary(const QStringList& actions);
    /// A volume whose file system the disk scan's raw probe identified as one Windows does not
    /// mount (ext, HFS+, APFS, Linux swap). Windows shows such a volume RAW, so fsutil and
    /// BitLocker have nothing to answer about it, and their failure says nothing about the scan
    /// or the volume. A RAW volume nothing identified is not one: that is what an unreadable
    /// volume looks like.
    [[nodiscard]] static bool identifiedNonWindowsVolume(const PartitionVolumeInfo& volume);
};

}  // namespace sak
