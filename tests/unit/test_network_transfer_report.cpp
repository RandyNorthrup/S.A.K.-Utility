// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_transfer_report.cpp
/// @brief Unit tests for TransferReport serialization

#include "sak/network_transfer_report.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest/QtTest>

class TestNetworkTransferReport : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── toJson ──────────────────────────────────────────────────────────
    void toJson_emptyReport();
    void toJson_populatedReport();
    void toJson_withErrors();
    void toJson_withManifest();
    void toJson_roundTripTimestamps();
};

// ============================================================================
// Helper
// ============================================================================

static sak::TransferReport makeBasicReport() {
    sak::TransferReport report;
    report.transfer_id = QStringLiteral("xfer-001");
    report.source_host = QStringLiteral("SOURCE-PC");
    report.destination_host = QStringLiteral("DEST-PC");
    report.status = QStringLiteral("success");
    report.started_at = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::UTC);
    report.completed_at = QDateTime(QDate(2025, 6, 15), QTime(10, 45, 0), QTimeZone::UTC);
    report.total_bytes = 1'048'576;
    report.total_files = 42;
    return report;
}

// ============================================================================
// Tests
// ============================================================================

void TestNetworkTransferReport::toJson_emptyReport() {
    const sak::TransferReport report;
    const QJsonObject json = report.toJson();

    QVERIFY(json.contains("transfer_id"));
    QVERIFY(json.contains("status"));
    QVERIFY(json.contains("total_bytes"));
    QVERIFY(json.contains("total_files"));
    QCOMPARE(json["total_bytes"].toInteger(), qint64(0));
    QCOMPARE(json["total_files"].toInt(), 0);
}

void TestNetworkTransferReport::toJson_populatedReport() {
    const sak::TransferReport report = makeBasicReport();
    const QJsonObject json = report.toJson();

    QCOMPARE(json["transfer_id"].toString(), QStringLiteral("xfer-001"));
    QCOMPARE(json["source_host"].toString(), QStringLiteral("SOURCE-PC"));
    QCOMPARE(json["destination_host"].toString(), QStringLiteral("DEST-PC"));
    QCOMPARE(json["status"].toString(), QStringLiteral("success"));
    QCOMPARE(json["total_bytes"].toInteger(), qint64(1'048'576));
    QCOMPARE(json["total_files"].toInt(), 42);
}

void TestNetworkTransferReport::toJson_withErrors() {
    sak::TransferReport report = makeBasicReport();
    report.status = QStringLiteral("failed");
    report.errors << QStringLiteral("Disk full") << QStringLiteral("Permission denied");
    report.warnings << QStringLiteral("Slow network");

    const QJsonObject json = report.toJson();

    const QJsonArray errors = json["errors"].toArray();
    QCOMPARE(errors.size(), 2);
    QCOMPARE(errors.at(0).toString(), QStringLiteral("Disk full"));
    QCOMPARE(errors.at(1).toString(), QStringLiteral("Permission denied"));

    const QJsonArray warnings = json["warnings"].toArray();
    QCOMPARE(warnings.size(), 1);
    QCOMPARE(warnings.at(0).toString(), QStringLiteral("Slow network"));
}

void TestNetworkTransferReport::toJson_withManifest() {
    sak::TransferReport report = makeBasicReport();
    report.manifest.protocol_version = QStringLiteral("1.0");
    report.manifest.transfer_id = report.transfer_id;
    report.manifest.source_hostname = report.source_host;
    report.manifest.total_bytes = report.total_bytes;
    report.manifest.total_files = report.total_files;

    const QJsonObject json = report.toJson();

    QVERIFY(json.contains("manifest"));
    const QJsonObject manifest = json["manifest"].toObject();
    QCOMPARE(manifest["protocol_version"].toString(), QStringLiteral("1.0"));
    QCOMPARE(manifest["source_hostname"].toString(), QStringLiteral("SOURCE-PC"));
}

void TestNetworkTransferReport::toJson_roundTripTimestamps() {
    const sak::TransferReport report = makeBasicReport();
    const QJsonObject json = report.toJson();

    const qint64 started_epoch = json["started_at"].toInteger();
    const qint64 completed_epoch = json["completed_at"].toInteger();

    QCOMPARE(started_epoch, report.started_at.toSecsSinceEpoch());
    QCOMPARE(completed_epoch, report.completed_at.toSecsSinceEpoch());
    QVERIFY(completed_epoch > started_epoch);
}

QTEST_MAIN(TestNetworkTransferReport)
#include "test_network_transfer_report.moc"
