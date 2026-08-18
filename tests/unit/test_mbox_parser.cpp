// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_parser.cpp
/// @brief Unit tests for MBOX file parser

#include "sak/email_constants.h"
#include "sak/mbox_parser.h"

#include <QFuture>
#include <QSignalSpy>
#include <QtConcurrent>
#include <QTemporaryFile>
#include <QtTest/QtTest>

#include <atomic>
#include <type_traits>

// R5-G14-19: the message-index / attachment-index guarantee is a COMPILE-TIME gate.
// Swapping the two arguments, or passing a bare int for either, must not compile; if
// that regresses this file stops building, which is what makes the silent swap
// readAttachmentData(message, attachment) named by the finding impossible to ship.
static_assert(std::is_invocable_v<decltype(&MboxParser::readAttachmentData),
                                  MboxParser&,
                                  sak::MboxMessageIndex,
                                  sak::MboxAttachmentIndex>,
              "readAttachmentData must accept (MboxMessageIndex, MboxAttachmentIndex)");
static_assert(!std::is_invocable_v<decltype(&MboxParser::readAttachmentData),
                                   MboxParser&,
                                   sak::MboxAttachmentIndex,
                                   sak::MboxMessageIndex>,
              "swapped index types must be a compile error -- the silent swap this closes");
static_assert(
    !std::is_invocable_v<decltype(&MboxParser::readAttachmentData), MboxParser&, int, int>,
    "a bare int must not implicitly become a message or attachment index");
// Mixed cases: a bare int must be refused even when the OTHER argument is correctly typed,
// so a caller cannot type one index and slip a raw int in for the other.
static_assert(!std::is_invocable_v<decltype(&MboxParser::readAttachmentData),
                                   MboxParser&,
                                   int,
                                   sak::MboxAttachmentIndex>,
              "a bare int must not stand in for the message index");
static_assert(!std::is_invocable_v<decltype(&MboxParser::readAttachmentData),
                                   MboxParser&,
                                   sak::MboxMessageIndex,
                                   int>,
              "a bare int must not stand in for the attachment index");

class TestMboxParser : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Lifecycle -------------------------------------------------------
    void defaultState();
    void openNonExistentFile();
    void openEmptyFile();
    void openAndClose();
    void doubleCloseIsHarmless();
    void cancelDoesNotCrash();

    // -- Indexing ---------------------------------------------------------
    void indexSingleMessage();
    void indexMultipleMessages();
    void emptyFileReturnsZeroMessages();

    // -- Message Loading -------------------------------------------------
    void loadMessagesFromValidFile();
    void loadMessageDetailFromValidFile();
    void readMessagesSync();
    void readMessageDetailSync();
    void nestedMultipartRecoversBodyAndAttachment();
    void readAttachmentDataAlignsWithRecursiveEnumeration();

    // -- Concurrency (B7-02) ---------------------------------------------
    void concurrentReadsDoNotRace();

    // -- From Line Detection ---------------------------------------------
    void fromLineDetectsValidSeparator();

    // -- Error Cases -----------------------------------------------------
    void loadMessagesWhenClosed();
    void loadDetailOutOfRange();

    // -- B7-24: a stale cancel must not block later reads ----------------
    void cancelDoesNotBlockLaterReads();

    // -- B7-33: MIME boundary + single-part attachment parsing -----------
    void singlePartAttachmentNotTreatedAsBody();
    void boundaryPrefixLineNotTreatedAsBoundary();

    // -- CODEX_REVIEW_2 remediation --------------------------------------
    void summaryAttachmentHeuristicBroadened();
    void quotedFilenameWithSpacesPreserved();
    void malformedBase64AttachmentFailsClosed();
    void fromPrefixedBodyLineIsNotASeparator();
    void multipartWithManyPartsSplitsCorrectly();

    // -- Trailing-part flush on a delimiter-truncated multipart body -----
    void trailingPartRecoveredWhenClosingDelimiterMissing();

private:
    /// Create a temp file with valid MBOX content
    QTemporaryFile* createSampleMboxFile();
};

