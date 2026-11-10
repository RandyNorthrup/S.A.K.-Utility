// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_partition_operation_requests.cpp
/// @brief What the panel and the certification runner both queue for a person's choices.

#include "sak/partition_operation_planner.h"
#include "sak/partition_operation_requests.h"
#include "sak/partition_script_builder.h"

#include <QJsonArray>
#include <QtTest>

using namespace sak;

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;

PartitionInfoEx dataPartition() {
    PartitionInfoEx partition;
    partition.disk_number = DiskNumber{3};
    partition.partition_number = PartitionNumber{2};
    partition.partition_guid = QStringLiteral("{PART}");
    partition.offset_bytes = 256 * kMiB;
    partition.size_bytes = 512 * kMiB;
    PartitionVolumeInfo volume;
    volume.drive_letter = QStringLiteral("t");
    volume.file_system = QStringLiteral("NTFS");
    volume.label = QStringLiteral("DATA");
    volume.volume_guid = QStringLiteral("\\\\?\\Volume{V}\\");
    partition.volume = volume;
    return partition;
}

PartitionDiskInfo dataDisk(const QString& style) {
    PartitionDiskInfo disk;
    disk.disk_number = DiskNumber{3};
    disk.partition_style = style;
    disk.size_bytes = 1024 * kMiB;
    return disk;
}

const BackupRestoreChoices kBackup{.backup_directory = QStringLiteral("C:\\backup"),
                                   .confirmed = true};

}  // namespace

class PartitionOperationRequestsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void targetsCarryWhatTheTableSelects();
    void createCarriesEachChoice();
    void createOmitsALetterNobodyChose();
    void formatCarriesEachChoice();
    void moveCarriesWhereTheStartGoesAndTheBackup();
    void freeSpaceBeforeAPartitionMovesItsStart();
    void freeSpaceAfterAPartitionGrowsIt();
    void allocateNamesTheDonorAndTheAmount();
    void primaryLogicalNamesTheNewLayoutAndTheBackup();
    void serialAndClusterReformatWithTheVolumesOwnFacts();
    void systemMbrToGptAsksForMbr2Gpt();
    void quickPartitionEqualSlicesFillTheDisk();
    void quickPartitionSizesFromOptionsRefuseAMalformedList();
    void quickPartitionSizesMustFitTheDisk();
    void quickPartitionOnARawDiskInitializesIt();
    void quickPartitionReplacesWhatIsThere();
    void quickPartitionGivesEachStyleItsType();
    void imageOfAnOnlineVolumeLocksAndOfAnOfflineDiskReadsTheDisk();
    void restoreImagePinsTheContent();
    void copyToAFreeRegionNamesWhere();
    void migrateCarriesItsPlanAndTheChoice();
    void bootRepairNamesWhichWindows();
    void aLetterIsOneCharacter();
    void secureEraseSaysItWasConfirmed();
    void everyRequestBuildsARunnableScript();
};

void PartitionOperationRequestsTests::targetsCarryWhatTheTableSelects() {
    const auto partition = dataPartition();
    const auto selected = partitionSelectionTarget(partition);
    QCOMPARE(selected.kind, PartitionTargetKind::Partition);
    QCOMPARE(selected.disk_number.value(), 3U);
    QCOMPARE(selected.partition_number.value(), 2U);
    QCOMPARE(selected.offset_bytes, 256 * kMiB);
    QCOMPARE(selected.size_bytes, 512 * kMiB);
    QCOMPARE(selected.drive_letter, QStringLiteral("t"));
    QVERIFY2(selected.partition_guid.isEmpty(), "a row of the table does not carry the GUID");

    const auto identity = partitionIdentityTarget(partition);
    QCOMPARE(identity.partition_guid, QStringLiteral("{PART}"));
    QCOMPARE(identity.volume_guid, QStringLiteral("\\\\?\\Volume{V}\\"));

    const auto disk = diskOperationTarget(dataDisk(QStringLiteral("GPT")));
    QCOMPARE(disk.kind, PartitionTargetKind::Disk);
    QCOMPARE(disk.size_bytes, 1024 * kMiB);

    const auto free_space = unallocatedOperationTarget(DiskNumber{3}, 9, 11);
    QCOMPARE(free_space.kind, PartitionTargetKind::Unallocated);
    QCOMPARE(free_space.offset_bytes, uint64_t{9});
    QCOMPARE(free_space.size_bytes, uint64_t{11});
}

