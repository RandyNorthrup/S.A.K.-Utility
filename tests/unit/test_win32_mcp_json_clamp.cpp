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
    // The "not a number" fallback is QJsonValue::toDouble(def)
    // (src/win32mcp/win32_mcp_json_clamp.cpp:15), which defaults for ALL non-Double types -- not
    // just strings. A guard that only recognises strings turns a hostile `"timeout_ms": true`
    // into 0, clamped to the FLOOR: a 200 ms wait instead of the caller's 10 s default
    // (src/win32mcp/win32_mcp_watch.cpp:260).
    const QString key = QStringLiteral("timeout_ms");
    QCOMPARE(clampMs(with(key, QStringLiteral("soon")), key, 500, 100, 1000), qint64(500));
    QCOMPARE(clampMs(with(key, true), key, 500, 100, 1000), qint64(500));
    QCOMPARE(clampMs(with(key, false), key, 500, 100, 1000), qint64(500));
    QCOMPARE(clampMs(with(key, QJsonValue(QJsonValue::Null)), key, 500, 100, 1000), qint64(500));
    QCOMPARE(clampMs(with(key, QJsonObject{}), key, 500, 100, 1000), qint64(500));
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
    // Real callers read timeout_ms, idle_ms and poll_ms out of the SAME args object
    // (src/win32mcp/win32_mcp_watch.cpp:284-286), so the lookup must select args[key] and nothing
    // else. Probing with a single-key object cannot tell args.value(key) apart from "take the
    // object's only entry".
    QJsonObject both = with(QStringLiteral("timeout_ms"), 3000);
    both.insert(QStringLiteral("poll_ms"), 250);
    QCOMPARE(clampMs(both, QStringLiteral("timeout_ms"), 500, 100, 7'200'000), qint64(3000));
    QCOMPARE(clampMs(both, QStringLiteral("poll_ms"), 500, 100, 7'200'000), qint64(250));
    // A sibling key that IS present must not stand in for the absent requested key.
    QCOMPARE(clampMs(both, QStringLiteral("idle_ms"), 600, 150, 30'000), qint64(600));
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
    // Symmetric underflow: a double BELOW INT64_MIN must pin to the floor, not to the cap. The
    // std::max half of the clamp (src/win32mcp/win32_mcp_json_clamp.cpp:16) is what enforces this;
    // a naive "doesn't fit in qint64 -> return hi" overflow guard passes both 1e19 cases above and
    // still turns a hostile -1e19 into a 2-hour wait / 10 s poll interval.
    QCOMPARE(clampMs(with(QStringLiteral("timeout_ms"), -1e19),
                     QStringLiteral("timeout_ms"),
                     10'000,
                     200,
                     7'200'000),
             qint64(200));
    QCOMPARE(
        clampMs(
            with(QStringLiteral("poll_ms"), -1e19), QStringLiteral("poll_ms"), 500, 150, 10'000),
        qint64(150));
}

QTEST_GUILESS_MAIN(Win32McpJsonClampTests)
#include "test_win32_mcp_json_clamp.moc"