// ============================================================================
// Lifecycle
// ============================================================================

void TestMboxParser::defaultState() {
    MboxParser parser;
    QVERIFY(!parser.isOpen());
    QCOMPARE(parser.messageCount(), 0);
}

void TestMboxParser::openNonExistentFile() {
    MboxParser parser;
    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
    parser.open(QStringLiteral("C:/nonexistent_mbox_xyz.mbox"));
    QVERIFY(error_spy.count() > 0);
    QVERIFY(!parser.isOpen());
}

void TestMboxParser::openEmptyFile() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.close();

    MboxParser parser;
    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
    parser.open(temp_file.fileName());

    // An empty file has no "From " separator, so open() fails closed: it reports the
    // reason and leaves the parser shut instead of accepting an "empty mailbox".
    QVERIFY(!parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    QVERIFY2(error_spy.first().at(0).toString().contains(QStringLiteral("Not a valid MBOX file")),
             qPrintable(error_spy.first().at(0).toString()));
    QCOMPARE(parser.messageCount(), 0);
}

void TestMboxParser::openAndClose() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);
    MboxParser parser;
    parser.open(temp_file->fileName());

    // The sample file starts with a well-formed "From " separator, so the open
    // succeeds; close() must then put the parser back in the closed state.
    QVERIFY(parser.isOpen());
    parser.close();
    QVERIFY(!parser.isOpen());
    delete temp_file;
}

void TestMboxParser::doubleCloseIsHarmless() {
    MboxParser parser;
    parser.close();
    QVERIFY(!parser.isOpen());

    // Closing an OPEN parser twice must not double-close the QFile, and each close
    // must drop the index rather than leave a count that no longer has a file
    // behind it.
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);
    parser.open(temp_file->fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 2);

    parser.close();
    parser.close();
    QVERIFY(!parser.isOpen());
    QCOMPARE(parser.messageCount(), 0);
    delete temp_file;
}

void TestMboxParser::cancelDoesNotCrash() {
    // cancel() only raises the atomic flag (it deliberately does not take the file
    // mutex), so on a parser that never opened anything it must be inert: the
    // parser stays closed, empty, and its synchronous reads still refuse cleanly.
    MboxParser parser;
    parser.cancel();
    QVERIFY(!parser.isOpen());
    QCOMPARE(parser.messageCount(), 0);
    QVERIFY(!parser.readMessages(0, 10).has_value());
    QVERIFY(!parser.readMessageDetail(0).has_value());
}

// ============================================================================
// Indexing
// ============================================================================

void TestMboxParser::indexSingleMessage() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    QSignalSpy opened_spy(&parser, &MboxParser::fileOpened);
    parser.open(temp_file->fileName());

    QVERIFY(parser.isOpen());
    QCOMPARE(opened_spy.count(), 1);
    QCOMPARE(opened_spy.first().at(0).toString(), temp_file->fileName());

    // indexMessages() runs synchronously, so indexingComplete has already fired by
    // the time it returns, and it carries the same count messageCount() reports --
    // exactly the two messages createSampleMboxFile() writes.
    QSignalSpy index_spy(&parser, &MboxParser::indexingComplete);
    parser.indexMessages();
    QCOMPARE(index_spy.count(), 1);
    QCOMPARE(index_spy.first().at(0).toInt(), 2);
    QCOMPARE(parser.messageCount(), 2);

    parser.close();
    delete temp_file;
}

void TestMboxParser::indexMultipleMessages() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());

    QByteArray content;
    for (int index = 0; index < 3; ++index) {
        content += QByteArray("From sender@example.com Mon Jan  1 00:00:00 2024\r\n");
        content += QByteArray("From: sender@example.com\r\n");
        content += QByteArray("To: recipient@example.com\r\n");
        content += QByteArray("Subject: Test Message ") + QByteArray::number(index + 1) +
                   QByteArray("\r\n");
        content += QByteArray("Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n");
        content += QByteArray("\r\n");
        content += QByteArray("Body of message ") + QByteArray::number(index + 1) +
                   QByteArray("\r\n\r\n");
    }
    temp_file.write(content);
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());

    QVERIFY(parser.isOpen());
    parser.indexMessages();
    // Exactly the 3 messages written above: the "From: " header lines inside each
    // message must not be counted as extra separators.
    QCOMPARE(parser.messageCount(), 3);
    parser.close();
}