void PartitionOperationRequestsTests::createCarriesEachChoice() {
    const auto payload = createPartitionPayload({.size_bytes = 256 * kMiB,
                                                 .relative_offset_bytes = kMiB,
                                                 .file_system = QStringLiteral("NTFS"),
                                                 .allocation_unit_bytes = 4096,
                                                 .partition_type = QStringLiteral("gpt:{TYPE}"),
                                                 .label = QStringLiteral("DATA"),
                                                 .full_format = true,
                                                 .drive_letter = QStringLiteral("T")});
    QCOMPARE(payload.value("size_bytes").toString(), QString::number(256 * kMiB));
    QCOMPARE(payload.value("relative_offset_bytes").toString(), QString::number(kMiB));
    QCOMPARE(payload.value("file_system").toString(), QStringLiteral("NTFS"));
    QCOMPARE(payload.value("allocation_unit_bytes").toString(), QStringLiteral("4096"));
    QCOMPARE(payload.value("gpt_type").toString(), QStringLiteral("{TYPE}"));
    QVERIFY(!payload.contains("mbr_type"));
    QCOMPARE(payload.value("label").toString(), QStringLiteral("DATA"));
    QVERIFY(payload.value("full_format").toBool());
    QCOMPARE(payload.value("drive_letter").toString(), QStringLiteral("T"));

    const auto mbr = createPartitionPayload({.partition_type = QStringLiteral("mbr:IFS")});
    QCOMPARE(mbr.value("mbr_type").toString(), QStringLiteral("IFS"));
    QVERIFY(!mbr.contains("gpt_type"));
}

void PartitionOperationRequestsTests::createOmitsALetterNobodyChose() {
    QVERIFY(!createPartitionPayload({.size_bytes = kMiB}).contains("drive_letter"));
}

void PartitionOperationRequestsTests::formatCarriesEachChoice() {
    const auto payload = formatPartitionPayload({.file_system = QStringLiteral("exFAT"),
                                                 .allocation_unit_bytes = 8192,
                                                 .label = QStringLiteral("L"),
                                                 .full_format = true});
    QCOMPARE(payload.value("file_system").toString(), QStringLiteral("exFAT"));
    QCOMPARE(payload.value("allocation_unit_bytes").toString(), QStringLiteral("8192"));
    QCOMPARE(payload.value("label").toString(), QStringLiteral("L"));
    QVERIFY(payload.value("full_format").toBool());
}

void PartitionOperationRequestsTests::moveCarriesWhereTheStartGoesAndTheBackup() {
    QJsonObject payload = resizePartitionPayload(512 * kMiB, 7);
    addMovePartitionFields(&payload, dataPartition(), 64 * kMiB, kBackup);
    QCOMPARE(payload.value("target_size_bytes").toString(), QString::number(512 * kMiB));
    QCOMPARE(payload.value("adjacent_free_bytes").toString(), QStringLiteral("7"));
    QCOMPARE(payload.value("target_offset_bytes").toString(), QString::number(64 * kMiB));
    QCOMPARE(payload.value("drive_letter").toString(), QStringLiteral("T"));
    QCOMPARE(payload.value("file_system").toString(), QStringLiteral("NTFS"));
    QCOMPARE(payload.value("label").toString(), QStringLiteral("DATA"));
    QCOMPARE(payload.value("backup_directory").toString(), QStringLiteral("C:\\backup"));
    QVERIFY(payload.value("target_wipe_confirmed").toBool());
}

void PartitionOperationRequestsTests::freeSpaceBeforeAPartitionMovesItsStart() {
    const auto payload = freeSpaceMovePayload(dataPartition(), 64 * kMiB, kBackup);
    QCOMPARE(payload.value("target_offset_bytes").toString(), QString::number(192 * kMiB));
    QCOMPARE(payload.value("target_size_bytes").toString(), QString::number(576 * kMiB));
}

void PartitionOperationRequestsTests::freeSpaceAfterAPartitionGrowsIt() {
    const auto payload = freeSpaceResizePayload(dataPartition(), 128 * kMiB, 64 * kMiB);
    QCOMPARE(payload.value("target_size_bytes").toString(), QString::number(576 * kMiB));
    QCOMPARE(payload.value("adjacent_free_bytes").toString(), QString::number(128 * kMiB));
    QVERIFY(!payload.contains("target_offset_bytes"));
}

