// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_partition_manager_core.cpp
/// @brief Unit tests for Partition Manager core planning and safety.

#include "sak/file_recovery_engine.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_queue.h"
#include "sak/partition_script_builder.h"
#include "sak/storage_inventory_worker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

using namespace sak;

namespace {

constexpr int kExpectedRecoveredFixtureCount = 2;

QByteArray fileSha256(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

}  // namespace

class PartitionManagerCoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void inventoryParser_parsesDiskAndPartition();
    void inventoryParser_keepsRawBasicDiskInitializable();
    void inventoryScript_handlesRawDisksWithoutAbort();
    void safetyValidator_blocksSystemPartitionDelete();
    void scriptBuilder_createRespectsWizardPayload();
    void scriptBuilder_rejectsInvalidCreatePartitionType();
    void scriptBuilder_rejectsUnsupportedAllocationUnit();
    void scriptBuilder_buildsResizeScript();
    void scriptBuilder_buildsMergeScript();
    void scriptBuilder_formatsByPartitionIdentity();
    void scriptBuilder_setsPartitionLabelByMountedDriveLetter();
    void scriptBuilder_buildsAdvancedParityScripts();
    void scriptBuilder_buildsRecoveredPartitionRestoreScript();
    void scriptBuilder_buildsCloneVerificationScript();
    void scriptBuilder_buildsOffsetPartitionCloneScript();
    void scriptBuilder_buildsOsMigrationBootValidationScript();
    void safetyValidator_blocksUnsafeParityOperations();
    void scriptBuilder_buildsClearLevelDiskWipeScript();
    void safetyValidator_blocksUnsafeSystemStyleConversion();
    void scriptBuilder_buildsEmptyDataDiskStyleConversionScript();
    void safetyValidator_requiresCloneOverwriteConfirmation();
    void safetyValidator_createImageUsesReadOnlyRiskAndBlocksUnsafeDestinations();
    void safetyValidator_restoreImageRequiresSizesAndOverwriteConfirmation();
    void safetyValidator_requiresRecoveredPartitionRestoreAcknowledgement();
    void safetyValidator_requiresPartitionRegionCloneConfirmation();
    void safetyValidator_blocksUnsafePayloadTargetDisk();
    void safetyValidator_blocksTooSmallCloneTarget();
    void safetyValidator_blocksTooSmallPartitionRegionClone();
    void safetyValidator_blocksUnsupportedFileSystemConversion();
    void safetyValidator_allowsResizeIntoAdjacentFreeSpace();
    void safetyValidator_blocksResizeBeyondAdjacentFreeSpace();
    void safetyValidator_blocksResizeBelowUsedBytes();
    void safetyValidator_blocksNoopResize();
    void safetyValidator_blocksResizeStartMovePayload();
    void safetyValidator_blocksResizeDonorPayload();
    void safetyValidator_blocksOversizedCreate();
    void safetyValidator_blocksInvalidCreateTypePayload();
    void safetyValidator_blocksCreateOffsetOutsideSelectedRegion();
    void safetyValidator_blocksDynamicUnallocatedCreate();
    void scriptBuilder_buildsMbr2GptScript();
    void scriptBuilder_buildsAllocateFreeSpaceScript();
    void safetyValidator_blocksUnsafeAllocateFreeSpacePayloads();
    void scriptBuilder_buildsOfflineMoveAndMetadataScripts();
    void safetyValidator_allowsConfirmedOfflineRebuildOperations();
    void safetyValidator_blocksUnsafeOfflineRebuildOperations();
    void scriptBuilder_buildsChangeClusterSizeScript();
    void safetyValidator_blocksUnsafeClusterSizePayloads();
    void scriptBuilder_buildsBitLockerMutationScripts();
    void scriptBuilder_buildsDirectDefragScript();
    void safetyValidator_allowsHddDefragOnlyOnReportedHdd();
    void scriptBuilder_buildsBiosBootRepairScript();
    void operationQueue_blocksLayoutMismatch();
    void operationQueue_redoAvailableOnlyAfterUndo();
    void powershellQuoting_escapesSingleQuotes();
    void fileRecoveryEngine_scansAndRestoresOfflineImage();
};

QByteArray fixtureJson() {
    return R"JSON([
      {
        "Number":0,
        "FriendlyName":"Test SSD",
        "SerialNumber":"ABC123",
        "HealthStatus":"Healthy",
        "OperationalStatus":"Online",
        "BusType":"NVMe",
        "MediaType":"SSD",
        "PartitionStyle":"GPT",
        "SmartSummary":"Storage reliability counters available",
        "TemperatureCelsius":38,
        "PowerOnHours":"1234",
        "ReadErrorsTotal":"2",
        "WriteErrorsTotal":"3",
        "WearPercent":"4",
        "Size":"107374182400",
        "IsBoot":true,
        "IsSystem":true,
        "IsReadOnly":false,
        "Partitions":[
          {
            "PartitionNumber":1,
            "Guid":"{efi}",
            "Type":"System",
            "GptType":"C12A7328-F81F-11D2-BA4B-00A0C93EC93B",
            "Offset":"1048576",
            "Size":"104857600",
            "IsBoot":false,
            "IsSystem":true,
            "IsReadOnly":false,
            "Volume":null
          },
          {
            "PartitionNumber":2,
            "Guid":"{data}",
            "Type":"Basic",
            "GptType":"EBD0A0A2-B9E5-4433-87C0-68B6B72699C7",
            "Offset":"105906176",
            "Size":"53687091200",
            "IsBoot":true,
            "IsSystem":true,
            "IsReadOnly":false,
            "Volume":{
              "DriveLetter":"C",
              "FileSystem":"NTFS",
              "FileSystemLabel":"Windows",
              "HealthStatus":"Healthy",
              "Size":"53687091200",
              "SizeRemaining":"26843545600",
              "BitLockerEnabled":true,
              "BitLockerLocked":false,
              "DirtyBitSet":true
            }
          }
        ]
      }
    ])JSON";
}

