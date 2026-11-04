// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_partition_os_migration.cpp
/// @brief What Migrate OS plans, what refuses it, and what its script says, with no disk involved.
///
/// The script itself runs only in the VM. Here its text is checked, and the fixture parses it,
/// compiles its C#, and runs its copy code and its pure functions on ordinary files.

#include "sak/partition_executor.h"
#include "sak/partition_operation_planner.h"
#include "sak/partition_os_migration.h"
#include "sak/partition_script_builder.h"
#include "sak/process_runner.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

using namespace sak;

namespace {

constexpr uint64_t kMiB = 1024ULL * 1024ULL;
constexpr uint64_t kGiB = 1024ULL * kMiB;
constexpr uint64_t kSourceBytes = 256ULL * kGiB;
constexpr uint64_t kWindowsBytes = 254ULL * kGiB;
constexpr uint64_t kWindowsOffset = 217ULL * kMiB;
constexpr uint64_t kUsedBytes = 34ULL * kGiB;
constexpr int kRuntimeTimeoutMs = 120'000;

PartitionInfoEx makePartition(uint32_t number, uint64_t offset, uint64_t size) {
    PartitionInfoEx partition;
    partition.disk_number = DiskNumber{0};
    partition.partition_number = PartitionNumber{number};
    partition.offset_bytes = offset;
    partition.size_bytes = size;
    partition.type_name = QStringLiteral("Basic");
    return partition;
}

PartitionVolumeInfo ntfsVolume(const QString& letter, uint64_t total, uint64_t free) {
    PartitionVolumeInfo volume;
    volume.drive_letter = letter;
    volume.file_system = QStringLiteral("NTFS");
    volume.total_bytes = total;
    volume.free_bytes = free;
    volume.volume_guid = QStringLiteral("\\\\?\\Volume{%1}\\").arg(letter);
    return volume;
}

/// A disk as Windows setup leaves it: EFI, reserved, Windows, recovery.
PartitionDiskInfo gptWindowsDisk() {
    PartitionDiskInfo disk;
    disk.disk_number = DiskNumber{0};
    disk.partition_style = QStringLiteral("GPT");
    disk.size_bytes = kSourceBytes;
    disk.is_boot = true;
    disk.is_system = true;
    disk.unique_id = QStringLiteral("SOURCE-ID");
    auto esp = makePartition(1, kMiB, 200 * kMiB);
    esp.is_efi = true;
    esp.is_system = true;
    auto msr = makePartition(2, 201 * kMiB, 16 * kMiB);
    msr.is_msr = true;
    auto windows = makePartition(3, kWindowsOffset, kWindowsBytes);
    windows.is_boot = true;
    windows.volume = ntfsVolume(QStringLiteral("C"), kWindowsBytes, kWindowsBytes - kUsedBytes);
    auto recovery = makePartition(4, kWindowsOffset + kWindowsBytes, kGiB);
    recovery.is_recovery = true;
    disk.partitions = {esp, msr, windows, recovery};
    return disk;
}

/// A BIOS install: System Reserved, active, then Windows.
PartitionDiskInfo mbrWindowsDisk() {
    PartitionDiskInfo disk;
    disk.disk_number = DiskNumber{0};
    disk.partition_style = QStringLiteral("MBR");
    disk.size_bytes = kSourceBytes;
    disk.is_boot = true;
    disk.is_system = true;
    disk.unique_id = QStringLiteral("SOURCE-ID");
    auto reserved = makePartition(1, kMiB, 500 * kMiB);
    reserved.is_active = true;
    reserved.is_system = true;
    auto windows = makePartition(2, 501 * kMiB, kWindowsBytes);
    windows.is_boot = true;
    windows.volume = ntfsVolume(QStringLiteral("C"), kWindowsBytes, kWindowsBytes - kUsedBytes);
    disk.partitions = {reserved, windows};
    return disk;
}

const OsMigrationPartition& entryOf(const OsMigrationPlan& plan, OsMigrationPartitionKind kind) {
    const auto found = std::ranges::find(plan.partitions, kind, &OsMigrationPartition::kind);
    Q_ASSERT(found != plan.partitions.end());
    return *found;
}

bool noOverlapInOrder(const OsMigrationPlan& plan) {
    uint64_t end = 0;
    for (const auto& entry : plan.partitions) {
        if (entry.target_offset < end) {
            return false;
        }
        end = entry.target_offset + entry.final_size;
    }
    return end <= plan.target_bytes;
}

PartitionInventory inventoryWith(const PartitionDiskInfo& source, uint64_t target_bytes) {
    PartitionDiskInfo target;
    target.disk_number = DiskNumber{1};
    target.partition_style = QStringLiteral("GPT");
    target.size_bytes = target_bytes;
    target.unique_id = QStringLiteral("TARGET-ID");
    target.serial_number = QStringLiteral("TARGET-SERIAL");
    PartitionInventory inventory;
    inventory.disks = {source, target};
    return inventory;
}

PartitionTarget sourceTarget(const PartitionDiskInfo& source) {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = source.disk_number;
    target.disk_unique_id = source.unique_id;
    target.disk_serial_number = source.serial_number;
    target.disk_size_bytes = source.size_bytes;
    return target;
}

QJsonObject migrationPayload(const PartitionDiskInfo& source, uint64_t target_bytes) {
    QJsonObject payload;
    payload[QStringLiteral("source_path")] = QStringLiteral("\\\\.\\PhysicalDrive0");
    payload[QStringLiteral("target_path")] = QStringLiteral("\\\\.\\PhysicalDrive1");
    payload[QStringLiteral("verify_mode")] = QStringLiteral("Sample verification");
    payload[QStringLiteral("source_size_bytes")] = QString::number(source.size_bytes);
    payload[QStringLiteral("target_size_bytes")] = QString::number(target_bytes);
    payload[QStringLiteral("target_wipe_confirmed")] = true;
    payload[QStringLiteral("target_disk_unique_id")] = QStringLiteral("TARGET-ID");
    payload[QStringLiteral("target_disk_serial_number")] = QStringLiteral("TARGET-SERIAL");
    payload[QStringLiteral("target_disk_size_bytes")] = QString::number(target_bytes);
    payload[QStringLiteral("migration_plan")] =
        osMigrationPlanToJson(planOsMigration(source, target_bytes));
    return payload;
}

PartitionScript migrationScript(const PartitionDiskInfo& source, uint64_t target_bytes) {
    return PartitionScriptBuilder().buildScript(
        PartitionOperationPlanner::makeOperation(PartitionOperationType::MigrateOs,
                                                 sourceTarget(source),
                                                 migrationPayload(source, target_bytes)));
}

QStringList previewBlockers(const PartitionInventory& inventory, const QJsonObject& payload) {
    PartitionOperationPlanner planner;
    return planner
        .previewOperation(
            inventory,
            PartitionOperationPlanner::makeOperation(
                PartitionOperationType::MigrateOs, sourceTarget(inventory.disks.first()), payload))
        .blockers;
}

}  // namespace

class PartitionOsMigrationTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void largerTargetGrowsWindowsAndKeepsRecoveryLast();
    void sameSizeTargetCopiesBlocksEvenWithWindowsLast();
    void smallerTargetGoesThroughAnImageOnTheTarget();
    void targetTooSmallForTheUsedDataIsRefused();
    void targetHoldingTheDataButNotTheImageIsRefusedWithTheNumber();
    void bitLockerOnTheSourceIsRefusedWithTheReason();
    void sourceItCannotRebuildIsRefused();
    void windowsStartingFromAnotherDiskIsRefused();
    void mbrKeepsItsActivePartitionAndItsLimit();
    void fourPartitionMbrHasNoRoomForTheImagePartition();
    void planSurvivesThePayloadAndChangesAreNoticed();
    void validatorRefusesATooSmallTargetBeforeAnythingIsWritten();
    void validatorRefusesAQueuedPlanTheDiskNoLongerMatches();
    void validatorWarnsOnlyWhenACopyWithoutASnapshotIsAllowed();
    void scriptRefusesEverythingBeforeTheFirstWrite();
    void scriptReleasesSnapshotsAndNeverTouchesFirmware();
    void scriptWithoutConsentStopsOnNoSnapshot();
    void scriptNeedsPinnedIdentityAndConfirmation();
    void attachedRepairFindsWindowsAndRefusesTheRunningDisk();
    void attachedRepairNeedsTheDisksIdentity();
    void repairScopeMustMatchTheDisk();
    void scriptParsesCompilesAndCopiesRealFiles();
    void scriptIsTooLongForACommandLineAndStillStarts();
    void aRefusalReachesThePersonInItsOwnWords();
};

