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
    const AbsPoint far_corner = toAbsCoord(65'535, 65'535, big);
    QCOMPARE(far_corner.nx, 65'535L);
    QCOMPARE(far_corner.ny, 65'535L);  // the y divisor is (h - 1), not h
    // Half-way pixel of a 3px span: 65535 / 2 == 32767.5, and the rounding is half-away-from-
    // zero, so a truncating cast would land one short here.
    const AbsPoint half = toAbsCoord(1, 1, ScreenBox{0, 0, 3, 3});
    QCOMPARE(half.nx, 32'768L);
    QCOMPARE(half.ny, 32'768L);
}

void Win32McpInputPlanTests::absCoordHonorsNegativeOrigin() {
    // Secondary monitor to the left: leftmost pixel -> 0, rightmost -> 65535.
    const ScreenBox left{-100, 0, 101, 1};
    QCOMPARE(toAbsCoord(-100, 0, left).nx, 0L);
    QCOMPARE(toAbsCoord(0, 0, left).nx, 65'535L);

    // Transposed case: a secondary monitor ABOVE the primary, and NOT square. Every other fixture
    // in this file whose .ny is read sits at y == 0 and is square, so the y arm's origin term and
    // its (h - 1) divisor would otherwise be unpinned -- a y arm that dropped `- screen.y`, or that
    // copy-pasted `screen.w - 1` as its divisor, passes every other assertion here.
    const ScreenBox above{0, -100, 1, 101};
    QCOMPARE(toAbsCoord(0, -100, above).ny, 0L);    // topmost pixel; without `- screen.y`: -65535
    QCOMPARE(toAbsCoord(0, 0, above).ny, 65'535L);  // bottom pixel; with a (w - 1) divisor: 6553500
}

void Win32McpInputPlanTests::absCoordDegenerateScreenDoesNotDivideByZero() {
    // width 1 -> divisor floored at 1, no division by zero, maps to 0.
    const AbsPoint at_origin = toAbsCoord(0, 0, ScreenBox{0, 0, 1, 1});
    QCOMPARE(at_origin.nx, 0L);
    QCOMPARE(at_origin.ny, 0L);
    // ...but pixel 0 maps to 0 under ANY divisor, so that says nothing about the floor this
    // test is named for. One pixel past a 1x1 screen scales by the full span instead of
    // dividing by zero -- on BOTH axes.
    const AbsPoint past = toAbsCoord(1, 1, ScreenBox{0, 0, 1, 1});
    QCOMPARE(past.nx, 65'535L);
    QCOMPARE(past.ny, 65'535L);
}

void Win32McpInputPlanTests::pointInVirtualScreenBounds() {
    const ScreenBox s{0, 0, 100, 100};
    QVERIFY(pointInVirtualScreen(10, 10, s));
    QVERIFY(pointInVirtualScreen(99, 99, s));                         // last inside pixel
    QVERIFY(pointInVirtualScreen(0, 0, s));                           // top-left inclusive
    QVERIFY(!pointInVirtualScreen(100, 50, s));                       // right edge exclusive
    QVERIFY(!pointInVirtualScreen(50, 100, s));                       // bottom edge exclusive
    QVERIFY(!pointInVirtualScreen(-1, 10, s));                        // left of origin
    QVERIFY(!pointInVirtualScreen(10, 10, ScreenBox{0, 0, 0, 100}));  // no width -> nothing inside
    QVERIFY(pointInVirtualScreen(-50, -50, ScreenBox{-100, -100, 100, 100}));  // negative origin
    QVERIFY(!pointInVirtualScreen(
        10, -1, s));  // above the top edge -- the ONLY arm no other row refuses through
    QVERIFY(!pointInVirtualScreen(
        -50, -101, ScreenBox{-100, -100, 100, 100}));  // top edge follows a negative origin
    QVERIFY(!pointInVirtualScreen(
        10, 10, ScreenBox{0, 0, 100, 0}));  // no height -> nothing inside (twin of the width row)
}

void Win32McpInputPlanTests::buttonFlagsMapAndDefault() {
    unsigned long down = 0;
    unsigned long up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral("left"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_LEFTUP));
    // Production writes a PAIR of out-params, and only `down` was checked past the first case:
    // a row whose UP flag came from the wrong button would press one button and release another.
    down = 0;
    up = 0;
    QVERIFY(mouseButtonFlags(QString(), down, up));  // empty -> left
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_LEFTUP));
    down = 0;
    up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral("right"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_RIGHTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_RIGHTUP));
    down = 0;
    up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral("middle"), down, up));
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_MIDDLEDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_MIDDLEUP));
}