void TestMboxParser::emptyFileReturnsZeroMessages() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());
    QVERIFY(!parser.isOpen());  // an empty file is rejected at open (no "From " line)

    // Indexing without an open file must say so and leave the count at zero, rather
    // than quietly indexing nothing and passing for an empty mailbox.
    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
    parser.indexMessages();
    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(error_spy.first().at(0).toString(), QStringLiteral("No file is open"));
    QCOMPARE(parser.messageCount(), 0);
    parser.close();
}

// ============================================================================
// Message Loading
// ============================================================================

void TestMboxParser::loadMessagesFromValidFile() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QSignalSpy msg_spy(&parser, &MboxParser::messagesLoaded);
    parser.loadMessages(0, 10);

    // Both sample messages come back, in file order, with their headers parsed --
    // and the emitted total matches what messageCount() indexed.
    QCOMPARE(msg_spy.count(), 1);
    const auto messages = msg_spy.at(0).at(0).value<QVector<sak::MboxMessage>>();
    QCOMPARE(messages.size(), 2);
    QCOMPARE(messages.at(0).subject, QStringLiteral("Hello World"));
    QCOMPARE(messages.at(1).subject, QStringLiteral("Second Message"));
    QCOMPARE(msg_spy.at(0).at(1).toInt(), 2);

    parser.close();
    delete temp_file;
}

void TestMboxParser::loadMessageDetailFromValidFile() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QSignalSpy detail_spy(&parser, &MboxParser::messageDetailLoaded);
    parser.loadMessageDetail(0);

    // The detail carries message 0's own headers and body -- not the next message's
    // (the raw slice ends at the following "From " separator).
    QCOMPARE(detail_spy.count(), 1);
    const auto detail = detail_spy.at(0).at(0).value<sak::MboxMessageDetail>();
    QCOMPARE(detail.subject, QStringLiteral("Hello World"));
    QCOMPARE(detail.from, QStringLiteral("Test Sender <sender@example.com>"));
    QVERIFY(detail.body_plain.contains(QStringLiteral("This is the body of the test email.")));
    QVERIFY(!detail.body_plain.contains(QStringLiteral("Body of the second email message.")));

    parser.close();
    delete temp_file;
}

void TestMboxParser::readMessagesSync() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    QVERIFY(parser.isOpen());
    parser.indexMessages();
    const auto result = parser.readMessages(0, 10);
    QVERIFY(result.has_value());
    QCOMPARE(result->size(), 2);
    QCOMPARE(result->at(0).message_index, 0);
    QCOMPARE(result->at(0).subject, QStringLiteral("Hello World"));
    QCOMPARE(result->at(1).message_index, 1);
    QCOMPARE(result->at(1).subject, QStringLiteral("Second Message"));
    // text/plain with no attachment disposition: the summary flag must stay false.
    QVERIFY(!result->at(0).has_attachments);
    parser.close();
    delete temp_file;
}

void TestMboxParser::readMessageDetailSync() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    QVERIFY(parser.isOpen());
    parser.indexMessages();
    const auto result = parser.readMessageDetail(1);
    QVERIFY(result.has_value());
    QCOMPARE(result->message_index, 1);
    QCOMPARE(result->subject, QStringLiteral("Second Message"));
    QCOMPARE(result->from, QStringLiteral("Another Sender <another@example.com>"));
    QCOMPARE(result->message_id, QStringLiteral("<test-002@example.com>"));
    QVERIFY(result->body_plain.contains(QStringLiteral("Body of the second email message.")));
    parser.close();
    delete temp_file;
}

