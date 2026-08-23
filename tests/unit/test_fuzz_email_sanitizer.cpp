// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_email_sanitizer.cpp
/// @brief Mutation-fuzz of the untrusted-email HTML sanitizer (G14-6/G14-12).
///
/// The email body is attacker-controlled, and sanitizeEmailBodyHtml runs a stack
/// of QRegularExpression passes over it -- exactly the shape that hides a crash or
/// a catastrophic-backtracking (ReDoS) hang. The harness feeds thousands of mutated
/// bodies through it deterministically and asserts two invariants that must hold for
/// EVERY input, malformed or not:
///
///   1. The sanitizer never grows its input. Every operator either removes a span
///      or replaces it with a shorter token ("javascript:" -> "blocked:",
///      "url(...)" -> "none"), so the output length is non-increasing. A future edit
///      that accidentally amplifies content -- a classic path to a decompression- or
///      billion-laughs-style blowup -- trips this immediately.
///   2. The sanitizer converges: a second pass over its own output is a no-op. The
///      multi-pass loop exists precisely to reach that fixpoint (so a tag spliced
///      back together by an earlier removal is caught), and this asserts it actually
///      does within the pass cap for every fuzzed input.
///
/// "No crash / no hang" is covered implicitly: a fault never returns the empty
/// string, and a ReDoS hang trips the ctest timeout with the fixed seed recorded in
/// fuzz_harness.h, so the offending input is reproducible.

#include "sak/email_html_sanitizer.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

/// A spread of ordinary and adversarial email-body fragments. The adversarial ones
/// (spliced tags, slash-separated handlers, CSS escapes) are the constructs the
/// sanitizer's re-scan loop and boundary rules were written against, so mutating
/// around them exercises the fixpoint logic rather than only the happy path.
std::vector<QByteArray> sanitizerCorpus() {
    return {
        QByteArray(),
        QByteArray("<p>Hello, technician. Your ticket is resolved.</p>"),
        QByteArray("<script>alert(1)</script>"),
        QByteArray("<scr<form>ipt>alert(1)</script>"),
        QByteArray("<img src=x onerror=alert(1)>"),
        QByteArray("<svg/onload=alert(1)>"),
        QByteArray("<a href=\"javascript:alert(document.cookie)\">click</a>"),
        QByteArray("<a href=\"vbscript:msgbox(1)\">click</a>"),
        QByteArray("<div style=\"background:url('http://evil.example/x.png')\">hi</div>"),
        QByteArray("<div style=\"width:expression(alert(1))\">hi</div>"),
        QByteArray("<img src=\"data:image/png;base64,iVBORw0KGgo=\">"),
        QByteArray("<iframe src=http://evil.example></iframe>"),
        QByteArray("<style>@import url(http://evil.example/x.css); body{color:red}</style>"),
        QByteArray("<base href=\"http://evil.example/\"><link rel=stylesheet href=x>"),
        QByteArray("<object data=x></object><embed src=x><applet code=x></applet>"),
        QByteArray("plain text with < and > and & but no tags at all"),
        QByteArray("<p onclick =\t'steal()' >nested</p><SCRIPT>a</SCRIPT>"),
    };
}

/// The per-input invariant. Empty return means both properties held.
QString sanitizerInvariant(const QByteArray& input) {
    const QString html = QString::fromUtf8(input);
    const QString out = sak::sanitizeEmailBodyHtml(html);
    if (out.size() > html.size()) {
        return QStringLiteral("sanitizer grew output: in=%1 chars, out=%2 chars")
            .arg(html.size())
            .arg(out.size());
    }
    const QString again = sak::sanitizeEmailBodyHtml(out);
    if (again != out) {
        return QStringLiteral("sanitizer did not converge: first pass %1 chars, second %2 chars")
            .arg(out.size())
            .arg(again.size());
    }
    return {};
}

/// Build a one-line failure banner with a hex reproducer of the exact bytes.
QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("fuzz failed after %1 inputs: %2\n  reproducer bytes (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class EmailSanitizerFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void neverGrowsAndAlwaysConverges() {
        const std::vector<QByteArray> corpus = sanitizerCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, sanitizerInvariant, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // The seed pass alone runs corpus.size() checks; the mutation pass adds the
        // configured iterations. Guard the count so a harness that silently ran zero
        // inputs cannot masquerade as a pass.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }
};

QTEST_APPLESS_MAIN(EmailSanitizerFuzzTests)
#include "test_fuzz_email_sanitizer.moc"