QByteArray rawBasicDiskFixtureJson() {
    return R"JSON([
      {
        "Number":2,
        "FriendlyName":"Disposable RAW Disk",
        "SerialNumber":"RAW123",
        "HealthStatus":"Healthy",
        "OperationalStatus":"Online",
        "BusType":"SATA",
        "MediaType":"HDD",
        "PartitionStyle":"RAW",
        "Size":"4294967296",
        "IsBoot":false,
        "IsSystem":false,
        "IsReadOnly":false,
        "IsDynamic":false,
        "Partitions":[]
      }
    ])JSON";
}

void makeFixtureDataPartitionMutable(PartitionInventory* inventory) {
    auto& disk = inventory->disks.first();
    auto& partition = disk.partitions[1];
    disk.is_system = false;
    disk.is_boot = false;
    partition.is_system = false;
    partition.is_boot = false;
    partition.is_efi = false;
    partition.is_msr = false;
    partition.is_recovery = false;
    if (partition.volume) {
        partition.volume->bitlocker_enabled = false;
        partition.volume->bitlocker_locked = false;
        partition.volume->dirty_bit_set = false;
    }
}

void appendDisposableTargetDisk(PartitionInventory* inventory,
                                uint32_t diskNumber = 1,
                                uint64_t sizeBytes = 107'374'182'400ULL) {
    PartitionDiskInfo disk;
    disk.disk_number = diskNumber;
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(diskNumber);
    disk.model = QStringLiteral("Disposable Target Disk");
    disk.bus_type = QStringLiteral("SATA");
    disk.media_type = QStringLiteral("HDD");
    disk.partition_style = QStringLiteral("RAW");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = sizeBytes;
    disk.unallocated_regions.append({diskNumber, 1'048'576ULL, sizeBytes - 1'048'576ULL});
    inventory->disks.append(disk);
}

const UnallocatedRegion* adjacentFreeRegionAfter(const PartitionDiskInfo& disk,
                                                 const PartitionInfoEx& partition) {
    const uint64_t partitionEnd = partition.offset_bytes + partition.size_bytes;
    const auto it = std::find_if(disk.unallocated_regions.cbegin(),
                                 disk.unallocated_regions.cend(),
                                 [partitionEnd](const auto& region) {
                                     return region.offset_bytes == partitionEnd;
                                 });
    return it == disk.unallocated_regions.cend() ? nullptr : &(*it);
}

void appendAdjacentDonorPartition(PartitionInventory* inventory) {
    auto& disk = inventory->disks.first();
    const auto& target = disk.partitions.at(1);
    PartitionVolumeInfo donorVolume;
    donorVolume.drive_letter = QStringLiteral("D");
    donorVolume.label = QStringLiteral("Donor");
    donorVolume.file_system = QStringLiteral("NTFS");
    donorVolume.total_bytes = 1024ULL * 1024ULL * 1024ULL;
    donorVolume.free_bytes = 768ULL * 1024ULL * 1024ULL;
    donorVolume.health_status = QStringLiteral("Healthy");

    PartitionInfoEx donor;
    donor.disk_number = disk.disk_number;
    donor.partition_number = 3;
    donor.type_name = QStringLiteral("Basic");
    donor.offset_bytes = target.offset_bytes + target.size_bytes;
    donor.size_bytes = donorVolume.total_bytes;
    donor.volume = donorVolume;
    disk.partitions.append(donor);
}

PartitionInventory singleDataDiskInventory(bool dynamicDisk = false) {
    PartitionInventory inventory;
    PartitionDiskInfo disk;
    disk.disk_number = 2;
    disk.device_path = QStringLiteral("\\\\.\\PhysicalDrive2");
    disk.model = QStringLiteral("Disposable Data Disk");
    disk.bus_type = QStringLiteral("SATA");
    disk.media_type = QStringLiteral("HDD");
    disk.partition_style = dynamicDisk ? QStringLiteral("Dynamic") : QStringLiteral("MBR");
    disk.health_status = QStringLiteral("Healthy");
    disk.operational_status = QStringLiteral("Online");
    disk.size_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    disk.is_dynamic = dynamicDisk;

    PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("T");
    volume.label = dynamicDisk ? QStringLiteral("DynData") : QStringLiteral("Data");
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = 1024ULL * 1024ULL * 1024ULL;
    volume.free_bytes = 768ULL * 1024ULL * 1024ULL;
    volume.health_status = QStringLiteral("Healthy");

    PartitionInfoEx partition;
    partition.disk_number = disk.disk_number;
    partition.partition_number = 1;
    partition.type_name = dynamicDisk ? QStringLiteral("Simple") : QStringLiteral("Basic");
    partition.offset_bytes = 1024ULL * 1024ULL;
    partition.size_bytes = volume.total_bytes;
    partition.volume = volume;
    disk.partitions.append(partition);
    disk.unallocated_regions.append(
        {disk.disk_number,
         partition.offset_bytes + partition.size_bytes,
         disk.size_bytes - partition.offset_bytes - partition.size_bytes});
    inventory.disks.append(disk);
    return inventory;
}

void PartitionManagerCoreTests::inventoryParser_parsesDiskAndPartition() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    QCOMPARE(inventory.disks.size(), 1);
    QCOMPARE(inventory.disks.first().disk_number, 0u);
    QCOMPARE(inventory.disks.first().partitions.size(), 2);
    QVERIFY(inventory.disks.first().partitions.first().is_efi);
    QCOMPARE(inventory.disks.first().operational_status, QStringLiteral("Online"));
    QCOMPARE(inventory.disks.first().temperature_celsius, 38);
    QCOMPARE(inventory.disks.first().power_on_hours, 1234ULL);
    QCOMPARE(inventory.disks.first().read_errors_total, 2ULL);
    QCOMPARE(inventory.disks.first().write_errors_total, 3ULL);
    QCOMPARE(inventory.disks.first().wear_percent, 4ULL);
    QVERIFY(inventory.disks.first().partitions.at(1).volume->bitlocker_enabled);
    QVERIFY(inventory.disks.first().partitions.at(1).volume->dirty_bit_set);
    QVERIFY(!inventory.layout_hash.isEmpty());
}