void TestMboxParser::nestedMultipartRecoversBodyAndAttachment() {
    // A multipart/mixed whose first part is itself a multipart/alternative:
    // without recursion the inner text bodies are lost entirely.
    QByteArray content =
        "From sender@example.com Mon Jan  1 00:00:00 2024\r\n"
        "Subject: Nested\r\n"
        "Content-Type: multipart/mixed; boundary=\"OUTER\"\r\n"
        "\r\n"
        "--OUTER\r\n"
        "Content-Type: multipart/alternative; boundary=\"INNER\"\r\n"
        "\r\n"
        "--INNER\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "Hello plain body\r\n"
        "--INNER\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<p>Hello html body</p>\r\n"
        "--INNER--\r\n"
        "--OUTER\r\n"
        "Content-Type: application/octet-stream\r\n"
        "Content-Disposition: attachment; filename=\"a.bin\"\r\n"
        "\r\n"
        "payload-bytes\r\n"
        "--OUTER--\r\n";

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(content);
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 1);

    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    QVERIFY(detail->body_plain.contains(QStringLiteral("Hello plain body")));
    QVERIFY(detail->body_html.contains(QStringLiteral("Hello html body")));
    QCOMPARE(detail->attachments.size(), 1);
    QCOMPARE(detail->attachments.first().long_filename, QStringLiteral("a.bin"));
    parser.close();
}

// Regression: readAttachmentData's index must mean the SAME attachment as readMessageDetail's list,
// even when a nested multipart contains attachments. Shape: multipart/mixed { multipart/related {
// text/html ; image/png inline "pic.png" = "ABC" } ; application/pdf attachment "report.pdf" =
// "DEF" }. readMessageDetail enumerates recursively -> [pic.png, report.pdf]. The old
// readAttachmentData counted only top-level parts, so index 0 wrongly returned the PDF and index 1
// ran off the end.
void TestMboxParser::readAttachmentDataAlignsWithRecursiveEnumeration() {
    QByteArray content =
        "From sender@example.com Mon Jan  1 00:00:00 2024\r\n"
        "Subject: Nested attach\r\n"
        "MIME-Version: 1.0\r\n"
        "Content-Type: multipart/mixed; boundary=\"OUT\"\r\n"
        "\r\n"
        "--OUT\r\n"
        "Content-Type: multipart/related; boundary=\"IN\"\r\n"
        "\r\n"
        "--IN\r\n"
        "Content-Type: text/html\r\n"
        "\r\n"
        "<p>hi</p>\r\n"
        "--IN\r\n"
        "Content-Type: image/png\r\n"
        "Content-Disposition: inline; filename=\"pic.png\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "QUJD\r\n"
        "--IN--\r\n"
        "--OUT\r\n"
        "Content-Type: application/pdf\r\n"
        "Content-Disposition: attachment; filename=\"report.pdf\"\r\n"
        "Content-Transfer-Encoding: base64\r\n"
        "\r\n"
        "REVG\r\n"
        "--OUT--\r\n";

    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(content);
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());
    QVERIFY(parser.isOpen());

    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    QCOMPARE(detail->attachments.size(), 2);
    QCOMPARE(detail->attachments.at(0).long_filename, QStringLiteral("pic.png"));
    QCOMPARE(detail->attachments.at(1).long_filename, QStringLiteral("report.pdf"));

    // Bytes at each index must match the name at that index -- not the old top-level-only mapping.
    auto bytes0 = parser.readAttachmentData(sak::MboxMessageIndex{0}, sak::MboxAttachmentIndex{0});
    QVERIFY(bytes0.has_value());
    QCOMPARE(*bytes0, QByteArray("ABC"));
    auto bytes1 = parser.readAttachmentData(sak::MboxMessageIndex{0}, sak::MboxAttachmentIndex{1});
    QVERIFY(bytes1.has_value());
    QCOMPARE(*bytes1, QByteArray("DEF"));

    // Out-of-range index is rejected.
    QVERIFY(!parser.readAttachmentData(sak::MboxMessageIndex{0}, sak::MboxAttachmentIndex{2})
                 .has_value());
    parser.close();
}