void PartitionOsMigrationTests::largerTargetGrowsWindowsAndKeepsRecoveryLast() {
    const uint64_t target = 512ULL * kGiB;
    const auto plan = planOsMigration(gptWindowsDisk(), target);
    QVERIFY2(plan.canRun(), qPrintable(plan.blockers.join(u';')));
    QCOMPARE(plan.route, OsMigrationRoute::Blocks);
    QVERIFY(noOverlapInOrder(plan));
    const auto& windows = entryOf(plan, OsMigrationPartitionKind::Windows);
    // Copied at its own size, and the file system is then extended into the room.
    QCOMPARE(windows.initial_size, kWindowsBytes);
    QVERIFY(windows.final_size > kWindowsBytes);
    const auto& recovery = entryOf(plan, OsMigrationPartitionKind::Recovery);
    QCOMPARE(recovery.final_size, kGiB);
    QVERIFY(recovery.target_offset + recovery.final_size <= target);
    QVERIFY(target - (recovery.target_offset + recovery.final_size) < 2 * kMiB);
    QCOMPARE(windows.target_offset + windows.final_size, recovery.target_offset);
    QVERIFY(windows.from_snapshot);
    QVERIFY(!recovery.from_snapshot);
    QVERIFY(!entryOf(plan, OsMigrationPartitionKind::System).from_snapshot);
}

// A partition that runs almost to the end of its disk still fits a disk of the same size.
void PartitionOsMigrationTests::sameSizeTargetCopiesBlocksEvenWithWindowsLast() {
    auto source = gptWindowsDisk();
    source.partitions.removeLast();
    source.partitions.last().size_bytes = kSourceBytes - kWindowsOffset - (64 * 1024);
    const auto plan = planOsMigration(source, kSourceBytes);
    QVERIFY2(plan.canRun(), qPrintable(plan.blockers.join(u';')));
    QCOMPARE(plan.route, OsMigrationRoute::Blocks);
    QCOMPARE(entryOf(plan, OsMigrationPartitionKind::Windows).final_size,
             source.partitions.last().size_bytes);
}

void PartitionOsMigrationTests::smallerTargetGoesThroughAnImageOnTheTarget() {
    const uint64_t target = 80ULL * kGiB;
    const auto plan = planOsMigration(gptWindowsDisk(), target);
    QVERIFY2(plan.canRun(), qPrintable(plan.blockers.join(u';')));
    QCOMPARE(plan.route, OsMigrationRoute::Image);
    QVERIFY(noOverlapInOrder(plan));
    const auto& windows = entryOf(plan, OsMigrationPartitionKind::Windows);
    QVERIFY(windows.initial_size >= kUsedBytes + kOsMigrationMinimumHeadroomBytes);
    QVERIFY(windows.initial_size < windows.final_size);
    // The temporary partition sits right after Windows, so removing it leaves room to grow into.
    QVERIFY(plan.scratch_offset >= windows.target_offset + windows.initial_size);
    QCOMPARE(plan.scratch_offset + plan.scratch_size, windows.target_offset + windows.final_size);
    QVERIFY(plan.scratch_size >= kUsedBytes / kOsMigrationImageDivisor * kOsMigrationImageShare);
    QCOMPARE(entryOf(plan, OsMigrationPartitionKind::Recovery).target_offset,
             windows.target_offset + windows.final_size);
}

void PartitionOsMigrationTests::targetTooSmallForTheUsedDataIsRefused() {
    const auto plan = planOsMigration(gptWindowsDisk(), 30ULL * kGiB);
    QVERIFY(!plan.canRun());
    QVERIFY2(plan.blockers.join(u' ').contains(QStringLiteral("too small for the data")),
             qPrintable(plan.blockers.join(u';')));
    QVERIFY(plan.minimum_target_bytes > 30ULL * kGiB);
}

void PartitionOsMigrationTests::targetHoldingTheDataButNotTheImageIsRefusedWithTheNumber() {
    const auto plan = planOsMigration(gptWindowsDisk(), 45ULL * kGiB);
    QVERIFY(!plan.canRun());
    const QString text = plan.blockers.join(u' ');
    QVERIFY2(text.contains(QStringLiteral("temporary image")), qPrintable(text));
    QVERIFY2(text.contains(formatPartitionBytes(plan.minimum_target_bytes)), qPrintable(text));
    const auto enough = planOsMigration(gptWindowsDisk(), plan.minimum_target_bytes);
    QVERIFY2(enough.canRun(), qPrintable(enough.blockers.join(u';')));
}

void PartitionOsMigrationTests::bitLockerOnTheSourceIsRefusedWithTheReason() {
    for (const bool locked : {false, true}) {
        auto source = gptWindowsDisk();
        source.partitions[2].volume->bitlocker_enabled = !locked;
        source.partitions[2].volume->bitlocker_locked = locked;
        const auto plan = planOsMigration(source, 512ULL * kGiB);
        QVERIFY(!plan.canRun());
        const QString text = plan.blockers.join(u' ');
        QVERIFY2(text.contains(QStringLiteral("C: is encrypted with BitLocker")), qPrintable(text));
        QVERIFY2(text.contains(QStringLiteral("Turn BitLocker off")), qPrintable(text));
    }
}

