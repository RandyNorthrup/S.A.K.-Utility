// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_mbox_transfer_decoder.cpp
/// @brief Mutation-fuzz of the MIME Content-Transfer-Encoding decoders (G14-6).
///
/// The encoded body of a mail part is attacker-controlled. base64 decoding was a private
/// MboxParser member and quoted-printable a file-scope helper, so neither could be fuzzed
/// on its own; both were lifted to a pure seam (sak/mbox_transfer_decoder.h) with the
/// member delegating, behaviour unchanged. This harness feeds thousands of mutated
/// payloads through the decoders and asserts the contract for EVERY input:
///
///   * decodeQuotedPrintable never grows its input and never fails (a malformed "="
///     sequence is passed through).
///   * decodeTransferEncoding is strict for base64: on any invalid character it returns
///     ok == false with empty bytes (fail closed), never a partial decode; on success the
///     decoded output is no larger than the input.
///   * an unrecognized encoding passes the bytes through verbatim.
///
/// base64 and quoted-printable are textbook fuzz targets (partial-decode and malformed-
/// escape handling), and the strict-base64 fail-closed rule is a real security property:
/// a truncated body must not be handed onward as if it were complete. No crash / no hang
/// is implicit -- a fault never returns the empty invariant string.

#include "sak/mbox_transfer_decoder.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

constexpr int kExpectedCorpusSeeds = 13;
constexpr int kShippedFuzzIterations = 2000;  // tests/fuzz/fuzz_harness.h kDefaultIterations

std::vector<QByteArray> payloadCorpus() {
    return {
        QByteArray(),
        QByteArray("SGVsbG8gd29ybGQ="),      // base64 "Hello world"
        QByteArray("SGVsbG8=\r\nd29ybGQ="),  // wrapped base64, ILLEGAL padding mid-stream
        // A LEGITIMATELY wrapped payload. The seed above only looks like it covers the
        // line-unwrapping step: it stays invalid base64 even after unwrapping, so it is refused
        // either way. Every MIME base64 body is wrapped at 76 columns, so this is the shape that
        // actually runs in production.
        QByteArray("SGVsbG8g\r\nd29ybGQ="),
        QByteArray("Hello=20World=0A"),         // quoted-printable
        QByteArray("=E2=9C=93 check mark"),     // quoted-printable UTF-8
        QByteArray("plain text, no encoding"),  // passthrough
        QByteArray("===="),                     // malformed
        QByteArray("="),                        // lone equals
        QByteArray("=G1 not hex"),              // quoted-printable bad hex
        QByteArray("abc!@#$%^&*()"),            // invalid base64 characters
        QByteArray("A"),                        // invalid base64 length
        QByteArray("=\r\n"),                    // soft line break
    };
}

// base64: strict fail-closed, and success never grows.
QString checkBase64Contract(const QByteArray& input) {
    const sak::mbox::TransferDecodeResult b64 =
        sak::mbox::decodeTransferEncoding(input, QStringLiteral("base64"));
    if (!b64.ok && !b64.bytes.isEmpty()) {
        return QStringLiteral("base64 reported failure but returned %1 bytes")
            .arg(b64.bytes.size());
    }
    if (b64.ok && b64.bytes.size() > input.size()) {
        return QStringLiteral("base64 grew input: %1 -> %2")
            .arg(input.size())
            .arg(b64.bytes.size());
    }
    return {};
}

