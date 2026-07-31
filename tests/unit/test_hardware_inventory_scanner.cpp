// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_inventory_scanner.cpp
/// @brief Unit tests for HardwareInventoryScanner

#include "sak/diagnostic_types.h"
#include "sak/hardware_inventory_scanner.h"

#include <QJsonDocument>
#include <QSignalSpy>
#include <QtTest/QtTest>

using namespace sak;

class TestHardwareInventoryScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void cpuInfo_defaults();
    void cpuInfo_fieldAssignment();
    void memorySummary_defaults();
    void memoryModuleInfo_defaults();
    void storageDeviceInfo_defaults();
    void gpuInfo_defaults();
    void motherboardInfo_defaults();
    void batteryInfo_defaults();
    void cancel_doesNotCrash();
    void scanCpu_returnsRealCpuInfo();
    void volumeDiskMap_parsesArray();
    void volumeDiskMap_parsesSingleObject();
    void volumeDiskMap_dropsNullDiskIndex();
    void volumeDiskMap_dropsEmptyLetter();
    void volumeDiskMap_handlesMalformed();
    // B5-12: WMI failure must not read as an empty-but-successful result.
    void wmiFailure_successAndEmptyAreNotFailures();
    void wmiFailure_timeoutExitParseAreFailures();
    void wmiFailure_cancelIsNotFailure();
    void jsonDocToVariantMaps_arrayObjectAndSkips();
};

void TestHardwareInventoryScanner::construction_default() {
    HardwareInventoryScanner scanner;
    QVERIFY(dynamic_cast<QObject*>(&scanner) != nullptr);
}

void TestHardwareInventoryScanner::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<HardwareInventoryScanner>);
    QVERIFY(!std::is_move_constructible_v<HardwareInventoryScanner>);
}

void TestHardwareInventoryScanner::cpuInfo_defaults() {
    CpuInfo info;
    QVERIFY(info.name.isEmpty());
    QVERIFY(info.manufacturer.isEmpty());
    QCOMPARE(info.cores, 0u);
    QCOMPARE(info.threads, 0u);
    QCOMPARE(info.base_clock_mhz, 0u);
    QCOMPARE(info.max_clock_mhz, 0u);
    QCOMPARE(info.l2_cache_kb, 0u);
    QCOMPARE(info.l3_cache_kb, 0u);
}

void TestHardwareInventoryScanner::cpuInfo_fieldAssignment() {
    CpuInfo info;
    info.name = QStringLiteral("Test CPU");
    info.manufacturer = QStringLiteral("TestCorp");
    info.cores = 8;
    info.threads = 16;
    info.base_clock_mhz = 3600;
    info.max_clock_mhz = 5000;
    info.l2_cache_kb = 4096;
    info.l3_cache_kb = 32'768;

    QCOMPARE(info.name, QStringLiteral("Test CPU"));
    QCOMPARE(info.cores, 8u);
    QCOMPARE(info.threads, 16u);
    QCOMPARE(info.base_clock_mhz, 3600u);
    QCOMPARE(info.max_clock_mhz, 5000u);
}

void TestHardwareInventoryScanner::memorySummary_defaults() {
    MemorySummary summary;
    QCOMPARE(summary.total_bytes, static_cast<uint64_t>(0));
    QCOMPARE(summary.available_bytes, static_cast<uint64_t>(0));
    QCOMPARE(summary.slots_used, 0u);
    QCOMPARE(summary.slots_total, 0u);
    QVERIFY(summary.modules.isEmpty());
}

void TestHardwareInventoryScanner::memoryModuleInfo_defaults() {
    MemoryModuleInfo mod;
    QVERIFY(mod.manufacturer.isEmpty());
    QVERIFY(mod.part_number.isEmpty());
    QCOMPARE(mod.capacity_bytes, static_cast<uint64_t>(0));
    QCOMPARE(mod.speed_mhz, 0u);
    QVERIFY(mod.memory_type.isEmpty());
    QVERIFY(mod.form_factor.isEmpty());
    QCOMPARE(mod.slot, 0u);
}

void TestHardwareInventoryScanner::storageDeviceInfo_defaults() {
    StorageDeviceInfo dev;
    QVERIFY(dev.model.isEmpty());
    QCOMPARE(dev.size_bytes, static_cast<uint64_t>(0));
    QVERIFY(dev.interface_type.isEmpty());
    QVERIFY(dev.media_type.isEmpty());
    QCOMPARE(dev.disk_number, 0u);
    QVERIFY(dev.partitions.isEmpty());
}

void TestHardwareInventoryScanner::gpuInfo_defaults() {
    GpuInfo info;
    QVERIFY(info.name.isEmpty());
    QVERIFY(info.manufacturer.isEmpty());
    QCOMPARE(info.vram_bytes, static_cast<uint64_t>(0));
    QVERIFY(info.driver_version.isEmpty());
    QCOMPARE(info.current_res_x, 0u);
    QCOMPARE(info.current_res_y, 0u);
    QCOMPARE(info.refresh_rate, 0u);
}

void TestHardwareInventoryScanner::motherboardInfo_defaults() {
    MotherboardInfo info;
    QVERIFY(info.manufacturer.isEmpty());
    QVERIFY(info.product.isEmpty());
    QVERIFY(info.serial_number.isEmpty());
    QVERIFY(info.bios_version.isEmpty());
}

void TestHardwareInventoryScanner::batteryInfo_defaults() {
    BatteryInfo info;
    QVERIFY(!info.present);
    QVERIFY(info.status.isEmpty());
    QCOMPARE(info.current_charge, 0u);
}

void TestHardwareInventoryScanner::cancel_doesNotCrash() {
    HardwareInventoryScanner scanner;
    scanner.cancel();
    QVERIFY(dynamic_cast<QObject*>(&scanner) != nullptr);
}

