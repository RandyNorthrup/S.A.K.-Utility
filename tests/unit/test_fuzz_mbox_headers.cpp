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

constexpr int kExpectedCorpusSeeds = 12;
constexpr int kShippedFuzzIterations = 2000;  // tests/fuzz/fuzz_harness.h kDefaultIterations
// Of the twelve seeds, three yield nothing -- the empty array, the colon-less "aaaa..." line, and
// the immediate blank line -- so the remaining nine each emit at least one header.
constexpr int kSeedsYieldingHeaders = 9;

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
        // headerInvariant walks the emitted map, so an EMPTY map runs its loop body zero times
        // and returns a pass. The whole slot therefore degrades to a no-op the moment the parser
        // starts emitting nothing, with every input scored green -- and nothing asserted that any
        // input produced a header at all. run() only refuses an entirely empty corpus, so seeds
        // silently lost from headerCorpus() shrink the surface unnoticed, including the two that
        // are the ONLY things reaching the colon guards and the empty-name guard.
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);
        int seeds_yielding_headers = 0;
        for (const QByteArray& seed : corpus) {
            if (!sak::mbox::parseRfc5322Headers(seed).isEmpty()) {
                ++seeds_yielding_headers;
            }
        }
        QCOMPARE(seeds_yielding_headers, kSeedsYieldingHeaders);
        // ... and the two guard-reaching seeds still reach their guards: a line with no colon and
        // a line whose colon is at position 0 must both be skipped while the following real
        // header survives, which is what proves the colon test is `> 0` rather than `>= 0`.
        const QMap<QString, QString> no_colon =
            sak::mbox::parseRfc5322Headers(QByteArray("NoColonHeaderLine\r\nFrom: x@y\r\n\r\n"));
        QCOMPARE(no_colon.size(), 1);
        QCOMPARE(no_colon.value(QStringLiteral("from")), QStringLiteral("x@y"));
        const QMap<QString, QString> leading_colon = sak::mbox::parseRfc5322Headers(
            QByteArray(": leading colon has no name\r\nFrom: x@y\r\n\r\n"));
        QCOMPARE(leading_colon.size(), 1);
        QCOMPARE(leading_colon.value(QStringLiteral("from")), QStringLiteral("x@y"));

        const int budget = sak::fuzz::iterationsFromEnv();
        QVERIFY2(budget > 0, "the clamp must never hand run() a non-positive iteration budget");
        if (!qEnvironmentVariableIsSet("SAK_FUZZ_ITERS")) {
            QCOMPARE(budget, kShippedFuzzIterations);
        }
        const sak::fuzz::FuzzOutcome outcome =
            sak::fuzz::run(corpus, headerInvariant, budget, sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. Both sides
        // are LITERALS: drawing either from corpus.size() or a second iterationsFromEnv() call is
        // self-satisfying, since with a budget of 0 the mutation loop runs zero times,
        // iterations_run equals the seed count, and seed_count + 0 still matches.
        QCOMPARE(outcome.iterations_run, kExpectedCorpusSeeds + budget);
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

        // The value is trimmed TWICE -- once at extraction, once at insert -- and for every
        // fixture above the two collapse to the same result, because no value has TRAILING
        // whitespace that is then followed by a folded continuation. So either trim could be
        // deleted alone and this stayed green. Whitespace held past extraction is carried INTO
        // the join, where the outer trim can no longer reach it, and surfaces as run-on spaces in
        // the MIDDLE of the value. A folding Content-Type is the common real case, and a
        // downstream charset/boundary split would mis-tokenise the result.
        const QMap<QString, QString> trailing = sak::mbox::parseRfc5322Headers(
            QByteArray("Content-Type: text/plain;   \r\n charset=utf-8\r\n\r\nbody"));
        QCOMPARE(trailing.value(QStringLiteral("content-type")),
                 QStringLiteral("text/plain; charset=utf-8"));
    }

    // The header/body boundary is a blank line in LF-only messages too, and a name may carry
    // whitespace before the colon. Pins the LF boundary (so later headers are not lost) and
    // the name trim -- the property fuzz only catches the latter by chance.
    void lfBoundaryKeepsAllHeadersAndNameIsTrimmed() {
        const QMap<QString, QString> lf =
            sak::mbox::parseRfc5322Headers(QByteArray("From: a@b\nTo: c@d\n\nbody"));
        QCOMPARE(lf.size(), 2);
        QCOMPARE(lf.value(QStringLiteral("to")), QStringLiteral("c@d"));

        // The boundary is decided TWICE -- the block is pre-cut at the first blank line, and
        // inside the loop a trailing CR is chopped so a "\r" line reads as the empty line that
        // breaks. Every fixture puts the blank line exactly at the pre-cut, so each mechanism
        // alone yields the identical map and the CR strip could be deleted silently. The input
        // that separates them is a blank line that is NOT the first one: a message whose header
        // section is EMPTY. Correct is zero headers -- parsing stops at the first blank line --
        // and a broken chop parses the BODY as headers, which is the header-injection shape for
        // the caller whose Content-Type lookups would then key on attacker body text.
        const QMap<QString, QString> empty_section =
            sak::mbox::parseRfc5322Headers(QByteArray("\r\nFrom: a@b\r\n\r\nbody"));
        QVERIFY2(empty_section.isEmpty(),
                 "a message whose header section is empty must yield no headers");
        const QMap<QString, QString> empty_section_lf =
            sak::mbox::parseRfc5322Headers(QByteArray("\nFrom: a@b\n\nbody"));
        QVERIFY2(empty_section_lf.isEmpty(),
                 "the same holds for an LF-only message with an empty header section");

        const QMap<QString, QString> spaced =
            sak::mbox::parseRfc5322Headers(QByteArray("From : x\r\n\r\n"));
        QVERIFY(spaced.contains(QStringLiteral("from")));
        QCOMPARE(spaced.value(QStringLiteral("from")), QStringLiteral("x"));

        // The OTHER side of that trim. "From : x" only proves the trim runs on a name with a
        // surviving non-space core; a name that the trim consumes ENTIRELY was never fed. It is
        // reachable, because the fold check recognises only ' ' and '\t' as an indent while
        // QString::trimmed() also strips \v, \f and \r -- so "\v: x" is treated as a new header,
        // passes the raw-name guard, and is inserted under a key that trims to empty. The
        // documented output contract is that "an empty name is never emitted".
        for (const char* whitespace_name : {"\v: x\r\n\r\n", "\f: x\r\n\r\n"}) {
            const QMap<QString, QString> blank_name =
                sak::mbox::parseRfc5322Headers(QByteArray(whitespace_name));
            QVERIFY2(!blank_name.contains(QString()),
                     "a whitespace-only header name must never be emitted as an empty key");
            for (auto it = blank_name.constBegin(); it != blank_name.constEnd(); ++it) {
                QVERIFY2(!it.key().isEmpty(), "no emitted header name may be empty");
            }
        }
    }
};

QTEST_APPLESS_MAIN(MboxHeaderFuzzTests)
#include "test_fuzz_mbox_headers.moc"
