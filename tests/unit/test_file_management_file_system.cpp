// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_management_file_system.cpp
/// @brief Unit tests for File Management file-system target bridge.

#include "sak/file_management_file_system.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

class FileManagementFileSystemTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void localTargetListsAndReadsFiles() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QFile file(QDir(temp.path()).filePath(QStringLiteral("note.txt")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("hello target bridge");
        file.close();

        const auto target = sak::FileManagementFileSystemBridge::localTarget(temp.path());
        QVERIFY(target.local_file_system);
        QVERIFY(target.can_organize);

        const auto listing =
            sak::FileManagementFileSystemBridge::listDirectory(target, temp.path(), 100);
        QVERIFY2(listing.ok, qPrintable(listing.blockers.join(QStringLiteral("; "))));
        QCOMPARE(listing.entries.size(), 1);
        QCOMPARE(listing.entries.first().name, QStringLiteral("note.txt"));

        const auto read = sak::FileManagementFileSystemBridge::readFile(
            target, listing.entries.first().path, 1024);
        QVERIFY(read.ok);
        QCOMPARE(QString::fromUtf8(read.data), QStringLiteral("hello target bridge"));
    }

    void manualApfsTargetIsReadOnlySearchableButNotOrganizable() {
        const auto target = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("C:/fixtures/apfs.img"), QStringLiteral("APFS"));
        QVERIFY(!target.local_file_system);
        QVERIFY(target.read_only);
        QVERIFY(target.can_browse);
        QVERIFY(target.can_duplicate_scan);
        QVERIFY(target.can_advanced_search);
        QVERIFY(!target.can_organize);
        QVERIFY(!target.blockers.isEmpty());
    }

    void apfsRawWritesSpanCertifiedMultiCibRange() {
        // A1/A2: the in-place COW engine is Apple-certified across the single-CIB,
        // multi-CIB, metadata-overflow, and CAB tiers, so the File Explorer write gate
        // must accept generated containers from 64 MiB through the 24 TiB ceiling.
        const auto singleChunk = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"),
            QStringLiteral("APFS"),
            128ULL * 1024ULL * 1024ULL);
        QVERIFY(singleChunk.can_browse);
        QVERIFY(singleChunk.can_write_files);

        // Multi-CIB / metadata-overflow size that the pre-fix 128 MiB cap wrongly blocked.
        const auto multiCib = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition4"),
            QStringLiteral("APFS"),
            4ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
        QVERIFY(multiCib.can_write_files);

        // Below the 64 MiB floor remains read-only.
        const auto tooSmall = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition5"),
            QStringLiteral("APFS"),
            32ULL * 1024ULL * 1024ULL);
        QVERIFY(!tooSmall.can_write_files);

        // Above the 24 TiB ceiling remains fail-closed with the certified-range blocker.
        const auto oversized = sak::FileManagementFileSystemBridge::manualTarget(
            QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition3"),
            QStringLiteral("APFS"),
            32ULL * 1024ULL * 1024ULL * 1024ULL * 1024ULL);
        QVERIFY(oversized.can_browse);
        QVERIFY(!oversized.can_write_files);
        QVERIFY(oversized.blockers.join(' ').contains(QStringLiteral("24 TiB")));
    }

    void inventoryPartitionBuildsRawAlias() {
        sak::PartitionInventory inventory;
        sak::PartitionDiskInfo disk;
        disk.disk_number = 4;
        sak::PartitionInfoEx partition;
        partition.disk_number = 4;
        partition.partition_number = 2;
        sak::PartitionVolumeInfo volume;
        volume.file_system = QStringLiteral("HFS+");
        partition.volume = volume;
        disk.partitions.append(partition);
        inventory.disks.append(disk);

        const auto targets = sak::FileManagementFileSystemBridge::targetsFromInventory(inventory);
        const auto it = std::find_if(targets.cbegin(), targets.cend(), [](const auto& target) {
            return target.id == QStringLiteral("disk:4:partition:2");
        });
        QVERIFY(it != targets.cend());
        QCOMPARE(it->root_path, QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2"));
        QVERIFY(it->can_browse);
        QVERIFY(!it->can_organize);
    }
};

QTEST_MAIN(FileManagementFileSystemTests)
#include "test_file_management_file_system.moc"
