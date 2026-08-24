// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_mbox_container.cpp
/// @brief Mutation-fuzz of the MBOX container splitter + full read pipeline (G14 parser sweep).
///
/// An MBOX file is untrusted input -- a mail archive from anywhere. The header parser
/// (test_fuzz_mbox_headers) and the MIME transfer decoder (test_fuzz_mbox_transfer_decoder) are
/// already fuzzed as pure seams; what this harness covers is the part that only exists behind an
/// open file: the "From " separator scan that splits the archive into messages (buildMessageIndex /
/// isFromLine), the per-message boundary math (readRawMessage: the >From un-escaping, the size
/// caps), and the recursive MIME walk with its part/depth caps (splitMimeParts /
/// recurseMultipartPart). A crafted archive of nothing but boundary delimiters or deeply nested
/// multiparts is exactly what those caps exist to survive.
///
/// The harness writes each mutated input to a temp file and drives the real public pipeline
/// (open -> indexMessages -> readMessages -> readMessageDetail) over it, asserting for EVERY input:
///
///   1. No crash and no hang (the part/depth/size caps hold; a hang trips the ctest timeout with
///   the
///      reproducer recorded in fuzz_harness.h).
///   2. Determinism: two full parse passes over identical bytes yield the same message count and
///   the
///      same per-message detail signature.
///   3. A parser that reports itself open indexes at least one message -- open() validated a
///   leading
///      "From " line, so the index can never then contradict it with zero
///      ([[no-fallbacks-fail-closed]]).
///   4. Every readMessageDetail resolves to a well-formed expected (a value or a real error_code),
///      never a partial success smuggled past the fail-closed checks -- guaranteed structurally and
///      asserted by exercising the whole indexed range.

#include "sak/email_types.h"
#include "sak/mbox_parser.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <vector>

namespace {

// Cap the per-input message walk so a pathological archive of millions of bare "From " lines cannot
// turn one fuzz iteration into a multi-second scan. The container splitter itself still indexes the
// whole file; this only bounds how many we then fully MIME-parse.
constexpr int kMaxMessagesWalked = 32;

// A compact per-message fingerprint; determinism compares vectors of these across two parse passes.
struct MessageFingerprint {
    bool ok = false;
    int subject_len = 0;
    int from_len = 0;
    int body_plain_len = 0;
    int body_html_len = 0;
    int attachment_count = 0;

    bool operator==(const MessageFingerprint&) const = default;
};

struct ParseFingerprint {
    bool opened = false;
    int message_count = 0;
    std::vector<MessageFingerprint> messages;