void PartitionOsMigrationTests::sourceItCannotRebuildIsRefused() {
    auto dynamic = gptWindowsDisk();
    dynamic.is_dynamic = true;
    QVERIFY(planOsMigration(dynamic, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("dynamic disk")));
    auto spaces = gptWindowsDisk();
    spaces.is_storage_spaces = true;
    QVERIFY(planOsMigration(spaces, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("Storage Spaces")));
    auto logical = mbrWindowsDisk();
    logical.partitions.last().type_name = QStringLiteral("Logical");
    QVERIFY(planOsMigration(logical, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("logical partitions")));
    auto failed = gptWindowsDisk();
    failed.partition_enumeration_failed = true;
    QVERIFY(planOsMigration(failed, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("could not read")));
    QVERIFY(planOsMigration(gptWindowsDisk(), 0)
                .blockers.join(u' ')
                .contains(QStringLiteral("known size")));
}

void PartitionOsMigrationTests::windowsStartingFromAnotherDiskIsRefused() {
    auto no_esp = gptWindowsDisk();
    no_esp.partitions[0].is_efi = false;
    QVERIFY(planOsMigration(no_esp, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("no boot partition of its own")));
    auto no_windows = gptWindowsDisk();
    no_windows.partitions[2].is_boot = false;
    QVERIFY(planOsMigration(no_windows, 512ULL * kGiB)
                .blockers.join(u' ')
                .contains(QStringLiteral("not running from this disk")));
}

void PartitionOsMigrationTests::mbrKeepsItsActivePartitionAndItsLimit() {
    const auto plan = planOsMigration(mbrWindowsDisk(), 512ULL * kGiB);
    QVERIFY2(plan.canRun(), qPrintable(plan.blockers.join(u';')));
    QCOMPARE(plan.partition_scheme, QStringLiteral("MBR"));
    QCOMPARE(entryOf(plan, OsMigrationPartitionKind::System).final_size, 500 * kMiB);
    const auto huge = planOsMigration(mbrWindowsDisk(), 4096ULL * kGiB);
    QVERIFY(huge.canRun());
    const auto& windows = entryOf(huge, OsMigrationPartitionKind::Windows);
    QCOMPARE(windows.target_offset + windows.final_size, kOsMigrationMbrLimitBytes);
}

void PartitionOsMigrationTests::fourPartitionMbrHasNoRoomForTheImagePartition() {
    auto source = mbrWindowsDisk();
    const uint64_t end = source.partitions.last().offset_bytes + kWindowsBytes;
    source.size_bytes += 4 * kGiB;
    source.partitions.append(makePartition(3, end, kGiB));
    source.partitions.append(makePartition(4, end + kGiB, kGiB));
    const auto plan = planOsMigration(source, 100ULL * kGiB);
    QVERIFY(!plan.canRun());
    QVERIFY2(plan.blockers.join(u' ').contains(QStringLiteral("already uses all four")),
             qPrintable(plan.blockers.join(u';')));
}

void PartitionOsMigrationTests::planSurvivesThePayloadAndChangesAreNoticed() {
    const auto plan = planOsMigration(gptWindowsDisk(), 80ULL * kGiB);
    const auto back = osMigrationPlanFromJson(osMigrationPlanToJson(plan));
    QVERIFY(back.has_value());
    QVERIFY(osMigrationPlansAgree(plan, *back));
    QCOMPARE(back->scratch_offset, plan.scratch_offset);
    QCOMPARE(back->scratch_size, plan.scratch_size);
    QCOMPARE(entryOf(*back, OsMigrationPartitionKind::Windows).initial_size,
             entryOf(plan, OsMigrationPartitionKind::Windows).initial_size);
    // How much is in use moves all the time, and is not a change in the plan.
    auto busier = gptWindowsDisk();
    busier.partitions[2].volume->free_bytes -= kGiB;
    QVERIFY(osMigrationPlansAgree(plan, planOsMigration(busier, 80ULL * kGiB)));
    // A different target, or a partition that moved, is.
    QVERIFY(!osMigrationPlansAgree(plan, planOsMigration(gptWindowsDisk(), 90ULL * kGiB)));
    auto moved = gptWindowsDisk();
    moved.partitions[2].offset_bytes += kMiB;
    QVERIFY(!osMigrationPlansAgree(plan, planOsMigration(moved, 80ULL * kGiB)));
    auto shifted = plan;
    shifted.partitions.last().target_offset += kMiB;
    QVERIFY(!osMigrationPlansAgree(plan, shifted));
    QVERIFY(!osMigrationPlanFromJson(QJsonObject{}).has_value());
}

