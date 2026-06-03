// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.cpp
/// @brief Storage inventory collection for Partition Manager.

#include "sak/storage_inventory_worker.h"

#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace sak {

namespace {

uint64_t jsonUInt64(const QJsonObject& object, const QString& key) {
    const auto value = object.value(key);
    if (value.isDouble()) {
        return static_cast<uint64_t>(value.toDouble());
    }
    bool ok = false;
    const uint64_t parsed = value.toString().toULongLong(&ok);
    return ok ? parsed : 0;
}

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

PartitionVolumeInfo parseVolume(const QJsonObject& object) {
    PartitionVolumeInfo volume;
    volume.volume_guid = object.value(QStringLiteral("UniqueId")).toString();
    volume.drive_letter = object.value(QStringLiteral("DriveLetter")).toString();
    volume.label = object.value(QStringLiteral("FileSystemLabel")).toString();
    volume.file_system = object.value(QStringLiteral("FileSystem")).toString();
    volume.health_status = object.value(QStringLiteral("HealthStatus")).toString();
    volume.total_bytes = jsonUInt64(object, QStringLiteral("Size"));
    volume.free_bytes = jsonUInt64(object, QStringLiteral("SizeRemaining"));
    volume.bitlocker_enabled = jsonBool(object, QStringLiteral("BitLockerEnabled"));
    volume.bitlocker_locked = jsonBool(object, QStringLiteral("BitLockerLocked"));
    volume.dirty_bit_set = jsonBool(object, QStringLiteral("DirtyBitSet"));
    return volume;
}

PartitionInfoEx parsePartition(uint32_t disk_number, const QJsonObject& object) {
    PartitionInfoEx partition;
    partition.disk_number = disk_number;
    partition.partition_number =
        static_cast<uint32_t>(object.value(QStringLiteral("PartitionNumber")).toInt());
    partition.partition_guid = object.value(QStringLiteral("Guid")).toString();
    partition.type_name = object.value(QStringLiteral("Type")).toString();
    partition.gpt_type = object.value(QStringLiteral("GptType")).toString();
    partition.offset_bytes = jsonUInt64(object, QStringLiteral("Offset"));
    partition.size_bytes = jsonUInt64(object, QStringLiteral("Size"));
    partition.is_active = jsonBool(object, QStringLiteral("IsActive"));
    partition.is_boot = jsonBool(object, QStringLiteral("IsBoot"));
    partition.is_system = jsonBool(object, QStringLiteral("IsSystem"));
    partition.is_read_only = jsonBool(object, QStringLiteral("IsReadOnly"));
    const auto volume = object.value(QStringLiteral("Volume"));
    if (volume.isObject()) {
        partition.volume = parseVolume(volume.toObject());
    }
    classifyPartition(&partition);
    return partition;
}

void appendUnallocatedRegions(PartitionDiskInfo* disk) {
    std::sort(disk->partitions.begin(),
              disk->partitions.end(),
              [](const auto& left, const auto& right) {
                  return left.offset_bytes < right.offset_bytes;
              });
    uint64_t cursor = 0;
    for (const auto& partition : disk->partitions) {
        if (partition.offset_bytes > cursor) {
            disk->unallocated_regions.append(
                {disk->disk_number, cursor, partition.offset_bytes - cursor});
        }
        cursor = std::max(cursor, partition.offset_bytes + partition.size_bytes);
    }
    if (disk->size_bytes > cursor) {
        disk->unallocated_regions.append({disk->disk_number, cursor, disk->size_bytes - cursor});
    }
}

QString buildLayoutHash(const PartitionInventory& inventory) {
    QCryptographicHash hash(QCryptographicHash::Sha256);
    for (const auto& disk : inventory.disks) {
        hash.addData(QStringLiteral("%1|%2|%3|%4|%5\n")
                         .arg(disk.disk_number)
                         .arg(disk.serial_number, disk.partition_style)
                         .arg(disk.size_bytes)
                         .toUtf8());
        for (const auto& partition : disk.partitions) {
            hash.addData(QStringLiteral("%1|%2|%3|%4|%5|%6\n")
                             .arg(partition.partition_number)
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

PartitionDiskInfo parseDisk(const QJsonObject& object) {
    PartitionDiskInfo disk;
    disk.disk_number = static_cast<uint32_t>(object.value(QStringLiteral("Number")).toInt());
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(disk.disk_number);
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
    const auto temperatureValue = object.value(QStringLiteral("TemperatureCelsius"));
    disk.temperature_celsius =
        static_cast<int>(jsonUInt64(object, QStringLiteral("TemperatureCelsius")));
    if (temperatureValue.isUndefined() || temperatureValue.isNull()) {
        disk.temperature_celsius = -1;
    }
    disk.power_on_hours = jsonUInt64(object, QStringLiteral("PowerOnHours"));
    disk.read_errors_total = jsonUInt64(object, QStringLiteral("ReadErrorsTotal"));
    disk.write_errors_total = jsonUInt64(object, QStringLiteral("WriteErrorsTotal"));
    disk.wear_percent = jsonUInt64(object, QStringLiteral("WearPercent"));
    disk.is_system = jsonBool(object, QStringLiteral("IsSystem"));
    disk.is_boot = jsonBool(object, QStringLiteral("IsBoot"));
    disk.is_read_only = jsonBool(object, QStringLiteral("IsReadOnly"));
    disk.is_dynamic = jsonBool(object, QStringLiteral("IsDynamic"));
    disk.is_storage_spaces = disk.bus_type.contains(QStringLiteral("Storage Spaces"),
                                                    Qt::CaseInsensitive);
    disk.size_bytes = jsonUInt64(object, QStringLiteral("Size"));

    const auto parts = object.value(QStringLiteral("Partitions")).toArray();
    for (const auto& entry : parts) {
        if (entry.isObject()) {
            disk.partitions.append(parsePartition(disk.disk_number, entry.toObject()));
        }
    }
    appendUnallocatedRegions(&disk);
    return disk;
}

}  // namespace

StorageInventoryWorker::StorageInventoryWorker(QObject* parent) : QObject(parent) {}

PartitionInventory StorageInventoryWorker::scan() {
    auto inventory = scanCurrentSystem();
    if (inventory.isEmpty()) {
        const QString message = inventory.warnings.isEmpty()
                                    ? QStringLiteral("Inventory scan failed")
                                    : inventory.warnings.join(QStringLiteral("; "));
        Q_EMIT inventoryError(message);
    } else {
        Q_EMIT inventoryReady(inventory);
    }
    return inventory;
}

PartitionInventory StorageInventoryWorker::scanCurrentSystem() {
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

    return parseInventoryJson(result.std_out.toUtf8());
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
    inventory.layout_hash = buildLayoutHash(inventory);
    return inventory;
}

QString StorageInventoryWorker::inventoryPowerShellScript() {
    return QStringLiteral(R"PS(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$hasBitLocker = [bool](Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)
$physicalDisks = @(Get-PhysicalDisk -ErrorAction SilentlyContinue)
$disks = Get-Disk | ForEach-Object {
  $disk = $_
  $physical = $physicalDisks | Where-Object {
    "$($_.DeviceId)" -eq "$($disk.Number)" -or
    ($_.SerialNumber -and $disk.SerialNumber -and "$($_.SerialNumber)" -eq "$($disk.SerialNumber)") -or
    ($_.FriendlyName -and $disk.FriendlyName -and "$($_.FriendlyName)" -eq "$($disk.FriendlyName)")
  } | Select-Object -First 1
  $reliability = if ($physical) {
    $physical | Get-StorageReliabilityCounter -ErrorAction SilentlyContinue
  } else {
    $null
  }
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
    TemperatureCelsius = if ($reliability -and $null -ne $reliability.Temperature) { [int]$reliability.Temperature } else { $null }
    PowerOnHours = if ($reliability -and $null -ne $reliability.PowerOnHours) { [uint64]$reliability.PowerOnHours } else { 0 }
    ReadErrorsTotal = if ($reliability -and $null -ne $reliability.ReadErrorsTotal) { [uint64]$reliability.ReadErrorsTotal } else { 0 }
    WriteErrorsTotal = if ($reliability -and $null -ne $reliability.WriteErrorsTotal) { [uint64]$reliability.WriteErrorsTotal } else { 0 }
    WearPercent = if ($reliability -and $null -ne $reliability.Wear) { [uint64]$reliability.Wear } else { 0 }
    Size = [uint64]$disk.Size
    IsBoot = [bool]$disk.IsBoot
    IsSystem = [bool]$disk.IsSystem
    IsReadOnly = [bool]$disk.IsReadOnly
    IsDynamic = if ($disk.PSObject.Properties.Name -contains 'IsDynamic') { [bool]$disk.IsDynamic } else { $false }
    Partitions = @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | ForEach-Object {
      $partition = $_
      $volume = $partition | Get-Volume -ErrorAction SilentlyContinue
      $bitlocker = if ($hasBitLocker -and $volume -and $volume.DriveLetter) {
        Get-BitLockerVolume -MountPoint "$($volume.DriveLetter):" -ErrorAction SilentlyContinue
      } else { $null }
      $dirty = $false
      if ($volume -and $volume.DriveLetter) {
        $dirtyText = (& fsutil.exe dirty query "$($volume.DriveLetter):" 2>$null) -join ' '
        $dirty = ($dirtyText -match 'dirty') -and -not ($dirtyText -match 'not\s+dirty')
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
        Volume = if ($volume) {
          [pscustomobject]@{
            UniqueId = "$($volume.UniqueId)"
            DriveLetter = "$($volume.DriveLetter)"
            FileSystem = "$($volume.FileSystem)"
            FileSystemLabel = "$($volume.FileSystemLabel)"
            HealthStatus = "$($volume.HealthStatus)"
            Size = [uint64]$volume.Size
            SizeRemaining = [uint64]$volume.SizeRemaining
            BitLockerEnabled = if ($bitlocker) { "$($bitlocker.ProtectionStatus)" -eq 'On' } else { $false }
            BitLockerLocked = if ($bitlocker) { "$($bitlocker.LockStatus)" -eq 'Locked' } else { $false }
            DirtyBitSet = [bool]$dirty
          }
        } else { $null }
      }
    })
  }
}
$disks | ConvertTo-Json -Depth 8 -Compress
)PS");
}

}  // namespace sak
