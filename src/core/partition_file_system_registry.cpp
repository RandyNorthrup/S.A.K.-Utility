// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_registry.cpp
/// @brief File-system support capability registry for Partition Manager.

#include "sak/partition_file_system_registry.h"

#include "sak/partition_file_system_detector.h"
#include "sak/partition_manager_types.h"

#include <QRegularExpression>

#include <algorithm>
#include <optional>

namespace sak {

namespace {

// Windows' format rules (windowsFormatBlocker). FAT32 holds 65,525 to 268,435,445 clusters.
// Windows lays FAT32 out with its first data cluster past a reserved area and two FATs, and the
// clusters it counts are fewer than size / cluster: within 2 MiB plus both FATs of the size, the
// rule every measurement above fits (256 MiB with 4 KiB clusters, refused, holds 64,896 here).
constexpr uint64_t kFat32MinimumClusters = 65'525;
constexpr uint64_t kFat32MaximumClusters = 268'435'445;
constexpr uint64_t kFat32LayoutReserveBytes = 2ULL * 1024ULL * 1024ULL;
constexpr uint64_t kFat32EntryBytes = 4;
constexpr uint64_t kFat32Copies = 2;
constexpr uint64_t kSmallestClusterBytes = 512;
constexpr uint64_t kLargestClusterBytes = 64ULL * 1024ULL;
constexpr uint64_t kExfatMaximumClusters = 4'294'967'285ULL;
constexpr uint64_t kNtfsMaximumClusters = 4'294'967'295ULL;
constexpr uint64_t kRefsSmallClusterBytes = 4ULL * 1024ULL;
constexpr qsizetype kFatLabelMaxChars = 11;
constexpr qsizetype kNtfsLabelMaxChars = 32;
constexpr char16_t kFirstPrintableAscii = 0x20;
constexpr char16_t kLastPrintableAscii = 0x7E;
constexpr auto kFatLabelForbidden = "*?.,;:/\\|+=<>[]\"";
constexpr auto kExfatLabelForbidden = "*?/\\|:<>\"";
constexpr auto kRefsEditionPattern = "^(Enterprise|ProfessionalWorkstation|IoTEnterprise|Server)";

constexpr auto kNotWindowsFormatMessage = "Windows formats NTFS, FAT32, exFAT and ReFS only.";
constexpr auto kRefsEditionMessage =
    "This edition of Windows cannot format ReFS: Windows formats it on its Enterprise, Pro for "
    "Workstations and Server editions. Choose NTFS.";
constexpr auto kRefsClusterMessage = "ReFS uses 4 KB or 64 KB clusters.";
constexpr auto kFat32TooSmallMessage =
    "The partition is too small for FAT32: Windows needs room for at least 65,525 clusters. "
    "Choose exFAT or NTFS, or a larger partition.";
constexpr auto kFat32FewClustersMessage =
    "Windows cannot format this partition as FAT32 with {0}-byte clusters: that makes fewer than "
    "the 65,525 clusters FAT32 needs. Choose a smaller cluster size.";
constexpr auto kTooManyClustersMessage =
    "Windows cannot format this partition as {0} with {1}-byte clusters: that makes more clusters "
    "than {0} can hold. Choose a larger cluster size.";
constexpr auto kSectorMessage =
    "Windows formats {0} on this disk only with clusters of at least {1} bytes, its physical "
    "sector size. Choose a larger cluster size.";
constexpr auto kLabelLengthMessage =
    "A {0} label has at most {1} characters, and Windows drops a longer one without saying so. "
    "Choose a shorter label.";
constexpr auto kFatLabelMessage =
    "A FAT32 label can use letters A to Z, digits, spaces and plain marks, but not * ? . , ; : / "
    "\\ | + = < > [ ] or \", and Windows drops any other label without saying so. Choose another "
    "label.";
constexpr auto kExfatLabelMessage =
    "An exFAT label cannot contain * ? / \\ | : < > \" or control characters, and Windows drops "
    "such a label without saying so. Choose another label.";
constexpr auto kNtfsLabelMessage =
    "A {0} label cannot contain control characters, and Windows drops such a label without "
    "saying so. Choose another label.";

QString formatMessage(const char* message, const QStringList& values) {
    QString text = QString::fromLatin1(message);
    for (qsizetype index = 0; index < values.size(); ++index) {
        text.replace(QStringLiteral("{%1}").arg(index), values.at(index));
    }
    return text;
}

QString powerShellQuoted(const char* text) {
    return QStringLiteral("'") +
           QString::fromLatin1(text).replace(QLatin1Char('\''), QStringLiteral("''")) +
           QStringLiteral("'");
}

/// The clusters Windows' FAT32 layout leaves for data (see kFat32LayoutReserveBytes).
uint64_t fat32DataClusters(uint64_t size_bytes, uint64_t cluster_bytes) {
    const uint64_t total = size_bytes / cluster_bytes;
    const uint64_t layout = kFat32LayoutReserveBytes + kFat32Copies * kFat32EntryBytes * total;
    return size_bytes > layout ? (size_bytes - layout) / cluster_bytes : 0;
}

QString fat32ClusterBlocker(uint64_t size_bytes, uint64_t cluster_bytes) {
    if (fat32DataClusters(size_bytes, kSmallestClusterBytes) < kFat32MinimumClusters) {
        return QString::fromLatin1(kFat32TooSmallMessage);
    }
    if (cluster_bytes != 0 &&
        fat32DataClusters(size_bytes, cluster_bytes) < kFat32MinimumClusters) {
        return formatMessage(kFat32FewClustersMessage, {QString::number(cluster_bytes)});
    }
    const uint64_t upper_cluster = cluster_bytes == 0 ? kLargestClusterBytes : cluster_bytes;
    if (size_bytes / upper_cluster > kFat32MaximumClusters) {
        return formatMessage(kTooManyClustersMessage,
                             {QStringLiteral("FAT32"), QString::number(upper_cluster)});
    }
    return {};
}

QString clusterBlocker(const QString& fs, uint64_t size_bytes, uint64_t cluster_bytes) {
    if (fs == QStringLiteral("FAT32")) {
        return fat32ClusterBlocker(size_bytes, cluster_bytes);
    }
    if (cluster_bytes == 0) {
        return {};
    }
    if (fs == QStringLiteral("REFS") && cluster_bytes != kRefsSmallClusterBytes &&
        cluster_bytes != kLargestClusterBytes) {
        return QString::fromLatin1(kRefsClusterMessage);
    }
    const uint64_t maximum = fs == QStringLiteral("EXFAT") ? kExfatMaximumClusters
                                                           : kNtfsMaximumClusters;
    if (size_bytes / cluster_bytes > maximum) {
        return formatMessage(kTooManyClustersMessage,
                             {fs == QStringLiteral("EXFAT") ? QStringLiteral("exFAT")
                                                            : QStringLiteral("NTFS"),
                              QString::number(cluster_bytes)});
    }
    return {};
}

bool hasControlCharacter(const QString& text) {
    return std::ranges::any_of(text,
                               [](QChar ch) { return ch.category() == QChar::Other_Control; });
}

bool fatLabelRefuses(QChar ch) {
    return ch.unicode() < kFirstPrintableAscii || ch.unicode() > kLastPrintableAscii ||
           QString::fromLatin1(kFatLabelForbidden).contains(ch);
}

bool exfatLabelRefuses(QChar ch) {
    return ch.category() == QChar::Other_Control ||
           QString::fromLatin1(kExfatLabelForbidden).contains(ch);
}

/// The name a person knows @p fs (upper case) by.
QString windowsFormatName(const QString& fs) {
    if (fs == QStringLiteral("EXFAT")) {
        return QStringLiteral("exFAT");
    }
    return fs == QStringLiteral("REFS") ? QStringLiteral("ReFS") : fs;
}

QString labelCharacterBlocker(const QString& fs, const QString& label) {
    if (fs == QStringLiteral("FAT32")) {
        return std::ranges::any_of(label, fatLabelRefuses) ? QString::fromLatin1(kFatLabelMessage)
                                                           : QString();
    }
    if (fs == QStringLiteral("EXFAT")) {
        return std::ranges::any_of(label, exfatLabelRefuses)
                   ? QString::fromLatin1(kExfatLabelMessage)
                   : QString();
    }
    return hasControlCharacter(label) ? formatMessage(kNtfsLabelMessage, {windowsFormatName(fs)})
                                      : QString();
}

QString labelBlocker(const QString& fs, const QString& label) {
    const qsizetype maximum = fs == QStringLiteral("FAT32") || fs == QStringLiteral("EXFAT")
                                  ? kFatLabelMaxChars
                                  : kNtfsLabelMaxChars;
    if (label.size() > maximum) {
        return formatMessage(kLabelLengthMessage,
                             {windowsFormatName(fs), QString::number(maximum)});
    }
    return labelCharacterBlocker(fs, label);
}

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
        .blocked_actions = {QStringLiteral("Writing files inside an ext volume is not available: "
                                           "S.A.K. Utility reads and copies out ext files only")},
        .required_tools = {QStringLiteral("e2fsck"),
                           QStringLiteral("mke2fs"),
                           QStringLiteral("resize2fs")},
        .non_native = true};
}

