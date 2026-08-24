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
    // Containment is ONE-directional: an OCR word that is only a PREFIX of the query is not a
    // whole-word hit either, and stays rejected even with substring matching opted in --
    // production tests word.contains(token), never token.contains(word).
    QVERIFY(locateText(line({QStringLiteral("Sca")}), QStringLiteral("Scan"), false).isEmpty());
    QVERIFY(locateText(line({QStringLiteral("Sca")}), QStringLiteral("Scan"), true).isEmpty());
    // Control on the same fixture, so the emptiness above is the whole-word rule and not a
    // dead matcher.
    const QVector<TextMatch> control = locateText(
        line({QStringLiteral("432"), QStringLiteral("cookies")}), QStringLiteral("cookies"), false);
    QCOMPARE(control.size(), 1);
    QCOMPARE(control.first().text, QStringLiteral("cookies"));
    QCOMPARE(control.first().x, 20);
}

void Win32McpTextMatchTests::exactWholeWordMatches() {
    const QVector<TextMatch> m = locateText(line({QStringLiteral("Quick"), QStringLiteral("Scan")}),
                                            QStringLiteral("Scan"),
                                            false);
    QCOMPARE(m.size(), 1);
    QCOMPARE(m.first().text, QStringLiteral("Scan"));
    // The box is the MATCHED word's own, never the vector head's -- the OCR click path uses its
    // centre, so a box seeded from the wrong hit clicks "Quick".
    QCOMPARE(m.first().x, 20);
    QCOMPARE(m.first().y, 0);
    QCOMPARE(m.first().w, 18);
    QCOMPARE(m.first().h, 16);
    QCOMPARE(m.first().line, 0);
}

void Win32McpTextMatchTests::punctuationIsStripped() {
    // OCR often appends punctuation ("Continue:"); a whole-word query must still match.
    const QVector<TextMatch> m =
        locateText(line({QStringLiteral("Continue:")}), QStringLiteral("Continue"), false);
    QCOMPARE(m.size(), 1);
    // The caption reported back is the RAW OCR word, punctuation included -- only the
    // comparison is normalized.
    QCOMPARE(m.first().text, QStringLiteral("Continue:"));
    // The QUERY side is normalized the same way, which nothing exercised: caller punctuation
    // must still find the words.
    const QVector<TextMatch> q =
        locateText(line({QStringLiteral("Scan"), QStringLiteral("Results")}),
                   QStringLiteral("Scan, Results"),
                   false);
    QCOMPARE(q.size(), 1);
    QCOMPARE(q.first().text, QStringLiteral("Scan Results"));
}

void Win32McpTextMatchTests::multiWordMatchesConsecutiveSameLineRun() {
    const QVector<TextMatch> m =
        locateText(line({QStringLiteral("Scan"), QStringLiteral("Results")}),
                   QStringLiteral("Scan Results"),
                   false);
    QCOMPARE(m.size(), 1);
    QCOMPARE(m.first().text, QStringLiteral("Scan Results"));
    // The box is the UNION of both words -- "Scan" [0,18) and "Results" [20,38) -- which is the
    // documented point of a multi-word run and was unchecked.
    QCOMPARE(m.first().x, 0);
    QCOMPARE(m.first().y, 0);
    QCOMPARE(m.first().w, 38);
    QCOMPARE(m.first().h, 16);
    QCOMPARE(m.first().line, 0);
}

void Win32McpTextMatchTests::multiWordDoesNotMatchAcrossLines() {
    QVector<WordHit> hits;
    add(hits, QStringLiteral("Scan"), 0);
    add(hits, QStringLiteral("Results"), 1);  // next line -> not a same-line run
    QVERIFY(locateText(hits, QStringLiteral("Scan Results"), false).isEmpty());
    // Control: the same two words DO match as a run once they share a line -- on a line other
    // than 0 and starting past the head of the vector, so the emptiness above is the same-line
    // rule and not multi-word matching that only ever works at line 0 / hits[0]. The box is the
    // run's own, never the vector head's.
    add(hits, QStringLiteral("Scan"), 3);
    add(hits, QStringLiteral("Results"), 3);
    const QVector<TextMatch> matched = locateText(hits, QStringLiteral("Scan Results"), false);
    QCOMPARE(matched.size(), 1);
    QCOMPARE(matched.first().text, QStringLiteral("Scan Results"));
    QCOMPARE(matched.first().line, 3);
    QCOMPARE(matched.first().x, 40);
    QCOMPARE(matched.first().y, 60);
    QCOMPARE(matched.first().w, 38);
    QCOMPARE(matched.first().h, 16);
}