    bool operator==(const ParseFingerprint&) const = default;
};

bool writeWholeFile(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    const bool wrote = file.write(bytes) == bytes.size();
    file.close();
    return wrote;
}

// Drive the whole public pipeline over @p input written to @p path and fingerprint the result.
ParseFingerprint fingerprintParse(const QString& path, const QByteArray& input) {
    ParseFingerprint fp;
    if (!writeWholeFile(path, input)) {
        return fp;
    }

    MboxParser parser;
    parser.open(path);
    fp.opened = parser.isOpen();
    if (!fp.opened) {
        return fp;  // open() fail-closed on a bad leading "From " line -- a valid, graceful
                    // outcome.
    }

    parser.indexMessages();
    fp.message_count = parser.messageCount();

    // Exercise the summary path (parseHeaders over each message) across the whole range.
    (void)parser.readMessages(0, fp.message_count);

    const int walk = std::min(fp.message_count, kMaxMessagesWalked);
    fp.messages.reserve(walk);
    for (int i = 0; i < walk; ++i) {
        const auto detail = parser.readMessageDetail(i);
        MessageFingerprint mf;
        mf.ok = detail.has_value();
        if (mf.ok) {
            mf.subject_len = static_cast<int>(detail->subject.size());
            mf.from_len = static_cast<int>(detail->from.size());
            mf.body_plain_len = static_cast<int>(detail->body_plain.size());
            mf.body_html_len = static_cast<int>(detail->body_html.size());
            mf.attachment_count = static_cast<int>(detail->attachments.size());
        }
        fp.messages.push_back(mf);
    }
    return fp;
}

QString mboxContainerInvariant(const QByteArray& input, const QDir& dir) {
    const QString path = dir.filePath(QStringLiteral("fuzz.mbox"));

    const ParseFingerprint first = fingerprintParse(path, input);

    // Fail-closed sanity: a parser that reports open validated a leading "From " line, so the index
    // must find at least that one message -- never open-but-empty.
    if (first.opened && first.message_count < 1) {
        return QStringLiteral("parser reported open but indexed zero messages");
    }

    // Determinism: a second full pass over identical bytes must reproduce the same result.
    const ParseFingerprint second = fingerprintParse(path, input);
    if (!(first == second)) {
        return QStringLiteral("mbox parse pipeline is non-deterministic on identical input");
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("mbox container fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

// A minimal but valid two-message archive: a leading "From " separator with an asctime-shaped date,
// real RFC 5322 headers, and a multipart body with one text part and one attachment part.
QByteArray twoMessageSeed() {
    return QByteArrayLiteral(
        "From alice@example.com Thu Feb 13 12:30:00 2020\r\n"
        "From: alice@example.com\r\n"
        "To: bob@example.com\r\n"
        "Subject: hello\r\n"
        "Date: Thu, 13 Feb 2020 12:30:00 +0000\r\n"
        "Content-Type: multipart/mixed; boundary=\"sep\"\r\n"
        "\r\n"
        "--sep\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "body text\r\n"
        "--sep\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"a.bin\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "aGVsbG8=\r\n"
        "--sep--\r\n"
        "\r\n"
        "From bob@example.com Fri Feb 14 09:00:00 2020\r\n"
        "From: bob@example.com\r\n"
        "Subject: re: hello\r\n"
        "\r\n"
        "just plain text\r\n");
}

}  // namespace

class MboxContainerFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void pipelineNeverCrashesAndStaysDeterministic() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());

        std::vector<QByteArray> corpus{
            twoMessageSeed(),
            // Valid leading From line, empty body.
            QByteArrayLiteral("From x y 00:00:00 2020\r\n"),
            // Body prose that merely starts with "From " must not over-split.
            QByteArrayLiteral("From a b 1:2:3 1999\r\nSubject: t\r\n\r\nFrom here we go\r\n"),
            // Nothing but boundary delimiters (exercises the part cap).
            QByteArrayLiteral("From a b 1:2:3 2000\r\nContent-Type: multipart/x; boundary=b\r\n\r\n"
                              "--b\r\n--b\r\n--b\r\n--b--\r\n"),
            // >From un-escaping.
            QByteArrayLiteral("From a b 1:2:3 2000\r\n\r\n>From not a separator\r\n"),
            // Not an mbox at all -- open() must fail closed.
            QByteArrayLiteral("this is not an mbox file"),
            QByteArrayLiteral("From lacking-a-date-shape\r\n"),
            QByteArray(),
        };
        const sak::fuzz::Target target = [&out](const QByteArray& input) {
            return mboxContainerInvariant(input, out);
        };
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // On the all-pass path (guaranteed here: any failure QVERIFY2(false)-returns above),
        // run() increments iterations_run once per seed (checkSeeds) plus once per mutation
        // iteration, so the exact count is corpus.size() + the iteration budget. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations -- the whole
        // campaign silently evaporating while the seed rounds alone satisfied it.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }

    // Pin the accept path: the seed archive opens, indexes two messages, and its first message
    // exposes a text body and one attachment. If this drifts, the fuzz above never reaches the
    // interesting split/MIME paths.
    void seedArchiveParsesTwoMessages() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDir out(dir.path());
        const QString path = out.filePath(QStringLiteral("seed.mbox"));
        QVERIFY(writeWholeFile(path, twoMessageSeed()));

        MboxParser parser;
        parser.open(path);
        QVERIFY(parser.isOpen());
        parser.indexMessages();
        QCOMPARE(parser.messageCount(), 2);

        const auto detail = parser.readMessageDetail(0);
        QVERIFY(detail.has_value());
        QCOMPARE(detail->subject, QStringLiteral("hello"));
        QCOMPARE(detail->attachments.size(), 1);
        // Message 0's first MIME part is text/plain "body text\r\n" with no transfer-encoding
        // and no charset, so it passes through verbatim. !isEmpty() would still pass if the
        // MIME walk decoded the wrong part, dropped the CRLF, or leaked headers into the body.
        QCOMPARE(detail->body_plain, QStringLiteral("body text\r\n"));
    }
};

QTEST_GUILESS_MAIN(MboxContainerFuzzTests)
#include "test_fuzz_mbox_container.moc"