// ============================================================================
// Concurrency (B7-02): concurrent attachment/detail reads share one QFile cursor
// and offsets. Before serialization they raced -> one message's bytes/subject
// were returned for another's index. The parser now guards all file access with
// a recursive mutex; every concurrent read must return its OWN index's data.
// ============================================================================

void TestMboxParser::concurrentReadsDoNotRace() {
    constexpr int kMsgs = 8;
    QByteArray content;
    QVector<QByteArray> expectedPayloads;
    QVector<QString> expectedSubjects;
    for (int i = 0; i < kMsgs; ++i) {
        const QByteArray payload = QByteArray("PAYLOAD-") + QByteArray::number(i) +
                                   "-0123456789abcdef";
        expectedPayloads.append(payload);
        expectedSubjects.append(QStringLiteral("Msg%1").arg(i));
        content += "From s@e.com Mon Jan  1 00:00:00 2024\r\n";
        content += "Subject: Msg" + QByteArray::number(i) + "\r\n";
        content += "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n";
        content += "--B\r\n";
        content += "Content-Type: application/octet-stream\r\n";
        content += "Content-Disposition: attachment; filename=\"a" + QByteArray::number(i) +
                   ".bin\"\r\n";
        content += "Content-Transfer-Encoding: base64\r\n\r\n";
        content += payload.toBase64() + "\r\n";
        content += "--B--\r\n";
    }

    QTemporaryFile temp;
    QVERIFY(temp.open());
    temp.write(content);
    temp.close();

    MboxParser parser;
    parser.open(temp.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), kMsgs);

    // Hammer the shared parser from many pool threads. Each task reads a specific message's
    // attachment AND detail and checks BOTH belong to that same index.
    std::atomic<int> mismatches{0};
    QList<QFuture<void>> futures;
    for (int t = 0; t < 256; ++t) {
        const int idx = t % kMsgs;
        futures.append(
            QtConcurrent::run([&parser, idx, &mismatches, &expectedPayloads, &expectedSubjects]() {
                const auto bytes = parser.readAttachmentData(sak::MboxMessageIndex{idx},
                                                             sak::MboxAttachmentIndex{0});
                if (!bytes || *bytes != expectedPayloads[idx]) {
                    mismatches.fetch_add(1);
                    return;
                }
                const auto detail = parser.readMessageDetail(idx);
                if (!detail || detail->subject != expectedSubjects[idx]) {
                    mismatches.fetch_add(1);
                }
            }));
    }
    for (auto& f : futures) {
        f.waitForFinished();
    }

    QCOMPARE(mismatches.load(), 0);
    parser.close();
}

// ============================================================================
// From Line Detection
// ============================================================================

void TestMboxParser::fromLineDetectsValidSeparator() {
    // "From " followed by a sender and an asctime date is a valid MBOX separator;
    // the RFC 5322 "From:" header is not. isFromLine() is private and static, so
    // both cases are driven through open()/indexMessages().
    const QByteArray valid_line = "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    const QByteArray invalid_line = "From: sender@example.com\r\n";

    QTemporaryFile valid_file;
    QVERIFY(valid_file.open());
    valid_file.write(valid_line);
    valid_file.write("Subject: Test\r\n\r\nBody text\r\n");
    valid_file.close();

    MboxParser parser;
    parser.open(valid_file.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 1);
    parser.close();

    // A file whose first line is a "From:" HEADER is not an MBOX: the separator
    // check must reject it at open rather than treat the header as message one.
    QTemporaryFile invalid_first_line;
    QVERIFY(invalid_first_line.open());
    invalid_first_line.write(invalid_line);
    invalid_first_line.write("Subject: Test\r\n\r\nBody text\r\n");
    invalid_first_line.close();

    MboxParser rejecting_parser;
    QSignalSpy error_spy(&rejecting_parser, &MboxParser::errorOccurred);
    rejecting_parser.open(invalid_first_line.fileName());
    QVERIFY(!rejecting_parser.isOpen());
    QCOMPARE(error_spy.count(), 1);
    QCOMPARE(rejecting_parser.messageCount(), 0);
}

// ============================================================================
// Error Cases
// ============================================================================

