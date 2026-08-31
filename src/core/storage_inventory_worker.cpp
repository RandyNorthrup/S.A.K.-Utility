// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.cpp
/// @brief Storage inventory collection for Partition Manager.

#include "sak/storage_inventory_worker.h"
// sak::jsonUInt64 -- the clamped inventory field reader.
#include "sak/elevation_broker.h"
#include "sak/layout_constants.h"
#include "sak/partition_file_system_detector.h"
#include "sak/partition_safety_validator.h"
#include "sak/process_runner.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <memory>

namespace sak {

namespace {

bool jsonBool(const QJsonObject& object, const QString& key) {
    const auto value = object.value(key);
    if (value.isBool()) {
        return value.toBool();
    }
    return value.toString().compare(QStringLiteral("true"), Qt::CaseInsensitive) == 0;
}

bool typeContains(const PartitionInfoEx& partition, const QString& text) {
    return partition.type_name.contains(text, Qt::CaseInsensitive) ||
           partition.gpt_type.contains(text, Qt::CaseInsensitive);
}

void classifyPartition(PartitionInfoEx* partition) {
    partition->is_efi = typeContains(*partition, QStringLiteral("EFI")) ||
                        partition->gpt_type.contains(QStringLiteral("C12A7328"),
                                                     Qt::CaseInsensitive);
    partition->is_msr = typeContains(*partition, QStringLiteral("Reserved")) ||
                        partition->gpt_type.contains(QStringLiteral("E3C9E316"),
                                                     Qt::CaseInsensitive);
    partition->is_recovery = typeContains(*partition, QStringLiteral("Recovery")) ||
                             partition->gpt_type.contains(QStringLiteral("DE94BBA4"),
                                                          Qt::CaseInsensitive);
}

void applyProtectedPartitionFileSystemFallback(PartitionInfoEx* partition) {
    if ((partition == nullptr) ||
        (partition->volume && !partition->volume->file_system.trimmed().isEmpty())) {
        return;
    }

    QString fileSystem;
    QString healthStatus;
    if (partition->is_efi) {
        fileSystem = QStringLiteral("FAT32");
        healthStatus = QStringLiteral("System");
    } else if (partition->is_msr) {
        fileSystem = QStringLiteral("Other");
        healthStatus = QStringLiteral("Reserved");
    }

    if (fileSystem.isEmpty()) {
        return;
    }

    PartitionVolumeInfo volume = partition->volume.value_or(PartitionVolumeInfo{});
    volume.file_system = fileSystem;
    volume.file_system_source = PartitionFileSystemDetector::inferredProtectedSource();
    if (volume.health_status.isEmpty()) {
        volume.health_status = healthStatus;
    }
    partition->volume = volume;
}

PartitionVolumeInfo parseVolume(const QJsonObject& object) {
    PartitionVolumeInfo volume;
    volume.volume_guid = object.value(QStringLiteral("UniqueId")).toString();
    volume.drive_letter = object.value(QStringLiteral("DriveLetter")).toString();
    volume.label = object.value(QStringLiteral("FileSystemLabel")).toString();
    volume.file_system = object.value(QStringLiteral("FileSystem")).toString();
    if (!volume.file_system.trimmed().isEmpty()) {
        volume.file_system_source = PartitionFileSystemDetector::windowsVolumeSource();
    }
    volume.health_status = object.value(QStringLiteral("HealthStatus")).toString();
    volume.total_bytes = jsonUInt64(object, QStringLiteral("Size"));
    volume.free_bytes = jsonUInt64(object, QStringLiteral("SizeRemaining"));
    volume.bitlocker_enabled = jsonBool(object, QStringLiteral("BitLockerEnabled"));
    volume.bitlocker_locked = jsonBool(object, QStringLiteral("BitLockerLocked"));
    volume.bitlocker_state_known = jsonBool(object, QStringLiteral("BitLockerStateKnown"));
    volume.bitlocker_state_error =
        object.value(QStringLiteral("BitLockerStateError")).toString().trimmed();
    volume.dirty_bit_known = jsonBool(object, QStringLiteral("DirtyBitKnown"));
    volume.dirty_bit_set = volume.dirty_bit_known &&
                           jsonBool(object, QStringLiteral("DirtyBitSet"));
    volume.dirty_bit_error = object.value(QStringLiteral("DirtyBitError")).toString().trimmed();
    return volume;
}

PartitionInfoEx parsePartition(uint32_t disk_number, const QJsonObject& object) {
    PartitionInfoEx partition;
    partition.disk_number = DiskNumber{disk_number};
    partition.partition_number = PartitionNumber{
        static_cast<uint32_t>(object.value(QStringLiteral("PartitionNumber")).toInt())};
    partition.partition_guid = object.value(QStringLiteral("Guid")).toString();
    partition.type_name = object.value(QStringLiteral("Type")).toString();
    partition.gpt_type = object.value(QStringLiteral("GptType")).toString();
    partition.offset_bytes = jsonUInt64(object, QStringLiteral("Offset"));
    partition.size_bytes = jsonUInt64(object, QStringLiteral("Size"));
    partition.is_active = jsonBool(object, QStringLiteral("IsActive"));
    partition.is_boot = jsonBool(object, QStringLiteral("IsBoot"));
    partition.is_system = jsonBool(object, QStringLiteral("IsSystem"));
    partition.is_read_only = jsonBool(object, QStringLiteral("IsReadOnly"));
    partition.volume_lookup_failed = jsonBool(object, QStringLiteral("VolumeLookupFailed"));
    partition.volume_lookup_error =
        object.value(QStringLiteral("VolumeLookupError")).toString().trimmed();
    const auto volume = object.value(QStringLiteral("Volume"));
    if (volume.isObject()) {
        partition.volume = parseVolume(volume.toObject());
    }
    classifyPartition(&partition);
    applyProtectedPartitionFileSystemFallback(&partition);
    return partition;
}

void appendUnallocatedRegions(PartitionDiskInfo* disk) {
    std::ranges::sort(disk->partitions,

                      [](const auto& left, const auto& right) {
                          return left.offset_bytes < right.offset_bytes;
                      });
    uint64_t cursor = 0;
    for (const auto& partition : disk->partitions) {
        if (partition.offset_bytes > cursor) {
            disk->unallocated_regions.append({.disk_number = disk->disk_number,
                                              .offset_bytes = cursor,
                                              .size_bytes = partition.offset_bytes - cursor});
        }
        cursor = std::max(cursor, partition.offset_bytes + partition.size_bytes);
    }
    if (disk->size_bytes > cursor) {
        disk->unallocated_regions.append({.disk_number = disk->disk_number,
                                          .offset_bytes = cursor,
                                          .size_bytes = disk->size_bytes - cursor});
    }
}

QString buildLayoutHash(const PartitionInventory& inventory) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& disk : inventory.disks) {
        // is_system/is_boot are folded in so that a change to a disk's boot or
        // system status since planning invalidates the queue's apply hash and
        // forces a refresh + re-validation (which then applies the OS-disk block)
        // rather than dispatching a destructive op against a now-bootable disk.
        hash.addData(QStringLiteral("%1|%2|%3|%4|%5|%6\n")
                         .arg(disk.disk_number.value())
                         .arg(disk.serial_number, disk.partition_style)
                         .arg(disk.size_bytes)
                         .arg(disk.is_system ? 1 : 0)
                         .arg(disk.is_boot ? 1 : 0)
                         .toUtf8());
        for (const auto& partition : disk.partitions) {
            hash.addData(QStringLiteral("%1|%2|%3|%4|%5|%6\n")
                             .arg(partition.partition_number.value())
                             .arg(partition.partition_guid)
                             .arg(partition.offset_bytes)
                             .arg(partition.size_bytes)
                             .arg(partition.volume ? partition.volume->file_system : QString())
                             .arg(partition.volume ? partition.volume->drive_letter : QString())
                             .toUtf8());
        }
    }
    return QString::fromLatin1(hash.result().toHex());
}

