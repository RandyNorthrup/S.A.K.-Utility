// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_registry.h
/// @brief File-system support capability registry for Partition Manager.

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

namespace sak {

struct PartitionVolumeInfo;

/// What a Windows format (Format-Volume) is asked to make.
struct WindowsFormatChoice {
    QString file_system;
    uint64_t partition_bytes{0};
    /// 0: Windows' default for the size.
    uint64_t allocation_unit_bytes{0};
    QString label;
    /// This edition of Windows formats ReFS (PartitionInventory::refs_format_available).
    bool refs_available{false};
};

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
    /// Why Windows would not resize a @p file_system partition from @p current_bytes to
    /// @p target_bytes with Resize-Partition, in plain words; empty for NTFS and ReFS, which it
    /// resizes (its supported-size answer then says how far). Measured on testpc1 (2026-11-12):
    /// Resize-Partition answered "Not Supported" for FAT32 and exFAT, shrinking and growing, while
    /// Get-PartitionSupportedSize offered room to grow them. ext is resized with S.A.K.'s bundled
    /// tools, not here; any other file system is not resized at all.
    [[nodiscard]] static QString windowsResizeBlocker(const QString& file_system,
                                                      uint64_t current_bytes,
                                                      uint64_t target_bytes);

    /// Why Windows' Format-Volume would refuse @p choice, or drop its label, in plain words;
    /// empty when it would make what was asked. Format-Volume writes the start of the volume
    /// before it refuses a FAT32 cluster size: an NTFS volume asked for FAT32 with 4 KiB clusters
    /// on 256 MiB was left RAW (operation proof and testpc1, 2026-11-11). So every refusal is
    /// found here, at queue time and again before the script runs. Measured with Format-Volume on
    /// Windows 11 Pro 25H2 (testpc1, 2026-11-11): FAT32 refused 4 KiB clusters on 256 and
    /// 257 MiB, 512 bytes on 32 and 33 MiB, and 32 KiB on 2049 MiB, and formatted 2 KiB on
    /// 256 MiB, 512 bytes on 40 MiB and 40 GiB with 32 KiB; Windows' own FAT32 default was always
    /// one it formats; a FAT32 or exFAT label over 11 characters, a FAT32 label with * and an
    /// NTFS label over 32 were dropped without an error; ReFS was refused on Pro, with the old
    /// file system left. Get-SupportedClusterSizes and Get-SupportedFileSystems are not that
    /// answer: they list no exFAT size at all, and no FAT32 on 40 MiB or 40 GiB, which
    /// Format-Volume formats.
    [[nodiscard]] static QString windowsFormatBlocker(const WindowsFormatChoice& choice);
    /// The file systems S.A.K. formats with its bundled tools or writers, not Windows: ext2, ext3,
    /// ext4, HFS+, HFSX, APFS and Linux swap.
    [[nodiscard]] static bool formattedByBundledTools(const QString& file_system);
    /// Why Create does not make a @p file_system partition, in plain words, or empty for a file
    /// system Windows formats. Create makes a partition with a Windows file system only; one of
    /// the bundled tools' file systems is made by Formatting it afterwards.
    [[nodiscard]] static QString createFileSystemBlocker(const QString& file_system);
    /// Whether a Windows edition (the registry's EditionID) formats ReFS: Enterprise, Pro for
    /// Workstations, IoT Enterprise and Server editions. Education is left out: not confirmed.
    [[nodiscard]] static bool editionFormatsRefs(const QString& edition_id);
    /// Assert-SakWindowsFormatChoice: the size, cluster and edition rules of
    /// windowsFormatBlocker in Windows PowerShell, for a format script to run on the partition as
    /// it is then, before anything is written. It also checks the disk's physical sector size,
    /// which Windows requires an NTFS or ReFS cluster to be at least.
    [[nodiscard]] static QString windowsFormatCheckFunctionScript();
};

}  // namespace sak