void TestMboxParser::loadMessagesWhenClosed() {
    MboxParser parser;
    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
    QSignalSpy loaded_spy(&parser, &MboxParser::messagesLoaded);

    // Loading without an open file must fail closed: report the failure and emit no
    // messagesLoaded, so a view never renders an empty list as a successful load.
    parser.loadMessages(0, 10);
    QCOMPARE(error_spy.count(), 1);
    QVERIFY2(error_spy.first().at(0).toString().startsWith(
                 QStringLiteral("Failed to load messages")),
             qPrintable(error_spy.first().at(0).toString()));
    QCOMPARE(loaded_spy.count(), 0);
}

void TestMboxParser::loadDetailOutOfRange() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
    QSignalSpy detail_spy(&parser, &MboxParser::messageDetailLoaded);
    // Request an out-of-range index: the read is rejected, so the failure is
    // reported and no (stale or empty) detail is emitted.
    parser.loadMessageDetail(99'999);
    QCOMPARE(error_spy.count(), 1);
    QVERIFY2(error_spy.first().at(0).toString().startsWith(
                 QStringLiteral("Failed to load message detail")),
             qPrintable(error_spy.first().at(0).toString()));
    QCOMPARE(detail_spy.count(), 0);

    parser.close();
    delete temp_file;
}

// ============================================================================
// Helpers
// ============================================================================

QTemporaryFile* TestMboxParser::createSampleMboxFile() {
    auto* temp_file = new QTemporaryFile();
    if (!temp_file->open()) {
        delete temp_file;
        return nullptr;
    }

    QByteArray content;
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: Test Sender <sender@example.com>\r\n";
    content += "To: Test Recipient <recipient@example.com>\r\n";
    content += "Subject: Hello World\r\n";
    content += "Date: Mon, 01 Jan 2024 00:00:00 +0000\r\n";
    content += "Message-ID: <test-001@example.com>\r\n";
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "\r\n";
    content += "This is the body of the test email.\r\n";
    content += "\r\n";
    content += "From another@example.com Tue Jan  2 12:00:00 2024\r\n";
    content += "From: Another Sender <another@example.com>\r\n";
    content += "To: Test Recipient <recipient@example.com>\r\n";
    content += "Subject: Second Message\r\n";
    content += "Date: Tue, 02 Jan 2024 12:00:00 +0000\r\n";
    content += "Message-ID: <test-002@example.com>\r\n";
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "\r\n";
    content += "Body of the second email message.\r\n";

    temp_file->write(content);
    temp_file->close();
    return temp_file;
}

void TestMboxParser::cancelDoesNotBlockLaterReads() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    // Simulate a cancel of a previous operation (e.g. controller cancelOperation()).
    parser.cancel();

    // A fresh read must succeed, not return operation_cancelled: the stale cancel
    // flag is cleared at the start of the read (B7-24).
    auto messages = parser.readMessages(0, 10);
    QVERIFY2(messages.has_value(),
             "read after a stale cancel must not fail with operation_cancelled");
    QCOMPARE(messages->size(), 2);

    parser.cancel();
    auto detail = parser.readMessageDetail(0);
    QVERIFY2(detail.has_value(), "detail read after a stale cancel must not fail");
    QCOMPARE(detail->subject, QStringLiteral("Hello World"));

    parser.close();
    delete temp_file;
}

// A single-part message that is itself a file (application/pdf marked as an
// attachment) must surface as an attachment, not as garbled body text (B7-33).
void TestMboxParser::singlePartAttachmentNotTreatedAsBody() {
    QTemporaryFile f;
    QVERIFY(f.open());
    QByteArray c;
    c += "From x@example.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: A PDF\r\n";
    c += "MIME-Version: 1.0\r\n";
    c += "Content-Type: application/pdf; name=\"doc.pdf\"\r\n";
    c += "Content-Transfer-Encoding: base64\r\n";
    c += "Content-Disposition: attachment; filename=\"doc.pdf\"\r\n";
    c += "\r\n";
    c += "SGVsbG8gUERG\r\n";  // "Hello PDF"
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    QCOMPARE(detail->attachments.size(), 1);
    QCOMPARE(detail->attachments.first().long_filename, QStringLiteral("doc.pdf"));
    QVERIFY(detail->body_plain.isEmpty());  // NOT dumped into the body
    parser.close();
}