bool needsRawFileSystemDetection(const PartitionInfoEx& partition) {
    return !partition.volume || partition.volume->file_system.trimmed().isEmpty();
}

void applyDetectedVolumeMetadata(PartitionVolumeInfo* volume,
                                 const PartitionFileSystemDetection& detection) {
    volume->file_system = detection.file_system;
    volume->file_system_source = detection.source;
    volume->file_system_details = detection.details;
    if (volume->total_bytes == 0 && detection.total_bytes > 0) {
        volume->total_bytes = detection.total_bytes;
    }
    if (volume->free_bytes == 0 && detection.free_bytes > 0) {
        volume->free_bytes = detection.free_bytes;
    }
    if (volume->health_status.isEmpty()) {
        volume->health_status = QStringLiteral("Detected");
    }
}

void setProbeError(QString* errorMessage, const QString& message) {
    if (errorMessage != nullptr) {
        *errorMessage = message;
    }
}

QJsonObject elevatedProbePayload(const PartitionDiskInfo& disk, const PartitionInfoEx& partition) {
    QJsonObject payload;
    payload[QStringLiteral("device_path")] = disk.device_path;
    payload[QStringLiteral("partition_offset_bytes")] = QString::number(partition.offset_bytes);
    payload[QStringLiteral("partition_size_bytes")] = QString::number(partition.size_bytes);
    payload[QStringLiteral("max_bytes")] =
        QString::number(PartitionFileSystemDetector::probeReadLimit(partition.size_bytes));
    return payload;
}