void PartitionManagerCoreTests::inventoryParser_keepsRawBasicDiskInitializable() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(rawBasicDiskFixtureJson());
    QCOMPARE(inventory.disks.size(), 1);
    const auto& disk = inventory.disks.first();
    QCOMPARE(disk.disk_number, 2u);
    QCOMPARE(disk.partition_style, QStringLiteral("RAW"));
    QVERIFY2(!disk.is_dynamic, "RAW partition style is not a dynamic-disk signal.");
    QCOMPARE(disk.partitions.size(), 0);
    QCOMPARE(disk.unallocated_regions.size(), 1);
    QCOMPARE(disk.unallocated_regions.first().offset_bytes, 0ULL);
    QCOMPARE(disk.unallocated_regions.first().size_bytes, disk.size_bytes);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = disk.disk_number;
    target.size_bytes = disk.size_bytes;
    const auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::InitializeDisk, target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY2(preview.canApply(), qPrintable(preview.blockers.join(QStringLiteral("; "))));
}

void PartitionManagerCoreTests::inventoryScript_handlesRawDisksWithoutAbort() {
    const QString script = StorageInventoryWorker::inventoryPowerShellScript();
    QVERIFY2(script.contains(QStringLiteral("$ProgressPreference = 'SilentlyContinue'")),
             "Inventory script should suppress progress records so stdout remains JSON-only.");
    QVERIFY2(script.contains(QStringLiteral(
                 "Get-Partition -DiskNumber $disk.Number -ErrorAction SilentlyContinue")),
             "RAW disks with no partitions must not abort the whole inventory scan.");
}

void PartitionManagerCoreTests::safetyValidator_blocksSystemPartitionDelete() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = inventory.disks.first().partitions.at(1).size_bytes;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::Delete,
                                                              target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("protected")));
}

void PartitionManagerCoreTests::scriptBuilder_createRespectsWizardPayload() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = 5;
    target.offset_bytes = 1'048'576;
    target.size_bytes = 64 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("33554432");
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("file_system")] = QStringLiteral("exFAT");
    payload[QStringLiteral("label")] = QStringLiteral("Field Media");
    payload[QStringLiteral("drive_letter")] = QStringLiteral("M");
    payload[QStringLiteral("full_format")] = true;
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("gpt_type")] = QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 5")));
    QVERIFY(script.script.contains(QStringLiteral("-DriveLetter M")));
    QVERIFY(script.script.contains(QStringLiteral("-Offset 3145728")));
    QVERIFY(script.script.contains(
        QStringLiteral("-GptType '{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}'")));
    QVERIFY(
        script.script.contains(QStringLiteral("Format-Volume -Partition $p -FileSystem EXFAT")));
    QVERIFY(script.script.contains(
        QStringLiteral("-NewFileSystemLabel 'Field Media' -Full -AllocationUnitSize 4096")));
}

void PartitionManagerCoreTests::scriptBuilder_rejectsInvalidCreatePartitionType() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = 5;
    target.size_bytes = 64 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("33554432");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("mbr_type")] = QStringLiteral("FAT32");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("incompatible")));
}

void PartitionManagerCoreTests::scriptBuilder_rejectsUnsupportedAllocationUnit() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("12345");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("allocation unit")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsResizeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("20971520");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition")));
    QVERIFY(script.script.contains(QStringLiteral("Get-PartitionSupportedSize")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsMergeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 50 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] = QStringLiteral("2");
    payload[QStringLiteral("target_folder")] = QStringLiteral("Merged");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Merge, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition")));
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition")));
}

