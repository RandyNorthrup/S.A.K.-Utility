// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_key_chord.h"

#include <QtTest/QtTest>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using sak::win32mcp::parseKeyChord;

namespace {

struct Parsed {
    bool ok;
    QVector<unsigned short> modifiers;
    unsigned short main_key;
};

Parsed parse(const QString& chord) {
    Parsed p{false, {}, 0};
    p.ok = parseKeyChord(chord, p.modifiers, p.main_key);
    return p;
}

}  // namespace

class Win32McpKeyChordTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void singleLetterMapsToUppercaseVk();
    void digitMapsToItsVk();
    void namedKeysMap();
    void modifierChordParsesInOrder();
    void allModifierAliasesRecognized();
    void caseInsensitive();
    void unknownMainKeyRefused();
    void unknownModifierRefused();
    void emptyOrSeparatorOnlyRefused();
    void malformedSeparatorRefused();
};

void Win32McpKeyChordTests::singleLetterMapsToUppercaseVk() {
    const Parsed p = parse(QStringLiteral("s"));
    QVERIFY(p.ok);
    QVERIFY(p.modifiers.isEmpty());
    QCOMPARE(p.main_key, static_cast<unsigned short>('S'));  // 0x53, case-folded to upper
}

void Win32McpKeyChordTests::digitMapsToItsVk() {
    const Parsed p = parse(QStringLiteral("Ctrl+1"));
    QVERIFY(p.ok);
    QCOMPARE(p.modifiers, QVector<unsigned short>{VK_CONTROL});
    QCOMPARE(p.main_key, static_cast<unsigned short>('1'));  // 0x31
}

void Win32McpKeyChordTests::namedKeysMap() {
    QCOMPARE(parse(QStringLiteral("enter")).main_key, static_cast<unsigned short>(VK_RETURN));
    QCOMPARE(parse(QStringLiteral("esc")).main_key, static_cast<unsigned short>(VK_ESCAPE));
    QCOMPARE(parse(QStringLiteral("F5")).main_key, static_cast<unsigned short>(VK_F5));
    QCOMPARE(parse(QStringLiteral("pagedown")).main_key, static_cast<unsigned short>(VK_NEXT));
    QCOMPARE(parse(QStringLiteral("delete")).main_key, static_cast<unsigned short>(VK_DELETE));
}

void Win32McpKeyChordTests::modifierChordParsesInOrder() {
    const Parsed p = parse(QStringLiteral("Ctrl+Alt+Delete"));
    QVERIFY(p.ok);
    const QVector<unsigned short> expected{static_cast<unsigned short>(VK_CONTROL),
                                           static_cast<unsigned short>(VK_MENU)};
    QCOMPARE(p.modifiers, expected);  // order preserved as written
    QCOMPARE(p.main_key, static_cast<unsigned short>(VK_DELETE));
}

void Win32McpKeyChordTests::allModifierAliasesRecognized() {
    QCOMPARE(parse(QStringLiteral("control+a")).modifiers, QVector<unsigned short>{VK_CONTROL});
    QCOMPARE(parse(QStringLiteral("alt+a")).modifiers, QVector<unsigned short>{VK_MENU});
    QCOMPARE(parse(QStringLiteral("shift+a")).modifiers, QVector<unsigned short>{VK_SHIFT});
    QCOMPARE(parse(QStringLiteral("win+d")).modifiers, QVector<unsigned short>{VK_LWIN});
    QCOMPARE(parse(QStringLiteral("meta+d")).modifiers, QVector<unsigned short>{VK_LWIN});
}

void Win32McpKeyChordTests::caseInsensitive() {
    const Parsed p = parse(QStringLiteral("CTRL+SHIFT+a"));
    QVERIFY(p.ok);
    const QVector<unsigned short> expected{static_cast<unsigned short>(VK_CONTROL),
                                           static_cast<unsigned short>(VK_SHIFT)};
    QCOMPARE(p.modifiers, expected);
    QCOMPARE(p.main_key, static_cast<unsigned short>('A'));
}

void Win32McpKeyChordTests::unknownMainKeyRefused() {
    // SAFETY: an unknown main key must be refused, never silently dropped or mis-injected.
    QVERIFY(!parse(QStringLiteral("Ctrl+bogus")).ok);
    QVERIFY(!parse(QStringLiteral("nonsense")).ok);
}

void Win32McpKeyChordTests::unknownModifierRefused() {
    // SAFETY: an unknown modifier fails the whole chord rather than being ignored.
    QVERIFY(!parse(QStringLiteral("hyper+a")).ok);
    QVERIFY(!parse(QStringLiteral("ctrl+super+a")).ok);
}

void Win32McpKeyChordTests::emptyOrSeparatorOnlyRefused() {
    QVERIFY(!parse(QString()).ok);
    QVERIFY(!parse(QStringLiteral("+")).ok);
    QVERIFY(!parse(QStringLiteral("++")).ok);
}

void Win32McpKeyChordTests::malformedSeparatorRefused() {
    // SAFETY: a doubled/leading/trailing '+' is malformed and must be rejected, not silently
    // collapsed into a different chord (e.g. "+S" quietly becoming "S", or "Ctrl++S" -> "Ctrl+S").
    QVERIFY(!parse(QStringLiteral("Ctrl++S")).ok);
    QVERIFY(!parse(QStringLiteral("+S")).ok);
    QVERIFY(!parse(QStringLiteral("Ctrl+")).ok);
    QVERIFY(!parse(QStringLiteral("Ctrl+ +S")).ok);  // whitespace-only segment
}

QTEST_GUILESS_MAIN(Win32McpKeyChordTests)
#include "test_win32_mcp_key_chord.moc"