PartitionFileSystemCapability xfsCapability() {
    return {.id = QStringLiteral("xfs"),
            .display_name = QStringLiteral("XFS"),
            .support_level = QStringLiteral("Detection plus original read-only metadata checks"),
            .aliases = {QStringLiteral("xfs")},
            .available_actions = {QStringLiteral("Read-only signature detection"),
                                  QStringLiteral("Read-only superblock metadata inspection"),
                                  QStringLiteral(
                                      "Original read-only superblock metadata consistency check")},
            // No XFS tool ships (tools/filesystem/manifest.json holds e2fsprogs and hfsprogs
            // only), so none is listed as a required bundled tool, and the blocked entry says
            // why plainly: it read as though tools were bundled and awaiting tests.
            .blocked_actions = {QStringLiteral(
                "XFS format, repair and the deep check (xfs_repair) are not available because "
                "S.A.K. Utility includes no XFS tools")},
            .required_tools = {},
            .non_native = true};
}

PartitionFileSystemCapability btrfsCapability() {
    return {.id = QStringLiteral("btrfs"),
            .display_name = QStringLiteral("Btrfs"),
            .support_level = QStringLiteral("Detection plus original read-only metadata checks"),
            .aliases = {QStringLiteral("btrfs")},
            .available_actions = {QStringLiteral("Read-only signature detection"),
                                  QStringLiteral("Read-only superblock metadata inspection"),
                                  QStringLiteral(
                                      "Original read-only superblock metadata consistency check")},
            // No Btrfs tool ships either; see xfsCapability.
            .blocked_actions = {QStringLiteral(
                "Btrfs format, repair and the deep check (btrfs check) are not available because "
                "S.A.K. Utility includes no Btrfs tools")},
            .required_tools = {},
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
             QStringLiteral("Image-only same-size HFS+ data-fork overwrite with an explicit "
                            "evidence ID"),
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
                            "rebalance, and node-pool growth (validated to depth-4 / >256 leaf "
                            "nodes with Apple fsck_hfs and a kernel read-write mount)"),
             QStringLiteral("Staged raw-partition HFS+ inline attribute replacement through "
                            "Pending Operations"),
             QStringLiteral("Staged raw-partition HFS+ fork-backed attribute replacement within "
                            "allocated blocks"),
             QStringLiteral("Staged raw-partition HFS+ bounded fork-backed attribute allocation "
                            "growth through Pending Operations"),
             QStringLiteral("Confirmed sparse-staged HFS+/HFSX format through bundled newfs_hfs"),
             QStringLiteral("Confirmed sparse-staged HFS+/HFSX repair through bundled fsck_hfs")},
        .blocked_actions =
            {QStringLiteral("Raw-partition HFS+ complex file delete, unbounded folder-tree delete "
                            "and broad allocation growth are not available"),
             QStringLiteral("Inline or broad attribute growth and recursive extents-overflow-file "
                            "overflow are not available")},
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
            "Detection, original read-only browse/extract, generated APFS format/repair, and "
            "in-place file and directory writes on S.A.K.-generated and Apple-created containers"),
        .aliases = {QStringLiteral("APFS"), QStringLiteral("apfs")},
        .available_actions =
            {QStringLiteral("Read-only signature detection"),
             QStringLiteral("Read-only container checkpoint ring, object-map, volume-OID, and "
                            "volume-superblock candidate metadata inspection"),
             QStringLiteral("Original read-only container metadata consistency check"),
             QStringLiteral("Original read-only APFS volume browse, selected-file extraction, and "
                            "bounded recursive export"),
             QStringLiteral("Confirmed S.A.K. single-CIB, multi-CIB, metadata-overflow, and "
                            "CAB-tier generated APFS create and format through Pending Operations "
                            "(64 MiB through 24 TiB targets; validated with Apple fsck_apfs and a "
                            "kernel read-write mount, incl. the CAB tier at 8 TiB / cab_count 2)"),
             QStringLiteral("Confirmed generated APFS metadata-checksum repair through Pending "
                            "Operations (64 MiB through 24 TiB targets)"),
             QStringLiteral("Confirmed in-place file and directory write, patch, rename, move, and "
                            "delete on S.A.K.-generated and Apple-created containers through "
                            "Pending Operations")},
        .blocked_actions = {QStringLiteral(
                                "Metadata-checksum repair is limited to S.A.K.-generated "
                                "layouts; generated format/repair targets above the "
                                "~24 TiB / 48 TiB-engine CAB ceiling remain "
                                "blocked"),
                            QStringLiteral("Fusion and multi-device containers, sealed "
                                           "(signed-system) volumes, and case-sensitive volumes "
                                           "refuse every write"),
                            QStringLiteral("Encrypted volumes stay locked until their password or "
                                           "recovery key is supplied")},
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
                "Repair does not apply to Linux swap, and resize changes only the partition, not "
                "the swap area inside it")},
            .required_tools = {},
            .non_native = true};
}