void Win32McpInputPlanTests::buttonFlagsTrimAndReject() {
    unsigned long down = 0;
    unsigned long up = 0;
    QVERIFY(mouseButtonFlags(QStringLiteral(" Right "), down, up));  // trimmed + case-folded
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_RIGHTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_RIGHTUP));
    QVERIFY(mouseButtonFlags(QStringLiteral("   "), down, up));  // whitespace-only -> left default
    QCOMPARE(down, static_cast<unsigned long>(MOUSEEVENTF_LEFTDOWN));
    QCOMPARE(up, static_cast<unsigned long>(MOUSEEVENTF_LEFTUP));
    // A refusal must leave the caller's flags UNTOUCHED, so a caller that ignores the return
    // value cannot fire an unintended button. The refusal was checked as a bare false, never
    // for that effect: seed sentinels and require them to survive.
    down = 0xDEADul;
    up = 0xBEEFul;
    QVERIFY(!mouseButtonFlags(QStringLiteral("scroll"), down, up));  // unknown -> refuse
    QCOMPARE(down, 0xDEADul);
    QCOMPARE(up, 0xBEEFul);

    // "scroll" shares no prefix with any accepted name, so refusing it cannot tell the exact
    // `==` at win32_mcp_input_plan.cpp:49,54,59 from a startsWith/contains/endsWith match. A name
    // that EXTENDS an accepted one must refuse too, or "leftclick" fires a real left button.
    down = 0xDEADul;
    up = 0xBEEFul;
    QVERIFY(!mouseButtonFlags(
        QStringLiteral("leftclick"), down, up));  // prefix of "left" is not "left"
    QCOMPARE(down, 0xDEADul);
    QCOMPARE(up, 0xBEEFul);
    QVERIFY(!mouseButtonFlags(
        QStringLiteral("double right"), down, up));  // inner/trailing match refused
    QCOMPARE(down, 0xDEADul);
    QCOMPARE(up, 0xBEEFul);
    QVERIFY(!mouseButtonFlags(QStringLiteral("middle click"), down, up));
    QCOMPARE(down, 0xDEADul);
    QCOMPARE(up, 0xBEEFul);
}

void Win32McpInputPlanTests::typePlanAsciiAndNewline() {
    // Whole ordered catalogs. a[1] and nl[1] were probed only through .key_up, so their .code
    // and .is_vk were asserted nowhere -- a release stroke carrying the wrong key or the wrong
    // vk flag leaves the modifier stuck down on the real desktop.
    const QVector<KeyStroke> a = planTypeText(QStringLiteral("a"));
    const unsigned short a_unit = static_cast<unsigned short>('a');
    QCOMPARE(a,
             (QVector<KeyStroke>{KeyStroke{.code = a_unit, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = a_unit, .is_vk = false, .key_up = true}}));

    const QVector<KeyStroke> nl = planTypeText(QStringLiteral("\n"));
    const unsigned short enter = static_cast<unsigned short>(VK_RETURN);
    QCOMPARE(nl,
             (QVector<KeyStroke>{KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = true}}));
}

void Win32McpInputPlanTests::typePlanCollapsesCrlf() {
    // The old first line compared planTypeText against planTypeText -- the same production
    // symbol on both sides, so any implementation satisfied it. Pin the literal plan instead.
    const unsigned short enter = static_cast<unsigned short>(VK_RETURN);
    const QVector<KeyStroke> enter_pair{KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                        KeyStroke{.code = enter, .is_vk = true, .key_up = true}};
    // CRLF is a single Enter; the CR is dropped entirely.
    QCOMPARE(planTypeText(QStringLiteral("\r\n")), enter_pair);
    // A lone CR is itself a line break and must NOT be swallowed the way the CR of a CRLF is.
    QCOMPARE(planTypeText(QStringLiteral("\r")), enter_pair);
    // "a\r\nb" -> 'a' pair + Enter pair + 'b' pair, in that order and with no stray CR stroke;
    // the size alone could not see a CR emitted in place of one of them.
    const unsigned short a_unit = static_cast<unsigned short>('a');
    const unsigned short b_unit = static_cast<unsigned short>('b');
    QCOMPARE(planTypeText(QStringLiteral("a\r\nb")),
             (QVector<KeyStroke>{KeyStroke{.code = a_unit, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = a_unit, .is_vk = false, .key_up = true},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = true},
                                 KeyStroke{.code = b_unit, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = b_unit, .is_vk = false, .key_up = true}}));

    // Every row above decides the CR by the FIRST arm of the CRLF lookahead alone
    // (win32_mcp_input_plan.cpp:76): "\r" ends the string so `i + 1 < count` is already false, and
    // both "\r\n" rows put a real LF next so the second arm is true whenever the first is. Present
    // a CR that is IN RANGE but NOT followed by LF -- the only input the second arm exists to let
    // through. Classic-Mac text pasted by a caller must keep its line break.
    QCOMPARE(planTypeText(QStringLiteral("\ra")),
             (QVector<KeyStroke>{KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = true},
                                 KeyStroke{.code = a_unit, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = a_unit, .is_vk = false, .key_up = true}}));
    // Two CRs in a row: only the SECOND one belongs to the CRLF, so this is TWO line breaks. A
    // lookahead that drops any non-final CR would collapse them into one.
    QCOMPARE(planTypeText(QStringLiteral("\r\r\n")),
             (QVector<KeyStroke>{KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = true},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = false},
                                 KeyStroke{.code = enter, .is_vk = true, .key_up = true}}));
}

void Win32McpInputPlanTests::typePlanEmitsSurrogatesForAstralChar() {
    // U+1F600 is two UTF-16 code units; each surrogate emits its own unicode down/up pair.
    const char32_t cp = 0x1F600;
    const QString emoji = QString::fromUcs4(&cp, 1);
    QCOMPARE(emoji.size(), 2);  // surrogate pair
    const QVector<KeyStroke> strokes = planTypeText(emoji);
    // The two RELEASE strokes were never read at all. U+1F600 encodes as the surrogate pair
    // D83D DE00; each unit emits its own down+up pair, and the literals here are independent of
    // the QString the test built.
    const unsigned short high = 0xD83D;
    const unsigned short low = 0xDE00;
    QCOMPARE(strokes,
             (QVector<KeyStroke>{KeyStroke{.code = high, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = high, .is_vk = false, .key_up = true},
                                 KeyStroke{.code = low, .is_vk = false, .key_up = false},
                                 KeyStroke{.code = low, .is_vk = false, .key_up = true}}));
}

QTEST_GUILESS_MAIN(Win32McpInputPlanTests)
#include "test_win32_mcp_input_plan.moc"
