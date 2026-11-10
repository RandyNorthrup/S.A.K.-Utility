// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_partition_image_volume_lock.cpp
/// @brief Runtime proof for each Create Image source-lock shape.

#include "sak/partition_operation_planner.h"
#include "sak/partition_script_builder.h"
#include "sak/process_runner.h"

#include <QFile>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class PartitionImageVolumeLockTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scriptBuilder_createImageVolumeLocksRuntime_data();
    void scriptBuilder_createImageVolumeLocksRuntime();
};

void PartitionImageVolumeLockTests::scriptBuilder_createImageVolumeLocksRuntime_data() {
    QTest::addColumn<bool>("partition");
    QTest::addColumn<bool>("pinned_guid");
    QTest::addColumn<bool>("offline");
    QTest::newRow("whole-disk") << false << false << false;
    QTest::newRow("selected-partition") << true << true << false;
    QTest::newRow("selected-mbr-unpinned-guid") << true << false << false;
    QTest::newRow("offline-whole-disk") << false << false << true;
    QTest::newRow("offline-selected-partition") << true << true << true;
}

void PartitionImageVolumeLockTests::scriptBuilder_createImageVolumeLocksRuntime() {
    QFETCH(bool, partition);
    QFETCH(bool, pinned_guid);
    QFETCH(bool, offline);
    constexpr uint64_t kImageBytes = 4096;
    constexpr uint64_t kPartitionBytes = 1024;
    constexpr int kRuntimeTimeoutMs = 180'000;
    PartitionTarget target;
    target.disk_number = DiskNumber{7};
    target.kind = partition ? PartitionTargetKind::Partition : PartitionTargetKind::Disk;
    target.partition_number = PartitionNumber{2};
    target.offset_bytes = kPartitionBytes;
    target.size_bytes = partition ? kPartitionBytes : kImageBytes;
    target.drive_letter = QStringLiteral("E");
    target.partition_guid = pinned_guid ? QStringLiteral("{11111111-1111-1111-1111-111111111111}")
                                        : QString();
    target.disk_unique_id = QStringLiteral("IMAGE-SOURCE");
    target.disk_serial_number = QStringLiteral("IMAGE-SERIAL");
    target.disk_size_bytes = kImageBytes;
    QJsonObject payload{
        {"source_path", partition && !offline ? "\\\\.\\E:" : "\\\\.\\PhysicalDrive7"},
        {"target_path", "D:\\images\\fixture.img"},
        {"source_size_bytes", QString::number(target.size_bytes)},
        {"verify_mode", "Full"},
        {"source_capture_mode", offline ? "OfflineDisk" : "LockedVolumes"},
        {"source_offset_bytes", QString::number(offline && partition ? target.offset_bytes : 0)}};
    const auto script =
        PartitionScriptBuilder().buildScript(PartitionOperationPlanner::makeOperation(
            PartitionOperationType::CreateImage, target, payload));
    QVERIFY2(script.valid(), qPrintable(script.blockers.join(';')));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString script_path = temp.filePath(QStringLiteral("create-image.ps1"));
    QFile output(script_path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    const QByteArray bytes = script.script.toUtf8();
    QCOMPARE(output.write(bytes), bytes.size());
    output.close();
    const QString fixture = QFINDTESTDATA("../support/partition_image_volume_lock_fixture.ps1");
    QVERIFY(!fixture.isEmpty());
    const QString powershell = sak::systemPowerShellPath();
    QVERIFY(!powershell.isEmpty());
    const auto result = sak::runProcess(powershell,
                                        {"-NoProfile",
                                         "-NonInteractive",
                                         "-ExecutionPolicy",
                                         "RemoteSigned",
                                         "-File",
                                         fixture,
                                         "-ScriptPath",
                                         script_path,
                                         "-RunGuardDrills"},
                                        kRuntimeTimeoutMs);
    QVERIFY2(result.completedSuccessfully(), qPrintable(result.std_out + result.std_err));
    QVERIFY(result.std_out.contains(QStringLiteral("CREATE_IMAGE_SOURCE_LOCK_FIXTURE_PASSED")));
}

QTEST_MAIN(PartitionImageVolumeLockTests)
#include "test_partition_image_volume_lock.moc"
