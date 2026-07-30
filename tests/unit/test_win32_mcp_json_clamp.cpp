// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_json_clamp.h"

#include <QJsonObject>
#include <QtTest/QtTest>

using sak::win32mcp::clampMs;

namespace {

QJsonObject with(const QString& key, const QJsonValue& value) {
    return QJsonObject{{key, value}};
}

}  // namespace

class Win32McpJsonClampTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void missingKeyReturnsDefault();
    void nonNumericReturnsDefault();
    void belowFloorClampsToLo();
    void aboveCapClampsToHi();
    void inRangePassesThrough();
    void fractionalTruncates();
    void hugeValueClampsToCapNotFloor();
};

void Win32McpJsonClampTests::missingKeyReturnsDefault() {
    QCOMPARE(clampMs(QJsonObject{}, QStringLiteral("timeout_ms"), 500, 100, 1000), qint64(500));
}

void Win32McpJsonClampTests::nonNumericReturnsDefault() {
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), QStringLiteral("soon")),
                     QStringLiteral("timeout_ms"),
                     500,
                     100,
                     1000),
             qint64(500));
}

void Win32McpJsonClampTests::belowFloorClampsToLo() {
    QCOMPARE(
        clampMs(with(QStringLiteral("poll_ms"), 20), QStringLiteral("poll_ms"), 200, 100, 1000),
        qint64(100));
    QCOMPARE(
        clampMs(with(QStringLiteral("poll_ms"), -5), QStringLiteral("poll_ms"), 200, 100, 1000),
        qint64(100));
}

void Win32McpJsonClampTests::aboveCapClampsToHi() {
    // 99'999 is within [100, 7'200'000] -> passes through unchanged.
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), 99'999),
                     QStringLiteral("timeout_ms"),
                     500,
                     100,
                     7'200'000),
             qint64(99'999));
    // 9'000'000 exceeds the 2-hour cap -> pinned to hi.
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), 9'000'000),
                     QStringLiteral("timeout_ms"),
                     500,
                     100,
                     7'200'000),
             qint64(7'200'000));
}

void Win32McpJsonClampTests::inRangePassesThrough() {
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), 3000),
                     QStringLiteral("timeout_ms"),
                     500,
                     100,
                     7'200'000),
             qint64(3000));
}

void Win32McpJsonClampTests::fractionalTruncates() {
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), 300.9),
                     QStringLiteral("timeout_ms"),
                     500,
                     100,
                     1000),
             qint64(300));
}

void Win32McpJsonClampTests::hugeValueClampsToCapNotFloor() {
    // Regression: a JSON number above INT64_MAX must pin to the cap, not overflow the narrowing
    // cast to INT64_MIN and collapse to the floor. 1e19 > INT64_MAX (~9.22e18).
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), 1e19),
                     QStringLiteral("timeout_ms"),
                     10'000,
                     200,
                     7'200'000),
             qint64(7'200'000));
    QCOMPARE(
        clampMs(with(QStringLiteral("poll_ms"), 1e19), QStringLiteral("poll_ms"), 500, 150, 10'000),
        qint64(10'000));
}

QTEST_GUILESS_MAIN(Win32McpJsonClampTests)
#include "test_win32_mcp_json_clamp.moc"