void PartitionManagerCoreTests::scriptBuilder_formatsByPartitionIdentity() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    QJsonObject payload;
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("65536");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Format, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(
        script.script.contains(QStringLiteral("Get-Partition -DiskNumber 1 -PartitionNumber 3")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -FileSystem NTFS")));
    QVERIFY(script.script.contains(QStringLiteral("-AllocationUnitSize 65536")));
    QVERIFY(!script.script.contains(QStringLiteral("Format-Volume -DriveLetter")));
}

void PartitionManagerCoreTests::scriptBuilder_setsPartitionLabelByMountedDriveLetter() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 1;
    target.partition_number = 3;
    target.size_bytes = 10 * 1024 * 1024;
    target.drive_letter = QStringLiteral("E");
    QJsonObject payload;
    payload[QStringLiteral("label")] = QStringLiteral("Backup Data");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::SetPartitionLabel, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Set-Volume -DriveLetter E")));
    QVERIFY(script.script.contains(QStringLiteral("-NewFileSystemLabel 'Backup Data'")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsAdvancedParityScripts() {
    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = 1;
    partitionTarget.partition_number = 3;
    partitionTarget.size_bytes = 10 * 1024 * 1024;
    partitionTarget.drive_letter = QStringLiteral("E");

    PartitionScriptBuilder builder;
    auto check = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::CheckFileSystem, partitionTarget));
    QVERIFY(check.valid());
    QVERIFY(check.script.contains(QStringLiteral("Repair-Volume -DriveLetter E -Scan")));

    auto hidden = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionHidden,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("hidden"), true}}));
    QVERIFY(hidden.valid());
    QVERIFY(hidden.script.contains(QStringLiteral("-IsHidden $true")));
    QVERIFY(hidden.script.contains(QStringLiteral("Remove-PartitionAccessPath")));

    auto active = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionActive,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("active"), true}}));
    QVERIFY(active.valid());
    QVERIFY(active.script.contains(QStringLiteral("-IsActive $true")));

    auto type = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::SetPartitionTypeId,
        partitionTarget,
        QJsonObject{
            {QStringLiteral("type_id"), QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7")}}));
    QVERIFY(type.valid());
    QVERIFY(type.script.contains(QStringLiteral("Set-Partition")));
    QVERIFY(type.script.contains(QStringLiteral("-GptType $typeId")));

    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = 7;
    auto init = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::InitializeDisk,
        diskTarget,
        QJsonObject{{QStringLiteral("target_style"), QStringLiteral("GPT")}}));
    QVERIFY(init.valid());
    QVERIFY(init.script.contains(QStringLiteral("Initialize-Disk -Number 7 -PartitionStyle GPT")));

    auto deleteAll = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::DeleteAllPartitions, diskTarget));
    QVERIFY(deleteAll.valid());
    QVERIFY(deleteAll.script.contains(QStringLiteral("Remove-Partition -DiskNumber 7")));

    auto surface = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SurfaceTest, diskTarget));
    QVERIFY(surface.valid());
    QVERIFY(surface.script.contains(QStringLiteral("Get-StorageReliabilityCounter")));

    auto recovery = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::PartitionRecoveryScan,
        diskTarget,
        QJsonObject{{QStringLiteral("scan_mode"), QStringLiteral("Quick")}}));
    QVERIFY(recovery.valid());
    QVERIFY(recovery.script.contains(QStringLiteral("\\\\.\\PhysicalDrive7")));
    QVERIFY(recovery.script.contains(QStringLiteral("Candidate partition boot sector")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsRecoveredPartitionRestoreScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    QJsonObject payload;
    payload[QStringLiteral("offset_bytes")] = QStringLiteral("1048576000");
    payload[QStringLiteral("size_bytes")] = QStringLiteral("209715200");
    payload[QStringLiteral("type_id")] = QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
    payload[QStringLiteral("partition_style")] = QStringLiteral("GPT");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RestoreRecoveredPartition, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 4")));
    QVERIFY(script.script.contains(QStringLiteral("-Offset 1048576000")));
    QVERIFY(script.script.contains(QStringLiteral("-GptType")));
    QVERIFY(script.script.contains(QStringLiteral("Candidate overlaps existing partition")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsCloneVerificationScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 3;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("C:\\images\\source.img");
    payload[QStringLiteral("target_path")] = QStringLiteral("C:\\images\\target.img");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Full verification");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Copy-SakBytes")));
    QVERIFY(script.script.contains(QStringLiteral("Running full clone verification")));
    QVERIFY(script.script.contains(QStringLiteral("Assert-SakFullCopy")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOffsetPartitionCloneScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("source_offset_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Sample verification");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$sourceOffset = [uint64]4096")));
    QVERIFY(script.script.contains(QStringLiteral("$targetOffset = [uint64]1048576")));
    QVERIFY(script.script.contains(QStringLiteral(
        "Assert-SakSampleCopy $srcVerify $dstVerify $expectedBytes $sourceOffset $targetOffset")));
    QVERIFY(script.script.contains(QStringLiteral("[int64]($targetStart + $point)")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOsMigrationBootValidationScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Sample verification");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::MigrateOs,
                                                              target,
                                                              payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("SAK OS migration boot validation")));
    QVERIFY(script.script.contains(QStringLiteral("EFI System Partition")));
    QVERIFY(script.script.contains(QStringLiteral("BIOS validation passed")));
    QVERIFY(script.script.contains(QStringLiteral("Boot Repair")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeParityOperations() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    auto& partition = disk.partitions[1];
    partition.is_system = false;
    partition.is_boot = false;

    PartitionOperationPlanner planner;
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = disk.disk_number;
    auto init = PartitionOperationPlanner::makeOperation(PartitionOperationType::InitializeDisk,
                                                         diskTarget);
    auto initPreview = planner.previewOperation(inventory, init);
    QVERIFY(!initPreview.canApply());
    QVERIFY(initPreview.blockers.join(' ').contains(QStringLiteral("empty/raw disk")));

    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = disk.disk_number;
    partitionTarget.partition_number = partition.partition_number;
    partitionTarget.size_bytes = partition.size_bytes;
    partitionTarget.drive_letter = partition.volume->drive_letter;
    auto active =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::SetPartitionActive,
                                                 partitionTarget,
                                                 QJsonObject{{QStringLiteral("active"), true}});
    auto activePreview = planner.previewOperation(inventory, active);
    QVERIFY(!activePreview.canApply());
    QVERIFY(activePreview.blockers.join(' ').contains(QStringLiteral("MBR")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsClearLevelDiskWipeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::WipeDisk,
                                                              target);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Clear-Disk -Number 4 -RemoveData -RemoveOEM")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -FileSystem NTFS -Full")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition -DiskNumber 4")));

    QJsonObject ssdPayload;
    ssdPayload[QStringLiteral("ssd_secure_erase")] = true;
    const auto ssdScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::WipeDisk, target, ssdPayload));
    QVERIFY(ssdScript.valid());
    QVERIFY(ssdScript.script.contains(QStringLiteral("Optimize-Volume -DriveLetter")));
    QVERIFY(ssdScript.script.contains(QStringLiteral("-ReTrim -Verbose")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeSystemStyleConversion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().partition_style = QStringLiteral("MBR");
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("GPT");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("MBR2GPT")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsEmptyDataDiskStyleConversionScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 4;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("MBR");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Set-Disk -Number 4 -IsOffline $false")));
    QVERIFY(script.script.contains(QStringLiteral("System disk conversion must use MBR2GPT")));
    QVERIFY(script.script.contains(
        QStringLiteral("Data disk partition-style conversion requires an empty disk")));
    QVERIFY(script.script.contains(QStringLiteral("Clear-Disk -Number 4 -RemoveData")));
    QVERIFY(
        script.script.contains(QStringLiteral("Initialize-Disk -Number 4 -PartitionStyle MBR")));
}

void PartitionManagerCoreTests::safetyValidator_requiresCloneOverwriteConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::
    safetyValidator_createImageUsesReadOnlyRiskAndBlocksUnsafeDestinations() {
    const auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    target.size_bytes = inventory.disks.first().size_bytes;

    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("D:\\images\\disk0.img");
    payload[QStringLiteral("source_size_bytes")] = QString::number(target.size_bytes);
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CreateImage,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
    QCOMPARE(preview.operations.first().risk, OperationRisk::ReadOnly);

    operation.payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("file path")));
    const auto rawTargetScript = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!rawTargetScript.valid());
    QVERIFY(rawTargetScript.blockers.join(' ').contains(QStringLiteral("file path")));

    operation.payload[QStringLiteral("target_path")] = QStringLiteral("C:\\disk0.img");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("source disk")));
}