void PartitionOsMigrationTests::validatorRefusesATooSmallTargetBeforeAnythingIsWritten() {
    const auto source = gptWindowsDisk();
    const QStringList blockers = previewBlockers(inventoryWith(source, 30ULL * kGiB),
                                                 migrationPayload(source, 30ULL * kGiB));
    QVERIFY2(blockers.join(u' ').contains(QStringLiteral("too small for the data")),
             qPrintable(blockers.join(u';')));
    // A target smaller than the old disk is no longer refused for that alone.
    const QStringList smaller = previewBlockers(inventoryWith(source, 100ULL * kGiB),
                                                migrationPayload(source, 100ULL * kGiB));
    QVERIFY2(smaller.isEmpty(), qPrintable(smaller.join(u';')));
}

void PartitionOsMigrationTests::validatorRefusesAQueuedPlanTheDiskNoLongerMatches() {
    const auto source = gptWindowsDisk();
    auto moved = source;
    moved.partitions[2].offset_bytes += kMiB;
    const QStringList blockers = previewBlockers(inventoryWith(moved, 512ULL * kGiB),
                                                 migrationPayload(source, 512ULL * kGiB));
    QVERIFY2(blockers.join(u' ').contains(QStringLiteral("changed since Migrate OS was planned")),
             qPrintable(blockers.join(u';')));
    auto missing = migrationPayload(source, 512ULL * kGiB);
    missing.remove(QStringLiteral("migration_plan"));
    QVERIFY(previewBlockers(inventoryWith(source, 512ULL * kGiB), missing)
                .join(u' ')
                .contains(QStringLiteral("no plan worked out")));
}

void PartitionOsMigrationTests::validatorWarnsOnlyWhenACopyWithoutASnapshotIsAllowed() {
    const auto source = gptWindowsDisk();
    const auto inventory = inventoryWith(source, 512ULL * kGiB);
    auto payload = migrationPayload(source, 512ULL * kGiB);
    PartitionOperationPlanner planner;
    const auto warningsFor = [&]() {
        return planner
            .previewOperation(inventory,
                              PartitionOperationPlanner::makeOperation(
                                  PartitionOperationType::MigrateOs, sourceTarget(source), payload))
            .warnings.join(u' ');
    };
    QVERIFY(!warningsFor().contains(QStringLiteral("snapshot")));
    payload[QStringLiteral("allow_no_snapshot")] = true;
    QVERIFY2(warningsFor().contains(QStringLiteral("may not start")), qPrintable(warningsFor()));
}

// Everything that can refuse must run before the first thing that writes to the target.
void PartitionOsMigrationTests::scriptRefusesEverythingBeforeTheFirstWrite() {
    const auto script = migrationScript(gptWindowsDisk(), 512ULL * kGiB);
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(u';')));
    QCOMPARE(script.timeout_seconds, kPartitionLongTaskTimeoutSeconds);
    const QString text = script.script;
    const qsizetype main = text.lastIndexOf(QStringLiteral("  Assert-SakMigrationStart\n"));
    QVERIFY(main > 0);
    const qsizetype snapshot = text.indexOf(QStringLiteral("  Open-SakSnapshots\n"), main);
    const qsizetype fits = text.indexOf(QStringLiteral("  Assert-SakWindowsFits\n"), main);
    const qsizetype layout = text.indexOf(QStringLiteral("  Write-SakTargetLayout\n"), main);
    const qsizetype copy = text.indexOf(QStringLiteral("  Copy-SakPartitions\n"), main);
    QVERIFY(main < snapshot && snapshot < fits && fits < layout && layout < copy);
    for (const char* check : {"  Assert-SakNoBitLocker\n",
                              "Assert-SakPinnedDisk $sakSourceDisk",
                              "Assert-SakPinnedDisk $sakTargetDisk",
                              "LogicalSectorSize",
                              "partitions changed since this was queued"}) {
        QVERIFY2(text.contains(QLatin1String(check)), check);
    }
    // Nothing writes to the target outside the one function that has flagged it as touched.
    const qsizetype layout_function =
        text.indexOf(QStringLiteral("function Write-SakTargetLayout"));
    QVERIFY(layout_function >= 0);
    QVERIFY(text.indexOf(QStringLiteral("$script:sakTargetTouched = $true"), layout_function) <
            text.indexOf(QStringLiteral("Set-Disk -Number $sakTargetDisk"), layout_function));
}

