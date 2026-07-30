// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_text_match.h"

#include <QtTest/QtTest>

using sak::win32mcp::locateText;
using sak::win32mcp::TextMatch;
using sak::win32mcp::WordHit;

namespace {

// Append a word on `line`; boxes are placeholders (position does not affect matching/ranking here,
// only the line grouping and consecutive index order do). x staggers so union boxes stay sane.
void add(QVector<WordHit>& hits, const QString& text, int line) {
    hits.append(WordHit{text, static_cast<int>(hits.size()) * 20, line * 20, 18, 16, line});
}

QVector<WordHit> line(const QStringList& words, int line_index = 0) {
    QVector<WordHit> hits;
    for (const QString& w : words) {
        add(hits, w, line_index);
    }
    return hits;
}

}  // namespace

class Win32McpTextMatchTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void wholeWordDoesNotMatchSubstring();
    void exactWholeWordMatches();
    void punctuationIsStripped();
    void multiWordMatchesConsecutiveSameLineRun();
    void multiWordDoesNotMatchAcrossLines();
    void multiWordDoesNotMatchNonConsecutive();
    void containsIsOptIn();
    void wholeWordOutranksSubstring();
    void standaloneLabelOutranksSentenceWord();
    void blankQueryReturnsNothing();
    void emptyHitsReturnNothing();
};

void Win32McpTextMatchTests::wholeWordDoesNotMatchSubstring() {
    // The two regressions that started the Q1 fix: "OK" must not hit "cookies", "Scan" not
    // "Scanning". Default (allow_sub=false) is whole-word only.
    QVERIFY(locateText(line({QStringLiteral("432"), QStringLiteral("cookies")}),
                       QStringLiteral("OK"),
                       false)
                .isEmpty());
    QVERIFY(
        locateText(line({QStringLiteral("Scanning")}), QStringLiteral("Scan"), false).isEmpty());
}

void Win32McpTextMatchTests::exactWholeWordMatches() {
    const QVector<TextMatch> m = locateText(line({QStringLiteral("Quick"), QStringLiteral("Scan")}),
                                            QStringLiteral("Scan"),
                                            false);
    QCOMPARE(m.size(), 1);
    QCOMPARE(m.first().text, QStringLiteral("Scan"));
}

void Win32McpTextMatchTests::punctuationIsStripped() {
    // OCR often appends punctuation ("Continue:"); a whole-word query must still match.
    const QVector<TextMatch> m =
        locateText(line({QStringLiteral("Continue:")}), QStringLiteral("Continue"), false);
    QCOMPARE(m.size(), 1);
}

void Win32McpTextMatchTests::multiWordMatchesConsecutiveSameLineRun() {
    const QVector<TextMatch> m =
        locateText(line({QStringLiteral("Scan"), QStringLiteral("Results")}),
                   QStringLiteral("Scan Results"),
                   false);
    QCOMPARE(m.size(), 1);
    QCOMPARE(m.first().text, QStringLiteral("Scan Results"));
}

void Win32McpTextMatchTests::multiWordDoesNotMatchAcrossLines() {
    QVector<WordHit> hits;
    add(hits, QStringLiteral("Scan"), 0);
    add(hits, QStringLiteral("Results"), 1);  // next line -> not a same-line run
    QVERIFY(locateText(hits, QStringLiteral("Scan Results"), false).isEmpty());
}

void Win32McpTextMatchTests::multiWordDoesNotMatchNonConsecutive() {
    QVERIFY(locateText(
                line({QStringLiteral("Scan"), QStringLiteral("your"), QStringLiteral("Results")}),
                QStringLiteral("Scan Results"),
                false)
                .isEmpty());
}

void Win32McpTextMatchTests::containsIsOptIn() {
    // "Scan" as a substring of "Scanning" matches only when the caller opts in (contains=true).
    QVERIFY(locateText(line({QStringLiteral("Scanning")}), QStringLiteral("Scan"), true).size() ==
            1);
    QVERIFY(
        locateText(line({QStringLiteral("Scanning")}), QStringLiteral("Scan"), false).isEmpty());
}

void Win32McpTextMatchTests::wholeWordOutranksSubstring() {
    // With contains enabled, a whole-word "Scan" must still rank above a substring hit "Scanning".
    QVector<WordHit> hits;
    add(hits, QStringLiteral("Scanning"), 0);  // substring (strength 1)
    add(hits, QStringLiteral("Scan"), 1);      // whole word (strength 2)
    const QVector<TextMatch> m = locateText(hits, QStringLiteral("Scan"), true);
    QCOMPARE(m.size(), 2);
    QCOMPARE(m.first().text, QStringLiteral("Scan"));  // whole word wins regardless of order
}

void Win32McpTextMatchTests::standaloneLabelOutranksSentenceWord() {
    // "check" as a lone button label (its own line) beats "check" buried in a sentence.
    QVector<WordHit> hits = line({QStringLiteral("Please"),
                                  QStringLiteral("check"),
                                  QStringLiteral("the"),
                                  QStringLiteral("box")},
                                 0);
    add(hits, QStringLiteral("check"), 1);  // standalone label on its own line
    const QVector<TextMatch> m = locateText(hits, QStringLiteral("check"), false);
    QCOMPARE(m.size(), 2);
    QCOMPARE(m.first().line, 1);  // the standalone one ranks first
}

void Win32McpTextMatchTests::blankQueryReturnsNothing() {
    QVERIFY(locateText(line({QStringLiteral("OK")}), QString(), false).isEmpty());
    QVERIFY(locateText(line({QStringLiteral("OK")}), QStringLiteral("   "), false).isEmpty());
}

void Win32McpTextMatchTests::emptyHitsReturnNothing() {
    QVERIFY(locateText({}, QStringLiteral("OK"), false).isEmpty());
}

QTEST_GUILESS_MAIN(Win32McpTextMatchTests)
#include "test_win32_mcp_text_match.moc"
