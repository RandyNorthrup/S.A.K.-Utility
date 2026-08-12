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

std::vector<QByteArray> payloadCorpus() {
    return {
        QByteArray(),
        QByteArray("SGVsbG8gd29ybGQ="),         // base64 "Hello world"
        QByteArray("SGVsbG8=\r\nd29ybGQ="),     // wrapped base64
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

QString transferDecoderInvariant(const QByteArray& input) {
    // Quoted-printable: never grows, never fails.
    const QByteArray qp = sak::mbox::decodeQuotedPrintable(input);
    if (qp.size() > input.size()) {
        return QStringLiteral("quoted-printable grew input: %1 -> %2")
            .arg(input.size())
            .arg(qp.size());
    }

    // base64: strict fail-closed, and success never grows.
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
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(corpus,
                                                              transferDecoderInvariant,
                                                              sak::fuzz::iterationsFromEnv(),
                                                              sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_APPLESS_MAIN(MboxTransferDecoderFuzzTests)
#include "test_fuzz_mbox_transfer_decoder.moc"
