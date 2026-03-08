// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_format_utils.cpp
/// @brief Unit tests for sak::formatBytes() utility

#include <QtTest/QtTest>

#include "sak/format_utils.h"

class TestFormatUtils : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── formatBytes(qint64) ─────────────────────────────────────────────
    void formatBytes_zero();
    void formatBytes_negative();
    void formatBytes_bytes();
    void formatBytes_kilobytes();
    void formatBytes_megabytes();
    void formatBytes_gigabytes();
    void formatBytes_terabytes();
    void formatBytes_exactBoundaries();

    // ── formatBytes(uint64_t) overload ──────────────────────────────────
    void formatBytes_unsignedOverload();
};

// ============================================================================
// formatBytes(qint64) Tests
// ============================================================================

void TestFormatUtils::formatBytes_zero()
{
    const QString result = sak::formatBytes(qint64(0));
    QCOMPARE(result, QStringLiteral("0 bytes"));
}

void TestFormatUtils::formatBytes_negative()
{
    const QString result = sak::formatBytes(qint64(-100));
    QCOMPARE(result, QStringLiteral("0 bytes"));
}

void TestFormatUtils::formatBytes_bytes()
{
    QCOMPARE(sak::formatBytes(qint64(1)), QStringLiteral("1 bytes"));
    QCOMPARE(sak::formatBytes(qint64(512)), QStringLiteral("512 bytes"));
    QCOMPARE(sak::formatBytes(qint64(1023)), QStringLiteral("1023 bytes"));
}

void TestFormatUtils::formatBytes_kilobytes()
{
    // 1 KB = 1024 bytes
    const QString result_1kb = sak::formatBytes(qint64(1024));
    QVERIFY(result_1kb.contains("KB"));
    QVERIFY(result_1kb.startsWith("1.0"));

    // 1.5 KB
    const QString result_1_5kb = sak::formatBytes(qint64(1536));
    QVERIFY(result_1_5kb.contains("KB"));
    QVERIFY(result_1_5kb.startsWith("1.5"));
}

void TestFormatUtils::formatBytes_megabytes()
{
    // 1 MB = 1048576 bytes
    const QString result_1mb = sak::formatBytes(qint64(1048576));
    QVERIFY(result_1mb.contains("MB"));
    QVERIFY(result_1mb.startsWith("1.0"));

    // 500 MB
    const QString result_500mb = sak::formatBytes(qint64(524288000));
    QVERIFY(result_500mb.contains("MB"));
    QVERIFY(result_500mb.startsWith("500.0"));
}

void TestFormatUtils::formatBytes_gigabytes()
{
    // 1 GB = 1073741824 bytes
    const QString result_1gb = sak::formatBytes(qint64(1073741824));
    QVERIFY(result_1gb.contains("GB"));
    QVERIFY(result_1gb.startsWith("1.00"));

    // 2.5 GB
    const qint64 two_point_five_gb = 2684354560LL;
    const QString result_2_5gb = sak::formatBytes(two_point_five_gb);
    QVERIFY(result_2_5gb.contains("GB"));
    QVERIFY(result_2_5gb.startsWith("2.50"));
}

void TestFormatUtils::formatBytes_terabytes()
{
    // 1 TB = 1099511627776 bytes
    const qint64 one_tb = 1099511627776LL;
    const QString result_1tb = sak::formatBytes(one_tb);
    QVERIFY(result_1tb.contains("TB"));
    QVERIFY(result_1tb.startsWith("1.00"));

    // 3 TB
    const qint64 three_tb = 3298534883328LL;
    const QString result_3tb = sak::formatBytes(three_tb);
    QVERIFY(result_3tb.contains("TB"));
    QVERIFY(result_3tb.startsWith("3.00"));
}

void TestFormatUtils::formatBytes_exactBoundaries()
{
    // Just below KB boundary
    QVERIFY(sak::formatBytes(qint64(1023)).contains("bytes"));

    // Exactly KB boundary
    QVERIFY(sak::formatBytes(qint64(1024)).contains("KB"));

    // Just below MB boundary
    QVERIFY(sak::formatBytes(qint64(1048575)).contains("KB"));

    // Exactly MB boundary
    QVERIFY(sak::formatBytes(qint64(1048576)).contains("MB"));

    // Just below GB boundary
    QVERIFY(sak::formatBytes(qint64(1073741823)).contains("MB"));

    // Exactly GB boundary
    QVERIFY(sak::formatBytes(qint64(1073741824)).contains("GB"));
}

// ============================================================================
// formatBytes(uint64_t) Overload
// ============================================================================

void TestFormatUtils::formatBytes_unsignedOverload()
{
    // Should delegate to qint64 version
    const uint64_t value = 1048576;
    const QString result = sak::formatBytes(value);
    QVERIFY(result.contains("MB"));
    QVERIFY(result.startsWith("1.0"));
}

QTEST_MAIN(TestFormatUtils)
#include "test_format_utils.moc"