QString partitionDevicePath(const PartitionDiskInfo& disk, const PartitionInfoEx& partition) {
    if (partition.partition_number.value() == 0) {
        return {};
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(disk.disk_number.value())
        .arg(partition.partition_number.value());
}

QString elevationErrorText(error_code code) {
    const auto text = to_string(code);
    return QStringLiteral("Elevation failed: %1")
        .arg(QString::fromLatin1(text.data(), static_cast<qsizetype>(text.size())));
}

std::optional<QByteArray> decodeElevatedProbeBytes(const QJsonObject& data, QString* errorMessage) {
    QByteArray bytes =
        QByteArray::fromBase64(data.value(QStringLiteral("bytes_base64")).toString().toLatin1());
    const int expectedBytes =
        static_cast<int>(data.value(QStringLiteral("bytes_read")).toDouble(-1));
    if (expectedBytes < 0 || bytes.size() == expectedBytes) {
        return bytes;
    }
    setProbeError(errorMessage, QStringLiteral("Elevated raw probe returned truncated data"));
    return std::nullopt;
}

QString normalizedProbeError(const QString& errorText, const QString& fallback);

std::optional<QByteArray> readElevatedProbeBytes(ElevationBroker* broker,
                                                 const PartitionDiskInfo& disk,
                                                 const PartitionInfoEx& partition,
                                                 QString* errorMessage) {
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    if (broker == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = QStringLiteral("Elevation broker is unavailable");
        }
        return std::nullopt;
    }

    const auto result = broker->executeTask(QStringLiteral("ReadPartitionProbe"),
                                            QStringLiteral("Read partition signature probe"),
                                            elevatedProbePayload(disk, partition));
    if (!result.has_value()) {
        setProbeError(errorMessage, elevationErrorText(result.error()));
        return std::nullopt;
    }
    if (!result->success) {
        setProbeError(errorMessage,
                      normalizedProbeError(result->error_message,
                                           QStringLiteral("Elevated raw probe failed")));
        return std::nullopt;
    }
    return decodeElevatedProbeBytes(result->data, errorMessage);
}

QString normalizedProbeError(const QString& errorText, const QString& fallback) {
    return errorText.isEmpty() ? fallback : errorText;
}

bool elevatedProbeErrorDisablesRetry(const QString& errorText) {
    return errorText.contains(QStringLiteral("denied"), Qt::CaseInsensitive) ||
           errorText.contains(QStringLiteral("cancelled"), Qt::CaseInsensitive) ||
           errorText.contains(QStringLiteral("not found"), Qt::CaseInsensitive) ||
           errorText.contains(QStringLiteral("Elevation failed"), Qt::CaseInsensitive);
}

void appendRawProbeWarning(PartitionInventory* inventory,
                           const PartitionDiskInfo& disk,
                           const PartitionInfoEx& partition,
                           const QString& localError,
                           const QString& elevatedError) {
    inventory->warnings.append(
        QStringLiteral("Raw file-system probe failed for disk %1 partition %2: %3; "
                       "elevated retry: %4")
            .arg(disk.disk_number.value())
            .arg(partition.partition_number.value())
            .arg(normalizedProbeError(localError, QStringLiteral("local read did not return data")))
            .arg(normalizedProbeError(elevatedError, QStringLiteral("not available"))));
}

std::optional<QByteArray> readProbeBytesWithRetry(const PartitionDiskInfo& disk,
                                                  const PartitionInfoEx& partition,
                                                  ElevationBroker* broker,
                                                  bool* elevatedProbeUnavailable,
                                                  PartitionInventory* inventory) {
    QString localProbeError;
    auto bytes = PartitionFileSystemDetector::readProbeBytesFromDevicePath(
        disk.device_path, partition.offset_bytes, partition.size_bytes, &localProbeError);
    if (bytes.has_value() || *elevatedProbeUnavailable) {
        return bytes;
    }

    const QString partitionPath = partitionDevicePath(disk, partition);
    QString partitionProbeError;
    if (!partitionPath.isEmpty()) {
        bytes = PartitionFileSystemDetector::readProbeBytesFromDevicePath(
            partitionPath, 0, partition.size_bytes, &partitionProbeError);
        if (bytes.has_value()) {
            return bytes;
        }
        localProbeError =
            QStringLiteral("%1; partition alias %2: %3")
                .arg(normalizedProbeError(localProbeError,
                                          QStringLiteral("physical drive read failed")),
                     partitionPath,
                     normalizedProbeError(partitionProbeError,
                                          QStringLiteral("local partition read failed")));
    }

    QString elevatedProbeError;
    bytes = readElevatedProbeBytes(broker, disk, partition, &elevatedProbeError);
    if (bytes.has_value()) {
        return bytes;
    }

    appendRawProbeWarning(inventory, disk, partition, localProbeError, elevatedProbeError);
    if (elevatedProbeErrorDisablesRetry(elevatedProbeError)) {
        *elevatedProbeUnavailable = true;
    }
    return std::nullopt;
}

void applyDetectionToPartition(PartitionInfoEx* partition,
                               const PartitionFileSystemDetection& detection) {
    PartitionVolumeInfo volume = partition->volume.value_or(PartitionVolumeInfo{});
    applyDetectedVolumeMetadata(&volume, detection);
    partition->volume = volume;
}

bool apfsDetectionNeedsSupplementalSpaceManager(const PartitionFileSystemDetection& detection) {
    if (detection.file_system.compare(QStringLiteral("APFS"), Qt::CaseInsensitive) != 0) {
        return false;
    }
    return std::ranges::none_of(detection.details, [](const auto& detail) {
        return detail.startsWith(QStringLiteral("APFS space manager block:"));
    });
}