void PartitionManagerCoreTests::
    safetyValidator_restoreImageRequiresSizesAndOverwriteConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().is_system = false;
    inventory.disks.first().is_boot = false;
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    target.size_bytes = inventory.disks.first().size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("D:\\images\\disk0.img");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::RestoreImage,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    const auto missingEvidenceBlockers = preview.blockers.join(' ');
    QVERIFY(missingEvidenceBlockers.contains(QStringLiteral("overwrite confirmation")));
    QVERIFY(missingEvidenceBlockers.contains(QStringLiteral("known image and target sizes")));
    auto script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("known image and target sizes")));

    operation.payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    operation.payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(!script.valid());
    QVERIFY(script.blockers.join(' ').contains(QStringLiteral("smaller than source")));

    operation.payload[QStringLiteral("target_size_bytes")] = QStringLiteral("4194304");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
    script = PartitionScriptBuilder().buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("PhysicalDrive0")));
}

void PartitionManagerCoreTests::safetyValidator_requiresRecoveredPartitionRestoreAcknowledgement() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    inventory.disks.first().is_system = false;
    inventory.disks.first().is_boot = false;
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("offset_bytes")] = QStringLiteral("85899345920");
    payload[QStringLiteral("size_bytes")] = QStringLiteral("104857600");
    payload[QStringLiteral("type_id")] = QStringLiteral("EBD0A0A2-B9E5-4433-87C0-68B6B72699C7");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RestoreRecoveredPartition, target, payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("acknowledgement")));

    operation.payload[QStringLiteral("restore_acknowledged")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_requiresPartitionRegionCloneConfirmation() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendDisposableTargetDisk(&inventory);
    const auto& partition = inventory.disks.first().partitions.at(1);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_disk_number")] = 1;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("overwrite confirmation")));

    operation.payload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafePayloadTargetDisk() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory);
    inventory.disks.last().is_system = true;

    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("4194304");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Payload target disk")));
}

void PartitionManagerCoreTests::safetyValidator_blocksTooSmallCloneTarget() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    appendDisposableTargetDisk(&inventory, 1, 1'048'576ULL);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::CloneDisk,
                                                              target,
                                                              payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
}

void PartitionManagerCoreTests::safetyValidator_blocksTooSmallPartitionRegionClone() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendDisposableTargetDisk(&inventory, 1, 1'048'576ULL);
    const auto& partition = inventory.disks.first().partitions.at(1);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\C:");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("2097152");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_disk_number")] = 1;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ClonePartition, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("smaller than the source")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsupportedFileSystemConversion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    auto& partition = disk.partitions[1];
    disk.is_system = false;
    partition.is_system = false;
    partition.is_boot = false;
    partition.volume->file_system = QStringLiteral("NTFS");

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::ConvertFileSystem, target);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("FAT/FAT32")));
}

void PartitionManagerCoreTests::safetyValidator_allowsResizeIntoAdjacentFreeSpace() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    QVERIFY(adjacentFreeRegionAfter(disk, partition) != nullptr);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.size_bytes + 1024 * 1024);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeBeyondAdjacentFreeSpace() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    const auto* region = adjacentFreeRegionAfter(disk, partition);
    QVERIFY(region != nullptr);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.size_bytes + region->size_bytes + 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("contiguous free space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeBelowUsedBytes() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] =
        QString::number(partition.volume->total_bytes - partition.volume->free_bytes - 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("used volume space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksNoopResize() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("must change")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeStartMovePayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes + 1);
    payload[QStringLiteral("target_offset_bytes")] = QString::number(partition.offset_bytes - 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("offline move engine")));
}

void PartitionManagerCoreTests::safetyValidator_blocksResizeDonorPayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;
    QJsonObject payload;
    payload[QStringLiteral("target_size_bytes")] = QString::number(partition.size_bytes + 1);
    payload[QStringLiteral("donor_partition_number")] = QStringLiteral("1");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Resize, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Donor-space")));
}

void PartitionManagerCoreTests::safetyValidator_blocksOversizedCreate() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(region.size_bytes + 1);
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("unallocated region")));
}

void PartitionManagerCoreTests::safetyValidator_blocksInvalidCreateTypePayload() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("1048576");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("mbr_type")] = QStringLiteral("FAT32");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("incompatible")));
}

void PartitionManagerCoreTests::safetyValidator_blocksCreateOffsetOutsideSelectedRegion() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QString::number(region.size_bytes);
    payload[QStringLiteral("relative_offset_bytes")] = QStringLiteral("1048576");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected unallocated region")));
}