void PartitionOsMigrationTests::scriptReleasesSnapshotsAndNeverTouchesFirmware() {
    const QString text = migrationScript(gptWindowsDisk(), 512ULL * kGiB).script;
    QVERIFY(text.contains(QStringLiteral("finally { Close-SakMigration }")));
    QVERIFY(text.contains(QStringLiteral("$script:sakSnapshotSet.Release()")));
    // Not persistent: Windows drops the set when this process ends, which is what Cancel does.
    QVERIFY(text.contains(QStringLiteral("ContextBackup = 0")));
    QVERIFY(!text.contains(QStringLiteral("Win32_ShadowCopy")));
    // Left running, this writer deletes Outlook's OST files from the snapshot, so from the new
    // drive.
    QVERIFY(text.contains(QStringLiteral("4dc3bdd4-ab48-4d07-adb0-3bee2926fd7f")));
    // What is checked is what the copy read: a free cluster or the page file reads as the running
    // volume does, so asking the snapshot again would report Windows' own writes as a bad copy.
    QVERIFY(text.contains(
        QStringLiteral("[SakMigration.Copy]::Verify($target, $p.TargetOffset, $ranges, $check)")));
    QVERIFY(text.contains(QStringLiteral("$ranges, \"$($p.Kind) partition\", $check)")));
    QVERIFY(text.contains(QStringLiteral("check.Note(block, buffer, take);")));
    QVERIFY2(text.contains(QStringLiteral(
                 "if ($help -match 'nofirmwaresync') { '/nofirmwaresync' } else { '/addlast' }")),
             "a bcdboot on another disk must not move this PC's boot order");
    QVERIFY(text.contains(QStringLiteral("'/deletevalue', '{bootmgr}', 'nofirmwaresync'")));
    QVERIFY(text.contains(QStringLiteral("function Set-SakTargetDriveLetters")));
    QVERIFY(text.contains(QStringLiteral("  Set-SakTargetBoot\n")));
    QVERIFY(text.contains(QStringLiteral("  Set-SakTargetRecovery $windowsRoot\n")));
    QVERIFY(text.contains(QStringLiteral("/ConfigFile:$config")));
    QVERIFY(!text.contains(QStringLiteral("\\Windows\\CSC'")));
    QVERIFY(text.contains(QStringLiteral("[Guid]::NewGuid()")));
}

void PartitionOsMigrationTests::scriptWithoutConsentStopsOnNoSnapshot() {
    const QString text = migrationScript(gptWindowsDisk(), 512ULL * kGiB).script;
    QVERIFY(text.contains(QStringLiteral("$sakAllowNoSnapshot = $false")));
    QVERIFY(text.contains(QStringLiteral("if ($reason -and -not $sakAllowNoSnapshot) {")));
    QVERIFY(text.contains(QStringLiteral("Nothing was written to the target")));
    QVERIFY(text.contains(QStringLiteral("if ($reason -and $sakRoute -eq 'Image') {")));
    auto payload = migrationPayload(gptWindowsDisk(), 512ULL * kGiB);
    payload[QStringLiteral("allow_no_snapshot")] = true;
    const auto allowed =
        PartitionScriptBuilder().buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MigrateOs, sourceTarget(gptWindowsDisk()), payload));
    QVERIFY(allowed.script.contains(QStringLiteral("$sakAllowNoSnapshot = $true")));
}

void PartitionOsMigrationTests::scriptNeedsPinnedIdentityAndConfirmation() {
    const auto source = gptWindowsDisk();
    const auto build = [&source](const QJsonObject& payload, const PartitionTarget& target) {
        return PartitionScriptBuilder().buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::MigrateOs, target, payload));
    };
    auto no_confirm = migrationPayload(source, 512ULL * kGiB);
    no_confirm[QStringLiteral("target_wipe_confirmed")] = false;
    QVERIFY(!build(no_confirm, sourceTarget(source)).valid());
    auto no_plan = migrationPayload(source, 512ULL * kGiB);
    no_plan.remove(QStringLiteral("migration_plan"));
    QVERIFY(!build(no_plan, sourceTarget(source)).valid());
    auto no_target_pin = migrationPayload(source, 512ULL * kGiB);
    no_target_pin.remove(QStringLiteral("target_disk_unique_id"));
    QVERIFY(!build(no_target_pin, sourceTarget(source)).valid());
    auto unpinned = sourceTarget(source);
    unpinned.disk_unique_id.clear();
    QVERIFY(!build(migrationPayload(source, 512ULL * kGiB), unpinned).valid());
    auto not_a_disk = migrationPayload(source, 512ULL * kGiB);
    not_a_disk[QStringLiteral("target_path")] = QStringLiteral("C:\\image.img");
    QVERIFY(!build(not_a_disk, sourceTarget(source)).valid());
}