void Win32McpTextMatchTests::multiWordDoesNotMatchNonConsecutive() {
    QVERIFY(locateText(
                line({QStringLiteral("Scan"), QStringLiteral("your"), QStringLiteral("Results")}),
                QStringLiteral("Scan Results"),
                false)
                .isEmpty());
    // Control: a consecutive run still matches when it starts MID-line, so the emptiness above
    // is the consecutive rule and not a scan that only ever tries the head of the line.
    const QVector<TextMatch> mid = locateText(
        line({QStringLiteral("your"), QStringLiteral("Scan"), QStringLiteral("Results")}),
        QStringLiteral("Scan Results"),
        false);
    QCOMPARE(mid.size(), 1);
    QCOMPARE(mid.first().text, QStringLiteral("Scan Results"));
    QCOMPARE(mid.first().x, 20);
    QCOMPARE(mid.first().w, 38);
}

void Win32McpTextMatchTests::containsIsOptIn() {
    // "Scan" as a substring of "Scanning" matches only when the caller opts in (contains=true).
    const QVector<TextMatch> opt_in =
        locateText(line({QStringLiteral("Scanning")}), QStringLiteral("Scan"), true);
    QCOMPARE(opt_in.size(), 1);
    QCOMPARE(opt_in.first().text, QStringLiteral("Scanning"));  // full OCR word, not the token
    QCOMPARE(opt_in.first().x, 0);                              // ...and the whole word's box
    QCOMPARE(opt_in.first().w, 18);
    // Substring strength is a FULL weight below whole word, so no extra-words tie-breaker can
    // ever lift a substring past a whole-word hit.
    QCOMPARE(opt_in.first().score, 100'000);
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
    QCOMPARE(m.first().line, 1);
    QCOMPARE(m.first().score, 200'000);                // 2 * 100000, no extra words on line 1
    QCOMPARE(m.at(1).text, QStringLiteral("Scanning"));
    QCOMPARE(m.at(1).line, 0);
    QCOMPARE(m.at(1).score, 100'000);  // a full weight below, not a tie-breaker apart
    // Mirrored: whole word on the EARLIER line, so ranking by score is distinguishable from
    // ranking by position. The "regardless of order" claim only holds if BOTH are exercised.
    QVector<WordHit> mirrored;
    add(mirrored, QStringLiteral("Scan"), 0);
    add(mirrored, QStringLiteral("Scanning"), 1);
    const QVector<TextMatch> m2 = locateText(mirrored, QStringLiteral("Scan"), true);
    QCOMPARE(m2.size(), 2);
    QCOMPARE(m2.first().text, QStringLiteral("Scan"));
    QCOMPARE(m2.first().line, 0);
    QCOMPARE(m2.at(1).text, QStringLiteral("Scanning"));
    QCOMPARE(m2.at(1).line, 1);
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
    QCOMPARE(m.first().text, QStringLiteral("check"));
    QCOMPARE(m.first().x, 80);
    QCOMPARE(m.first().score, 200'000);  // own line: 2 * 100000, 0 extra words
    // The loser is the same single word inside the sentence: its box covers only "check", and
    // it is scored by the EXACT number of extra words on its line (4 - 1 = 3), not by a boolean
    // "is standalone" flag.
    QCOMPARE(m.at(1).line, 0);
    QCOMPARE(m.at(1).text, QStringLiteral("check"));
    QCOMPARE(m.at(1).x, 20);
    QCOMPARE(m.at(1).w, 18);
    QCOMPARE(m.at(1).score, 199'997);
}

void Win32McpTextMatchTests::blankQueryReturnsNothing() {
    QVERIFY(locateText(line({QStringLiteral("OK")}), QString(), false).isEmpty());
    QVERIFY(locateText(line({QStringLiteral("OK")}), QStringLiteral("   "), false).isEmpty());
}

void Win32McpTextMatchTests::emptyHitsReturnNothing() {
    QVERIFY(locateText({}, QStringLiteral("OK"), false).isEmpty());
    // With an empty vector the scan loop body never executes, so that says nothing about the
    // loop BOUND. Fewer hits than tokens is the case that must stop at i + tokens <= hits and
    // never read past the end of the vector.
    QVERIFY(locateText(line({QStringLiteral("Scan")}), QStringLiteral("Scan Results"), false)
                .isEmpty());
    QVERIFY(locateText(line({QStringLiteral("Scan"), QStringLiteral("Results")}),
                       QStringLiteral("Scan Results Now"),
                       false)
                .isEmpty());
}

QTEST_GUILESS_MAIN(Win32McpTextMatchTests)
#include "test_win32_mcp_text_match.moc"
