// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_inventory_scanner.cpp
/// @brief Unit tests for HardwareInventoryScanner

#include "sak/diagnostic_types.h"
#include "sak/hardware_inventory_scanner.h"

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

QTEST_MAIN(TestHardwareInventoryScanner)
#include "test_hardware_inventory_scanner.moc"