void PartitionOperationRequestsTests::allocateNamesTheDonorAndTheAmount() {
    const auto payload = allocateFreeSpacePayload(dataPartition(), 64 * kMiB, kBackup);
    QCOMPARE(payload.value("source_partition_number").toString(), QStringLiteral("2"));
    QCOMPARE(payload.value("source_size_bytes").toString(), QString::number(512 * kMiB));
    QCOMPARE(payload.value("bytes_to_allocate").toString(), QString::number(64 * kMiB));
    QCOMPARE(payload.value("source_drive_letter").toString(), QStringLiteral("T"));
    QCOMPARE(payload.value("source_file_system").toString(), QStringLiteral("NTFS"));
    QCOMPARE(payload.value("source_label").toString(), QStringLiteral("DATA"));
    QCOMPARE(payload.value("backup_directory").toString(), QStringLiteral("C:\\backup"));
    QVERIFY(payload.value("target_wipe_confirmed").toBool());
}

void PartitionOperationRequestsTests::primaryLogicalNamesTheNewLayoutAndTheBackup() {
    const auto payload = primaryLogicalPayload(dataPartition(), QStringLiteral("logical"), kBackup);
    QCOMPARE(payload.value("target_layout").toString(), QStringLiteral("logical"));
    QCOMPARE(payload.value("source_size_bytes").toString(), QString::number(512 * kMiB));
    QCOMPARE(payload.value("drive_letter").toString(), QStringLiteral("T"));
    QCOMPARE(payload.value("backup_directory").toString(), QStringLiteral("C:\\backup"));
}

void PartitionOperationRequestsTests::serialAndClusterReformatWithTheVolumesOwnFacts() {
    const PartitionVolumeInfo volume = *dataPartition().volume;
    const auto serial = volumeSerialPayload(volume, kBackup);
    QCOMPARE(serial.value("drive_letter").toString(), QStringLiteral("T"));
    QCOMPARE(serial.value("file_system").toString(), QStringLiteral("NTFS"));
    QCOMPARE(serial.value("label").toString(), QStringLiteral("DATA"));
    QVERIFY(serial.value("target_wipe_confirmed").toBool());

    const auto cluster = clusterSizePayload(volume,
                                            {.file_system = QStringLiteral("NTFS"),
                                             .allocation_unit_bytes = 16'384,
                                             .label = QStringLiteral("NEW")},
                                            kBackup);
    QCOMPARE(cluster.value("allocation_unit_bytes").toString(), QStringLiteral("16384"));
    QCOMPARE(cluster.value("label").toString(), QStringLiteral("NEW"));
    QCOMPARE(cluster.value("drive_letter").toString(), QStringLiteral("T"));
}

void PartitionOperationRequestsTests::systemMbrToGptAsksForMbr2Gpt() {
    auto disk = dataDisk(QStringLiteral("MBR"));
    QVERIFY(!convertStylePayload(disk, QStringLiteral("GPT")).contains("mode"));
    disk.is_system = true;
    QCOMPARE(convertStylePayload(disk, QStringLiteral("GPT")).value("mode").toString(),
             QStringLiteral("mbr2gpt"));
    QVERIFY(!convertStylePayload(disk, QStringLiteral("MBR")).contains("mode"));
    disk.partition_style = QStringLiteral("GPT");
    QVERIFY(!convertStylePayload(disk, QStringLiteral("GPT")).contains("mode"));
}

void PartitionOperationRequestsTests::quickPartitionEqualSlicesFillTheDisk() {
    const auto disk = dataDisk(QStringLiteral("GPT"));
    const uint64_t usable = quickPartitionUsableBytes(disk);
    QCOMPARE(usable, (1024 - kQuickPartitionReservedTailMb - 1) * kMiB);
    const auto sizes = quickPartitionEqualSizes(usable, 4);
    QCOMPARE(sizes.size(), 4);
    QCOMPARE(sizes.at(0), sizes.at(1));
    QCOMPARE(quickPartitionTotalBytes(sizes), usable);
    QVERIFY(quickPartitionSizesAreValid(sizes, usable));
    PartitionDiskInfo tiny = disk;
    tiny.size_bytes = kMiB;
    QCOMPARE(quickPartitionUsableBytes(tiny), uint64_t{0});
}