PartitionFileSystemCapability unidentifiedCapability() {
    return {.id = QStringLiteral("unknown"),
            .display_name = QStringLiteral("Unknown"),
            .support_level = QStringLiteral("Unknown"),
            .aliases = {},
            .available_actions = {},
            .blocked_actions = {QStringLiteral(
                "Filesystem must be identified before S.A.K. can offer actions")},
            .required_tools = {},
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
            .aliases = {},
            .available_actions = {},
            .blocked_actions = {QStringLiteral(
                "S.A.K. has no registered filesystem workflow for this label")},
            .required_tools = {},
            .non_native = false};
}

QString PartitionFileSystemRegistry::actionSummary(const QStringList& actions) {
    if (actions.isEmpty()) {
        return QStringLiteral("None");
    }
    return actions.join(QStringLiteral("; "));
}

bool PartitionFileSystemRegistry::identifiedNonWindowsVolume(const PartitionVolumeInfo& volume) {
    const QString& source = volume.file_system_source;
    const bool found_by_raw_probe = source != PartitionFileSystemDetector::windowsVolumeSource() &&
                                    source !=
                                        PartitionFileSystemDetector::inferredProtectedSource();
    return found_by_raw_probe && !volume.file_system.trimmed().isEmpty() &&
           capabilityFor(volume.file_system).non_native;
}

