// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_os_migration.cpp
/// @brief What Migrate OS will do to a target of a given size, worked out from the inventory.

#include "sak/partition_os_migration.h"

#include <QJsonArray>

#include <algorithm>
#include <limits>

namespace sak {

namespace {

// The image route warns when the shadow-copy storage left is under this share of the bytes
// it captures (shadowStorageTooSmallForCapture).
constexpr uint64_t kShadowStorageCaptureShare = 4;

uint64_t alignUp(uint64_t value) {
    const uint64_t remainder = value % kOsMigrationAlignmentBytes;
    return remainder == 0 ? value : value + (kOsMigrationAlignmentBytes - remainder);
}

uint64_t alignDown(uint64_t value) {
    return value - (value % kOsMigrationAlignmentBytes);
}

bool hasSnapshotFileSystem(const PartitionInfoEx& partition) {
    if (!partition.volume) {
        return false;
    }
    const QString file_system = partition.volume->file_system.trimmed().toUpper();
    return file_system == QStringLiteral("NTFS") || file_system == QStringLiteral("REFS");
}

OsMigrationPartitionKind kindOf(const PartitionInfoEx& partition) {
    if (partition.is_boot) {
        return OsMigrationPartitionKind::Windows;
    }
    if (partition.is_efi || partition.is_system || partition.is_active) {
        return OsMigrationPartitionKind::System;
    }
    if (partition.is_msr) {
        return OsMigrationPartitionKind::Reserved;
    }
    if (partition.is_recovery) {
        return OsMigrationPartitionKind::Recovery;
    }
    return partition.volume && !partition.volume->file_system.trimmed().isEmpty()
               ? OsMigrationPartitionKind::Data
               : OsMigrationPartitionKind::Other;
}

OsMigrationPartition planEntry(const PartitionInfoEx& partition) {
    OsMigrationPartition entry;
    entry.source_number = partition.partition_number.value();
    entry.source_offset = partition.offset_bytes;
    entry.source_size = partition.size_bytes;
    entry.initial_size = partition.size_bytes;
    entry.final_size = partition.size_bytes;
    entry.kind = kindOf(partition);
    if (partition.volume) {
        entry.drive_letter = partition.volume->drive_letter;
        entry.volume_path = partition.volume->volume_guid;
        entry.file_system = partition.volume->file_system;
    }
    entry.from_snapshot = hasSnapshotFileSystem(partition) &&
                          (entry.kind == OsMigrationPartitionKind::Windows ||
                           entry.kind == OsMigrationPartitionKind::Data);
    return entry;
}

bool isLogicalOrExtended(const PartitionInfoEx& partition) {
    return partition.type_name.contains(QStringLiteral("Extended"), Qt::CaseInsensitive) ||
           partition.type_name.contains(QStringLiteral("Logical"), Qt::CaseInsensitive);
}

bool isGpt(const PartitionDiskInfo& disk) {
    return disk.partition_style.compare(QStringLiteral("GPT"), Qt::CaseInsensitive) == 0;
}

bool isMbr(const PartitionDiskInfo& disk) {
    return disk.partition_style.compare(QStringLiteral("MBR"), Qt::CaseInsensitive) == 0;
}

QString bitLockerBlocker(const PartitionVolumeInfo& volume) {
    const QString name = volume.drive_letter.isEmpty()
                             ? QStringLiteral("A volume on this disk")
                             : QStringLiteral("%1:").arg(volume.drive_letter);
    return QStringLiteral(
               "%1 is encrypted with BitLocker. A copy would either stay unreadable or put its "
               "data "
               "on the new drive unencrypted without saying so. Turn BitLocker off for this "
               "volume, "
               "migrate, and turn it on again on the new drive.")
        .arg(name);
}

QStringList diskKindBlockers(const PartitionDiskInfo& source) {
    QStringList blockers;
    if (source.partition_enumeration_failed) {
        blockers.append(QStringLiteral("Migrate OS could not read the source disk's partitions"));
    }
    if (!isGpt(source) && !isMbr(source)) {
        blockers.append(QStringLiteral("Migrate OS needs a source disk that is GPT or MBR"));
    }
    if (source.is_dynamic || source.is_storage_spaces) {
        blockers.append(
            QStringLiteral("Migrate OS cannot copy a dynamic disk or a Storage Spaces disk"));
    }
    return blockers;
}

/// What is wrong with the source itself, whatever the target.
QStringList sourceBlockers(const PartitionDiskInfo& source) {
    QStringList blockers = diskKindBlockers(source);
    if (std::ranges::any_of(source.partitions, isLogicalOrExtended)) {
        blockers.append(
            QStringLiteral("Migrate OS cannot rebuild extended or logical partitions. Convert them "
                           "to primary partitions first."));
    }
    if (!std::ranges::any_of(source.partitions, &PartitionInfoEx::is_boot)) {
        blockers.append(QStringLiteral("Windows is not running from this disk"));
    }
    const bool has_system_partition =
        std::ranges::any_of(source.partitions, [&source](const PartitionInfoEx& partition) {
            return isGpt(source) ? partition.is_efi : partition.is_active;
        });
    if (!has_system_partition) {
        blockers.append(
            QStringLiteral("This disk has no boot partition of its own, so Windows starts from "
                           "another disk. Migrate OS copies one disk."));
    }
    for (const auto& partition : source.partitions) {
        if (partition.volume &&
            (partition.volume->bitlocker_enabled || partition.volume->bitlocker_locked)) {
            blockers.append(bitLockerBlocker(*partition.volume));
        }
    }
    return blockers;
}

struct PlacedPartitions {
    uint64_t windows_offset{0};
    uint64_t windows_room{0};
    uint64_t others_bytes{0};
    bool fits{false};
};

/// Partitions before Windows keep their order from the start of the disk, those after it keep
/// theirs against the end, and Windows gets what is between them.
PlacedPartitions placeAroundWindows(OsMigrationPlan* plan, uint64_t usable_end) {
    PlacedPartitions placed;
    auto windows = std::ranges::find(plan->partitions,
                                     OsMigrationPartitionKind::Windows,
                                     &OsMigrationPartition::kind);
    uint64_t cursor = kOsMigrationAlignmentBytes;
    for (auto entry = plan->partitions.begin(); entry != windows; ++entry) {
        entry->target_offset = cursor;
        cursor = alignUp(cursor + entry->source_size);
        placed.others_bytes += entry->source_size;
    }
    placed.windows_offset = cursor;
    uint64_t end = usable_end;
    for (auto entry = plan->partitions.end(); entry != windows + 1;) {
        --entry;
        placed.others_bytes += entry->source_size;
        if (entry->source_size > end) {
            return placed;
        }
        end = alignDown(end - entry->source_size);
        entry->target_offset = end;
    }
    placed.fits = end > placed.windows_offset;
    placed.windows_room = placed.fits ? end - placed.windows_offset : 0;
    return placed;
}

uint64_t headroomFor(uint64_t used_bytes) {
    return std::max(kOsMigrationMinimumHeadroomBytes, used_bytes / kOsMigrationHeadroomDivisor);
}

uint64_t scratchFor(uint64_t used_bytes) {
    return (used_bytes / kOsMigrationImageDivisor * kOsMigrationImageShare) +
           kOsMigrationImageSlackBytes;
}

/// The whole-disk size the image route needs, for the message that refuses a target.
uint64_t imageRouteMinimum(const PlacedPartitions& placed, uint64_t used_bytes) {
    return placed.others_bytes + used_bytes + headroomFor(used_bytes) + scratchFor(used_bytes) +
           kOsMigrationPlacementSlackBytes + kOsMigrationDiskTailBytes;
}

void planImageRoute(OsMigrationPlan* plan,
                    OsMigrationPartition* windows,
                    const PlacedPartitions& placed) {
    plan->route = OsMigrationRoute::Image;
    const uint64_t used = plan->windows_used_bytes;
    if (used == 0) {
        plan->blockers.append(
            QStringLiteral("This target is smaller than the Windows partition, and how much of "
                           "that partition is in use could not be read"));
        return;
    }
    plan->minimum_target_bytes = imageRouteMinimum(placed, used);
    const uint64_t needed = used + headroomFor(used);
    if (placed.windows_room < needed) {
        plan->blockers.append(
            QStringLiteral("Target disk is too small for the data on the Windows volume: %1 in "
                           "use needs a disk of at least %2")
                .arg(formatPartitionBytes(used), formatPartitionBytes(plan->minimum_target_bytes)));
        return;
    }
    if (plan->partition_scheme == QStringLiteral("MBR") &&
        plan->partitions.size() >= kOsMigrationMbrPrimaryLimit) {
        plan->blockers.append(
            QStringLiteral("A smaller target needs a temporary partition, and this MBR source "
                           "already uses all four"));
        return;
    }
    // Neither the image nor what it expands to can be known yet, so what is spare goes half to
    // each.
    const uint64_t required = needed + scratchFor(used);
    const uint64_t spare = placed.windows_room > required ? placed.windows_room - required : 0;
    windows->initial_size = alignUp(needed + (spare / kOsMigrationSpareShareDivisor));
    plan->scratch_offset = alignUp(windows->target_offset + windows->initial_size);
    const uint64_t room_end = windows->target_offset + placed.windows_room;
    plan->scratch_size = room_end > plan->scratch_offset ? room_end - plan->scratch_offset : 0;
    if (plan->scratch_size < scratchFor(used)) {
        plan->blockers.append(
            QStringLiteral("Target disk holds the %1 in use on the Windows volume but not the "
                           "temporary image a smaller disk also needs. It needs at least %2.")
                .arg(formatPartitionBytes(used), formatPartitionBytes(plan->minimum_target_bytes)));
    }
}

uint64_t lastPartitionEnd(const PartitionDiskInfo& source) {
    uint64_t end = 0;
    for (const auto& partition : source.partitions) {
        end = std::max(end, partition.offset_bytes + partition.size_bytes);
    }
    return end;
}

uint64_t usableEnd(const PartitionDiskInfo& source, uint64_t target_bytes) {
    uint64_t end =
        target_bytes > kOsMigrationDiskTailBytes ? target_bytes - kOsMigrationDiskTailBytes : 0;
    // A target at least as large can always hold a partition table the source's size held.
    if (target_bytes >= source.size_bytes) {
        end = std::max(end, lastPartitionEnd(source));
    }
    return isMbr(source) ? std::min(end, kOsMigrationMbrLimitBytes) : end;
}

void placePartitions(const PartitionDiskInfo& source, OsMigrationPlan* plan) {
    const PlacedPartitions placed = placeAroundWindows(plan, usableEnd(source, plan->target_bytes));
    auto windows = std::ranges::find(plan->partitions,
                                     OsMigrationPartitionKind::Windows,
                                     &OsMigrationPartition::kind);
    plan->minimum_target_bytes = placed.others_bytes + windows->source_size +
                                 kOsMigrationPlacementSlackBytes + kOsMigrationDiskTailBytes;
    if (!placed.fits) {
        plan->blockers.append(
            QStringLiteral("Target disk cannot even hold the partitions around Windows"));
        return;
    }
    windows->target_offset = placed.windows_offset;
    windows->final_size = placed.windows_room;
    if (placed.windows_room >= windows->source_size) {
        plan->route = OsMigrationRoute::Blocks;
        windows->initial_size = windows->source_size;
        return;
    }
    planImageRoute(plan, &*windows, placed);
}

std::optional<OsMigrationPartitionKind> kindFromName(const QString& name) {
    for (const auto kind : {OsMigrationPartitionKind::System,
                            OsMigrationPartitionKind::Reserved,
                            OsMigrationPartitionKind::Windows,
                            OsMigrationPartitionKind::Recovery,
                            OsMigrationPartitionKind::Data,
                            OsMigrationPartitionKind::Other}) {
        if (osMigrationKindName(kind) == name) {
            return kind;
        }
    }
    return std::nullopt;
}

uint64_t jsonBytes(const QJsonObject& json, const QString& key) {
    return json.value(key).toString().toULongLong();
}

QString bytesText(uint64_t value) {
    return QString::number(value);
}

QJsonObject partitionToJson(const OsMigrationPartition& entry) {
    QJsonObject json;
    json[QStringLiteral("number")] = static_cast<int>(entry.source_number);
    json[QStringLiteral("source_offset")] = bytesText(entry.source_offset);
    json[QStringLiteral("source_size")] = bytesText(entry.source_size);
    json[QStringLiteral("target_offset")] = bytesText(entry.target_offset);
    json[QStringLiteral("initial_size")] = bytesText(entry.initial_size);
    json[QStringLiteral("final_size")] = bytesText(entry.final_size);
    json[QStringLiteral("kind")] = osMigrationKindName(entry.kind);
    json[QStringLiteral("letter")] = entry.drive_letter;
    json[QStringLiteral("volume")] = entry.volume_path;
    json[QStringLiteral("file_system")] = entry.file_system;
    json[QStringLiteral("snapshot")] = entry.from_snapshot;
    return json;
}

std::optional<OsMigrationPartition> partitionFromJson(const QJsonObject& json) {
    const auto kind = kindFromName(json.value(QStringLiteral("kind")).toString());
    if (!kind) {
        return std::nullopt;
    }
    OsMigrationPartition entry;
    entry.source_number = static_cast<uint32_t>(json.value(QStringLiteral("number")).toInt());
    entry.source_offset = jsonBytes(json, QStringLiteral("source_offset"));
    entry.source_size = jsonBytes(json, QStringLiteral("source_size"));
    entry.target_offset = jsonBytes(json, QStringLiteral("target_offset"));
    entry.initial_size = jsonBytes(json, QStringLiteral("initial_size"));
    entry.final_size = jsonBytes(json, QStringLiteral("final_size"));
    entry.kind = *kind;
    entry.drive_letter = json.value(QStringLiteral("letter")).toString();
    entry.volume_path = json.value(QStringLiteral("volume")).toString();
    entry.file_system = json.value(QStringLiteral("file_system")).toString();
    entry.from_snapshot = json.value(QStringLiteral("snapshot")).toBool();
    return entry;
}

bool samePlacement(const OsMigrationPartition& left, const OsMigrationPartition& right) {
    return left.source_number == right.source_number && left.source_offset == right.source_offset &&
           left.source_size == right.source_size && left.target_offset == right.target_offset &&
           left.final_size == right.final_size && left.kind == right.kind &&
           left.from_snapshot == right.from_snapshot;
}

}  // namespace

QString osMigrationKindName(OsMigrationPartitionKind kind) {
    switch (kind) {
    case OsMigrationPartitionKind::System:
        return QStringLiteral("System");
    case OsMigrationPartitionKind::Reserved:
        return QStringLiteral("Reserved");
    case OsMigrationPartitionKind::Windows:
        return QStringLiteral("Windows");
    case OsMigrationPartitionKind::Recovery:
        return QStringLiteral("Recovery");
    case OsMigrationPartitionKind::Data:
        return QStringLiteral("Data");
    case OsMigrationPartitionKind::Other:
        break;
    }
    return QStringLiteral("Other");
}

QString osMigrationPlanSummary(const OsMigrationPlan& plan) {
    QStringList lines;
    lines.append(plan.route == OsMigrationRoute::Image
                     ? QStringLiteral("Windows is captured from its snapshot to a temporary image "
                                      "on the target, and applied to a new NTFS volume there.")
                     : QStringLiteral("Used blocks are copied from each volume's snapshot, and the "
                                      "Windows volume is then extended to fit."));
    if (const QString shadow = osMigrationShadowStorageText(plan); !shadow.isEmpty()) {
        lines.append(shadow);
    }
    for (const auto& entry : plan.partitions) {
        lines.append(QStringLiteral("%1, %2 at byte %3 on the source: %4 at byte %5 on the target, "
                                    "%6")
                         .arg(osMigrationKindName(entry.kind),
                              formatPartitionBytes(entry.source_size),
                              QString::number(entry.source_offset),
                              formatPartitionBytes(entry.final_size),
                              QString::number(entry.target_offset),
                              entry.from_snapshot ? QStringLiteral("read from a snapshot")
                                                  : QStringLiteral("read raw")));
    }
    return lines.join(QLatin1Char('\n'));
}

bool shadowStorageTooSmallForCapture(uint64_t max_bytes,
                                     uint64_t used_bytes,
                                     uint64_t windows_used_bytes) {
    if (max_bytes == std::numeric_limits<uint64_t>::max()) {
        return false;
    }
    const uint64_t left = max_bytes > used_bytes ? max_bytes - used_bytes : 0;
    return left < windows_used_bytes / kShadowStorageCaptureShare;
}

QString osMigrationShadowStorageText(const OsMigrationPlan& plan) {
    const auto* windows = plan.windowsPartition();
    if (plan.route != OsMigrationRoute::Image || windows == nullptr) {
        return {};
    }
    const QString letter = windows->drive_letter.isEmpty()
                               ? QStringLiteral("the Windows volume")
                               : windows->drive_letter.left(1).toUpper() + QLatin1Char(':');
    if (!plan.windows_shadow_storage_known) {
        return QStringLiteral(
                   "Shadow-copy storage for %1 was not read: Windows shows it to an administrator "
                   "only (Scan Disks as administrator). S.A.K. reads it before it writes the "
                   "target, and warns there if it is small.")
            .arg(letter);
    }
    const uint64_t max = plan.windows_shadow_storage_max_bytes;
    const uint64_t used = plan.windows_shadow_storage_used_bytes;
    QString text = max == std::numeric_limits<uint64_t>::max()
                       ? QStringLiteral("Shadow-copy storage for %1 has no limit, %2 in use.")
                             .arg(letter, formatPartitionBytes(used))
                       : QStringLiteral("Shadow-copy storage for %1 up to %2, %3 in use.")
                             .arg(letter, formatPartitionBytes(max), formatPartitionBytes(used));
    if (shadowStorageTooSmallForCapture(max, used, plan.windows_used_bytes)) {
        text += QStringLiteral(
                    "\nWarning: that leaves %1 for the capture, less than a quarter of the %2 in "
                    "use on %3. While DISM reads every file, Windows keeps the old copy of each "
                    "block that changes on %3 there, and if it fills, Windows deletes the snapshot "
                    "and the capture stops (Windows Update can change several GB). Let Windows "
                    "Update finish first, or give it more room: vssadmin resize shadowstorage "
                    "/for=%3 /on=%3 /maxsize=10%")
                    .arg(formatPartitionBytes(max > used ? max - used : 0),
                         formatPartitionBytes(plan.windows_used_bytes),
                         letter);
    }
    return text;
}

const OsMigrationPartition* OsMigrationPlan::windowsPartition() const {
    const auto found = std::ranges::find(partitions,
                                         OsMigrationPartitionKind::Windows,
                                         &OsMigrationPartition::kind);
    return found == partitions.end() ? nullptr : &*found;
}

OsMigrationPlan planOsMigration(const PartitionDiskInfo& source, uint64_t target_bytes) {
    OsMigrationPlan plan;
    plan.partition_scheme = source.partition_style.trimmed().toUpper();
    plan.source_bytes = source.size_bytes;
    plan.target_bytes = target_bytes;
    plan.blockers = sourceBlockers(source);
    auto partitions = source.partitions;
    std::ranges::sort(partitions, {}, &PartitionInfoEx::offset_bytes);
    for (const auto& partition : partitions) {
        plan.partitions.append(planEntry(partition));
        if (partition.is_boot && partition.volume &&
            partition.volume->total_bytes >= partition.volume->free_bytes) {
            plan.windows_used_bytes = partition.volume->total_bytes - partition.volume->free_bytes;
        }
        if (partition.is_boot && partition.volume) {
            plan.windows_shadow_storage_known = partition.volume->shadow_storage_known;
            plan.windows_shadow_storage_max_bytes = partition.volume->shadow_storage_max_bytes;
            plan.windows_shadow_storage_used_bytes = partition.volume->shadow_storage_used_bytes;
        }
    }
    if (target_bytes == 0) {
        plan.blockers.append(QStringLiteral("Migrate OS needs a target disk of a known size"));
    }
    if (plan.windowsPartition() != nullptr && target_bytes != 0) {
        placePartitions(source, &plan);
    }
    return plan;
}

QJsonObject osMigrationPlanToJson(const OsMigrationPlan& plan) {
    QJsonArray partitions;
    for (const auto& entry : plan.partitions) {
        partitions.append(partitionToJson(entry));
    }
    QJsonObject json;
    json[QStringLiteral("scheme")] = plan.partition_scheme;
    json[QStringLiteral("route")] =
        plan.route == OsMigrationRoute::Image ? QStringLiteral("Image") : QStringLiteral("Blocks");
    json[QStringLiteral("partitions")] = partitions;
    json[QStringLiteral("source_bytes")] = bytesText(plan.source_bytes);
    json[QStringLiteral("target_bytes")] = bytesText(plan.target_bytes);
    json[QStringLiteral("used_bytes")] = bytesText(plan.windows_used_bytes);
    json[QStringLiteral("minimum_bytes")] = bytesText(plan.minimum_target_bytes);
    json[QStringLiteral("scratch_offset")] = bytesText(plan.scratch_offset);
    json[QStringLiteral("scratch_size")] = bytesText(plan.scratch_size);
    return json;
}

std::optional<OsMigrationPlan> osMigrationPlanFromJson(const QJsonObject& json) {
    const QString route = json.value(QStringLiteral("route")).toString();
    if (route != QStringLiteral("Blocks") && route != QStringLiteral("Image")) {
        return std::nullopt;
    }
    OsMigrationPlan plan;
    plan.partition_scheme = json.value(QStringLiteral("scheme")).toString();
    plan.route = route == QStringLiteral("Image") ? OsMigrationRoute::Image
                                                  : OsMigrationRoute::Blocks;
    for (const auto& value : json.value(QStringLiteral("partitions")).toArray()) {
        const auto entry = partitionFromJson(value.toObject());
        if (!entry) {
            return std::nullopt;
        }
        plan.partitions.append(*entry);
    }
    plan.source_bytes = jsonBytes(json, QStringLiteral("source_bytes"));
    plan.target_bytes = jsonBytes(json, QStringLiteral("target_bytes"));
    plan.windows_used_bytes = jsonBytes(json, QStringLiteral("used_bytes"));
    plan.minimum_target_bytes = jsonBytes(json, QStringLiteral("minimum_bytes"));
    plan.scratch_offset = jsonBytes(json, QStringLiteral("scratch_offset"));
    plan.scratch_size = jsonBytes(json, QStringLiteral("scratch_size"));
    return plan.windowsPartition() == nullptr ? std::nullopt : std::make_optional(plan);
}

bool osMigrationPlansAgree(const OsMigrationPlan& left, const OsMigrationPlan& right) {
    return left.partition_scheme == right.partition_scheme && left.route == right.route &&
           left.target_bytes == right.target_bytes &&
           std::ranges::equal(left.partitions, right.partitions, samePlacement);
}

}  // namespace sak