// A body line that merely shares the boundary PREFIX ("--ABCDEF" vs boundary
// "ABC") must not be treated as a MIME boundary and split the message (B7-33).
void TestMboxParser::boundaryPrefixLineNotTreatedAsBoundary() {
    QTemporaryFile f;
    QVERIFY(f.open());
    QByteArray c;
    c += "From x@example.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: Multipart\r\n";
    c += "MIME-Version: 1.0\r\n";
    c += "Content-Type: multipart/mixed; boundary=\"ABC\"\r\n";
    c += "\r\n";
    c += "--ABC\r\n";
    c += "Content-Type: text/plain\r\n";
    c += "\r\n";
    c += "Line one.\r\n";
    c += "--ABCDEF is not a boundary.\r\n";
    c += "Line three.\r\n";
    c += "--ABC--\r\n";
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();

    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    // The whole part body survived: the prefix-sharing line stayed in the text.
    QVERIFY(detail->body_plain.contains(QStringLiteral("--ABCDEF is not a boundary.")));
    QVERIFY(detail->body_plain.contains(QStringLiteral("Line three.")));
    parser.close();
}

// Summary has_attachments must match what parseMimeMessage would surface: any multipart, an
// explicit attachment disposition, or a lone non-text part -- not just multipart/mixed.
void TestMboxParser::summaryAttachmentHeuristicBroadened() {
    QByteArray content;
    content += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    content += "Subject: plain\r\n";
    content += "Content-Type: text/plain\r\n\r\n";
    content += "just text\r\n\r\n";
    content += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    content += "Subject: pdf\r\n";
    content += "Content-Type: application/pdf\r\n\r\n";
    content += "%PDF-1.4\r\n\r\n";
    content += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    content += "Subject: related\r\n";
    content += "Content-Type: multipart/related; boundary=\"B\"\r\n\r\n";
    content += "--B\r\nContent-Type: text/html\r\n\r\n<p>x</p>\r\n--B--\r\n\r\n";

    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(content);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 3);

    auto msgs = parser.readMessages(0, 10);
    QVERIFY(msgs.has_value());
    QCOMPARE(msgs->size(), 3);
    QVERIFY(!msgs->at(0).has_attachments);  // plain text
    QVERIFY(msgs->at(1).has_attachments);   // lone non-text part
    QVERIFY(msgs->at(2).has_attachments);   // multipart/related (old code missed this)
    parser.close();
}

// A quoted filename containing spaces must survive intact (old regex truncated at the first space).
void TestMboxParser::quotedFilenameWithSpacesPreserved() {
    QByteArray c;
    c += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: attach\r\n";
    c += "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n";
    c += "--B\r\n";
    c += "Content-Type: application/octet-stream\r\n";
    c += "Content-Disposition: attachment; filename=\"my long report.pdf\"\r\n";
    c += "\r\n";
    c += "data\r\n";
    c += "--B--\r\n";
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    QCOMPARE(detail->attachments.size(), 1);
    QCOMPARE(detail->attachments.first().long_filename, QStringLiteral("my long report.pdf"));
    parser.close();
}

// Malformed base64 must fail closed: strict decoding rejects it instead of handing back the
// partial bytes Qt's permissive decoder would otherwise produce.
void TestMboxParser::malformedBase64AttachmentFailsClosed() {
    QByteArray c;
    c += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: bad b64\r\n";
    c += "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n";
    c += "--B\r\n";
    c += "Content-Type: application/octet-stream\r\n";
    c += "Content-Disposition: attachment; filename=\"x.bin\"\r\n";
    c += "Content-Transfer-Encoding: base64\r\n\r\n";
    c += "@@@not-valid-base64@@@\r\n";
    c += "--B--\r\n";
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    QVERIFY(!parser.readMessageDetail(0).has_value());
    QVERIFY(!parser.readAttachmentData(sak::MboxMessageIndex{0}, sak::MboxAttachmentIndex{0})
                 .has_value());
    QVERIFY(!parser.readAllAttachments(0).has_value());
    parser.close();
}

