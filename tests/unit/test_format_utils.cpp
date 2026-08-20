// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_format_utils.cpp
/// @brief Unit tests for sak::formatBytes() utility

#include "sak/format_utils.h"

#include <QtTest/QtTest>

class TestFormatUtils : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- formatBytes(qint64) ---------------------------------------------
    void formatBytes_zero();
    void formatBytes_negative();
    void formatBytes_bytes();
    void formatBytes_kilobytes();
    void formatBytes_megabytes();
    void formatBytes_gigabytes();
    void formatBytes_terabytes();
    void formatBytes_exactBoundaries();

    // -- formatBytes(uint64_t) overload ----------------------------------
    void formatBytes_unsignedOverload();
};

// ============================================================================
// formatBytes(qint64) Tests
// ============================================================================

void TestFormatUtils::formatBytes_zero() {
    const QString result = sak::formatBytes(qint64(0));
    QCOMPARE(result, QStringLiteral("0 bytes"));
}

void TestFormatUtils::formatBytes_negative() {
    const QString result = sak::formatBytes(qint64(-100));
    QCOMPARE(result, QStringLiteral("0 bytes"));
}

void TestFormatUtils::formatBytes_bytes() {
    QCOMPARE(sak::formatBytes(qint64(1)), QStringLiteral("1 bytes"));
    QCOMPARE(sak::formatBytes(qint64(512)), QStringLiteral("512 bytes"));
    QCOMPARE(sak::formatBytes(qint64(1023)), QStringLiteral("1023 bytes"));
}

void TestFormatUtils::formatBytes_kilobytes() {
    // 1 KB = 1024 bytes
    const QString result_1kb = sak::formatBytes(qint64(1024));
    QCOMPARE(result_1kb, QStringLiteral("1.0 KB"));

    // 1.5 KB
    const QString result_1_5kb = sak::formatBytes(qint64(1536));
    QCOMPARE(result_1_5kb, QStringLiteral("1.5 KB"));
}

void TestFormatUtils::formatBytes_megabytes() {
    // 1 MB = 1048576 bytes
    const QString result_1mb = sak::formatBytes(qint64(1'048'576));
    QCOMPARE(result_1mb, QStringLiteral("1.0 MB"));

    // 500 MB
    const QString result_500mb = sak::formatBytes(qint64(524'288'000));
    QCOMPARE(result_500mb, QStringLiteral("500.0 MB"));
}

void TestFormatUtils::formatBytes_gigabytes() {
    // 1 GB = 1073741824 bytes
    const QString result_1gb = sak::formatBytes(qint64(1'073'741'824));
    QCOMPARE(result_1gb, QStringLiteral("1.00 GB"));

    // 2.5 GB
    const qint64 two_point_five_gb = 2'684'354'560LL;
    const QString result_2_5gb = sak::formatBytes(two_point_five_gb);
    QCOMPARE(result_2_5gb, QStringLiteral("2.50 GB"));
}

void TestFormatUtils::formatBytes_terabytes() {
    // 1 TB = 1099511627776 bytes
    const qint64 one_tb = 1'099'511'627'776LL;
    const QString result_1tb = sak::formatBytes(one_tb);
    QCOMPARE(result_1tb, QStringLiteral("1.00 TB"));

    // 3 TB
    const qint64 three_tb = 3'298'534'883'328LL;
    const QString result_3tb = sak::formatBytes(three_tb);
    QCOMPARE(result_3tb, QStringLiteral("3.00 TB"));
}

void TestFormatUtils::formatBytes_exactBoundaries() {
    // Just below KB boundary
    QCOMPARE(sak::formatBytes(qint64(1023)), QStringLiteral("1023 bytes"));

    // Exactly KB boundary
    QCOMPARE(sak::formatBytes(qint64(1024)), QStringLiteral("1.0 KB"));

    // Just below MB boundary
    QCOMPARE(sak::formatBytes(qint64(1'048'575)), QStringLiteral("1024.0 KB"));

    // Exactly MB boundary
    QCOMPARE(sak::formatBytes(qint64(1'048'576)), QStringLiteral("1.0 MB"));

    // Just below GB boundary
    QCOMPARE(sak::formatBytes(qint64(1'073'741'823)), QStringLiteral("1024.0 MB"));

    // Exactly GB boundary
    QCOMPARE(sak::formatBytes(qint64(1'073'741'824)), QStringLiteral("1.00 GB"));
}

// ============================================================================
// formatBytes(uint64_t) Overload
// ============================================================================

void TestFormatUtils::formatBytes_unsignedOverload() {
    // Should delegate to qint64 version
    const uint64_t value = 1'048'576;
    const QString result = sak::formatBytes(value);
    QCOMPARE(result, QStringLiteral("1.0 MB"));
}

QTEST_MAIN(TestFormatUtils)
#include "test_format_utils.moc"