void PartitionManagerCoreTests::safetyValidator_blocksDynamicUnallocatedCreate() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    auto& disk = inventory.disks.first();
    disk.is_system = false;
    disk.is_dynamic = true;
    const auto region = disk.unallocated_regions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Unallocated;
    target.disk_number = region.disk_number;
    target.offset_bytes = region.offset_bytes;
    target.size_bytes = region.size_bytes;
    QJsonObject payload;
    payload[QStringLiteral("size_bytes")] = QStringLiteral("1048576");
    auto operation =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::Create, target, payload);

    PartitionOperationPlanner planner;
    const auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("Dynamic disks")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsMbr2GptScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("target_style")] = QStringLiteral("GPT");
    payload[QStringLiteral("mode")] = QStringLiteral("mbr2gpt");
    auto operation = PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPartitionStyle, target, payload);

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(operation);
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("mbr2gpt.exe /validate")));
    QVERIFY(script.script.contains(QStringLiteral("mbr2gpt.exe /convert")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsAllocateFreeSpaceScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 512ULL * 1024ULL * 1024ULL;
    target.drive_letter = QStringLiteral("T");

    QJsonObject payload;
    payload[QStringLiteral("source_partition_number")] = QStringLiteral("2");
    payload[QStringLiteral("source_size_bytes")] = QStringLiteral("536870912");
    payload[QStringLiteral("bytes_to_allocate")] = QStringLiteral("134217728");
    payload[QStringLiteral("source_drive_letter")] = QStringLiteral("S");
    payload[QStringLiteral("source_file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("source_label")] = QStringLiteral("Donor");
    payload[QStringLiteral("backup_directory")] = QStringLiteral("C:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::AllocateFreeSpace, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("Donor partition must be directly after")));
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe $from $to /MIR /COPYALL")));
    QVERIFY(script.script.contains(QStringLiteral("Get-SakFileManifest")));
    QVERIFY(script.script.contains(QStringLiteral("Remove-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("Resize-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("New-Partition -DiskNumber 2")));
    QVERIFY(script.script.contains(QStringLiteral("Compare-Object")));
    QVERIFY(script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $sourceDrive")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeAllocateFreeSpacePayloads() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    appendAdjacentDonorPartition(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);
    const auto& donor = disk.partitions.last();

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(inventory,
                                            PartitionOperationPlanner::makeOperation(
                                                PartitionOperationType::AllocateFreeSpace, target));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("donor partition")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("amount")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject validPayload;
    validPayload[QStringLiteral("source_partition_number")] =
        QString::number(donor.partition_number);
    validPayload[QStringLiteral("source_size_bytes")] = QString::number(donor.size_bytes);
    validPayload[QStringLiteral("bytes_to_allocate")] = QStringLiteral("134217728");
    validPayload[QStringLiteral("source_drive_letter")] = donor.volume->drive_letter;
    validPayload[QStringLiteral("source_file_system")] = donor.volume->file_system;
    validPayload[QStringLiteral("source_label")] = donor.volume->label;
    validPayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    validPayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::AllocateFreeSpace, target, validPayload));
    QVERIFY(preview.canApply());

    QJsonObject sameVolumeBackup = validPayload;
    sameVolumeBackup[QStringLiteral("backup_directory")] = QStringLiteral("D:\\backup");
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::AllocateFreeSpace, target, sameVolumeBackup));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("target or donor")));

    QJsonObject tooLarge = validPayload;
    tooLarge[QStringLiteral("bytes_to_allocate")] = QStringLiteral("1000000000");
    preview =
        planner.previewOperation(inventory,
                                 PartitionOperationPlanner::makeOperation(
                                     PartitionOperationType::AllocateFreeSpace, target, tooLarge));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("safety reserve")));

    QJsonObject staleSize = validPayload;
    staleSize[QStringLiteral("source_size_bytes")] = QStringLiteral("1");
    preview =
        planner.previewOperation(inventory,
                                 PartitionOperationPlanner::makeOperation(
                                     PartitionOperationType::AllocateFreeSpace, target, staleSize));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("stale")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsOfflineMoveAndMetadataScripts() {
    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = 2;
    partitionTarget.partition_number = 1;
    partitionTarget.size_bytes = 1024ULL * 1024ULL * 1024ULL;
    partitionTarget.drive_letter = QStringLiteral("T");

    QJsonObject movePayload;
    movePayload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    movePayload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    movePayload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    movePayload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    movePayload[QStringLiteral("label")] = QStringLiteral("Moved");
    movePayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    movePayload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto moveScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::MovePartition, partitionTarget, movePayload));
    QVERIFY(moveScript.valid());
    QVERIFY(moveScript.script.contains(QStringLiteral("Remove-Partition -DiskNumber 2")));
    QVERIFY(moveScript.script.contains(QStringLiteral("New-Partition -DiskNumber 2")));
    QVERIFY(moveScript.script.contains(QStringLiteral("-Offset $targetOffset")));
    QVERIFY(moveScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));

    QJsonObject primaryPayload = movePayload;
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QStringLiteral("1073741824");
    const auto primaryScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertPrimaryLogical, partitionTarget, primaryPayload));
    QVERIFY(primaryScript.valid());
    QVERIFY(primaryScript.script.contains(QStringLiteral("Invoke-SakDiskPart")));
    QVERIFY(primaryScript.script.contains(QStringLiteral("create partition logical")));
    QVERIFY(primaryScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));

    const auto serialScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ChangeVolumeSerialNumber, partitionTarget, movePayload));
    QVERIFY(serialScript.valid());
    QVERIFY(serialScript.script.contains(QStringLiteral("Get-SakVolumeSerial")));
    QVERIFY(serialScript.script.contains(QStringLiteral("Format-Volume -DriveLetter $drive")));
    QVERIFY(serialScript.script.contains(QStringLiteral("Volume serial changed")));

    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = 2;
    diskTarget.size_bytes = 4ULL * 1024ULL * 1024ULL * 1024ULL;
    QJsonObject dynamicPayload = primaryPayload;
    const auto dynamicScript = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ConvertDynamicDiskToBasic, diskTarget, dynamicPayload));
    QVERIFY(dynamicScript.valid());
    QVERIFY(dynamicScript.script.contains(QStringLiteral("delete volume override")));
    QVERIFY(dynamicScript.script.contains(QStringLiteral("convert basic")));
    QVERIFY(dynamicScript.script.contains(QStringLiteral("Assert-SakManifestMatch")));
}