QString transferDecoderInvariant(const QByteArray& input) {
    // Quoted-printable: never grows, never fails.
    const QByteArray qp = sak::mbox::decodeQuotedPrintable(input);
    if (qp.size() > input.size()) {
        return QStringLiteral("quoted-printable grew input: %1 -> %2")
            .arg(input.size())
            .arg(qp.size());
    }
    // The other side of that contract, which is exactly knowable. "Never grows" is
    // one-directional: a decoder that DROPPED bytes, or returned an empty QByteArray for every
    // input, satisfied it everywhere. When the input contains no '=' at all, the decode IS the
    // identity -- the fast path at mbox_transfer_decoder.h:43-47 copies every byte, control and
    // high-bit bytes included -- and this fires on roughly a third of the mutants as well as four
    // of the seeds.
    if (!input.contains('=') && qp != input) {
        return QStringLiteral("quoted-printable altered an input that contains no '='");
    }

    const QString base64_verdict = checkBase64Contract(input);
    if (!base64_verdict.isEmpty()) {
        return base64_verdict;
    }

    // quoted-printable via the dispatcher matches the direct helper and always succeeds.
    const sak::mbox::TransferDecodeResult qpr =
        sak::mbox::decodeTransferEncoding(input, QStringLiteral("quoted-printable"));
    if (!qpr.ok || qpr.bytes != qp) {
        return QStringLiteral("quoted-printable dispatch diverged from the direct decoder");
    }

    // An unrecognized encoding passes the bytes through unchanged.
    const sak::mbox::TransferDecodeResult passthrough =
        sak::mbox::decodeTransferEncoding(input, QStringLiteral("7bit"));
    if (!passthrough.ok || passthrough.bytes != input) {
        return QStringLiteral("7bit passthrough altered the input");
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("MBOX transfer-decoder fuzz failed after %1 inputs: %2\n  bytes (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class MboxTransferDecoderFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void decodersHonorTheirContract() {
        const std::vector<QByteArray> corpus = payloadCorpus();
        const int budget = sak::fuzz::iterationsFromEnv();
        QVERIFY2(budget > 0, "the clamp must never hand run() a non-positive iteration budget");
        const sak::fuzz::FuzzOutcome outcome =
            sak::fuzz::run(corpus, transferDecoderInvariant, budget, sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. Both the
        // budget and the seed count are pinned to LITERALS: deriving either side from
        // corpus.size() or from a second iterationsFromEnv() call is self-satisfying, since both
        // sides then move together -- with a budget of 0 the mutation loop runs no iterations,
        // iterations_run equals the seed count, and seed_count + 0 still matches, and a seed
        // silently dropped from payloadCorpus() shrinks the fuzz surface with nothing to notice.
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);
        if (!qEnvironmentVariableIsSet("SAK_FUZZ_ITERS")) {
            QCOMPARE(budget, kShippedFuzzIterations);
        }
        QCOMPARE(outcome.iterations_run, kExpectedCorpusSeeds + budget);
    }

    // Exact-value coverage. The property fuzz above only checks that decoding never
    // grows the input and that base64 fails closed via the ok flag -- it never pins the
    // decoded VALUE, so a mutation to the =XX arithmetic (nibble shift, hex-digit offsets)
    // slips through undetected. These lock the decoded bytes for known inputs.
    void quotedPrintableDecodesExactBytes() {
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("Hello=20World=0A")),
                 QByteArray("Hello World\n"));
        QByteArray utf8 = QByteArray::fromHex("E29C93");
        utf8 += " check mark";
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("=E2=9C=93 check mark")), utf8);
        // LOWER-case hex. Both exact-value inputs above use upper case, so nothing pinned that
        // "=e2" decodes at all -- RFC 2045 decoders must accept either case, and the fuzz
        // invariant cannot see the difference because refusing lower case SHRINKS the output.
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("=e2=9c=93 check mark")), utf8);
        // The hex arm is `if (ok_first && ok_second)`. Every malformed-hex input in the file has
        // a bad FIRST digit, so ok_first is false, the second conjunct is short-circuited away
        // and never observed -- delete `&& ok_second` and everything stays green. A
        // good-first/bad-second pair is the only probe that reaches it.
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("=1G")), QByteArray("=1G"));
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("x=1Gy")), QByteArray("x=1Gy"));
    }

    /// A malformed "=" sequence is PASSED THROUGH -- the contract this file's header and the
    /// production doc comment both state, and which nothing asserted. The exact-value slots use
    /// only well-formed escapes, and the fuzz invariant only forbids growth, so dropping a byte
    /// on either recovery arm (the too-short tail and the non-hex fallthrough) was invisible by
    /// construction, even though the corpus deliberately carries these very seeds.
    void quotedPrintableMalformedEscapesArePassedThrough() {
        const QByteArray malformed[] = {
            QByteArray("="),            // lone equals: the too-short-tail arm
            QByteArray("=A"),           // still too short for a hex pair
            QByteArray("===="),         // every '=' falls through the non-hex arm
            QByteArray("=G1 not hex"),  // bad first hex digit
            QByteArray("plain=")        // a trailing '=' must survive, not be swallowed
        };
        for (const QByteArray& input : malformed) {
            QCOMPARE(sak::mbox::decodeQuotedPrintable(input), input);
        }
    }

    /// The dispatcher's two exact token compares, probed with NEAR MISSES. The only passthrough
    /// proof in the file used "7bit", which shares no prefix, suffix or substring with either
    /// "base64" or "quoted-printable", so loosening either compare to startsWith / contains / a
    /// truncated compare left it passing through exactly as before. A loosened compare would
    /// route an unknown encoding into a decoder and silently corrupt the part body.
    void encodingTokenMatchIsExact() {
        const QByteArray payload("SGVsbG8gd29ybGQ=");
        const QString near_misses[] = {
            QStringLiteral("base6"),               // truncation
            QStringLiteral("base64x"),             // extension
            QStringLiteral("xbase64"),             // embedding
            QStringLiteral("base-64"),             // separator
            QStringLiteral("quoted-printabl"),     // truncation
            QStringLiteral("quoted-printable-x"),  // extension
            QStringLiteral("x-quoted-printable"),  // embedding
        };
        for (const QString& token : near_misses) {
            const auto res = sak::mbox::decodeTransferEncoding(payload, token);
            QVERIFY2(res.ok, qPrintable(token));
            QVERIFY2(res.bytes == payload,
                     qPrintable(
                         QStringLiteral("token '%1' was treated as a known encoding").arg(token)));
        }
    }

    // A bare CR after "=" is a malformed soft line break: skip only the CR and keep the
    // byte that follows; a CRLF skips both. Distinguishes the '&&' soft-break guard from
    // an over-eager '||' that would also swallow the trailing byte.
    void quotedPrintableBareCrSoftBreakSkipsOnlyCr() {
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("A=\rB")), QByteArray("AB"));
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("A=\r\nB")), QByteArray("AB"));
        // The OUTER guard is `hex1 == '\r' || hex1 == '\n'`, and both assertions above enter it
        // through the '\r' arm -- the comment and the catalog mutant both concern the INNER
        // ternary. A bare LF soft break is the spelling every Unix mailer writes into an mbox,
        // and no assertion reached it: dropping that arm makes the output GROW toward, but never
        // past, the input length, so the size-bounded fuzz invariant cannot fire either.
        QCOMPARE(sak::mbox::decodeQuotedPrintable(QByteArray("A=\nB")), QByteArray("AB"));
    }

    /// base64 line-unwrapping. Every accept-path input in this file is whitespace-free, so the
    /// removeIf step that strips CR, LF, space and TAB before the strict decode was claimed by
    /// nothing -- and it is the step that actually runs in production, since every MIME base64
    /// body is wrapped at 76 columns. Deleting it turns every real wrapped body into a
    /// fail-closed refusal, i.e. an unreadable attachment.
    void base64UnwrapsFoldedLinesBeforeDecoding() {
        const QByteArray expected("Hello world");
        const QByteArray wrapped[] = {
            QByteArray("SGVsbG8g\r\nd29ybGQ="),  // CRLF fold, the MIME spelling
            QByteArray("SGVsbG8g\nd29ybGQ="),    // bare LF fold
            QByteArray("SGVsbG8g d29ybGQ="),     // space
            QByteArray("SGVsbG8g\td29ybGQ="),    // tab
            QByteArray("\r\nSGVsbG8gd29ybGQ=\r\n"),
        };
        for (const QByteArray& input : wrapped) {
            const auto res = sak::mbox::decodeTransferEncoding(input, QStringLiteral("base64"));
            QVERIFY2(res.ok, input.constData());
            QCOMPARE(res.bytes, expected);
        }
    }

    // SECURITY: strict base64 must fail closed on malformed input (ok == false, empty
    // bytes) rather than hand back a partial decode, and must decode valid input to the
    // exact bytes. Pins both the AbortOnBase64DecodingErrors flag and the fail-closed
    // {{}, false} return -- the property fuzz asserts neither on a known-bad input.
    void base64IsStrictAndFailsClosed() {
        const auto good = sak::mbox::decodeTransferEncoding(QByteArray("SGVsbG8gd29ybGQ="),
                                                            QStringLiteral("base64"));
        QVERIFY(good.ok);
        QCOMPARE(good.bytes, QByteArray("Hello world"));

        const QByteArray bad_inputs[] = {QByteArray("abc!@#$%^&*()"), QByteArray("SGVsbG8*")};
        for (const QByteArray& bad : bad_inputs) {
            const auto res = sak::mbox::decodeTransferEncoding(bad, QStringLiteral("base64"));
            QVERIFY2(!res.ok, bad.constData());
            QVERIFY(res.bytes.isEmpty());
        }
    }

    // Content-Transfer-Encoding tokens are case-insensitive per RFC 2045; an upper-case
    // "BASE64" must still decode, not fall through to verbatim passthrough.
    void encodingTokenMatchIsCaseInsensitive() {
        const auto res = sak::mbox::decodeTransferEncoding(QByteArray("SGVsbG8="),
                                                           QStringLiteral("BASE64"));
        QVERIFY(res.ok);
        QCOMPARE(res.bytes, QByteArray("Hello"));
    }
};

QTEST_APPLESS_MAIN(MboxTransferDecoderFuzzTests)
#include "test_fuzz_mbox_transfer_decoder.moc"
