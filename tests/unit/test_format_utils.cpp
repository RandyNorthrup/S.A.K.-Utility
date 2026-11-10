// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_format_utils.cpp
/// @brief Unit tests for sak::formatBytes(), the application's one byte style

#include "sak/format_utils.h"

#include <QtTest/QtTest>

#include <limits>

class TestFormatUtils : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void formatBytes_zero();
    void formatBytes_negativeIsShownNotHidden();
    void formatBytes_bytes();
    void formatBytes_kilobytes();
    void formatBytes_megabytes();
    void formatBytes_gigabytes();
    void formatBytes_terabytes();
    void formatBytes_exactBoundaries();
    void formatBytes_unsignedOverload();
    void formatBytes_precisionPerUnit();
    void formatBytes_oneStyleForEverySize();
    void pathsBreakAfterABackslashNotAtAHyphen();
};

void TestFormatUtils::formatBytes_zero() {
    QCOMPARE(sak::formatBytes(qint64(0)), QStringLiteral("0 B"));
    QCOMPARE(sak::formatBytes(uint64_t(0)), QStringLiteral("0 B"));
}

// A negative size is a fault; shown as "0 bytes" it passed for an empty file.
void TestFormatUtils::formatBytes_negativeIsShownNotHidden() {
    QCOMPARE(sak::formatBytes(qint64(-100)), QStringLiteral("-100 B"));
}

// "B", never "bytes": "1 bytes" was the long style's reading of one byte, and "242 bytes" stood
// beside the file list's "242 B" (review 4, Y10).
void TestFormatUtils::formatBytes_bytes() {
    QCOMPARE(sak::formatBytes(qint64(1)), QStringLiteral("1 B"));
    QCOMPARE(sak::formatBytes(qint64(512)), QStringLiteral("512 B"));
    QCOMPARE(sak::formatBytes(qint64(1023)), QStringLiteral("1023 B"));
}

void TestFormatUtils::formatBytes_kilobytes() {
    QCOMPARE(sak::formatBytes(qint64(1024)), QStringLiteral("1.0 KB"));
    QCOMPARE(sak::formatBytes(qint64(1536)), QStringLiteral("1.5 KB"));
}

void TestFormatUtils::formatBytes_megabytes() {
    QCOMPARE(sak::formatBytes(qint64(1'048'576)), QStringLiteral("1.0 MB"));
    QCOMPARE(sak::formatBytes(qint64(524'288'000)), QStringLiteral("500.0 MB"));
}

void TestFormatUtils::formatBytes_gigabytes() {
    QCOMPARE(sak::formatBytes(qint64(1'073'741'824)), QStringLiteral("1.00 GB"));
    QCOMPARE(sak::formatBytes(qint64(2'684'354'560LL)), QStringLiteral("2.50 GB"));
}

void TestFormatUtils::formatBytes_terabytes() {
    QCOMPARE(sak::formatBytes(qint64(1'099'511'627'776LL)), QStringLiteral("1.00 TB"));
    QCOMPARE(sak::formatBytes(qint64(3'298'534'883'328LL)), QStringLiteral("3.00 TB"));
    // The Seagate the partition rigs use, as the disk map names it.
    QCOMPARE(sak::formatBytes(uint64_t(8'001'563'221'504ULL)), QStringLiteral("7.28 TB"));
}

void TestFormatUtils::formatBytes_exactBoundaries() {
    QCOMPARE(sak::formatBytes(qint64(1023)), QStringLiteral("1023 B"));
    QCOMPARE(sak::formatBytes(qint64(1024)), QStringLiteral("1.0 KB"));
    QCOMPARE(sak::formatBytes(qint64(1'048'575)), QStringLiteral("1024.0 KB"));
    QCOMPARE(sak::formatBytes(qint64(1'048'576)), QStringLiteral("1.0 MB"));
    QCOMPARE(sak::formatBytes(qint64(1'073'741'823)), QStringLiteral("1024.0 MB"));
    QCOMPARE(sak::formatBytes(qint64(1'073'741'824)), QStringLiteral("1.00 GB"));
}

void TestFormatUtils::formatBytes_unsignedOverload() {
    QCOMPARE(sak::formatBytes(uint64_t(1'048'576)), QStringLiteral("1.0 MB"));
    // Above INT64_MAX the count is not narrowed to a negative one.
    QVERIFY(sak::formatBytes(std::numeric_limits<uint64_t>::max()).endsWith(QStringLiteral(" TB")));
}

void TestFormatUtils::formatBytes_precisionPerUnit() {
    // KB and MB take one decimal; GB and TB two. The copies this replaced differed on exactly
    // this: one truncated KB with integer division ("1 KB" for 1536 bytes, and the AI page's
    // "(3 KB)"), another printed GB to one decimal ("4.9 GB" in the backup wizard).
    QCOMPARE(sak::formatBytes(qint64(1536)), QStringLiteral("1.5 KB"));
    QCOMPARE(sak::formatBytes(qint64(1024 * 1024 * 3 / 2)), QStringLiteral("1.5 MB"));
    QCOMPARE(sak::formatBytes(qint64(1024) * 1024 * 1024 * 3 / 2), QStringLiteral("1.50 GB"));
}

// The signed and unsigned forms agree everywhere a size can be.
void TestFormatUtils::formatBytes_oneStyleForEverySize() {
    for (const qint64 bytes :
         {qint64(0), qint64(242), qint64(1536), qint64(5'242'880), qint64(5'239'226'163LL)}) {
        QCOMPARE(sak::formatBytes(bytes), sak::formatBytes(static_cast<uint64_t>(bytes)));
    }
}

// Review 4 (Y28): "C:\SAK-Samples\Docs" wrapped as "C:\SAK-" and "Samples\Docs".
void TestFormatUtils::pathsBreakAfterABackslashNotAtAHyphen() {
    const QString path = QStringLiteral("C:\\SAK-Samples\\Docs");
    const QString shown = sak::withPathBreaks(path);
    const QString expected = QStringLiteral("C:\\") + QChar(sak::kPathBreakChance) +
                             QStringLiteral("SAK-") + QChar(sak::kPathNoBreak) +
                             QStringLiteral("Samples\\") + QChar(sak::kPathBreakChance) +
                             QStringLiteral("Docs");
    QCOMPARE(shown, expected);
    QCOMPARE(sak::withoutPathBreaks(shown), path);
}

QTEST_MAIN(TestFormatUtils)
#include "test_format_utils.moc"