void PartitionManagerCoreTests::safetyValidator_allowsConfirmedOfflineRebuildOperations() {
    auto inventory = singleDataDiskInventory();
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.first();

    PartitionTarget partitionTarget;
    partitionTarget.kind = PartitionTargetKind::Partition;
    partitionTarget.disk_number = disk.disk_number;
    partitionTarget.partition_number = partition.partition_number;
    partitionTarget.size_bytes = partition.size_bytes;
    partitionTarget.drive_letter = partition.volume->drive_letter;

    QJsonObject payload;
    payload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    payload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    payload[QStringLiteral("drive_letter")] = partition.volume->drive_letter;
    payload[QStringLiteral("file_system")] = partition.volume->file_system;
    payload[QStringLiteral("label")] = partition.volume->label;
    payload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MovePartition, partitionTarget, payload));
    QVERIFY(preview.canApply());

    QJsonObject primaryPayload = payload;
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertPrimaryLogical, partitionTarget, primaryPayload));
    QVERIFY(preview.canApply());

    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeVolumeSerialNumber, partitionTarget, payload));
    QVERIFY(preview.canApply());

    auto dynamicInventory = singleDataDiskInventory(true);
    PartitionTarget diskTarget;
    diskTarget.kind = PartitionTargetKind::Disk;
    diskTarget.disk_number = dynamicInventory.disks.first().disk_number;
    diskTarget.size_bytes = dynamicInventory.disks.first().size_bytes;
    preview = planner.previewOperation(
        dynamicInventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertDynamicDiskToBasic, diskTarget, primaryPayload));
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeOfflineRebuildOperations() {
    auto inventory = singleDataDiskInventory();
    const auto& partition = inventory.disks.first().partitions.first();
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = partition.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(PartitionOperationType::MovePartition, target));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject sameVolumePayload;
    sameVolumePayload[QStringLiteral("target_offset_bytes")] = QStringLiteral("2147483648");
    sameVolumePayload[QStringLiteral("target_size_bytes")] = QStringLiteral("1073741824");
    sameVolumePayload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    sameVolumePayload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    sameVolumePayload[QStringLiteral("backup_directory")] = QStringLiteral("T:\\backup");
    sameVolumePayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MovePartition, target, sameVolumePayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected volume")));

    auto multiPartitionInventory = inventory;
    PartitionInfoEx extra = multiPartitionInventory.disks.first().partitions.first();
    extra.partition_number = 2;
    extra.offset_bytes = extra.offset_bytes + extra.size_bytes;
    extra.volume->drive_letter = QStringLiteral("U");
    multiPartitionInventory.disks.first().partitions.append(extra);
    QJsonObject primaryPayload = sameVolumePayload;
    primaryPayload[QStringLiteral("backup_directory")] = QStringLiteral("Z:\\backup");
    primaryPayload[QStringLiteral("target_layout")] = QStringLiteral("logical");
    primaryPayload[QStringLiteral("source_size_bytes")] = QString::number(partition.size_bytes);
    preview = planner.previewOperation(
        multiPartitionInventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ConvertPrimaryLogical, target, primaryPayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("single mounted")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsChangeClusterSizeScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.size_bytes = 128 * 1024 * 1024;
    target.drive_letter = QStringLiteral("T");

    QJsonObject payload;
    payload[QStringLiteral("drive_letter")] = QStringLiteral("T");
    payload[QStringLiteral("file_system")] = QStringLiteral("NTFS");
    payload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    payload[QStringLiteral("label")] = QStringLiteral("Data");
    payload[QStringLiteral("backup_directory")] = QStringLiteral("C:\\SAKBackups");
    payload[QStringLiteral("target_wipe_confirmed")] = true;

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::ChangeClusterSize, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$drive = 'T'")));
    QVERIFY(script.script.contains(QStringLiteral("robocopy.exe $from $to /MIR /COPYALL")));
    QVERIFY(script.script.contains(QStringLiteral("Get-SakFileManifest")));
    QVERIFY(script.script.contains(QStringLiteral("Format-Volume -DriveLetter $drive")));
    QVERIFY(script.script.contains(QStringLiteral("-AllocationUnitSize $allocationUnitBytes")));
    QVERIFY(script.script.contains(QStringLiteral("Compare-Object")));
    QVERIFY(script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $drive -Scan")));
}

void PartitionManagerCoreTests::safetyValidator_blocksUnsafeClusterSizePayloads() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    const auto& disk = inventory.disks.first();
    const auto& partition = disk.partitions.at(1);

    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = disk.disk_number;
    target.partition_number = partition.partition_number;
    target.size_bytes = partition.size_bytes;
    target.drive_letter = partition.volume->drive_letter;

    PartitionOperationPlanner planner;
    auto missing =
        PartitionOperationPlanner::makeOperation(PartitionOperationType::ChangeClusterSize, target);
    auto preview = planner.previewOperation(inventory, missing);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("allocation unit")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("backup directory")));
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("confirmation")));

    QJsonObject sameVolumePayload;
    sameVolumePayload[QStringLiteral("allocation_unit_bytes")] = QStringLiteral("4096");
    sameVolumePayload[QStringLiteral("backup_directory")] =
        QStringLiteral("%1:\\backup").arg(target.drive_letter.left(1).toUpper());
    sameVolumePayload[QStringLiteral("target_wipe_confirmed")] = true;
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeClusterSize, target, sameVolumePayload));
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("selected volume")));

    QJsonObject validPayload = sameVolumePayload;
    validPayload[QStringLiteral("backup_directory")] = QStringLiteral("D:\\SAKBackups");
    preview = planner.previewOperation(
        inventory,
        PartitionOperationPlanner::makeOperation(
            PartitionOperationType::ChangeClusterSize, target, validPayload));
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::scriptBuilder_buildsBitLockerMutationScripts() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.drive_letter = QStringLiteral("D");

    QJsonObject unlockPayload;
    unlockPayload[QStringLiteral("recovery_password")] =
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888");
    auto unlock = PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerUnlock,
                                                           target,
                                                           unlockPayload);
    PartitionScriptBuilder builder;
    const auto unlockScript = builder.buildScript(unlock);
    QVERIFY(unlockScript.valid());
    QVERIFY(unlockScript.script.contains(QStringLiteral("manage-bde.exe -unlock")));
    QVERIFY(unlockScript.script.contains(
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888")));
    QVERIFY(unlockScript.dry_run_script.contains(QStringLiteral("<redacted>")));
    QVERIFY(!unlockScript.dry_run_script.contains(
        QStringLiteral("111111-222222-333333-444444-555555-666666-777777-888888")));

    const auto suspend = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerSuspend, target));
    QVERIFY(suspend.valid());
    QVERIFY(suspend.script.contains(QStringLiteral("manage-bde.exe -protectors -disable")));

    const auto resume = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::BitLockerResume, target));
    QVERIFY(resume.valid());
    QVERIFY(resume.script.contains(QStringLiteral("manage-bde.exe -protectors -enable")));
}

void PartitionManagerCoreTests::scriptBuilder_buildsDirectDefragScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 2;
    target.partition_number = 1;
    target.drive_letter = QStringLiteral("T");

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::DefragVolume, target));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("$driveLetter = 'T'")));
    QVERIFY(script.script.contains(QStringLiteral("Refusing HDD defrag on SSD/NVMe media")));
    QVERIFY(script.script.contains(
        QStringLiteral("Optimize-Volume -DriveLetter $driveLetter -Analyze")));
    QVERIFY(script.script.contains(
        QStringLiteral("Optimize-Volume -DriveLetter $driveLetter -Defrag")));
    QVERIFY(
        script.script.contains(QStringLiteral("Repair-Volume -DriveLetter $driveLetter -Scan")));
}

