// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_mbox_headers.cpp
/// @brief Mutation-fuzz of the RFC 5322 header parser (G14-6, MBOX/EML).
///
/// The header block of a mail message is attacker-controlled. The parsing used to be a
/// private member of MboxParser (which owns an open QFile), so it could not be fuzzed on
/// its own; it was lifted to a pure seam (sak::mbox::parseRfc5322Headers) with the member
/// now delegating to it, behaviour unchanged. This harness feeds thousands of mutated
/// header blocks -- folded continuation lines, missing colons, embedded CRs, no blank-line
/// terminator, oversized values -- and asserts the parser's output contract for EVERY
/// input, malformed or not:
///
///   every emitted header name is non-empty, lower-cased, and trimmed of surrounding
///   whitespace.
///
/// Callers key on that contract (a raw or mixed-case name would silently miss a
/// Content-Type / Content-Transfer-Encoding lookup and mis-handle the body), so a
/// regression that leaked an untrimmed or upper-cased name is a real bug this catches.
/// No crash / no hang is implicit: a fault never returns the empty invariant string.

#include "sak/mbox_header_parser.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

std::vector<QByteArray> headerCorpus() {
    return {
        QByteArray(),
        QByteArray("From: alice@example.com\r\nTo: bob@example.com\r\nSubject: hi\r\n\r\nbody"),
        QByteArray("From: alice@example.com\nTo: bob@example.com\n\nbody"),  // LF-only
        QByteArray("Subject: folded value\r\n continued here\r\n\tand more\r\n\r\nbody"),
        QByteArray("NoColonHeaderLine\r\nFrom: x@y\r\n\r\n"),
        QByteArray(": leading colon has no name\r\nFrom: x@y\r\n\r\n"),
        QByteArray("X-Weird:    value with trailing spaces    \r\n\r\n"),
        QByteArray("Content-Type: text/plain; charset=\"utf-8\"\r\n\r\n"),
        QByteArray("MixedCASE-Header: v\r\nCONTENT-TRANSFER-ENCODING: base64\r\n\r\n"),
        QByteArray("From: a@b"),         // no blank line, no body
        QByteArray("aaaaaaaaaaaaaaaa"),  // no terminator, no colon
        QByteArray("\r\n\r\n"),          // immediate blank line
    };
}

// The parser's output contract: every header name is non-empty, lower-cased, and trimmed.
QString headerInvariant(const QByteArray& input) {
    const QMap<QString, QString> headers = sak::mbox::parseRfc5322Headers(input);
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        const QString& key = it.key();
        if (key.isEmpty()) {
            return QStringLiteral("parser emitted an empty header name");
        }
        if (key != key.toLower()) {
            return QStringLiteral("header name is not lower-cased: \"%1\"").arg(key);
        }
        if (key != key.trimmed()) {
            return QStringLiteral("header name has untrimmed whitespace: \"%1\"").arg(key);
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("MBOX header fuzz failed after %1 inputs: %2\n  bytes (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class MboxHeaderFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void headerNamesAlwaysNormalized() {
        const std::vector<QByteArray> corpus = headerCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, headerInvariant, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }

    // Exact-value coverage. The property fuzz above only asserts that every emitted name is
    // non-empty, lower-cased, and trimmed -- it never pins the VALUE, the header COUNT, or
    // the header/body boundary, so a mutation to the colon split or the CRLF boundary slips
    // through. This locks the fields and the count for a known CRLF block.
    void parsesNameValueAndCountExactly() {
        const QMap<QString, QString> h = sak::mbox::parseRfc5322Headers(
            QByteArray("From: alice@example.com\r\nTo: bob@example.com\r\nSubject: hi\r\n\r\nb"));
        QCOMPARE(h.size(), 3);
        QCOMPARE(h.value(QStringLiteral("from")), QStringLiteral("alice@example.com"));
        QCOMPARE(h.value(QStringLiteral("to")), QStringLiteral("bob@example.com"));
        QCOMPARE(h.value(QStringLiteral("subject")), QStringLiteral("hi"));
    }

    // A folded continuation line (leading space or tab) joins onto the preceding value with
    // exactly one space and its own indent stripped -- distinguishes the correct join from a
    // dropped separator ("valuecontinued") or a kept indent ("value  continued").
    void foldedContinuationJoinsWithSingleSpace() {
        const QMap<QString, QString> h = sak::mbox::parseRfc5322Headers(
            QByteArray("Subject: folded value\r\n continued here\r\n\tand more\r\n\r\nbody"));
        QCOMPARE(h.value(QStringLiteral("subject")),
                 QStringLiteral("folded value continued here and more"));
    }

    // The header/body boundary is a blank line in LF-only messages too, and a name may carry
    // whitespace before the colon. Pins the LF boundary (so later headers are not lost) and
    // the name trim -- the property fuzz only catches the latter by chance.
    void lfBoundaryKeepsAllHeadersAndNameIsTrimmed() {
        const QMap<QString, QString> lf =
            sak::mbox::parseRfc5322Headers(QByteArray("From: a@b\nTo: c@d\n\nbody"));
        QCOMPARE(lf.size(), 2);
        QCOMPARE(lf.value(QStringLiteral("to")), QStringLiteral("c@d"));

        const QMap<QString, QString> spaced =
            sak::mbox::parseRfc5322Headers(QByteArray("From : x\r\n\r\n"));
        QVERIFY(spaced.contains(QStringLiteral("from")));
        QCOMPARE(spaced.value(QStringLiteral("from")), QStringLiteral("x"));
    }
};

QTEST_APPLESS_MAIN(MboxHeaderFuzzTests)
#include "test_fuzz_mbox_headers.moc"
