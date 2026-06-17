// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_registry.cpp
/// @brief File-system support capability registry for Partition Manager.

#include "sak/partition_file_system_registry.h"

#include <optional>

namespace sak {

namespace {

QString token(const QString& value) {
    return value.trimmed().toLower();
}

bool matches(const QString& value, const QStringList& aliases) {
    return aliases.contains(value, Qt::CaseInsensitive);
}

PartitionFileSystemCapability nativeWindowsCapability(const QString& id,
                                                      const QString& displayName,
                                                      const QStringList& aliases) {
    return {.id = id,
            .display_name = displayName,
            .support_level = QStringLiteral("Windows-native support"),
            .aliases = aliases,
            .available_actions = {QStringLiteral("Browse mounted volume"),
                                  QStringLiteral("Check/repair where Windows supports it"),
                                  QStringLiteral("Format/resize where safety checks pass")},
            .blocked_actions = {},
            .required_tools = {},
            .non_native = false};
}

PartitionFileSystemCapability extCapability(const QString& id) {
    return {
        .id = id,
        .display_name = id,
        .support_level = QStringLiteral("Detection plus manifest-gated e2fsprogs tool support"),
        .aliases = {id},
        .available_actions =
            {QStringLiteral("Read-only signature detection"),
             QStringLiteral("Read-only directory browse and selected-file extract with original "
                            "S.A.K. parser"),
             QStringLiteral("Read-only bounded recursive directory export"),
             QStringLiteral("Read-only e2fsck check when manifest-approved"),
             QStringLiteral("Confirmed ext format and repair through Pending Operations"),
             QStringLiteral("Confirmed ext grow through Pending Operations and resize2fs"),
             QStringLiteral("Confirmed ext shrink with e2fsck, resize2fs, and partition shrink")},
        .blocked_actions = {QStringLiteral("XFS/Btrfs/APFS write workflows and HFS+ file writes "
                                           "are not enabled by ext tool approval")},
        .required_tools = {QStringLiteral("e2fsck"),
                           QStringLiteral("mke2fs"),
                           QStringLiteral("resize2fs")},
        .non_native = true};
}

PartitionFileSystemCapability xfsCapability() {
    return {
        .id = QStringLiteral("xfs"),
        .display_name = QStringLiteral("XFS"),
        .support_level = QStringLiteral("Detection plus original read-only metadata checks"),
        .aliases = {QStringLiteral("xfs")},
        .available_actions = {QStringLiteral("Read-only signature detection"),
                              QStringLiteral("Read-only superblock metadata inspection"),
                              QStringLiteral(
                                  "Original read-only superblock metadata consistency check")},
        .blocked_actions =
            {QStringLiteral("Deep xfs_repair check requires manifest-approved xfs_repair"),
             QStringLiteral(
                 "Format and repair remain blocked until bundled tools and certification pass")},
        .required_tools = {QStringLiteral("xfs_repair")},
        .non_native = true};
}

PartitionFileSystemCapability btrfsCapability() {
    return {
        .id = QStringLiteral("btrfs"),
        .display_name = QStringLiteral("Btrfs"),
        .support_level = QStringLiteral("Detection plus original read-only metadata checks"),
        .aliases = {QStringLiteral("btrfs")},
        .available_actions = {QStringLiteral("Read-only signature detection"),
                              QStringLiteral("Read-only superblock metadata inspection"),
                              QStringLiteral(
                                  "Original read-only superblock metadata consistency check")},
        .blocked_actions = {QStringLiteral("Deep btrfs check requires manifest-approved btrfs"),
                            QStringLiteral(
                                "Repair remains disabled until destructive VM/lab certification "
                                "proves safe scenarios")},
        .required_tools = {QStringLiteral("btrfs")},
        .non_native = true};
}

PartitionFileSystemCapability hfsCapability(const QString& id, const QString& displayName) {
    return {
        .id = id,
        .display_name = displayName,
        .support_level = QStringLiteral(
            "Detection, original browse/extract, staged file mutation, and bundled format/repair"),
        .aliases = {displayName, id},
        .available_actions =
            {QStringLiteral("Read-only signature detection"),
             QStringLiteral("Read-only catalog browse and selected data-fork extract with original "
                            "S.A.K. parser"),
             QStringLiteral("Original read-only catalog consistency check"),
             QStringLiteral("Read-only HFS+ attributes B-tree key scan"),
             QStringLiteral("Read-only HFS+ selected attribute value extract"),
             QStringLiteral(
                 "Read-only HFS+ extents overflow resolution for catalog and data forks"),
             QStringLiteral("Read-only HFS+ resource-fork extract"),
             QStringLiteral(
                 "Read-only bounded recursive directory export with resource-fork sidecars"),
             QStringLiteral("Image-only same-size HFS+ data-fork overwrite with explicit "
                            "certification evidence"),
             QStringLiteral("Staged raw-partition HFS+ data/resource-fork replacement and truncate "
                            "through Pending Operations"),
             QStringLiteral("Staged raw-partition HFS+ bounded data/resource-fork allocation "
                            "growth through Pending Operations"),
             QStringLiteral("Staged raw-partition HFS+ empty-file create/delete with catalog "
                            "read-back verification"),
             QStringLiteral("Staged raw-partition HFS+ bounded file create with data-fork "
                            "allocation and read-back verification"),
             QStringLiteral("Staged raw-partition HFS+ allocated-file delete with allocation "
                            "bitmap release and optional released-block zeroing"),
             QStringLiteral("Staged raw-partition HFS+ empty-folder create/delete with catalog "
                            "read-back verification"),
             QStringLiteral("Staged raw-partition HFS+ bounded folder-tree delete with allocation "
                            "bitmap release"),
             QStringLiteral("Staged raw-partition HFS+ arbitrary-depth catalog B-tree split, "
                            "rebalance, and node-pool growth (certified to depth-4 / >256 leaf "
                            "nodes; Apple fsck_hfs + kernel RW mount)"),
             QStringLiteral("Staged raw-partition HFS+ inline attribute replacement through "
                            "Pending Operations"),
             QStringLiteral("Staged raw-partition HFS+ fork-backed attribute replacement within "
                            "allocated blocks"),
             QStringLiteral("Staged raw-partition HFS+ bounded fork-backed attribute allocation "
                            "growth through Pending Operations"),
             QStringLiteral("Confirmed sparse-staged HFS+/HFSX format through bundled newfs_hfs"),
             QStringLiteral("Confirmed sparse-staged HFS+/HFSX repair through bundled fsck_hfs")},
        .blocked_actions =
            {QStringLiteral("Raw-partition HFS+ complex file delete, unbounded folder-tree delete, "
                            "and broad allocation growth remain blocked "
                            "pending operation-specific certification"),
             QStringLiteral(
                 "Inline/broad attribute growth and recursive extents-overflow-file overflow "
                 "remain blocked in this milestone")},
        .required_tools = {QStringLiteral("newfs_hfs"),
                           QStringLiteral("fsck_hfs"),
                           QStringLiteral("sak_hfs_writer_cli")},
        .non_native = true};
}

PartitionFileSystemCapability apfsCapability() {
    return {
        .id = QStringLiteral("apfs"),
        .display_name = QStringLiteral("APFS"),
        .support_level = QStringLiteral(
            "Detection, original read-only browse/extract, and one-chunk generated "
            "APFS format/repair/root-file mutation"),
        .aliases = {QStringLiteral("APFS"), QStringLiteral("apfs")},
        .available_actions =
            {QStringLiteral("Read-only signature detection"),
             QStringLiteral("Read-only container checkpoint ring, object-map, volume-OID, and "
                            "volume-superblock candidate metadata inspection"),
             QStringLiteral("Original read-only container metadata consistency check"),
             QStringLiteral("Original read-only APFS volume browse, selected-file extraction, and "
                            "bounded recursive export"),
             QStringLiteral("Confirmed S.A.K. one-spaceman-chunk generated APFS create and format "
                            "through Pending Operations (64 MiB through 128 MiB targets)"),
             QStringLiteral("Confirmed one-spaceman-chunk generated APFS metadata-checksum repair "
                            "through Pending Operations"),
             QStringLiteral("Confirmed one-spaceman-chunk generated APFS root-file write, patch, "
                            "and delete through Pending Operations")},
        .blocked_actions = {QStringLiteral("Arbitrary existing Apple APFS mutation remains blocked "
                                           "by generated-layout guards"),
                            QStringLiteral(
                                "Generated APFS targets larger than one spaceman chunk require "
                                "multi-CIB spaceman support and Apple fsck validation"),
                            QStringLiteral("Encrypted/compressed files, file writes on arbitrary "
                                           "Apple APFS, and resize remain blocked")},
        .required_tools = {QStringLiteral("sak_apfs_writer_cli")},
        .non_native = true};
}

PartitionFileSystemCapability linuxSwapCapability() {
    return {.id = QStringLiteral("linux-swap"),
            .display_name = QStringLiteral("Linux swap"),
            .support_level =
                QStringLiteral("Detection plus original header metadata and confirmed format"),
            .aliases = {QStringLiteral("Linux swap"),
                        QStringLiteral("linux-swap"),
                        QStringLiteral("swap")},
            .available_actions = {QStringLiteral("Read-only signature detection"),
                                  QStringLiteral("Read-only swap header metadata inspection"),
                                  QStringLiteral(
                                      "Confirmed Linux swap format through Pending Operations")},
            .blocked_actions = {QStringLiteral(
                "Repair is not applicable to Linux swap; resize remains partition-only until "
                "operation-specific proof exists")},
            .required_tools = {},
            .non_native = true};
}

PartitionFileSystemCapability detectionOnlyCapability(const QString& id,
                                                      const QString& displayName,
                                                      const QStringList& blockedActions) {
    return {.id = id,
            .display_name = displayName,
            .support_level = QStringLiteral("Detection only"),
            .aliases = {displayName, id},
            .available_actions = {QStringLiteral("Read-only signature detection")},
            .blocked_actions = blockedActions,
            .required_tools = {},
            .non_native = true};
}

PartitionFileSystemCapability unidentifiedCapability() {
    return {.id = QStringLiteral("unknown"),
            .display_name = QStringLiteral("Unknown"),
            .support_level = QStringLiteral("Unknown"),
            .available_actions = {},
            .blocked_actions = {QStringLiteral(
                "Filesystem must be identified before S.A.K. can offer actions")},
            .non_native = false};
}

std::optional<PartitionFileSystemCapability> nativeCapabilityFor(const QString& value) {
    if (matches(value, {QStringLiteral("ntfs")})) {
        return nativeWindowsCapability(QStringLiteral("ntfs"),
                                       QStringLiteral("NTFS"),
                                       {QStringLiteral("ntfs")});
    }
    if (matches(value, {QStringLiteral("fat"), QStringLiteral("fat12")})) {
        return nativeWindowsCapability(QStringLiteral("fat12"),
                                       QStringLiteral("FAT12"),
                                       {QStringLiteral("fat"), QStringLiteral("fat12")});
    }
    if (matches(value, {QStringLiteral("fat16")})) {
        return nativeWindowsCapability(QStringLiteral("fat16"),
                                       QStringLiteral("FAT16"),
                                       {QStringLiteral("fat16")});
    }
    if (matches(value, {QStringLiteral("fat32")})) {
        return nativeWindowsCapability(QStringLiteral("fat32"),
                                       QStringLiteral("FAT32"),
                                       {QStringLiteral("fat32")});
    }
    if (matches(value, {QStringLiteral("exfat")})) {
        return nativeWindowsCapability(QStringLiteral("exfat"),
                                       QStringLiteral("exFAT"),
                                       {QStringLiteral("exfat")});
    }
    return std::nullopt;
}

std::optional<PartitionFileSystemCapability> nonNativeCapabilityFor(const QString& value) {
    if (matches(value, {QStringLiteral("ext2")})) {
        return extCapability(QStringLiteral("ext2"));
    }
    if (matches(value, {QStringLiteral("ext3")})) {
        return extCapability(QStringLiteral("ext3"));
    }
    if (matches(value, {QStringLiteral("ext4")})) {
        return extCapability(QStringLiteral("ext4"));
    }
    if (matches(value, {QStringLiteral("xfs")})) {
        return xfsCapability();
    }
    if (matches(value, {QStringLiteral("btrfs")})) {
        return btrfsCapability();
    }
    if (matches(value, {QStringLiteral("hfs+"), QStringLiteral("hfsplus")})) {
        return hfsCapability(QStringLiteral("hfsplus"), QStringLiteral("HFS+"));
    }
    if (matches(value, {QStringLiteral("hfsx")})) {
        return hfsCapability(QStringLiteral("hfsx"), QStringLiteral("HFSX"));
    }
    if (matches(value, {QStringLiteral("apfs")})) {
        return apfsCapability();
    }
    if (matches(value, {QStringLiteral("linux swap"), QStringLiteral("swap")})) {
        return linuxSwapCapability();
    }
    return std::nullopt;
}

}  // namespace

PartitionFileSystemCapability PartitionFileSystemRegistry::capabilityFor(
    const QString& file_system) {
    const QString value = token(file_system);
    if (value.isEmpty()) {
        return unidentifiedCapability();
    }
    if (const auto capability = nativeCapabilityFor(value); capability.has_value()) {
        return *capability;
    }
    if (const auto capability = nonNativeCapabilityFor(value); capability.has_value()) {
        return *capability;
    }
    return {.id = value,
            .display_name = file_system.trimmed(),
            .support_level = QStringLiteral("Unknown or Windows-protected"),
            .available_actions = {},
            .blocked_actions = {QStringLiteral(
                "S.A.K. has no registered filesystem workflow for this label")},
            .non_native = false};
}

QString PartitionFileSystemRegistry::actionSummary(const QStringList& actions) {
    if (actions.isEmpty()) {
        return QStringLiteral("None");
    }
    return actions.join(QStringLiteral("; "));
}

}  // namespace sak
