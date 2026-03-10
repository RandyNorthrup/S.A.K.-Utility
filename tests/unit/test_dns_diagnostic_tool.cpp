// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_dns_diagnostic_tool.cpp
/// @brief Unit tests for DnsDiagnosticTool static helpers

#include "sak/dns_diagnostic_tool.h"

#include <QtTest/QtTest>

using namespace sak;

class TestDnsDiagnosticTool : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──────────────────────────────────────────────
    void construction_default();
    void construction_nonCopyable();

    // ── wellKnownDnsServers ───────────────────────────────────────
    void wellKnown_nonEmpty();
    void wellKnown_containsGoogle();
    void wellKnown_containsCloudflare();
    void wellKnown_pairsHaveNameAndIp();

    // ── supportedRecordTypes ──────────────────────────────────────
    void recordTypes_nonEmpty();
    void recordTypes_containsA();
    void recordTypes_containsAAAA();
    void recordTypes_containsMX();
    void recordTypes_containsCNAME();
    void recordTypes_noDuplicates();
};

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

void TestDnsDiagnosticTool::construction_default() {
    DnsDiagnosticTool tool;
    QVERIFY(tool.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<DnsDiagnosticTool>);
}

void TestDnsDiagnosticTool::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<DnsDiagnosticTool>);
    QVERIFY(!std::is_move_constructible_v<DnsDiagnosticTool>);
}

// ═══════════════════════════════════════════════════════════════════
// wellKnownDnsServers
// ═══════════════════════════════════════════════════════════════════

void TestDnsDiagnosticTool::wellKnown_nonEmpty() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    QVERIFY(!servers.isEmpty());
    QVERIFY(servers.size() >= 3);
}

void TestDnsDiagnosticTool::wellKnown_containsGoogle() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found_google = false;
    for (const auto& pair : servers) {
        if (pair.second.contains("8.8.8.8") || pair.first.contains("Google", Qt::CaseInsensitive)) {
            found_google = true;
            break;
        }
    }
    QVERIFY(found_google);
}

void TestDnsDiagnosticTool::wellKnown_containsCloudflare() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    bool found_cf = false;
    for (const auto& pair : servers) {
        if (pair.second.contains("1.1.1.1") ||
            pair.first.contains("Cloudflare", Qt::CaseInsensitive)) {
            found_cf = true;
            break;
        }
    }
    QVERIFY(found_cf);
}

void TestDnsDiagnosticTool::wellKnown_pairsHaveNameAndIp() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    for (const auto& pair : servers) {
        QVERIFY2(!pair.first.isEmpty(), "DNS server name must not be empty");
        // "System Default" intentionally has empty IP
        if (pair.first == QStringLiteral("System Default")) {
            continue;
        }
        QVERIFY2(!pair.second.isEmpty(), "DNS server IP must not be empty");
    }
}

// ═══════════════════════════════════════════════════════════════════
// supportedRecordTypes
// ═══════════════════════════════════════════════════════════════════

void TestDnsDiagnosticTool::recordTypes_nonEmpty() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(!types.isEmpty());
    QVERIFY(types.size() >= 4);
}

void TestDnsDiagnosticTool::recordTypes_containsA() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("A"));
}

void TestDnsDiagnosticTool::recordTypes_containsAAAA() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("AAAA"));
}

void TestDnsDiagnosticTool::recordTypes_containsMX() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("MX"));
}

void TestDnsDiagnosticTool::recordTypes_containsCNAME() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("CNAME"));
}

void TestDnsDiagnosticTool::recordTypes_noDuplicates() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QSet<QString> unique_types(types.begin(), types.end());
    QCOMPARE(unique_types.size(), types.size());
}

QTEST_MAIN(TestDnsDiagnosticTool)
#include "test_dns_diagnostic_tool.moc"
