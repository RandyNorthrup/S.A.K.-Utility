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
    // 's' here and the '1' in digitMapsToItsVk are both range INTERIOR, and caseInsensitive
    // pins 'A' -- but 'Z', '0' and '9' are pinned nowhere. Cover all four endpoints of the two
    // accepted ranges so an off-by-one bound cannot silently make Ctrl+Z / Alt+0 / Alt+9
    // unusable while the suite stays green.
    const Parsed a_low = parse(QStringLiteral("a"));
    QVERIFY(a_low.ok);
    QCOMPARE(a_low.main_key, static_cast<unsigned short>('A'));  // 0x41
    const Parsed z_low = parse(QStringLiteral("z"));
    QVERIFY(z_low.ok);
    QCOMPARE(z_low.main_key, static_cast<unsigned short>('Z'));  // 0x5A, case-folded to upper
    const Parsed z_up = parse(QStringLiteral("Z"));
    QVERIFY(z_up.ok);
    QCOMPARE(z_up.main_key, static_cast<unsigned short>('Z'));
    const Parsed zero = parse(QStringLiteral("0"));
    QVERIFY(zero.ok);
    QCOMPARE(zero.main_key, static_cast<unsigned short>('0'));  // 0x30
    const Parsed nine = parse(QStringLiteral("9"));
    QVERIFY(nine.ok);
    QCOMPARE(nine.main_key, static_cast<unsigned short>('9'));  // 0x39
}

void Win32McpKeyChordTests::digitMapsToItsVk() {
    const Parsed p = parse(QStringLiteral("Ctrl+1"));
    QVERIFY(p.ok);
    QCOMPARE(p.modifiers, QVector<unsigned short>{VK_CONTROL});
    QCOMPARE(p.main_key, static_cast<unsigned short>('1'));  // 0x31
}

void Win32McpKeyChordTests::namedKeysMap() {
    // The WHOLE named-key catalog. Five spot-checks left 25 of the 30 alias rows unasserted, so
    // a paired-row typo (up<->down) or a dropped alias ships green while scripted keystrokes go
    // to the wrong virtual key through SendInput. ok and modifiers were never asserted either.
    struct NamedKey {
        const char* m_name;
        unsigned short m_vk;
    };
    static const NamedKey kExpected[] = {
        {.m_name = "enter", .m_vk = VK_RETURN},   {.m_name = "return", .m_vk = VK_RETURN},
        {.m_name = "tab", .m_vk = VK_TAB},        {.m_name = "escape", .m_vk = VK_ESCAPE},
        {.m_name = "esc", .m_vk = VK_ESCAPE},     {.m_name = "space", .m_vk = VK_SPACE},
        {.m_name = "backspace", .m_vk = VK_BACK}, {.m_name = "delete", .m_vk = VK_DELETE},
        {.m_name = "del", .m_vk = VK_DELETE},     {.m_name = "up", .m_vk = VK_UP},
        {.m_name = "down", .m_vk = VK_DOWN},      {.m_name = "left", .m_vk = VK_LEFT},
        {.m_name = "right", .m_vk = VK_RIGHT},    {.m_name = "home", .m_vk = VK_HOME},
        {.m_name = "end", .m_vk = VK_END},        {.m_name = "pageup", .m_vk = VK_PRIOR},
        {.m_name = "pagedown", .m_vk = VK_NEXT},  {.m_name = "insert", .m_vk = VK_INSERT},
        {.m_name = "f1", .m_vk = VK_F1},          {.m_name = "f2", .m_vk = VK_F2},
        {.m_name = "f3", .m_vk = VK_F3},          {.m_name = "f4", .m_vk = VK_F4},
        {.m_name = "f5", .m_vk = VK_F5},          {.m_name = "f6", .m_vk = VK_F6},
        {.m_name = "f7", .m_vk = VK_F7},          {.m_name = "f8", .m_vk = VK_F8},
        {.m_name = "f9", .m_vk = VK_F9},          {.m_name = "f10", .m_vk = VK_F10},
        {.m_name = "f11", .m_vk = VK_F11},        {.m_name = "f12", .m_vk = VK_F12},
    };
    for (const NamedKey& expected : kExpected) {
        const QString name = QString::fromLatin1(expected.m_name);
        const Parsed lower = parse(name);
        QVERIFY2(lower.ok, qPrintable(name));
        QVERIFY2(lower.modifiers.isEmpty(), qPrintable(name));
        QCOMPARE(lower.main_key, expected.m_vk);
        const Parsed upper = parse(name.toUpper());  // matching is case-insensitive
        QVERIFY2(upper.ok, qPrintable(name));
        QCOMPARE(upper.main_key, expected.m_vk);
    }
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
    // All SIX aliases -- "ctrl" itself was missing from the "all aliases" test -- and the WHOLE
    // result. Asserting only .modifiers is weak because production appends the modifier BEFORE
    // deciding success, and the header states that a false return may still leave the parsed
    // prefix in `modifiers`: callers gate on ok, so the test must too, or a chord that is
    // refused outright still satisfies the assertion.
    struct ModifierAlias {
        const char* m_alias;
        unsigned short m_vk;
    };
    static const ModifierAlias kAliases[] = {
        {.m_alias = "ctrl", .m_vk = VK_CONTROL},
        {.m_alias = "control", .m_vk = VK_CONTROL},
        {.m_alias = "alt", .m_vk = VK_MENU},
        {.m_alias = "shift", .m_vk = VK_SHIFT},
        {.m_alias = "win", .m_vk = VK_LWIN},
        {.m_alias = "meta", .m_vk = VK_LWIN},
    };
    for (const ModifierAlias& alias : kAliases) {
        const QString chord = QString::fromLatin1(alias.m_alias) + QStringLiteral("+a");
        const Parsed p = parse(chord);
        QVERIFY2(p.ok, qPrintable(chord));
        QCOMPARE(p.modifiers, QVector<unsigned short>{alias.m_vk});
        QCOMPARE(p.main_key, static_cast<unsigned short>('A'));
    }
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
    // Both strings above are multi-character, so they exercise only the named-catalog lookup.
    // The single-character path accepts ONLY A-Z / 0-9; punctuation bracketing those ranges
    // must be refused, or a merged or widened range check hands the key injector an unassigned
    // virtual-key code.
    const QString punctuation = QStringLiteral("/:@[]-.,;'=");
    for (const QChar punct : punctuation) {
        const QString chord = QStringLiteral("Ctrl+") + punct;
        QVERIFY2(!parse(chord).ok, qPrintable(chord));
        const QString bare(1, punct);
        QVERIFY2(!parse(bare).ok, qPrintable(bare));
    }

    // A near-miss of a catalog name must be refused as firmly as a nonsense word. "bogus" and
    // "nonsense" are neither a prefix nor an extension of any catalog row, so on their own they
    // leave a loosened compare -- matching abbreviations ("ente" -> enter) or extensions
    // ("f13" -> f1) instead of the exact name -- completely green, and send_keys would inject a
    // keystroke the caller never wrote.
    static const char* const kNearMisses[] = {
        "ente", "es", "de", "page", "backspac", "enterkey", "f13", "spaceb"};
    for (const char* const near_miss : kNearMisses) {
        const QString bare_near = QString::fromLatin1(near_miss);
        QVERIFY2(!parse(bare_near).ok, near_miss);
        const QString chord = QStringLiteral("Ctrl+") + bare_near;
        QVERIFY2(!parse(chord).ok, qPrintable(chord));
    }
}

