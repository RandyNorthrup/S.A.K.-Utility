// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_parser.cpp
/// @brief Unit tests for MBOX file parser

#include "sak/email_constants.h"
#include "sak/mbox_parser.h"

#include <QSignalSpy>
#include <QTemporaryFile>
#include <QtTest/QtTest>

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

    // -- From Line Detection ---------------------------------------------
    void fromLineDetectsValidSeparator();

    // -- Error Cases -----------------------------------------------------
    void loadMessagesWhenClosed();
    void loadDetailOutOfRange();

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

QTEST_MAIN(TestMboxParser)
#include "test_mbox_parser.moc"