void PartitionOsMigrationTests::attachedRepairFindsWindowsAndRefusesTheRunningDisk() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = DiskNumber{2};
    target.disk_unique_id = QStringLiteral("ATTACHED");
    target.disk_size_bytes = kSourceBytes;
    const QJsonObject payload{{QStringLiteral("repair_scope"), QStringLiteral("attached")},
                              {QStringLiteral("boot_mode"), QStringLiteral("BIOS")}};
    const auto script =
        PartitionScriptBuilder().buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::RepairBoot, target, payload));
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(u';')));
    const QString text = script.script;
    QVERIFY(text.contains(QStringLiteral("$sakRepairDisk = [uint32]2")));
    QVERIFY(text.contains(QStringLiteral("$sakRepairMode = 'BIOS'")));
    const qsizetype identity = text.indexOf(QStringLiteral("Disk identity mismatch"));
    const qsizetype refuse = text.indexOf(QStringLiteral("if ($disk.IsBoot -or $disk.IsSystem)"));
    const qsizetype repair = text.lastIndexOf(QStringLiteral("Invoke-SakTargetBootFiles $system"));
    QVERIFY(identity >= 0 && identity < refuse && refuse < repair);
    // The identity is checked once before anything else in the script, not only by the guards
    // each line that changes a disk gets.
    QVERIFY(identity < text.indexOf(QStringLiteral("function ")));
    QVERIFY(text.contains(QStringLiteral("throw \"No Windows was found on Disk")));
    QVERIFY(text.contains(QStringLiteral("throw \"More than one Windows")));
    QVERIFY(text.contains(QStringLiteral("'/nofirmwaresync'")));
    QVERIFY(text.contains(QStringLiteral("Remove-PartitionAccessPath")));
    QVERIFY2(!text.contains(QStringLiteral("mountvol")),
             "mountvol /S names the running PC's own EFI partition");
}

void PartitionOsMigrationTests::attachedRepairNeedsTheDisksIdentity() {
    PartitionTarget target;
    target.kind = PartitionTargetKind::Disk;
    target.disk_number = DiskNumber{2};
    const QJsonObject payload{{QStringLiteral("repair_scope"), QStringLiteral("attached")},
                              {QStringLiteral("boot_mode"), QStringLiteral("UEFI")}};
    QVERIFY(!PartitionScriptBuilder()
                 .buildScript(PartitionOperationPlanner::makeOperation(
                     PartitionOperationType::RepairBoot, target, payload))
                 .valid());
    target.disk_unique_id = QStringLiteral("ATTACHED");
    target.disk_size_bytes = kSourceBytes;
    const QJsonObject wrong_mode{{QStringLiteral("repair_scope"), QStringLiteral("attached")},
                                 {QStringLiteral("boot_mode"), QStringLiteral("Neither")}};
    QVERIFY(!PartitionScriptBuilder()
                 .buildScript(PartitionOperationPlanner::makeOperation(
                     PartitionOperationType::RepairBoot, target, wrong_mode))
                 .valid());
}

void PartitionOsMigrationTests::repairScopeMustMatchTheDisk() {
    auto attached_disk = gptWindowsDisk();
    attached_disk.disk_number = DiskNumber{1};
    attached_disk.is_boot = false;
    attached_disk.is_system = false;
    const auto running_disk = gptWindowsDisk();
    PartitionInventory inventory;
    inventory.disks = {running_disk, attached_disk};
    PartitionOperationPlanner planner;
    const auto blockersFor = [&](const PartitionDiskInfo& disk, const QJsonObject& payload) {
        return planner
            .previewOperation(inventory,
                              PartitionOperationPlanner::makeOperation(
                                  PartitionOperationType::RepairBoot, sourceTarget(disk), payload))
            .blockers.join(u' ');
    };
    const QJsonObject attached{{QStringLiteral("repair_scope"), QStringLiteral("attached")},
                               {QStringLiteral("boot_mode"), QStringLiteral("UEFI")}};
    const QJsonObject running{{QStringLiteral("windows_path"), QStringLiteral("C:\\Windows")},
                              {QStringLiteral("esp_letter"), QStringLiteral("S")},
                              {QStringLiteral("boot_mode"), QStringLiteral("UEFI")}};
    QVERIFY(blockersFor(running_disk, attached).contains(QStringLiteral("running from")));
    QVERIFY(blockersFor(attached_disk, running).contains(QStringLiteral("not running from")));
    QVERIFY2(!blockersFor(attached_disk, attached).contains(QStringLiteral("running")),
             qPrintable(blockersFor(attached_disk, attached)));
    QVERIFY2(!blockersFor(running_disk, running).contains(QStringLiteral("running")),
             qPrintable(blockersFor(running_disk, running)));
}