void Win32McpKeyChordTests::unknownModifierRefused() {
    // SAFETY: an unknown modifier fails the whole chord rather than being ignored.
    QVERIFY(!parse(QStringLiteral("hyper+a")).ok);
    QVERIFY(!parse(QStringLiteral("ctrl+super+a")).ok);
    // "hyper" and "super" share no prefix, suffix or substring with any alias, so they prove
    // only that two arbitrary words miss. The modifier catalog has no self-shadowing pair
    // (unlike f1/f10 in namedKeysMap), so nothing else in the suite distinguishes the exact
    // '==' chain from a loosened compare. Near-miss tokens pin exactness in all three
    // directions -- extension, truncation and substring -- because a modifier that silently
    // resolves makes send_keys inject a REAL Ctrl/Alt/Win down (win32_mcp_input.cpp:411-412)
    // instead of returning "Unrecognized key combination": "alternate+f4" would close the
    // technician's window.
    static const char* const kNearMiss[] = {
        // extensions of an alias: killed by `low.startsWith(alias)`
        "ctrlx",
        "controls",
        "alternate",
        "shifty",
        "windows",
        "metal",
        // truncations of an alias: killed by `QLatin1String(alias).startsWith(low)`
        "ctr",
        "contro",
        "al",
        "sh",
        "wi",
        "met",
        // alias embedded with a prefix: killed by `low.contains(alias)` / `endsWith`
        "xctrl",
        "leftshift",
        "rightalt",
        "mymeta",
    };
    for (const char* const near_miss : kNearMiss) {
        const QString chord = QString::fromLatin1(near_miss) + QStringLiteral("+a");
        QVERIFY2(!parse(chord).ok, qPrintable(chord));
    }
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
    QVERIFY(!parse(QStringLiteral("Ctrl+ +S")).ok);  // a segment with no non-space content

    // Padding INTERIOR to the chord is handled only by the two per-segment trims
    // (win32_mcp_key_chord.cpp:97 and :103): the send_keys consumer trims just the whole `keys`
    // string (win32_mcp_input.cpp:401). Refusal assertions cannot see either trim -- "Ctrl+ +S"
    // is rejected by the empty-segment guard AND, independently, by modifierVk("")==0 -- so pin
    // acceptance, once for the modifier loop and once for the main key, or both trims delete free
    // and send_keys("Ctrl + S") starts answering "Unrecognized key combination" with this green.
    const Parsed padded_modifier = parse(QStringLiteral("Ctrl +S"));
    QVERIFY(padded_modifier.ok);
    QCOMPARE(padded_modifier.modifiers,
             QVector<unsigned short>{static_cast<unsigned short>(VK_CONTROL)});
    QCOMPARE(padded_modifier.main_key, static_cast<unsigned short>('S'));
    const Parsed padded_main_key = parse(QStringLiteral("Ctrl+ S"));
    QVERIFY(padded_main_key.ok);
    QCOMPARE(padded_main_key.modifiers,
             QVector<unsigned short>{static_cast<unsigned short>(VK_CONTROL)});
    QCOMPARE(padded_main_key.main_key, static_cast<unsigned short>('S'));
    // A padded named key on the bare (no-modifier) path exercises :103 with a multi-char name.
    const Parsed padded_named = parse(QStringLiteral(" Enter "));
    QVERIFY(padded_named.ok);
    QVERIFY(padded_named.modifiers.isEmpty());
    QCOMPARE(padded_named.main_key, static_cast<unsigned short>(VK_RETURN));
}

QTEST_GUILESS_MAIN(Win32McpKeyChordTests)
#include "test_win32_mcp_key_chord.moc"