std::optional<PartitionFileSystemDetection> detectFromLocalDevicePath(const QString& devicePath,
                                                                      uint64_t partitionOffsetBytes,
                                                                      uint64_t partitionSizeBytes) {
    QString error;
    auto detection = PartitionFileSystemDetector::detectFromDevicePath(
        devicePath, partitionOffsetBytes, partitionSizeBytes, &error);
    if (!detection.has_value() || detection->file_system.trimmed().isEmpty()) {
        return std::nullopt;
    }
    return detection;
}

std::optional<PartitionFileSystemDetection> supplementedRawFileSystemDetection(
    const PartitionDiskInfo& disk,
    const PartitionInfoEx& partition,
    const PartitionFileSystemDetection& detection) {
    if (!apfsDetectionNeedsSupplementalSpaceManager(detection)) {
        return detection;
    }

    if (auto supplemented = detectFromLocalDevicePath(
            disk.device_path, partition.offset_bytes, partition.size_bytes);
        supplemented.has_value()) {
        return supplemented;
    }

    const QString partitionPath = partitionDevicePath(disk, partition);
    if (!partitionPath.isEmpty()) {
        if (auto supplemented = detectFromLocalDevicePath(partitionPath, 0, partition.size_bytes);
            supplemented.has_value()) {
            return supplemented;
        }
    }

    return detection;
}

void applyRawFileSystemDetectionToPartition(const PartitionDiskInfo& disk,
                                            PartitionInfoEx* partition,
                                            ElevationBroker* broker,
                                            bool* elevatedProbeUnavailable,
                                            PartitionInventory* inventory) {
    if (!needsRawFileSystemDetection(*partition)) {
        return;
    }

    const auto bytes =
        readProbeBytesWithRetry(disk, *partition, broker, elevatedProbeUnavailable, inventory);
    if (!bytes.has_value()) {
        return;
    }

    const auto detection = PartitionFileSystemDetector::detectBytes(*bytes, partition->size_bytes);
    if (detection && !detection->file_system.trimmed().isEmpty()) {
        const auto supplemented = supplementedRawFileSystemDetection(disk, *partition, *detection);
        applyDetectionToPartition(partition, supplemented.value_or(*detection));
    }
}

void applyRawFileSystemDetection(PartitionInventory* inventory, bool allow_elevation) {
    if (inventory == nullptr) {
        return;
    }

    std::unique_ptr<ElevationBroker> elevatedBroker;
    // When elevation is not permitted the broker is never created: the raw probe
    // still runs both local (unprivileged) reads, but the elevated fallback is a
    // cheap no-op ("broker unavailable"), so a foreign partition whose local read
    // is denied keeps an empty file_system + a warning instead of triggering a
    // UAC prompt / elevated helper.
    bool elevatedProbeUnavailable = false;
    for (auto& disk : inventory->disks) {
        for (auto& partition : disk.partitions) {
            if (allow_elevation && !elevatedBroker && needsRawFileSystemDetection(partition)) {
                elevatedBroker = std::make_unique<ElevationBroker>();
            }
            applyRawFileSystemDetectionToPartition(
                disk, &partition, elevatedBroker.get(), &elevatedProbeUnavailable, inventory);
        }
    }
}