void PartitionOperationRequestsTests::quickPartitionSizesFromOptionsRefuseAMalformedList() {
    QJsonObject options{{"partition_count", 2}, {"size_mode", "custom"}};
    QVERIFY(quickPartitionSizesFromOptions(options, 1024 * kMiB).isEmpty());
    options["custom_size_bytes"] = QJsonArray{QString::number(64 * kMiB), QStringLiteral("nope")};
    QVERIFY(quickPartitionSizesFromOptions(options, 1024 * kMiB).isEmpty());
    options["custom_size_bytes"] = QJsonArray{QString::number(64 * kMiB),
                                              QString::number(128 * kMiB)};
    QCOMPARE(quickPartitionSizesFromOptions(options, 1024 * kMiB),
             (QVector<uint64_t>{64 * kMiB, 128 * kMiB}));
    options["size_mode"] = QStringLiteral("equal");
    QCOMPARE(quickPartitionSizesFromOptions(options, 1024 * kMiB).size(), 2);
}

void PartitionOperationRequestsTests::quickPartitionSizesMustFitTheDisk() {
    QVERIFY(!quickPartitionSizesAreValid({}, 1024 * kMiB));
    QVERIFY(!quickPartitionSizesAreValid({2048 * kMiB}, 1024 * kMiB));
    QVERIFY(!quickPartitionSizesAreValid({kMiB}, 1024 * kMiB));
    QVERIFY(quickPartitionSizesAreValid({kQuickPartitionMinimumSizeMb * kMiB}, 1024 * kMiB));
    // A sum that would wrap must not pass for a small one.
    QVERIFY(!quickPartitionSizesAreValid({UINT64_MAX, 128 * kMiB}, 1024 * kMiB));
}

void PartitionOperationRequestsTests::quickPartitionOnARawDiskInitializesIt() {
    const auto disk = dataDisk(QStringLiteral("RAW"));
    QVERIFY(diskNeedsInitializeForQuickPartition(disk));
    const QJsonObject options{{"partition_scheme", "GPT"}, {"file_system", "NTFS"}};
    const auto requests = quickPartitionRequests(disk, options, {128 * kMiB});
    QCOMPARE(requests.size(), 2);
    QCOMPARE(requests.at(0).type, PartitionOperationType::InitializeDisk);
    QCOMPARE(requests.at(0).payload.value("target_style").toString(), QStringLiteral("GPT"));
    QCOMPARE(requests.at(1).type, PartitionOperationType::Create);
}

void PartitionOperationRequestsTests::quickPartitionReplacesWhatIsThere() {
    auto disk = dataDisk(QStringLiteral("GPT"));
    disk.partitions.append(dataPartition());
    const QJsonObject options{{"partition_scheme", "GPT"}, {"label_prefix", "Q"}};
    const auto requests = quickPartitionRequests(disk, options, {128 * kMiB, 256 * kMiB});
    QCOMPARE(requests.size(), 3);
    QCOMPARE(requests.at(0).type, PartitionOperationType::DeleteAllPartitions);
    QCOMPARE(requests.at(1).target.offset_bytes, kMiB);
    QCOMPARE(requests.at(1).target.size_bytes, 128 * kMiB);
    QCOMPARE(requests.at(2).target.offset_bytes, 129 * kMiB);
    QCOMPARE(requests.at(1).payload.value("label").toString(), QStringLiteral("Q1"));
    QCOMPARE(requests.at(2).payload.value("label").toString(), QStringLiteral("Q2"));
    QVERIFY(quickPartitionRequests(disk, options, {UINT64_MAX, UINT64_MAX}).isEmpty());
}

