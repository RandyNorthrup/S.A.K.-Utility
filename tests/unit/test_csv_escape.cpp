// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_csv_escape.cpp
/// @brief Unit tests for sak::csvEscape, the single CSV cell writer.
///
/// Four writers existed and only two carried the formula guard. The two that did not -- the
/// vulnerability panel and the network diagnostic panel's table export -- both write values this
/// program did not author: advisory text from an external feed, and WiFi SSIDs, remote hosts,
/// share names and firewall rule names. Quoting alone was never enough there, because a
/// spreadsheet evaluates a leading '=' inside a quoted cell exactly as it does outside one.

#include "sak/csv_escape.h"

#include <QtTest/QtTest>

class TestCsvEscape : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void plainValuesPassThroughUnchanged();
    void neutralizesEveryFormulaLeadIn();
    void formulaGuardSurvivesLeadingWhitespace();
    void formulaGuardAppliesInsideAQuotedCell();
    void quotesOnlyWhenTheCellNeedsIt();
    void doublesEmbeddedQuotes();
    void honoursANonCommaDelimiter();
};

void TestCsvEscape::plainValuesPassThroughUnchanged() {
    // An ordinary value must NOT be quoted: doing so unconditionally is what let the network
    // panel's old export look correct while carrying no formula guard at all.
    QCOMPARE(sak::csvEscape(QStringLiteral("Ethernet")), QStringLiteral("Ethernet"));
    QCOMPARE(sak::csvEscape(QStringLiteral("192.168.1.1")), QStringLiteral("192.168.1.1"));
    QCOMPARE(sak::csvEscape(QString()), QString());
    // A '=' that is not leading is ordinary text.
    QCOMPARE(sak::csvEscape(QStringLiteral("a=b")), QStringLiteral("a=b"));
}

void TestCsvEscape::neutralizesEveryFormulaLeadIn() {
    // CWE-1236. Each of these is a character a spreadsheet treats as the start of a formula.
    QCOMPARE(sak::csvEscape(QStringLiteral("=1+1")), QStringLiteral("'=1+1"));
    QCOMPARE(sak::csvEscape(QStringLiteral("+1")), QStringLiteral("'+1"));
    QCOMPARE(sak::csvEscape(QStringLiteral("-1")), QStringLiteral("'-1"));
    QCOMPARE(sak::csvEscape(QStringLiteral("@SUM(A1)")), QStringLiteral("'@SUM(A1)"));

    // The concrete case: a WiFi SSID is attacker-chosen, and the network panel exports one into a
    // CSV a technician then opens. Without the guard this cell is a live hyperlink formula.
    const QString ssid = QStringLiteral("=HYPERLINK(\"http://attacker/\",\"Click\")");
    const QString cell = sak::csvEscape(ssid);
    QVERIFY(cell.startsWith(QStringLiteral("\"'=")));
}

void TestCsvEscape::formulaGuardSurvivesLeadingWhitespace() {
    // Leading whitespace does not stop a spreadsheet evaluating the formula, so it must not be
    // allowed to hide the character that triggers the guard.
    QCOMPARE(sak::csvEscape(QStringLiteral("  =1+1")), QStringLiteral("'  =1+1"));
    // Tab and CR both force quoting as well, since a bare CR would break the record.
    QVERIFY(sak::csvEscape(QStringLiteral("\t=1+1")).contains(QStringLiteral("'\t=1+1")));
}

void TestCsvEscape::formulaGuardAppliesInsideAQuotedCell() {
    // The apostrophe must land INSIDE the quotes. Prepending it after the quoting decision would
    // strand it outside the cell and produce a malformed record AND a live formula.
    const QString cell = sak::csvEscape(QStringLiteral("=A1,B2"));
    QCOMPARE(cell, QStringLiteral("\"'=A1,B2\""));
    QVERIFY(cell.startsWith(QLatin1Char('"')));
    QVERIFY(cell.endsWith(QLatin1Char('"')));
}

void TestCsvEscape::quotesOnlyWhenTheCellNeedsIt() {
    QCOMPARE(sak::csvEscape(QStringLiteral("a,b")), QStringLiteral("\"a,b\""));
    QCOMPARE(sak::csvEscape(QStringLiteral("line1\nline2")), QStringLiteral("\"line1\nline2\""));
    QCOMPARE(sak::csvEscape(QStringLiteral("line1\rline2")), QStringLiteral("\"line1\rline2\""));
    // No delimiter, quote, CR or LF -> no quoting.
    QCOMPARE(sak::csvEscape(QStringLiteral("plain text")), QStringLiteral("plain text"));
}

void TestCsvEscape::doublesEmbeddedQuotes() {
    QCOMPARE(sak::csvEscape(QStringLiteral("say \"hi\"")), QStringLiteral("\"say \"\"hi\"\"\""));
    // A lone quote still forces quoting, and is doubled inside it.
    QCOMPARE(sak::csvEscape(QStringLiteral("\"")), QStringLiteral("\"\"\"\""));
}

void TestCsvEscape::honoursANonCommaDelimiter() {
    // The email exporter writes semicolon-delimited CSV for locales where that is expected. A cell
    // containing the ACTIVE delimiter must be quoted; a comma in a semicolon file need not be.
    QCOMPARE(sak::csvEscape(QStringLiteral("a;b"), QLatin1Char(';')), QStringLiteral("\"a;b\""));
    QCOMPARE(sak::csvEscape(QStringLiteral("a,b"), QLatin1Char(';')), QStringLiteral("a,b"));
    // ...and the comma rule still applies when the comma IS the delimiter.
    QCOMPARE(sak::csvEscape(QStringLiteral("a;b"), QLatin1Char(',')), QStringLiteral("a;b"));
}

QTEST_MAIN(TestCsvEscape)
#include "test_csv_escape.moc"