PartitionDiskInfo parseDisk(const QJsonObject& object) {
    PartitionDiskInfo disk;
    disk.disk_number =
        DiskNumber{static_cast<uint32_t>(object.value(QStringLiteral("Number")).toInt())};
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk.disk_number.value());
    disk.model = object.value(QStringLiteral("FriendlyName")).toString();
    disk.serial_number = object.value(QStringLiteral("SerialNumber")).toString();
    disk.bus_type = object.value(QStringLiteral("BusType")).toString();
    disk.media_type = object.value(QStringLiteral("MediaType")).toString();
    QString partitionStyleText = object.value(QStringLiteral("PartitionStyle")).toString();
    if (partitionStyleText.isEmpty()) {
        partitionStyleText = object.value(QStringLiteral("PartitionScheme")).toString();
    }
    disk.partition_style.swap(partitionStyleText);
    disk.health_status = object.value(QStringLiteral("HealthStatus")).toString();
    disk.operational_status = object.value(QStringLiteral("OperationalStatus")).toString();
    disk.smart_summary = object.value(QStringLiteral("SmartSummary")).toString();
    disk.reliability_query_succeeded = jsonBool(object,
                                                QStringLiteral("ReliabilityQuerySucceeded"));
    disk.reliability_query_error =
        object.value(QStringLiteral("ReliabilityQueryError")).toString().trimmed();
    const auto temperatureValue = object.value(QStringLiteral("TemperatureCelsius"));
    disk.temperature_celsius =
        static_cast<int>(jsonUInt64(object, QStringLiteral("TemperatureCelsius")));
    if (temperatureValue.isUndefined() || temperatureValue.isNull()) {
        disk.temperature_celsius = -1;
    }
    disk.power_on_hours = jsonUInt64(object, QStringLiteral("PowerOnHours"));
    disk.power_on_hours_known = jsonBool(object, QStringLiteral("PowerOnHoursKnown"));
    disk.read_errors_total = jsonUInt64(object, QStringLiteral("ReadErrorsTotal"));
    disk.read_errors_total_known = jsonBool(object, QStringLiteral("ReadErrorsTotalKnown"));
    disk.write_errors_total = jsonUInt64(object, QStringLiteral("WriteErrorsTotal"));
    disk.write_errors_total_known = jsonBool(object, QStringLiteral("WriteErrorsTotalKnown"));
    disk.wear_percent = jsonUInt64(object, QStringLiteral("WearPercent"));
    disk.wear_percent_known = jsonBool(object, QStringLiteral("WearPercentKnown"));
    disk.is_system = jsonBool(object, QStringLiteral("IsSystem"));
    disk.is_boot = jsonBool(object, QStringLiteral("IsBoot"));
    disk.is_read_only = jsonBool(object, QStringLiteral("IsReadOnly"));
    disk.is_dynamic = jsonBool(object, QStringLiteral("IsDynamic"));
    // Get-Disk stringifies MSFT_Disk BusType enum value 16 as "Spaces" (NOT "Storage Spaces"), so
    // the old contains("Storage Spaces") never matched and every Storage-Spaces blocker keyed on
    // this flag was dead. Match the value Windows actually emits; keep the long form as an alias.
    disk.is_storage_spaces =
        disk.bus_type.compare(QStringLiteral("Spaces"), Qt::CaseInsensitive) == 0 ||
        disk.bus_type.contains(QStringLiteral("Storage Spaces"), Qt::CaseInsensitive);
    disk.size_bytes = jsonUInt64(object, QStringLiteral("Size"));

    disk.partition_enumeration_failed = jsonBool(object, QStringLiteral("PartitionQueryFailed"));
    disk.partition_enumeration_error =
        object.value(QStringLiteral("PartitionQueryError")).toString();

    const auto parts = object.value(QStringLiteral("Partitions")).toArray();
    for (const auto& entry : parts) {
        if (entry.isObject()) {
            disk.partitions.append(parsePartition(disk.disk_number.value(), entry.toObject()));
        }
    }
    if (disk.partition_enumeration_failed) {
        // The partition list is NOT authoritative here: deriving unallocated regions from it
        // would paint a populated disk as entirely free space (and invite a destructive
        // "create in free space"). Leave the layout unknown and let the caller fail closed.
        return disk;
    }
    appendUnallocatedRegions(&disk);
    return disk;
}

void appendPartitionEnumerationWarnings(PartitionInventory* inventory) {
    for (const auto& disk : inventory->disks) {
        if (!disk.partition_enumeration_failed) {
            continue;
        }
        const QString reason = disk.partition_enumeration_error.trimmed().isEmpty()
                                   ? QStringLiteral("no error text reported")
                                   : disk.partition_enumeration_error.trimmed();
        inventory->warnings.append(
            QStringLiteral("Partition enumeration failed for disk %1 (%2): %3; the layout of "
                           "this disk is unknown and is NOT reported as unallocated")
                .arg(disk.disk_number.value())
                .arg(disk.device_path, reason));
    }
}

void appendVolumeProbeWarnings(PartitionInventory* inventory) {
    for (const auto& disk : inventory->disks) {
        for (const auto& partition : disk.partitions) {
            if (!partition.volume || !partition.volume->hasDriveLetter() ||
                partition.volume->dirty_bit_known) {
                continue;
            }
            const QString reason = partition.volume->dirty_bit_error.trimmed().isEmpty()
                                       ? QStringLiteral("no error text reported")
                                       : partition.volume->dirty_bit_error.trimmed();
            inventory->warnings.append(
                QStringLiteral("Dirty-bit query failed for volume %1: %2; its file-system "
                               "dirty state is unknown")
                    .arg(partition.volume->drive_letter, reason));
        }
    }
}

void appendInventoryMetadataWarnings(PartitionInventory* inventory) {
    for (const auto& disk : inventory->disks) {
        if (!disk.reliability_query_succeeded) {
            const QString reason = disk.reliability_query_error.trimmed().isEmpty()
                                       ? QStringLiteral("no error text reported")
                                       : disk.reliability_query_error.trimmed();
            inventory->warnings.append(
                QStringLiteral("Storage reliability query failed for disk %1: %2; SMART metrics "
                               "are unknown")
                    .arg(disk.disk_number.value())
                    .arg(reason));
        }
        for (const auto& partition : disk.partitions) {
            if (partition.volume_lookup_failed) {
                const QString reason = partition.volume_lookup_error.trimmed().isEmpty()
                                           ? QStringLiteral("no error text reported")
                                           : partition.volume_lookup_error.trimmed();
                inventory->warnings.append(
                    QStringLiteral("Volume lookup failed for disk %1 partition %2: %3; mounted "
                                   "volume metadata is unknown")
                        .arg(disk.disk_number.value())
                        .arg(partition.partition_number.value())
                        .arg(reason));
            }
            if (partition.volume && !partition.volume->bitlocker_state_known &&
                !partition.volume->bitlocker_state_error.trimmed().isEmpty()) {
                inventory->warnings.append(
                    QStringLiteral("BitLocker query failed for volume %1: %2; protection and "
                                   "lock state are unknown")
                        .arg(partition.volume->drive_letter,
                             partition.volume->bitlocker_state_error.trimmed()));
            }
        }
    }
}

}  // namespace

