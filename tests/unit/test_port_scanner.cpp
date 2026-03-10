// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_port_scanner.cpp
/// @brief Unit tests for PortScanner static helpers and presets

#include "sak/port_scanner.h"

#include <QtTest/QtTest>

using namespace sak;

class TestPortScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──────────────────────────────────────────────
    void construction_default();
    void construction_nonCopyable();

    // ── getServiceName ────────────────────────────────────────────
    void serviceName_http();
    void serviceName_https();
    void serviceName_ssh();
    void serviceName_ftp();
    void serviceName_dns();
    void serviceName_smb();
    void serviceName_rdp();
    void serviceName_unknownPort();

    // ── getPresets ────────────────────────────────────────────────
    void presets_nonEmpty();
    void presets_haveValidPorts();
    void presets_haveNames();

    // ── ScanConfig defaults ──────────────────────────────────────
    void scanConfig_defaults();
};

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

void TestPortScanner::construction_default() {
    PortScanner scanner;
    QVERIFY(scanner.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<PortScanner>);
}

void TestPortScanner::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<PortScanner>);
    QVERIFY(!std::is_move_constructible_v<PortScanner>);
}

// ═══════════════════════════════════════════════════════════════════
// getServiceName
// ═══════════════════════════════════════════════════════════════════

void TestPortScanner::serviceName_http() {
    const auto name = PortScanner::getServiceName(80);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("HTTP", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_https() {
    const auto name = PortScanner::getServiceName(443);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("HTTPS", Qt::CaseInsensitive) ||
            name.contains("HTTP", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_ssh() {
    const auto name = PortScanner::getServiceName(22);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("SSH", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_ftp() {
    const auto name = PortScanner::getServiceName(21);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("FTP", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_dns() {
    const auto name = PortScanner::getServiceName(53);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("DNS", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_smb() {
    const auto name = PortScanner::getServiceName(445);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("SMB", Qt::CaseInsensitive) ||
            name.contains("Microsoft", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_rdp() {
    const auto name = PortScanner::getServiceName(3389);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("RDP", Qt::CaseInsensitive) ||
            name.contains("Remote", Qt::CaseInsensitive));
}

void TestPortScanner::serviceName_unknownPort() {
    const auto name = PortScanner::getServiceName(59'999);
    // Unknown port should return empty or "Unknown"
    QVERIFY(name.isEmpty() || name.contains("Unknown", Qt::CaseInsensitive));
}

// ═══════════════════════════════════════════════════════════════════
// getPresets
// ═══════════════════════════════════════════════════════════════════

void TestPortScanner::presets_nonEmpty() {
    const auto presets = PortScanner::getPresets();
    QVERIFY(!presets.isEmpty());
    QVERIFY(presets.size() >= 1);
}

void TestPortScanner::presets_haveValidPorts() {
    const auto presets = PortScanner::getPresets();
    for (const auto& preset : presets) {
        QVERIFY(!preset.ports.isEmpty());
        for (const auto port : preset.ports) {
            QVERIFY(port > 0);
        }
    }
}

void TestPortScanner::presets_haveNames() {
    const auto presets = PortScanner::getPresets();
    for (const auto& preset : presets) {
        QVERIFY(!preset.name.isEmpty());
    }
}

// ═══════════════════════════════════════════════════════════════════
// ScanConfig defaults
// ═══════════════════════════════════════════════════════════════════

void TestPortScanner::scanConfig_defaults() {
    PortScanner::ScanConfig config;
    QVERIFY(config.target.isEmpty());
    QVERIFY(config.ports.isEmpty());
    QCOMPARE(config.portRangeStart, static_cast<uint16_t>(0));
    QCOMPARE(config.portRangeEnd, static_cast<uint16_t>(0));
    QVERIFY(config.timeoutMs > 0);
    QVERIFY(config.maxConcurrent > 0);
    QCOMPARE(config.grabBanners, true);
}

QTEST_MAIN(TestPortScanner)
#include "test_port_scanner.moc"