void PartitionOperationRequestsTests::quickPartitionGivesEachStyleItsType() {
    const auto gpt = quickPartitionRequests(dataDisk(QStringLiteral("GPT")),
                                            QJsonObject{{"partition_scheme", "GPT"}},
                                            {128 * kMiB});
    QCOMPARE(gpt.constFirst().payload.value("gpt_type").toString(),
             QStringLiteral("{EBD0A0A2-B9E5-4433-87C0-68B6B72699C7}"));
    const auto mbr = quickPartitionRequests(dataDisk(QStringLiteral("MBR")),
                                            QJsonObject{{"partition_scheme", "MBR"}},
                                            {128 * kMiB});
    QCOMPARE(mbr.constFirst().payload.value("mbr_type").toString(), QStringLiteral("IFS"));
    const auto fat =
        quickPartitionRequests(dataDisk(QStringLiteral("MBR")),
                               QJsonObject{{"partition_scheme", "MBR"}, {"file_system", "fat32"}},
                               {128 * kMiB});
    QCOMPARE(fat.constFirst().payload.value("mbr_type").toString(), QStringLiteral("FAT32"));
    const QJsonObject custom{{"partition_scheme", "GPT"},
                             {"size_mode", "custom"},
                             {"label_prefix", "Q"},
                             {"custom_labels", QJsonArray{"ONE", " "}}};
    const auto labelled =
        quickPartitionRequests(dataDisk(QStringLiteral("GPT")), custom, {128 * kMiB, 128 * kMiB});
    QCOMPARE(labelled.at(0).payload.value("label").toString(), QStringLiteral("ONE"));
    QCOMPARE(labelled.at(1).payload.value("label").toString(), QStringLiteral("Q2"));
}

void PartitionOperationRequestsTests::imageOfAnOnlineVolumeLocksAndOfAnOfflineDiskReadsTheDisk() {
    auto disk = dataDisk(QStringLiteral("GPT"));
    disk.partitions.append(dataPartition());
    const auto partition = partitionSelectionTarget(disk.partitions.constFirst());
    QCOMPARE(createImageSourcePath(partition, disk, false), QStringLiteral("\\\\.\\T:"));
    QCOMPARE(createImageSourcePath(partition, disk, true), QStringLiteral("\\\\.\\PhysicalDrive3"));
    QCOMPARE(createImageSourcePath(diskOperationTarget(disk), disk, false),
             QStringLiteral("\\\\.\\PhysicalDrive3"));

    const auto online = createImagePayload(
        partition, false, QStringLiteral("\\\\.\\T:"), QStringLiteral("C:\\i.img"), false);
    QCOMPARE(online.value("source_capture_mode").toString(), QStringLiteral("LockedVolumes"));
    QCOMPARE(online.value("source_offset_bytes").toString(), QStringLiteral("0"));
    QVERIFY(!online.value("overwrite_confirmed").toBool());
    QCOMPARE(online.value("verify_mode").toString(), QStringLiteral("Full"));

    const auto offline = createImagePayload(partition,
                                            true,
                                            QStringLiteral("\\\\.\\PhysicalDrive3"),
                                            QStringLiteral("C:\\i.img"),
                                            true);
    QCOMPARE(offline.value("source_capture_mode").toString(), QStringLiteral("OfflineDisk"));
    QCOMPARE(offline.value("source_offset_bytes").toString(), QString::number(256 * kMiB));
    QVERIFY(offline.value("overwrite_confirmed").toBool());
}

void PartitionOperationRequestsTests::restoreImagePinsTheContent() {
    const auto target = diskOperationTarget(dataDisk(QStringLiteral("GPT")));
    const auto payload =
        restoreImagePayload(target, QStringLiteral("C:\\i.img"), 9, QStringLiteral("abc"));
    QCOMPARE(payload.value("source_path").toString(), QStringLiteral("C:\\i.img"));
    QCOMPARE(payload.value("target_path").toString(), QStringLiteral("\\\\.\\PhysicalDrive3"));
    QCOMPARE(payload.value("source_size_bytes").toString(), QStringLiteral("9"));
    QCOMPARE(payload.value("target_size_bytes").toString(), QString::number(1024 * kMiB));
    QCOMPARE(payload.value("source_sha256").toString(), QStringLiteral("abc"));
    QVERIFY(payload.value("target_wipe_confirmed").toBool());
}