StorageInventoryWorker::StorageInventoryWorker(QObject* parent) : QObject(parent) {}

PartitionInventory StorageInventoryWorker::scan() {
    auto inventory = scanCurrentSystem();
    // Fail closed: an empty inventory OR a disk whose partition enumeration failed is not a
    // faithful picture of the machine's storage, so it is never published as a ready inventory.
    if (inventory.isEmpty() || inventory.hasPartitionEnumerationFailure()) {
        const QString message = inventory.warnings.isEmpty()
                                    ? QStringLiteral("Inventory scan failed")
                                    : inventory.warnings.join(QStringLiteral("; "));
        Q_EMIT inventoryError(message);
    } else {
        Q_EMIT inventoryReady(inventory);
    }
    return inventory;
}

PartitionInventory StorageInventoryWorker::scanCurrentSystem(bool allow_elevation) {
    const auto result =
        runPowerShell(inventoryPowerShellScript(), sak::kTimeoutProcessLongMs, true, true);
    if (!result.succeeded()) {
        PartitionInventory inventory;
        inventory.captured_at = QDateTime::currentDateTime();
        inventory.warnings.append(result.std_err.trimmed().isEmpty()
                                      ? QStringLiteral("Inventory scan failed")
                                      : result.std_err.trimmed());
        return inventory;
    }

    auto inventory = parseInventoryJson(result.std_out.toUtf8());
    applyRawFileSystemDetection(&inventory, allow_elevation);
    inventory.layout_hash = buildLayoutHash(inventory);
    return inventory;
}

PartitionInventory StorageInventoryWorker::parseInventoryJson(const QByteArray& json_data) {
    PartitionInventory inventory;
    inventory.captured_at = QDateTime::currentDateTime();
    QJsonParseError error{};
    const QJsonDocument doc = QJsonDocument::fromJson(json_data, &error);
    if (error.error != QJsonParseError::NoError) {
        inventory.warnings.append(
            QStringLiteral("Storage inventory JSON parse failed: %1").arg(error.errorString()));
        return inventory;
    }

    const QJsonArray disks = doc.isArray() ? doc.array() : QJsonArray{doc.object()};
    for (const auto& entry : disks) {
        if (entry.isObject()) {
            inventory.disks.append(parseDisk(entry.toObject()));
        }
    }
    appendPartitionEnumerationWarnings(&inventory);
    appendVolumeProbeWarnings(&inventory);
    appendInventoryMetadataWarnings(&inventory);
    inventory.layout_hash = buildLayoutHash(inventory);
    return inventory;
}

static QString inventoryPowerShellHeaderScript() {
    return QStringLiteral(R"PS(
$ErrorActionPreference = 'Stop'
# Progress records are not errors. Suppress this host-only stream so stdout remains one JSON
# document; every command error still terminates or is captured into an explicit error field.
$ProgressPreference = 'SilentlyContinue'
$bitLockerCommandAvailable = $false
$bitLockerCommandError = ''
try {
  $bitLockerCommandAvailable = $null -ne (Get-Command Get-BitLockerVolume -ErrorAction Stop)
} catch {
  $bitLockerCommandError = "Get-BitLockerVolume is unavailable: $($_.Exception.Message)"
}
$physicalDiskQueryFailed = $false
$physicalDiskQueryError = ''
$physicalDisks = @()
try {
  $physicalDisks = @(Get-PhysicalDisk -ErrorAction Stop)
} catch {
  $physicalDiskQueryFailed = $true
  $physicalDiskQueryError = "$($_.Exception.Message)"
}
)PS");
}

static QString inventoryPowerShellVolumeLookupScript() {
    return QStringLiteral(R"PS(
function Get-SakVolumeForPartition {
  param([Parameter(Mandatory = $true)]$Partition)

  $errors = @()
  $volume = $null
  try {
    $volume = $Partition | Get-Volume -ErrorAction Stop | Select-Object -First 1
  } catch {
    if ("$($_.FullyQualifiedErrorId)" -notlike 'CmdletizationQuery_NotFound*') {
      $errors += "$($_.Exception.Message)"
    }
  }
  if (-not $volume -and $Partition.DriveLetter) {
    try {
      $volume = Get-Volume -DriveLetter $Partition.DriveLetter -ErrorAction Stop |
        Select-Object -First 1
    } catch {
      if ("$($_.FullyQualifiedErrorId)" -notlike 'CmdletizationQuery_NotFound*') {
        $errors += "$($_.Exception.Message)"
      }
    }
  }
  if (-not $volume -and
      $Partition.PSObject.Properties.Name -contains 'AccessPaths') {
    foreach ($accessPath in @($Partition.AccessPaths)) {
      if ([string]::IsNullOrWhiteSpace($accessPath)) { continue }
      try {
        $volume = Get-Volume -Path $accessPath -ErrorAction Stop | Select-Object -First 1
      } catch {
        if ("$($_.FullyQualifiedErrorId)" -notlike 'CmdletizationQuery_NotFound*') {
          $errors += "$($_.Exception.Message)"
        }
      }
      if ($volume) { break }
    }
  }

  [pscustomobject]@{
    Volume = $volume
    Failed = (-not $volume -and $errors.Count -gt 0)
    Error = ($errors | Select-Object -Unique) -join '; '
  }
}
)PS");
}

