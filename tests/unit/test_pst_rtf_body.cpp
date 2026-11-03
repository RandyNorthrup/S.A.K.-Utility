// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pst_rtf_body.cpp
/// @brief Compressed RTF message bodies: MS-OXRTFCP's own vectors, damage, and text extraction.

#include "sak/pst_rtf_body.h"

#include <QtTest/QtTest>

using sak::pst::decompressRtf;
using sak::pst::rtfToText;

class TestPstRtfBody : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // MS-OXRTFCP 4.1: a stream of literals and one dictionary reference.
    void inflatesTheSpecificationsFirstExample() {
        const QByteArray compressed = QByteArray::fromHex(
            "2d0000002b0000004c5a4675f1c5c7a703000a0072637067313235423"
            "20af32068656c090020627705b06c647d0a800fa0");
        const auto rtf = decompressRtf(compressed);
        QVERIFY(rtf.has_value());
        QCOMPARE(*rtf, QByteArray("{\\rtf1\\ansi\\ansicpg1252\\pard hello world}\r\n"));
    }

    // Outlook ends the stream with a NUL after the closing brace, and a note, task or journal
    // entry has no other body, so the NUL reached every export of one.
    void dropsTheNulThatEndsTheStream() {
        QByteArray rtf("{\\rtf1\\ansi\\fromtext Tide table\\par}");
        rtf.append('\0');
        QCOMPARE(rtfToText(rtf).text, QStringLiteral("Tide table"));
    }

    // MS-OXRTFCP 4.2: a reference that overlaps its own output, wrapping past the write position.
    void inflatesAReferenceOverlappingItsOwnOutput() {
        const QByteArray compressed =
            QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0");
        const auto rtf = decompressRtf(compressed);
        QVERIFY(rtf.has_value());
        QCOMPARE(*rtf, QByteArray("{\\rtf1 WXYZWXYZWXYZWXYZWXYZ}"));
    }

    void refusesAStreamWhoseCrcDoesNotMatch() {
        QByteArray compressed =
            QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0");
        compressed[20] = static_cast<char>(compressed[20] ^ 0x01);
        QVERIFY(!decompressRtf(compressed).has_value());
    }

    void refusesAStreamShorterThanItsDeclaredSize() {
        const QByteArray compressed =
            QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0");
        QVERIFY(!decompressRtf(compressed.left(compressed.size() - 4)).has_value());
    }

    void refusesAnUnknownCompressionType() {
        QByteArray compressed =
            QByteArray::fromHex("1a0000001c0000004c5a4675e2d44b51410004205758595a0d6e7d010eb0");
        compressed[8] = 'X';
        QVERIFY(!decompressRtf(compressed).has_value());
    }

    void returnsAnUncompressedBodyAsStored() {
        const QByteArray body("{\\rtf1 plain}");
        QByteArray stored = QByteArray::fromHex("00000000") + QByteArray(4, '\0') + "MELA" +
                            QByteArray(4, '\0') + body;
        const auto size = static_cast<uint32_t>(stored.size() - 4);
        stored[0] = static_cast<char>(size);
        stored[4] = static_cast<char>(body.size());
        const auto rtf = decompressRtf(stored);
        QVERIFY(rtf.has_value());
        QCOMPARE(*rtf, body);
    }

    void pullsTextOutOfPlainRtf() {
        const auto body = rtfToText(
            "{\\rtf1\\ansi\\ansicpg1252{\\fonttbl{\\f0 Arial;}}{\\colortbl;\\red0\\green0\\blue0;}"
            "\\pard\\plain Hello\\par world\\tab again\\line caf\\'e9\\par}");
        QVERIFY(!body.is_html);
        QCOMPARE(body.text, QStringLiteral("Hello\nworld\tagain\ncaf\u00E9"));
    }

    void decodesUnicodeEscapesAndSkipsTheirFallback() {
        const auto body = rtfToText("{\\rtf1\\ansi\\uc1 snow\\u9731?man}");
        QCOMPARE(body.text, QStringLiteral("snow\u2603man"));
    }

    void dropsIgnorableDestinationsButKeepsTheBraceEscapes() {
        const auto body = rtfToText("{\\rtf1{\\*\\generator Riched20;}{\\pict abc}a\\{b\\}\\\\c}");
        QCOMPARE(body.text, QStringLiteral("a{b}\\c"));
    }

    void marksEncapsulatedHtmlAndKeepsItsTags() {
        const auto body = rtfToText(
            "{\\rtf1\\ansi\\fromhtml1 {\\*\\htmltag64 <p>}\\htmlrtf \\par \\htmlrtf0 Hi"
            "{\\*\\htmltag64 </p>}}");
        QVERIFY(body.is_html);
        QCOMPARE(body.text, QStringLiteral("<p>Hi</p>"));
    }

    void treatsEncapsulatedTextAsPlain() {
        const auto body = rtfToText("{\\rtf1\\ansi\\fromtext Line one\\par Line two}");
        QVERIFY(!body.is_html);
        QCOMPARE(body.text, QStringLiteral("Line one\nLine two"));
    }

    void survivesUnbalancedBracesAndATrailingBackslash() {
        QCOMPARE(rtfToText("}}}{\\rtf1 tail\\").text, QStringLiteral("tail"));
    }
};

QTEST_GUILESS_MAIN(TestPstRtfBody)
#include "test_pst_rtf_body.moc"