void PartitionOperationRequestsTests::copyToAFreeRegionNamesWhere() {
    const CopyChoices choices{.source_path = QStringLiteral("\\\\.\\T:"),
                              .target_path = QStringLiteral("\\\\.\\PhysicalDrive4"),
                              .verify_mode = QStringLiteral("Full verification"),
                              .source_size_bytes = 5,
                              .target_size_bytes = 6,
                              .target_wipe_confirmed = true};
    const auto to_path = partitionCopyPayload(choices, std::nullopt);
    QVERIFY(!to_path.contains("target_offset_bytes"));
    QCOMPARE(to_path.value("verify_mode").toString(), QStringLiteral("Full verification"));
    const auto to_region = partitionCopyPayload(choices,
                                                CopyRegion{.disk_number = 4, .offset_bytes = 99});
    QCOMPARE(to_region.value("target_disk_number").toInt(), 4);
    QCOMPARE(to_region.value("target_offset_bytes").toString(), QStringLiteral("99"));
}

void PartitionOperationRequestsTests::migrateCarriesItsPlanAndTheChoice() {
    auto source = dataDisk(QStringLiteral("GPT"));
    source.is_boot = true;
    const CopyChoices choices{.source_path = rawDiskPath(3),
                              .target_path = rawDiskPath(4),
                              .target_size_bytes = 2048 * kMiB,
                              .target_wipe_confirmed = true};
    const auto payload = migrateOsPayload(source, choices, true);
    QVERIFY(payload.value("allow_no_snapshot").toBool());
    QVERIFY(payload.value("migration_plan").isObject());
    QCOMPARE(payload.value("target_path").toString(), QStringLiteral("\\\\.\\PhysicalDrive4"));
}

void PartitionOperationRequestsTests::bootRepairNamesWhichWindows() {
    const auto attached = repairAttachedBootPayload(QStringLiteral("BIOS"));
    QCOMPARE(attached.value("repair_scope").toString(), QStringLiteral("attached"));
    QCOMPARE(attached.value("boot_mode").toString(), QStringLiteral("BIOS"));
    const auto running = repairRunningBootPayload(QStringLiteral("C:\\Windows"),
                                                  QStringLiteral("UEFI"));
    QCOMPARE(running.value("windows_path").toString(), QStringLiteral("C:\\Windows"));
    QCOMPARE(running.value("esp_letter").toString(), QStringLiteral("S"));
    QVERIFY(!running.contains("repair_scope"));
}

void PartitionOperationRequestsTests::aLetterIsOneCharacter() {
    QCOMPARE(driveLetterPayload(QStringLiteral("K:")).value("drive_letter").toString(),
             QStringLiteral("K"));
    const auto unlock = bitLockerUnlockPayload(QStringLiteral("K:"), QStringLiteral(" 123 "));
    QCOMPARE(unlock.value("drive_letter").toString(), QStringLiteral("K"));
    QCOMPARE(unlock.value("recovery_password").toString(), QStringLiteral("123"));
}

void PartitionOperationRequestsTests::secureEraseSaysItWasConfirmed() {
    const auto payload = ssdSecureErasePayload();
    QVERIFY(payload.value("ssd_secure_erase").toBool());
    QVERIFY(payload.value("target_wipe_confirmed").toBool());
}

void PartitionOperationRequestsTests::everyRequestBuildsARunnableScript() {
    // Without a full inventory these only reach the script builder, which is where a field the
    // builder wants and the payload lacks would turn into a refusal.
    const PartitionScriptBuilder builder;
    const auto partition = dataPartition();
    const auto selected = partitionSelectionTarget(partition);
    const QVector<PartitionOperationRequest> requests{
        {PartitionOperationType::Format,
         selected,
         formatPartitionPayload({.file_system = QStringLiteral("NTFS"),
                                 .allocation_unit_bytes = 4096,
                                 .label = QStringLiteral("L")})},
        {PartitionOperationType::Resize, selected, resizePartitionPayload(600 * kMiB, 200 * kMiB)},
        {PartitionOperationType::ChangeVolumeSerialNumber,
         selected,
         volumeSerialPayload(*partition.volume, kBackup)},
        {PartitionOperationType::ConvertPrimaryLogical,
         selected,
         primaryLogicalPayload(partition, QStringLiteral("logical"), kBackup)},
    };
    for (const auto& request : requests) {
        const auto operation =
            PartitionOperationPlanner::makeOperation(request.type, request.target, request.payload);
        const auto script = builder.buildScript(operation);
        QVERIFY2(script.valid(), qPrintable(script.blockers.join(QStringLiteral("; "))));
    }
}

QTEST_APPLESS_MAIN(PartitionOperationRequestsTests)
#include "test_partition_operation_requests.moc"
