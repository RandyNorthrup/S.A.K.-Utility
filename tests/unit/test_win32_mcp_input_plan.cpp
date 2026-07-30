// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_input_plan.h"

#include <QtTest/QtTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using sak::win32mcp::AbsPoint;
using sak::win32mcp::KeyStroke;
using sak::win32mcp::mouseButtonFlags;
using sak::win32mcp::planTypeText;
using sak::win32mcp::pointInVirtualScreen;
using sak::win32mcp::ScreenBox;
using sak::win32mcp::toAbsCoord;

class Win32McpInputPlanTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void absCoordMapsEndpointsAndOrigin();
    void absCoordHonorsNegativeOrigin();
    void absCoordDegenerateScreenDoesNotDivideByZero();
    void pointInVirtualScreenBounds();
    void buttonFlagsMapAndDefault();
    void buttonFlagsTrimAndReject();
    void typePlanAsciiAndNewline();
    void typePlanCollapsesCrlf();
    void typePlanEmitsSurrogatesForAstralChar();
};

void Win32McpInputPlanTests::absCoordMapsEndpointsAndOrigin() {
    // A 65536-wide/high screen at origin makes the normalized coordinate equal the pixel.
    const ScreenBox big{0, 0, 65'536, 65'536};
    QCOMPARE(toAbsCoord(0, 0, big).nx, 0L);
    const AbsPoint mid = toAbsCoord(100, 200, big);
    QCOMPARE(mid.nx, 100L);
    QCOMPARE(mid.ny, 200L);
    QCOMPARE(toAbsCoord(65'535, 65'535, big).nx, 65'535L);
}

void Win32McpInputPlanTests::absCoordHonorsNegativeOrigin() {
    // Secondary monitor to the left: leftmost pixel -> 0, rightmost -> 65535.
    const ScreenBox left{-100, 0, 101, 1};
    QCOMPARE(toAbsCoord(-100, 0, left).nx, 0L);
    QCOMPARE(toAbsCoord(0, 0, left).nx, 65'535L);
}

void Win32McpInputPlanTests::absCoordDegenerateScreenDoesNotDivideByZero() {
    // width 1 -> divisor floored at 1, no division by zero, maps to 0.
    QCOMPARE(toAbsCoord(0, 0, ScreenBox{0, 0, 1, 1}).nx, 0L);
}

void Win32McpInputPlanTests::pointInVirtualScreenBounds() {
    const ScreenBox s{0, 0, 100, 100};
    QVERIFY(pointInVirtualScreen(10, 10, s));
    QVERIFY(pointInVirtualScreen(0, 0, s));                           // top-left inclusive
    QVERIFY(!pointInVirtualScreen(100, 50, s));                       // right edge exclusive
    QVERIFY(!pointInVirtualScreen(50, 100, s));                       // bottom edge exclusive
    QVERIFY(!pointInVirtualScreen(-1, 10, s));                        // left of origin
    QVERIFY(!pointInVirtualScreen(10, 10, ScreenBox{0, 0, 0, 100}));  // no width -> nothing inside
    QVERIFY(pointInVirtualScreen(-50, -50, ScreenBox{-100, -100, 100, 100}));  // negative origin
}

void Win32McpInputPlanTests::buttonFlagsMapAndDefault() {
    unsigned long down = 0;
    unsigned long up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral("left"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_LEFTUP));
    QVERIFY(mouseButtonFlags(QString(), down, up));  // empty -> left
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QVERIFY(mouseButtonFlags(QStringLiteral("right"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_RIGHTDOWN));
    QVERIFY(mouseButtonFlags(QStringLiteral("middle"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_MIDDLEDOWN));
}

void Win32McpInputPlanTests::buttonFlagsTrimAndReject() {
    unsigned long down = 0;
    unsigned long up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral(" Right "), down, up));  // trimmed + case-folded
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_RIGHTDOWN));
    QVERIFY(mouseButtonFlags(QStringLiteral("   "), down, up));  // whitespace-only -> left default
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QVERIFY(!mouseButtonFlags(QStringLiteral("scroll"), down, up));  // unknown -> refuse
}

void Win32McpInputPlanTests::typePlanAsciiAndNewline() {
    const QVector<KeyStroke> a = planTypeText(QStringLiteral("a"));
    QCOMPARE(a.size(), 2);
    QCOMPARE(a[0].code, static_cast<unsigned short>('a'));
    QVERIFY(!a[0].is_vk);
    QVERIFY(!a[0].key_up);
    QVERIFY(a[1].key_up);

    const QVector<KeyStroke> nl = planTypeText(QStringLiteral("\n"));
    QCOMPARE(nl.size(), 2);
    QVERIFY(nl[0].is_vk);
    QCOMPARE(nl[0].code, static_cast<unsigned short>(VK_RETURN));
    QVERIFY(!nl[0].key_up);
    QVERIFY(nl[1].key_up);
}

void Win32McpInputPlanTests::typePlanCollapsesCrlf() {
    // CRLF is a single Enter; the CR is dropped entirely.
    QCOMPARE(planTypeText(QStringLiteral("\r\n")), planTypeText(QStringLiteral("\n")));
    // "a\r\nb" -> 'a' pair + Enter pair + 'b' pair = 6 strokes (no stray CR keystroke).
    QCOMPARE(planTypeText(QStringLiteral("a\r\nb")).size(), 6);
}

void Win32McpInputPlanTests::typePlanEmitsSurrogatesForAstralChar() {
    // U+1F600 is two UTF-16 code units; each surrogate emits its own unicode down/up pair.
    const char32_t cp = 0x1F600;
    const QString emoji = QString::fromUcs4(&cp, 1);
    QCOMPARE(emoji.size(), 2);    // surrogate pair
    const QVector<KeyStroke> strokes = planTypeText(emoji);
    QCOMPARE(strokes.size(), 4);  // two units, each down+up
    QVERIFY(!strokes[0].is_vk);
    QCOMPARE(strokes[0].code, emoji.at(0).unicode());
    QCOMPARE(strokes[2].code, emoji.at(1).unicode());
}

QTEST_GUILESS_MAIN(Win32McpInputPlanTests)
#include "test_win32_mcp_input_plan.moc"