void TestHardwareInventoryScanner::scanCpu_returnsRealCpuInfo() {
    HardwareInventoryScanner scanner;
    QSignalSpy complete_spy(&scanner, &HardwareInventoryScanner::scanComplete);

    scanner.scanCpu();

    QCOMPARE(complete_spy.count(), 1);
    const auto inventory = complete_spy.takeFirst().at(0).value<HardwareInventory>();
    QVERIFY2(!inventory.cpu.name.isEmpty(), "Every system has a CPU with a name");
    QVERIFY(inventory.cpu.cores > 0);
    QVERIFY(inventory.cpu.threads > 0);
    QVERIFY(inventory.cpu.threads >= inventory.cpu.cores);
}

// P07-22: an array of {Letter, DiskIndex} maps each drive letter to its disk.
void TestHardwareInventoryScanner::volumeDiskMap_parsesArray() {
    const QByteArray json = R"([{"Letter":"C:","DiskIndex":0},{"Letter":"D:","DiskIndex":2}])";
    const auto map = HardwareInventoryScanner::parseVolumeDiskMap(json);
    QCOMPARE(map.size(), 2);
    QCOMPARE(map.value(QStringLiteral("C:")), 0u);
    QCOMPARE(map.value(QStringLiteral("D:")), 2u);
}

// A lone mapping is emitted by ConvertTo-Json as an object, not an array.
void TestHardwareInventoryScanner::volumeDiskMap_parsesSingleObject() {
    const QByteArray json = R"({"Letter":"C:","DiskIndex":0})";
    const auto map = HardwareInventoryScanner::parseVolumeDiskMap(json);
    QCOMPARE(map.size(), 1);
    QCOMPARE(map.value(QStringLiteral("C:")), 0u);
}

// A partition with no matching disk yields DiskIndex=null; it must be dropped
// so the volume is never silently attributed to disk 0.
void TestHardwareInventoryScanner::volumeDiskMap_dropsNullDiskIndex() {
    const QByteArray json = R"([{"Letter":"C:","DiskIndex":0},{"Letter":"E:","DiskIndex":null}])";
    const auto map = HardwareInventoryScanner::parseVolumeDiskMap(json);
    QCOMPARE(map.size(), 1);
    QVERIFY(map.contains(QStringLiteral("C:")));
    QVERIFY(!map.contains(QStringLiteral("E:")));
}

void TestHardwareInventoryScanner::volumeDiskMap_dropsEmptyLetter() {
    const QByteArray json = R"([{"Letter":"","DiskIndex":1}])";
    const auto map = HardwareInventoryScanner::parseVolumeDiskMap(json);
    QVERIFY(map.isEmpty());
}

void TestHardwareInventoryScanner::volumeDiskMap_handlesMalformed() {
    QVERIFY(HardwareInventoryScanner::parseVolumeDiskMap(QByteArray()).isEmpty());
    QVERIFY(HardwareInventoryScanner::parseVolumeDiskMap("not json").isEmpty());
    QVERIFY(HardwareInventoryScanner::parseVolumeDiskMap("[]").isEmpty());
}

// B5-12: a successful query (including a legitimately empty result set) is NOT a
// failure -- the scan must not falsely flag the inventory as incomplete.
void TestHardwareInventoryScanner::wmiFailure_successAndEmptyAreNotFailures() {
    // timedOut=false, cancelled=false, exit=0, parse ok -> success.
    QVERIFY(!HardwareInventoryScanner::wmiQueryIsReportableFailure(false, false, 0, false));
}

// A timeout, a non-zero exit, or a JSON parse error are all genuine failures
// that must be surfaced rather than swallowed as "no data".
void TestHardwareInventoryScanner::wmiFailure_timeoutExitParseAreFailures() {
    QVERIFY(
        HardwareInventoryScanner::wmiQueryIsReportableFailure(true, false, 0, false));   // timeout
    QVERIFY(
        HardwareInventoryScanner::wmiQueryIsReportableFailure(false, false, 1, false));  // exit!=0
    QVERIFY(
        HardwareInventoryScanner::wmiQueryIsReportableFailure(false, false, 0, true));   // bad JSON
}

// A user cancellation is not a data-integrity failure, even if the process was
// torn down with a non-zero exit code.
void TestHardwareInventoryScanner::wmiFailure_cancelIsNotFailure() {
    QVERIFY(!HardwareInventoryScanner::wmiQueryIsReportableFailure(false, true, 1, false));
    QVERIFY(!HardwareInventoryScanner::wmiQueryIsReportableFailure(true, true, 0, false));
}

void TestHardwareInventoryScanner::jsonDocToVariantMaps_arrayObjectAndSkips() {
    // Array of two objects -> two maps.
    const auto arr = HardwareInventoryScanner::jsonDocToVariantMaps(
        QJsonDocument::fromJson(R"([{"A":1},{"B":2}])"));
    QCOMPARE(arr.size(), 2);
    QCOMPARE(arr.at(0).value("A").toInt(), 1);
    QCOMPARE(arr.at(1).value("B").toInt(), 2);

    // Single object -> one map.
    const auto obj =
        HardwareInventoryScanner::jsonDocToVariantMaps(QJsonDocument::fromJson(R"({"X":9})"));
    QCOMPARE(obj.size(), 1);
    QCOMPARE(obj.at(0).value("X").toInt(), 9);

    // Non-object array entries are skipped.
    const auto mixed = HardwareInventoryScanner::jsonDocToVariantMaps(
        QJsonDocument::fromJson(R"([{"A":1},7,"str"])"));
    QCOMPARE(mixed.size(), 1);
}

QTEST_MAIN(TestHardwareInventoryScanner)
#include "test_hardware_inventory_scanner.moc"