static QString inventoryPowerShellDiskPrefixScript() {
    return QStringLiteral(R"PS(
$disks = Get-Disk | ForEach-Object {
  $disk = $_
  $physical = $physicalDisks | Where-Object {
    "$($_.DeviceId)" -eq "$($disk.Number)" -or
    ($_.SerialNumber -and $disk.SerialNumber -and "$($_.SerialNumber)" -eq "$($disk.SerialNumber)") -or
    ($_.FriendlyName -and $disk.FriendlyName -and "$($_.FriendlyName)" -eq "$($disk.FriendlyName)")
  } | Select-Object -First 1
  $reliability = $null
  $reliabilityError = ''
  if ($physicalDiskQueryFailed) {
    $reliabilityError = "Get-PhysicalDisk failed: $physicalDiskQueryError"
  } elseif (-not $physical) {
    $reliabilityError = 'No matching physical disk was reported'
  } else {
    try {
      $reliability = $physical | Get-StorageReliabilityCounter -ErrorAction Stop
      if (-not $reliability) {
        $reliabilityError = 'No storage reliability counters were reported'
      }
    } catch {
      $reliabilityError = "$($_.Exception.Message)"
    }
  }
  $partitionQuery = Get-SakPartitionsForDisk -DiskNumber $disk.Number
  [pscustomobject]@{
    Number = $disk.Number
    FriendlyName = $disk.FriendlyName
    SerialNumber = $disk.SerialNumber
    HealthStatus = "$($disk.HealthStatus)"
    OperationalStatus = @($disk.OperationalStatus) -join ', '
    BusType = "$($disk.BusType)"
    MediaType = "$($disk.MediaType)"
    PartitionScheme = "$($disk.PartitionStyle)"
    SmartSummary = if ($reliability) { 'Storage reliability counters available' } else { '' }
    ReliabilityQuerySucceeded = [bool]$reliability
    ReliabilityQueryError = "$reliabilityError"
    TemperatureCelsius = if ($reliability -and $null -ne $reliability.Temperature) { [int]$reliability.Temperature } else { $null }
    PowerOnHoursKnown = [bool]($reliability -and $null -ne $reliability.PowerOnHours)
    PowerOnHours = if ($reliability -and $null -ne $reliability.PowerOnHours) { [uint64]$reliability.PowerOnHours } else { $null }
    ReadErrorsTotalKnown = [bool]($reliability -and $null -ne $reliability.ReadErrorsTotal)
    ReadErrorsTotal = if ($reliability -and $null -ne $reliability.ReadErrorsTotal) { [uint64]$reliability.ReadErrorsTotal } else { $null }
    WriteErrorsTotalKnown = [bool]($reliability -and $null -ne $reliability.WriteErrorsTotal)
    WriteErrorsTotal = if ($reliability -and $null -ne $reliability.WriteErrorsTotal) { [uint64]$reliability.WriteErrorsTotal } else { $null }
    WearPercentKnown = [bool]($reliability -and $null -ne $reliability.Wear)
    WearPercent = if ($reliability -and $null -ne $reliability.Wear) { [uint64]$reliability.Wear } else { $null }
    Size = [uint64]$disk.Size
    IsBoot = [bool]$disk.IsBoot
    IsSystem = [bool]$disk.IsSystem
    IsReadOnly = [bool]$disk.IsReadOnly
    IsDynamic = if ($disk.PSObject.Properties.Name -contains 'IsDynamic') { [bool]$disk.IsDynamic } else { $false }
)PS");
}

static QString inventoryPowerShellPartitionProbeScript() {
    return QStringLiteral(R"PS(
    PartitionQueryFailed = [bool]$partitionQuery.Failed
    PartitionQueryError = "$($partitionQuery.Error)"
    Partitions = @($partitionQuery.Items | ForEach-Object {
      $partition = $_
      $volumeQuery = Get-SakVolumeForPartition -Partition $partition
      $volume = $volumeQuery.Volume
      $bitlocker = $null
      $bitLockerKnown = $false
      $bitLockerError = ''
      if ($volume -and $volume.DriveLetter) {
        if (-not $bitLockerCommandAvailable) {
          $bitLockerError = $bitLockerCommandError
        } else {
          try {
            $bitlocker = Get-BitLockerVolume -MountPoint "$($volume.DriveLetter):" -ErrorAction Stop
            $bitLockerKnown = $null -ne $bitlocker
            if (-not $bitLockerKnown) {
              $bitLockerError = 'Get-BitLockerVolume returned no result'
            }
          } catch {
            $bitLockerError = "$($_.Exception.Message)"
          }
        }
      }
)PS");
}

static QString inventoryPowerShellPartitionResultScript() {
    return QStringLiteral(R"PS(
      $dirty = $false
      $dirtyKnown = $false
      $dirtyError = ''
      if ($volume -and $volume.DriveLetter) {
        $dirtyOutput = @(& fsutil.exe dirty query "$($volume.DriveLetter):" 2>&1)
        $dirtyExit = $LASTEXITCODE
        $dirtyText = ($dirtyOutput | ForEach-Object { "$_" }) -join ' '
        if ($dirtyExit -ne 0) {
          $dirtyError = if ([string]::IsNullOrWhiteSpace($dirtyText)) {
            "fsutil exited $dirtyExit without error text"
          } else {
            "fsutil exited $dirtyExit`: $dirtyText"
          }
        } elseif ($dirtyText -match 'not\s+dirty') {
          $dirtyKnown = $true
          $dirty = $false
        } elseif ($dirtyText -match 'dirty') {
          $dirtyKnown = $true
          $dirty = $true
        } else {
          $dirtyError = "fsutil returned unrecognized output: $dirtyText"
        }
      }
      [pscustomobject]@{
        PartitionNumber = $partition.PartitionNumber
        Guid = "$($partition.Guid)"
        Type = "$($partition.Type)"
        GptType = "$($partition.GptType)"
        Offset = [uint64]$partition.Offset
        Size = [uint64]$partition.Size
        DriveLetter = "$($partition.DriveLetter)"
        IsActive = [bool]$partition.IsActive
        IsBoot = [bool]$partition.IsBoot
        IsSystem = [bool]$partition.IsSystem
        IsReadOnly = [bool]$partition.IsReadOnly
        VolumeLookupFailed = [bool]$volumeQuery.Failed
        VolumeLookupError = "$($volumeQuery.Error)"
        Volume = if ($volume) {
          [pscustomobject]@{
            UniqueId = "$($volume.UniqueId)"
            DriveLetter = "$($volume.DriveLetter)"
            FileSystem = "$($volume.FileSystem)"
            FileSystemLabel = "$($volume.FileSystemLabel)"
            HealthStatus = "$($volume.HealthStatus)"
            Size = [uint64]$volume.Size
            SizeRemaining = [uint64]$volume.SizeRemaining
            BitLockerStateKnown = [bool]$bitLockerKnown
            BitLockerStateError = "$bitLockerError"
            BitLockerEnabled = if ($bitlocker) { "$($bitlocker.ProtectionStatus)" -eq 'On' } else { $false }
            BitLockerLocked = if ($bitlocker) { "$($bitlocker.LockStatus)" -eq 'Locked' } else { $false }
            DirtyBitKnown = [bool]$dirtyKnown
            DirtyBitSet = [bool]$dirty
            DirtyBitError = "$dirtyError"
          }
        } else { $null }
      }
    })
)PS");
}

static QString inventoryPowerShellFooterScript() {
    return QStringLiteral(R"PS(
  }
}
$disks | ConvertTo-Json -Depth 8 -Compress
)PS");
}