QString PartitionFileSystemRegistry::windowsFormatBlocker(const WindowsFormatChoice& choice) {
    const QString fs = choice.file_system.trimmed().toUpper();
    if (fs != QStringLiteral("NTFS") && fs != QStringLiteral("FAT32") &&
        fs != QStringLiteral("EXFAT") && fs != QStringLiteral("REFS")) {
        return QString::fromLatin1(kNotWindowsFormatMessage);
    }
    if (fs == QStringLiteral("REFS") && !choice.refs_available) {
        return QString::fromLatin1(kRefsEditionMessage);
    }
    if (const QString cluster =
            clusterBlocker(fs, choice.partition_bytes, choice.allocation_unit_bytes);
        !cluster.isEmpty()) {
        return cluster;
    }
    return labelBlocker(fs, choice.label);
}

bool PartitionFileSystemRegistry::formattedByBundledTools(const QString& file_system) {
    return matches(file_system.trimmed(),
                   {QStringLiteral("ext2"),
                    QStringLiteral("ext3"),
                    QStringLiteral("ext4"),
                    QStringLiteral("HFS+"),
                    QStringLiteral("HFSX"),
                    QStringLiteral("hfsplus"),
                    QStringLiteral("APFS"),
                    QStringLiteral("Linux swap"),
                    QStringLiteral("linux-swap"),
                    QStringLiteral("swap")});
}

QString PartitionFileSystemRegistry::createFileSystemBlocker(const QString& file_system) {
    if (formattedByBundledTools(file_system)) {
        return QStringLiteral(
                   "S.A.K. creates partitions with a Windows file system only. Create the "
                   "partition with a Windows file system, then Format it as %1.")
            .arg(file_system.trimmed());
    }
    return {};
}

bool PartitionFileSystemRegistry::editionFormatsRefs(const QString& edition_id) {
    static const QRegularExpression kEditions(QString::fromLatin1(kRefsEditionPattern),
                                              QRegularExpression::CaseInsensitiveOption);
    return kEditions.match(edition_id.trimmed()).hasMatch();
}