// A body line beginning "From " but lacking an asctime-style time+year must NOT be indexed as a
// new message separator (RFC 4155 shape tightening).
void TestMboxParser::fromPrefixedBodyLineIsNotASeparator() {
    QByteArray c;
    c += "From real@sender.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: prose\r\n\r\n";
    c += "Dear friend,\r\n";
    c += "From now on we meet on Tuesdays.\r\n";
    c += "Regards\r\n\r\n";
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    parser.indexMessages();
    QCOMPARE(parser.messageCount(), 1);
    parser.close();
}

// Regression for the streaming (no per-line QList) splitMimeParts rewrite: a body with several
// parts still splits into every part.
void TestMboxParser::multipartWithManyPartsSplitsCorrectly() {
    QByteArray c;
    c += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: many\r\n";
    c += "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n";
    c += "--B\r\nContent-Type: text/plain\r\n\r\nbody text\r\n";
    for (int i = 0; i < 3; ++i) {
        c += "--B\r\n";
        c += "Content-Type: application/octet-stream\r\n";
        c += "Content-Disposition: attachment; filename=\"f" + QByteArray::number(i) +
             ".bin\"\r\n\r\n";
        c += "chunk" + QByteArray::number(i) + "\r\n";
    }
    c += "--B--\r\n";
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());
    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    QCOMPARE(detail->attachments.size(), 3);
    QVERIFY(detail->body_plain.contains(QStringLiteral("body text")));
    parser.close();
}

// A multipart/mixed body whose CLOSING "--B--" delimiter is missing: the stream ends right after
// the final part's bytes (a truncated / attacker-supplied message). The first part is still flushed
// by the SECOND part's opening "--B" delimiter, so it survives regardless. The final part, however,
// is held only in splitMimeParts' running buffer and is recovered SOLELY by the end-of-body
// trailing flush (`if (!current_part.isEmpty() && ...)`). Inverting that guard drops the un-flushed
// final part.
void TestMboxParser::trailingPartRecoveredWhenClosingDelimiterMissing() {
    QByteArray c;
    c += "From a@e.com Mon Jan  1 00:00:00 2024\r\n";
    c += "Subject: truncated multipart\r\n";
    c += "Content-Type: multipart/mixed; boundary=\"B\"\r\n\r\n";
    c += "--B\r\nContent-Type: text/plain\r\n\r\nfirst body\r\n";
    c += "--B\r\n";
    c += "Content-Type: application/octet-stream\r\n";
    c += "Content-Disposition: attachment; filename=\"tail.bin\"\r\n\r\n";
    c += "tail-bytes\r\n";
    // NOTE: deliberately NO closing "--B--" line -- the stream ends mid-body, after the final part.
    QTemporaryFile f;
    QVERIFY(f.open());
    f.write(c);
    f.close();

    MboxParser parser;
    parser.open(f.fileName());
    QVERIFY(parser.isOpen());

    auto detail = parser.readMessageDetail(0);
    QVERIFY(detail.has_value());
    // The first part parsed normally, and the delimiter-truncated FINAL part was still recovered.
    QVERIFY(detail->body_plain.contains(QStringLiteral("first body")));
    QCOMPARE(detail->attachments.size(), 1);
    QCOMPARE(detail->attachments.first().long_filename, QStringLiteral("tail.bin"));

    // The recovered part's bytes are reachable (index 0 must resolve to the trailing attachment).
    auto bytes = parser.readAttachmentData(sak::MboxMessageIndex{0}, sak::MboxAttachmentIndex{0});
    QVERIFY(bytes.has_value());
    QVERIFY(bytes->contains("tail-bytes"));
    parser.close();
}

QTEST_MAIN(TestMboxParser)
#include "test_mbox_parser.moc"