// Partition enumeration for ONE disk, reported honestly. A disk with no partition table
// legitimately yields no objects (CmdletizationQuery_NotFound); EVERY other failure (access
// denied, WMI/RPC error) is carried out as Failed/Error instead of being swallowed into an
// empty partition list, which the parser would otherwise render as a fully unallocated disk.
static QString inventoryPowerShellPartitionQueryScript() {
    return QStringLiteral(R"PS(
function Get-SakPartitionsForDisk {
  param([Parameter(Mandatory = $true)]$DiskNumber)

  $failed = $false
  $message = ''
  $items = @()
  try {
    $items = @(Get-Partition -DiskNumber $DiskNumber -ErrorAction Stop)
  } catch {
    if ("$($_.FullyQualifiedErrorId)" -notlike 'CmdletizationQuery_NotFound*') {
      $failed = $true
      $message = "$($_.Exception.Message)"
    }
  }

  [pscustomobject]@{ Failed = $failed; Error = $message; Items = $items }
}
)PS");
}

QString StorageInventoryWorker::inventoryPowerShellScript() {
    // Both helper functions must be defined BEFORE the Get-Disk pipeline that calls them.
    return inventoryPowerShellHeaderScript() + inventoryPowerShellVolumeLookupScript() +
           inventoryPowerShellPartitionQueryScript() + inventoryPowerShellDiskPrefixScript() +
           inventoryPowerShellPartitionProbeScript() + inventoryPowerShellPartitionResultScript() +
           inventoryPowerShellFooterScript();
}

}  // namespace sak
