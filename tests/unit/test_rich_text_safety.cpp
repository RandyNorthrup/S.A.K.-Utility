// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/rich_text_safety.h"

#include <QtTest/QtTest>

using sak::ui::asLiteralRichText;

namespace {

// The payload part of the wrapper output: everything the untrusted value turned into, with the
// fixed <html><span ...> prologue and </span></html> epilogue removed. A test that asserts on this
// cannot be fooled by markup that merely moved into the wrapper.
QString payload(const QString& untrusted) {
    const QString wrapped = asLiteralRichText(untrusted);
    const int open = wrapped.indexOf(QLatin1Char('>'), wrapped.indexOf(QLatin1String("<span")));
    const int close = wrapped.lastIndexOf(QLatin1String("</span></html>"));
    return wrapped.mid(open + 1, close - open - 1);
}

}  // namespace

class UiTextSafetyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void wrapperMarksTheStringAsRichText();
    void boldTagIsNeutralized();
    void imageTagCannotFetchAResource();
    void anchorCannotBecomeALink();
    void ampersandAndQuoteAreEscaped();
    void entityInInputIsNotDecoded();
    void newlinesBecomeLineBreaks();
    void plainTextIsUnchangedApartFromTheWrapper();
    void emptyInputStaysEmpty();
    void repeatedWrappingNeverUnescapes();
};

void UiTextSafetyTests::wrapperMarksTheStringAsRichText() {
    // The wrapper -- not the value -- has to be what makes Qt choose the rich-text branch;
    // otherwise an escaped "AT&T" would be shown as the literal "AT&amp;T" on a plain sink.
    const QString wrapped = asLiteralRichText(QStringLiteral("AT&T"));
    // Pin the full wrapped output (prologue + pre-wrap style + escaped payload + epilogue).
    QCOMPARE(wrapped,
             QStringLiteral("<html><span style='white-space: pre-wrap;'>AT&amp;T</span></html>"));
}

void UiTextSafetyTests::boldTagIsNeutralized() {
    // A registry program name of "<b>Windows Update</b>" must not be able to forge emphasis in a
    // destructive-action confirmation.
    const QString body = payload(QStringLiteral("<b>Windows Update</b>"));
    QVERIFY(!body.contains(QLatin1String("<b>")));
    QVERIFY(!body.contains(QLatin1String("</b>")));
    QCOMPARE(body, QStringLiteral("&lt;b&gt;Windows Update&lt;/b&gt;"));
}

void UiTextSafetyTests::imageTagCannotFetchAResource() {
    // The concrete impact in the finding: Qt's rich text loads <img src=...>, local or remote.
    const QString body = payload(QStringLiteral("<img src='http://attacker.example/x.png'>"));
    QCOMPARE(body, QStringLiteral("&lt;img src='http://attacker.example/x.png'&gt;"));
}

void UiTextSafetyTests::anchorCannotBecomeALink() {
    const QString body = payload(QStringLiteral("<a href=\"file:///C:/Windows\">click</a>"));
    QCOMPARE(body, QStringLiteral("&lt;a href=&quot;file:///C:/Windows&quot;&gt;click&lt;/a&gt;"));
}

void UiTextSafetyTests::ampersandAndQuoteAreEscaped() {
    const QString body = payload(QStringLiteral("Ben & Jerry's \"best\""));
    // ' is deliberately left unescaped (Qt's toHtmlEscaped) -- the load-bearing distinction.
    QCOMPARE(body, QStringLiteral("Ben &amp; Jerry's &quot;best&quot;"));
}

void UiTextSafetyTests::entityInInputIsNotDecoded() {
    // A value that already contains "&lt;b&gt;" must stay literal text, not turn back into a tag.
    const QString body = payload(QStringLiteral("&lt;b&gt;"));
    QCOMPARE(body, QStringLiteral("&amp;lt;b&amp;gt;"));
}

void UiTextSafetyTests::newlinesBecomeLineBreaks() {
    // Tooltips concatenate a label and a path with "\n"; a raw newline would collapse in rich
    // text, so it is turned into an explicit break.
    const QString body = payload(QStringLiteral("File: a.txt\nC:\\Users\\Username\\a.txt"));
    QCOMPARE(body, QStringLiteral("File: a.txt<br/>C:\\Users\\Username\\a.txt"));
}

void UiTextSafetyTests::plainTextIsUnchangedApartFromTheWrapper() {
    QCOMPARE(payload(QStringLiteral("C:\\Users\\Username\\Documents")),
             QStringLiteral("C:\\Users\\Username\\Documents"));
}

void UiTextSafetyTests::emptyInputStaysEmpty() {
    // "No tooltip" must stay "no tooltip"; a bare wrapper would pop up an empty box.
    QVERIFY(asLiteralRichText(QString()).isEmpty());
    QVERIFY(asLiteralRichText(QStringLiteral("")).isEmpty());
    // A whitespace-only value is still real text and keeps its wrapper.
    QCOMPARE(asLiteralRichText(QStringLiteral(" ")),
             QStringLiteral("<html><span style='white-space: pre-wrap;'> </span></html>"));
}

void UiTextSafetyTests::repeatedWrappingNeverUnescapes() {
    // Wrapping an already-wrapped string must not resurrect the markup it neutralized.
    const QString once = asLiteralRichText(QStringLiteral("<script>x</script>"));
    const QString twice = asLiteralRichText(once);
    // Re-wrapping re-escapes the already-escaped entities (& -> &amp;), never unescaping.
    QCOMPARE(twice,
             QStringLiteral("<html><span style='white-space: pre-wrap;'>"
                            "&lt;html&gt;&lt;span style='white-space: pre-wrap;'&gt;"
                            "&amp;lt;script&amp;gt;x&amp;lt;/script&amp;gt;"
                            "&lt;/span&gt;&lt;/html&gt;"
                            "</span></html>"));
}

QTEST_GUILESS_MAIN(UiTextSafetyTests)
#include "test_rich_text_safety.moc"
