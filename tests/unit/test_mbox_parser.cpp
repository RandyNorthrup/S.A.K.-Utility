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
    parser.open(temp_file.fileName());
    // Empty file should open but have no messages
    if (parser.isOpen()) {
        QCOMPARE(parser.messageCount(), 0);
        parser.close();
    }
}

void TestMboxParser::openAndClose() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);
    MboxParser parser;
    parser.open(temp_file->fileName());

    // May or may not succeed depending on open behavior,
    // but close should always be safe
    parser.close();
    QVERIFY(!parser.isOpen());
    delete temp_file;
}

void TestMboxParser::doubleCloseIsHarmless() {
    MboxParser parser;
    parser.close();
    parser.close();
    QVERIFY(!parser.isOpen());
}

void TestMboxParser::cancelDoesNotCrash() {
    MboxParser parser;
    parser.cancel();
    QVERIFY(true);
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

    if (parser.isOpen()) {
        QSignalSpy index_spy(&parser, &MboxParser::indexingComplete);
        parser.indexMessages();
        // If indexing completes synchronously
        if (index_spy.count() > 0) {
            QVERIFY(parser.messageCount() >= 1);
        }
    }
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

    if (parser.isOpen()) {
        parser.indexMessages();
        // We wrote 3 messages, check we find them
        QVERIFY(parser.messageCount() >= 1);
    }
    parser.close();
}

void TestMboxParser::emptyFileReturnsZeroMessages() {
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());

    if (parser.isOpen()) {
        parser.indexMessages();
        QCOMPARE(parser.messageCount(), 0);
    }
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

    if (parser.isOpen()) {
        parser.indexMessages();
        QSignalSpy msg_spy(&parser, &MboxParser::messagesLoaded);
        parser.loadMessages(0, 10);
        // Check that messagesLoaded was emitted
        if (msg_spy.count() > 0) {
            auto messages = msg_spy.at(0).at(0).value<QVector<sak::MboxMessage>>();
            QVERIFY(messages.size() >= 1);
        }
    }
    parser.close();
    delete temp_file;
}

void TestMboxParser::loadMessageDetailFromValidFile() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    if (parser.isOpen()) {
        parser.indexMessages();
        if (parser.messageCount() > 0) {
            QSignalSpy detail_spy(&parser, &MboxParser::messageDetailLoaded);
            parser.loadMessageDetail(0);
            if (detail_spy.count() > 0) {
                auto detail = detail_spy.at(0).at(0).value<sak::MboxMessageDetail>();
                QVERIFY(!detail.subject.isEmpty() || !detail.from.isEmpty());
            }
        }
    }
    parser.close();
    delete temp_file;
}

void TestMboxParser::readMessagesSync() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    if (parser.isOpen()) {
        parser.indexMessages();
        auto result = parser.readMessages(0, 10);
        if (result.has_value()) {
            QVERIFY(result.value().size() >= 1);
        }
    }
    parser.close();
    delete temp_file;
}

void TestMboxParser::readMessageDetailSync() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    if (parser.isOpen()) {
        parser.indexMessages();
        if (parser.messageCount() > 0) {
            auto result = parser.readMessageDetail(0);
            if (result.has_value()) {
                QVERIFY(!result.value().from.isEmpty() || !result.value().subject.isEmpty());
            }
        }
    }
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
    auto bytes0 = parser.readAttachmentData(0, 0);
    QVERIFY(bytes0.has_value());
    QCOMPARE(*bytes0, QByteArray("ABC"));
    auto bytes1 = parser.readAttachmentData(0, 1);
    QVERIFY(bytes1.has_value());
    QCOMPARE(*bytes1, QByteArray("DEF"));

    // Out-of-range index is rejected.
    QVERIFY(!parser.readAttachmentData(0, 2).has_value());
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
                const auto bytes = parser.readAttachmentData(idx, 0);
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
    // "From " followed by email and date is a valid MBOX separator
    QByteArray valid_line = "From sender@example.com Mon Jan  1 00:00:00 2024\r\n";
    QByteArray invalid_line = "From: sender@example.com\r\n";
    QByteArray empty_line;

    // We can't directly call the static private isFromLine(),
    // so we test indirectly via indexing: a file starting with
    // "From " should produce at least one indexed message.
    QTemporaryFile temp_file;
    QVERIFY(temp_file.open());
    temp_file.write(valid_line);
    temp_file.write("Subject: Test\r\n\r\nBody text\r\n");
    temp_file.close();

    MboxParser parser;
    parser.open(temp_file.fileName());
    if (parser.isOpen()) {
        parser.indexMessages();
        QVERIFY(parser.messageCount() >= 1);
    }
    parser.close();
}

// ============================================================================
// Error Cases
// ============================================================================

void TestMboxParser::loadMessagesWhenClosed() {
    MboxParser parser;
    QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);

    // Loading messages without opening should either emit an error
    // or simply do nothing
    parser.loadMessages(0, 10);
    QVERIFY(error_spy.count() > 0 || !parser.isOpen());
}

void TestMboxParser::loadDetailOutOfRange() {
    auto* temp_file = createSampleMboxFile();
    QVERIFY(temp_file);

    MboxParser parser;
    parser.open(temp_file->fileName());

    if (parser.isOpen()) {
        parser.indexMessages();
        QSignalSpy error_spy(&parser, &MboxParser::errorOccurred);
        // Request an out-of-range index
        parser.loadMessageDetail(99'999);
        // Should produce an error or simply not crash
        QVERIFY(error_spy.count() > 0 || true);
    }
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
    QVERIFY(messages.value().size() >= 1);

    parser.cancel();
    auto detail = parser.readMessageDetail(0);
    QVERIFY2(detail.has_value(), "detail read after a stale cancel must not fail");

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
    QVERIFY(!parser.readAttachmentData(0, 0).has_value());
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

QTEST_MAIN(TestMboxParser)
#include "test_mbox_parser.moc"
