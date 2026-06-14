// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file storage_inventory_worker.cpp
/// @brief Storage inventory collection for Partition Manager.

#include "sak/storage_inventory_worker.h"

#include "sak/elevation_broker.h"
#include "sak/layout_constants.h"
#include "sak/partition_file_system_detector.h"
#include "sak/process_runner.h"

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <memory>

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

void applyProtectedPartitionFileSystemFallback(PartitionInfoEx* partition) {
    if (!partition || (partition->volume && !partition->volume->file_system.trimmed().isEmpty())) {
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
    applyProtectedPartitionFileSystemFallback(&partition);
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
    if (errorMessage) {
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
    if (partition.partition_number == 0) {
        return {};
    }
    return QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk%1\\Partition%2")
        .arg(disk.disk_number)
        .arg(partition.partition_number);
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
    if (errorMessage) {
        errorMessage->clear();
    }
    if (!broker) {
        if (errorMessage) {
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
            .arg(disk.disk_number)
            .arg(partition.partition_number)
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
    for (const auto& detail : detection.details) {
        if (detail.startsWith(QStringLiteral("APFS space manager block:"))) {
            return false;
        }
    }
    return true;
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

void applyRawFileSystemDetection(PartitionInventory* inventory) {
    if (!inventory) {
        return;
    }

    std::unique_ptr<ElevationBroker> elevatedBroker;
    bool elevatedProbeUnavailable = false;
    for (auto& disk : inventory->disks) {
        for (auto& partition : disk.partitions) {
            if (!elevatedBroker && needsRawFileSystemDetection(partition)) {
                elevatedBroker = std::make_unique<ElevationBroker>();
            }
            applyRawFileSystemDetectionToPartition(
                disk, &partition, elevatedBroker.get(), &elevatedProbeUnavailable, inventory);
        }
    }
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

    auto inventory = parseInventoryJson(result.std_out.toUtf8());
    applyRawFileSystemDetection(&inventory);
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
    inventory.layout_hash = buildLayoutHash(inventory);
    return inventory;
}

QString inventoryPowerShellHeaderScript() {
    return QStringLiteral(R"PS(
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$hasBitLocker = [bool](Get-Command Get-BitLockerVolume -ErrorAction SilentlyContinue)
$physicalDisks = @(Get-PhysicalDisk -ErrorAction SilentlyContinue)
)PS");
}

QString inventoryPowerShellVolumeLookupScript() {
    return QStringLiteral(R"PS(
function Get-SakVolumeForPartition {
  param([Parameter(Mandatory = $true)]$Partition)

  $volume = $Partition | Get-Volume -ErrorAction SilentlyContinue
  if (-not $volume -and $Partition.DriveLetter) {
    $volume = Get-Volume -DriveLetter $Partition.DriveLetter -ErrorAction SilentlyContinue
  }
  if (-not $volume -and $Partition.PSObject.Properties.Name -contains 'AccessPaths') {
    foreach ($accessPath in @($Partition.AccessPaths)) {
      if ([string]::IsNullOrWhiteSpace($accessPath)) { continue }
      $volume = Get-Volume -Path $accessPath -ErrorAction SilentlyContinue
      if ($volume) { break }
    }
  }

  $volume | Select-Object -First 1
}
)PS");
}

QString inventoryPowerShellDiskPrefixScript() {
    return QStringLiteral(R"PS(
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
)PS");
}

QString inventoryPowerShellPartitionScript() {
    return QStringLiteral(R"PS(
    Partitions = @(Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue | ForEach-Object {
      $partition = $_
      $volume = Get-SakVolumeForPartition -Partition $partition
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
)PS");
}

QString inventoryPowerShellFooterScript() {
    return QStringLiteral(R"PS(
  }
}
$disks | ConvertTo-Json -Depth 8 -Compress
)PS");
}

QString StorageInventoryWorker::inventoryPowerShellScript() {
    return inventoryPowerShellHeaderScript() + inventoryPowerShellVolumeLookupScript() +
           inventoryPowerShellDiskPrefixScript() + inventoryPowerShellPartitionScript() +
           inventoryPowerShellFooterScript();
}

}  // namespace sak