void PartitionOsMigrationTests::scriptParsesCompilesAndCopiesRealFiles() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto write = [&temp](const QString& name, const QString& text) {
        const QString path = temp.filePath(name);
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly)) {
            return QString();
        }
        output.write(text.toUtf8());
        return path;
    };
    PartitionTarget attached;
    attached.kind = PartitionTargetKind::Disk;
    attached.disk_number = DiskNumber{2};
    attached.disk_unique_id = QStringLiteral("ATTACHED");
    attached.disk_size_bytes = kSourceBytes;
    const QJsonObject repair{{QStringLiteral("repair_scope"), QStringLiteral("attached")},
                             {QStringLiteral("boot_mode"), QStringLiteral("UEFI")}};
    const QString migration = write(QStringLiteral("migrate.ps1"),
                                    migrationScript(gptWindowsDisk(), 512ULL * kGiB).script);
    const QString repair_path = write(QStringLiteral("repair.ps1"),
                                      PartitionScriptBuilder()
                                          .buildScript(PartitionOperationPlanner::makeOperation(
                                              PartitionOperationType::RepairBoot, attached, repair))
                                          .script);
    QVERIFY(!migration.isEmpty() && !repair_path.isEmpty());
    const QString fixture = QFINDTESTDATA("../support/partition_os_migration_fixture.ps1");
    QVERIFY(!fixture.isEmpty());
    const auto result = runProcess(systemPowerShellPath(),
                                   {QStringLiteral("-NoProfile"),
                                    QStringLiteral("-NonInteractive"),
                                    QStringLiteral("-ExecutionPolicy"),
                                    QStringLiteral("Bypass"),
                                    QStringLiteral("-File"),
                                    fixture,
                                    QStringLiteral("-MigrationScript"),
                                    migration,
                                    QStringLiteral("-RepairScript"),
                                    repair_path,
                                    QStringLiteral("-WorkFolder"),
                                    temp.path()},
                                   kRuntimeTimeoutMs);
    QVERIFY2(result.completedSuccessfully(), qPrintable(result.std_out + result.std_err));
    QVERIFY2(result.std_out.contains(QStringLiteral("OS_MIGRATION_FIXTURE_PASSED")),
             qPrintable(result.std_out + result.std_err));
}

// Windows would refuse to start PowerShell with this on its command line.
void PartitionOsMigrationTests::scriptIsTooLongForACommandLineAndStillStarts() {
    const QString migration = migrationScript(gptWindowsDisk(), 512ULL * kGiB).script;
    QVERIFY(migration.size() > kMaxPowerShellCommandLineScriptChars);
    const auto invocation = powerShellInvocation(migration, true, true);
    QCOMPARE(QString::fromUtf8(invocation.std_in), migration);
    QVERIFY(invocation.args.join(u' ').size() < kMaxPowerShellCommandLineScriptChars);
}

// Captured from a real refusal in the VM. The person used to get "Command exited with code 1".
void PartitionOsMigrationTests::aRefusalReachesThePersonInItsOwnWords() {
    const QString captured = QStringLiteral(
        "Windows could not take a snapshot of the running disk: InitializeForBackup failed with\n"
        "VSS error 0x80042302. Nothing was written to the target.\n"
        "At line:1 char:92\n"
        "+     throw \"Windows could not take a snapshot ...\n"
        "    + CategoryInfo          : OperationStopped: (:) [], RuntimeException\n");
    QCOMPARE(PartitionExecutor::scriptFailureReason(captured),
             QStringLiteral("Windows could not take a snapshot of the running disk: "
                            "InitializeForBackup failed with VSS error 0x80042302. Nothing was "
                            "written to the target."));
    QVERIFY(PartitionExecutor::scriptFailureReason(QString()).isEmpty());
    // The script's own .NET code fails in PowerShell's words around its own, seen in the VM.
    QCOMPARE(PartitionExecutor::scriptFailureReason(QStringLiteral(
                 "Exception calling \"Verify\" with \"6\" argument(s): \"Verification found a "
                 "difference at target byte 228065280\"\n"
                 "At line:1 char:92\n")),
             QStringLiteral("Verification found a difference at target byte 228065280"));
    const QString generic = QStringLiteral("Command exited with code 1");
    QVERIFY(PartitionExecutor::elevatedFailureMessage(generic, QString(), captured, 1)
                .startsWith(QStringLiteral("Windows could not take a snapshot")));
    QCOMPARE(PartitionExecutor::elevatedFailureMessage(generic, QString(), QString(), 1), generic);
    // A script that succeeded and then lost its connection keeps what the helper said.
    QCOMPARE(PartitionExecutor::elevatedFailureMessage(generic, QString(), captured, 0), generic);
    QVERIFY(PartitionExecutor::elevatedFailureMessage(QString(), QString(), QString(), 3)
                .contains(QStringLiteral("exit code 3")));
}

QTEST_MAIN(PartitionOsMigrationTests)
#include "test_partition_os_migration.moc"
