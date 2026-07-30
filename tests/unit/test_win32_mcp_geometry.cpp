// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_geometry.h"

#include <QtTest/QtTest>

using sak::win32mcp::AbsBox;
using sak::win32mcp::inverseScale;
using sak::win32mcp::mapWordBox;
using sak::win32mcp::Scaled;
using sak::win32mcp::scaledSize;
using sak::win32mcp::validateCaptureRect;
using sak::win32mcp::WordRectF;

class Win32McpGeometryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scaledSizeNoOpWhenSourceFits();
    void scaledSizeDownscalesPreservingAspect();
    void scaledSizeFloorsThinEdgeToOne();
    void validateRejectsEmptyAndOversize();
    void inverseScaleGuardsNonPositive();
    void mapWordBoxIdentity();
    void mapWordBoxDownscaleAndOrigin();
    void mapWordBoxNegativeOrigin();
};

void Win32McpGeometryTests::scaledSizeNoOpWhenSourceFits() {
    const Scaled a = scaledSize(800, 600, 1000);  // already within max_edge
    QCOMPARE(a.w, 800);
    QCOMPARE(a.h, 600);
    QCOMPARE(a.scale, 1.0);
    const Scaled b = scaledSize(800, 600, 0);  // max_edge <= 0 -> no-op
    QCOMPARE(b.scale, 1.0);
    QCOMPARE(b.w, 800);
}

void Win32McpGeometryTests::scaledSizeDownscalesPreservingAspect() {
    const Scaled s = scaledSize(4000, 2000, 1000);
    QCOMPARE(s.w, 1000);
    QCOMPARE(s.h, 500);
    QCOMPARE(s.scale, 0.25);
}

void Win32McpGeometryTests::scaledSizeFloorsThinEdgeToOne() {
    // 200x1 capped to 50: scale 0.25 -> height int(0.25) == 0 must floor to 1.
    const Scaled s = scaledSize(200, 1, 50);
    QCOMPARE(s.w, 50);
    QCOMPARE(s.h, 1);
}

void Win32McpGeometryTests::validateRejectsEmptyAndOversize() {
    QVERIFY(!validateCaptureRect(0, 100).isEmpty());       // empty
    QVERIFY(!validateCaptureRect(100, -1).isEmpty());      // negative
    QVERIFY(!validateCaptureRect(16'385, 100).isEmpty());  // over the edge cap
    QVERIFY(validateCaptureRect(1920, 1080).isEmpty());    // ok
}

void Win32McpGeometryTests::inverseScaleGuardsNonPositive() {
    QCOMPARE(inverseScale(1.0), 1.0);
    QCOMPARE(inverseScale(0.5), 2.0);
    QCOMPARE(inverseScale(0.0), 1.0);   // no scale -> identity
    QCOMPARE(inverseScale(-0.5), 1.0);  // never divide by a non-positive scale
}

void Win32McpGeometryTests::mapWordBoxIdentity() {
    // inv 1.0, origin 0: the box is the rounded rect.
    const AbsBox b = mapWordBox(WordRectF{10.4, 20.6, 30.5, 40.5}, 0, 0, 1.0);
    QCOMPARE(b.x, 10);
    QCOMPARE(b.y, 21);
    QCOMPARE(b.w, 31);  // 30.5 rounds to 31 (llround: half away from zero)
    QCOMPARE(b.h, 41);
}

void Win32McpGeometryTests::mapWordBoxDownscaleAndOrigin() {
    // Captured at half scale (inv 2), window at origin (500, 300): a word at capture (100, 50)
    // sized 40x10 maps to (500+200, 300+100) = (700, 400) sized 80x20.
    const AbsBox b = mapWordBox(WordRectF{100.0, 50.0, 40.0, 10.0}, 500, 300, 2.0);
    QCOMPARE(b.x, 700);
    QCOMPARE(b.y, 400);
    QCOMPARE(b.w, 80);
    QCOMPARE(b.h, 20);
}

void Win32McpGeometryTests::mapWordBoxNegativeOrigin() {
    // Secondary monitor to the left/above: origin can be negative.
    const AbsBox b = mapWordBox(WordRectF{10.0, 10.0, 5.0, 5.0}, -1920, -100, 1.0);
    QCOMPARE(b.x, -1910);
    QCOMPARE(b.y, -90);
}

QTEST_GUILESS_MAIN(Win32McpGeometryTests)
#include "test_win32_mcp_geometry.moc"