QString PartitionFileSystemRegistry::windowsFormatCheckFunctionScript() {
    // The rules of windowsFormatBlocker that depend on the partition and the PC: its size, the
    // edition, and the disk's physical sector size. A label does not change, so it is checked
    // before the script is made. Placeholders: the edition pattern and messages.
    return QStringLiteral(R"PS(
function Get-SakFat32DataClusters([uint64]$SizeBytes, [uint64]$ClusterBytes) {
  $total = [math]::Floor($SizeBytes / $ClusterBytes)
  $layout = [double]%1 + [double]%2 * $total
  if ([double]$SizeBytes -le $layout) { return 0 }
  return [math]::Floor(([double]$SizeBytes - $layout) / $ClusterBytes)
}
function Assert-SakWindowsFormatChoice([uint32]$DiskNumber, [uint64]$SizeBytes, [string]$FileSystem, [uint64]$AllocationUnit) {
  $fs = $FileSystem.Trim()
  $problem = ''
  if ($fs -eq 'ReFS') {
    $edition = [string](Get-ItemProperty -LiteralPath 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion' -ErrorAction Stop).EditionID
    if ($edition -notmatch %3) { $problem = %4 }
  }
  if (-not $problem -and ($fs -eq 'NTFS' -or $fs -eq 'ReFS') -and $AllocationUnit -gt 0) {
    $sector = [uint64](Get-Disk -Number $DiskNumber -ErrorAction Stop).PhysicalSectorSize
    if ($sector -gt 0 -and $AllocationUnit -lt $sector) { $problem = (%5 -f $fs, $sector) }
  }
  if (-not $problem -and $fs -eq 'FAT32') {
    $upper = $(if ($AllocationUnit -gt 0) { $AllocationUnit } else { %6 })
    if ((Get-SakFat32DataClusters $SizeBytes %7) -lt %8) { $problem = %9 }
    elseif ($AllocationUnit -gt 0 -and (Get-SakFat32DataClusters $SizeBytes $AllocationUnit) -lt %8) { $problem = (%10 -f $AllocationUnit) }
    elseif ([math]::Floor($SizeBytes / $upper) -gt %11) { $problem = (%12 -f 'FAT32', $upper) }
  }
  if (-not $problem -and $AllocationUnit -gt 0) {
    if ($fs -eq 'exFAT' -and [math]::Floor($SizeBytes / $AllocationUnit) -gt %13) { $problem = (%12 -f 'exFAT', $AllocationUnit) }
    if ($fs -eq 'NTFS' -and [math]::Floor($SizeBytes / $AllocationUnit) -gt %14) { $problem = (%12 -f 'NTFS', $AllocationUnit) }
  }
  if ($problem) { throw ('Nothing was changed: ' + $problem) }
  Write-Output ('Windows formats {0} with these choices on this partition' -f $fs)
}
)PS")
        .arg(QString::number(kFat32LayoutReserveBytes),
             QString::number(kFat32Copies * kFat32EntryBytes),
             powerShellQuoted(kRefsEditionPattern),
             powerShellQuoted(kRefsEditionMessage),
             powerShellQuoted(kSectorMessage),
             QString::number(kLargestClusterBytes),
             QString::number(kSmallestClusterBytes),
             QString::number(kFat32MinimumClusters),
             powerShellQuoted(kFat32TooSmallMessage))
        .arg(powerShellQuoted(kFat32FewClustersMessage),
             QString::number(kFat32MaximumClusters),
             powerShellQuoted(kTooManyClustersMessage),
             QString::number(kExfatMaximumClusters),
             QString::number(kNtfsMaximumClusters));
}

QString PartitionFileSystemRegistry::windowsResizeBlocker(const QString& file_system,
                                                          uint64_t current_bytes,
                                                          uint64_t target_bytes) {
    const QString fs = file_system.trimmed();
    if (fs.isEmpty() || matches(fs, {QStringLiteral("RAW"), QStringLiteral("Unknown")})) {
        return QStringLiteral(
            "S.A.K. could not identify this partition's file system, so it will not resize it. "
            "Run Scan Disks as administrator first.");
    }
    if (matches(fs, {QStringLiteral("NTFS"), QStringLiteral("ReFS")})) {
        return {};
    }
    if (matches(fs, {QStringLiteral("FAT32"), QStringLiteral("exFAT")})) {
        return QStringLiteral(
                   "Windows cannot %1 FAT32 or exFAT partitions. Back up the files, then delete "
                   "and recreate the partition at the size you want.")
            .arg(target_bytes < current_bytes ? QStringLiteral("shrink") : QStringLiteral("grow"));
    }
    return QStringLiteral(
               "S.A.K. cannot resize a %1 partition. Windows resizes NTFS and ReFS partitions, and "
               "S.A.K. resizes ext2, ext3 and ext4 with its bundled tools.")
        .arg(fs);
}

}  // namespace sak