void PartitionManagerCoreTests::safetyValidator_allowsHddDefragOnlyOnReportedHdd() {
    auto inventory = StorageInventoryWorker::parseInventoryJson(fixtureJson());
    makeFixtureDataPartitionMutable(&inventory);
    PartitionTarget target;
    target.kind = PartitionTargetKind::Partition;
    target.disk_number = 0;
    target.partition_number = 2;
    target.size_bytes = inventory.disks.first().partitions.at(1).size_bytes;
    target.drive_letter = QStringLiteral("C");

    PartitionOperationPlanner planner;
    auto operation = PartitionOperationPlanner::makeOperation(PartitionOperationType::DefragVolume,
                                                              target);
    auto preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("SSD/NVMe")));

    inventory.disks.first().media_type = QStringLiteral("Unspecified");
    inventory.disks.first().bus_type = QStringLiteral("USB");
    inventory.disks.first().model = QStringLiteral("Virtual Disk");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(!preview.canApply());
    QVERIFY(preview.blockers.join(' ').contains(QStringLiteral("reported as HDD")));

    inventory.disks.first().media_type = QStringLiteral("HDD");
    inventory.disks.first().model = QStringLiteral("Disposable HDD");
    preview = planner.previewOperation(inventory, operation);
    QVERIFY(preview.canApply());
}

void PartitionManagerCoreTests::scriptBuilder_buildsBiosBootRepairScript() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = 0;
    QJsonObject payload;
    payload[QStringLiteral("windows_path")] = QStringLiteral("D:\\Windows");
    payload[QStringLiteral("esp_letter")] = QStringLiteral("S");
    payload[QStringLiteral("boot_mode")] = QStringLiteral("BIOS");

    PartitionScriptBuilder builder;
    const auto script = builder.buildScript(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::RepairBoot, target, payload));
    QVERIFY(script.valid());
    QVERIFY(script.script.contains(QStringLiteral("bcdboot.exe 'D:\\Windows' /s S: /f BIOS")));
    QVERIFY(script.script.contains(QStringLiteral("bootsect.exe /nt60 S: /mbr")));
}

void PartitionManagerCoreTests::operationQueue_blocksLayoutMismatch() {
    PartitionOperationQueue queue;
    OperationPreview preview;
    preview.before_layout_hash = QStringLiteral("a");
    preview.operations.append(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::OptimizeSsd, PartitionTarget{}));
    queue.addPreview(preview);
    QVERIFY(queue.canApply(QStringLiteral("a")));
    QVERIFY(!queue.canApply(QStringLiteral("b")));
}

void PartitionManagerCoreTests::operationQueue_redoAvailableOnlyAfterUndo() {
    PartitionOperationQueue queue;
    OperationPreview preview;
    preview.before_layout_hash = QStringLiteral("layout");
    preview.operations.append(PartitionOperationPlanner::makeOperation(
        PartitionOperationType::OptimizeSsd, PartitionTarget{}));
    queue.addPreview(preview);

    QVERIFY(!queue.canRedo());
    QVERIFY(queue.undo());
    QVERIFY(queue.isEmpty());
    QVERIFY(queue.canRedo());
    QVERIFY(queue.redo());
    QVERIFY(!queue.isEmpty());
    QVERIFY(!queue.canRedo());

    QVERIFY(queue.undo());
    QVERIFY(queue.canRedo());
    queue.discard();
    QVERIFY(!queue.canRedo());

    queue.addPreview(preview);
    QVERIFY(!queue.canRedo());
}

void PartitionManagerCoreTests::powershellQuoting_escapesSingleQuotes() {
    QCOMPARE(PartitionScriptBuilder::quotePowerShell(QStringLiteral("Randy's Disk")),
             QStringLiteral("'Randy''s Disk'"));
}

void PartitionManagerCoreTests::fileRecoveryEngine_scansAndRestoresOfflineImage() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QDir root(temp.path());
    QVERIFY(root.mkpath(QStringLiteral("source")));
    QVERIFY(root.mkpath(QStringLiteral("restore")));

    const QString imagePath = root.filePath(QStringLiteral("source/recovery-image.bin"));
    QFile image(imagePath);
    QVERIFY(image.open(QIODevice::WriteOnly));
    const QByteArray pdf =
        QByteArrayLiteral("%PDF-1.7\n1 0 obj\n<< /Type /Catalog >>\nendobj\n%%EOF");
    const QByteArray jpeg = QByteArray::fromHex("ffd8ffe000104a46494600010100000100010000ffd9");
    image.write(QByteArrayLiteral("padding-before"));
    image.write(pdf);
    image.write(QByteArrayLiteral("padding-between"));
    image.write(jpeg);
    image.write(QByteArrayLiteral("padding-after"));
    image.close();

    const QByteArray beforeHash = fileSha256(imagePath);
    FileRecoveryScanOptions scanOptions;
    scanOptions.image_path = imagePath;
    const auto scan = FileRecoveryEngine::scanOfflineImage(scanOptions);
    QVERIFY(scan.source_opened_read_only);
    QVERIFY(scan.warnings.isEmpty());
    QCOMPARE(scan.candidates.size(), kExpectedRecoveredFixtureCount);

    QStringList extensions;
    for (const auto& candidate : scan.candidates) {
        extensions.append(candidate.extension);
        QVERIFY(candidate.size_bytes > 0);
        QVERIFY(!candidate.sha256.isEmpty());
    }
    QVERIFY(extensions.contains(QStringLiteral("pdf")));
    QVERIFY(extensions.contains(QStringLiteral("jpg")));

    FileRecoveryRestoreOptions restoreOptions;
    restoreOptions.image_path = imagePath;
    restoreOptions.destination_directory = root.filePath(QStringLiteral("restore"));
    restoreOptions.candidates = scan.candidates;
    const auto restore = FileRecoveryEngine::restoreCandidates(restoreOptions);
    QVERIFY(restore.source_opened_read_only);
    QVERIFY(restore.source_not_mutated);
    QVERIFY(restore.warnings.isEmpty());
    QCOMPARE(restore.restored_paths.size(), kExpectedRecoveredFixtureCount);
    QCOMPARE(fileSha256(imagePath), beforeHash);

    for (const auto& restoredPath : restore.restored_paths) {
        QVERIFY(QFileInfo::exists(restoredPath));
        QVERIFY(QFileInfo(restoredPath).size() > 0);
    }
}

QTEST_MAIN(PartitionManagerCoreTests)
#include "test_partition_manager_core.moc"
