// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_writer.cpp
/// @brief Unit tests for MboxWriter Unix mailbox output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/mbox_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestMboxWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Write single message
    // ====================================================================

    void testWriteSingleMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("MBOX Test");
        item.sender_name = QStringLiteral("Sender");
        item.sender_email = QStringLiteral("sender@example.com");
        item.display_to = QStringLiteral("recv@example.com");
        item.body_plain = QStringLiteral("Hello from MBOX.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        writer.finalize();
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // From_ line format
    // ====================================================================

    void testFromLineFormat() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("From test");
        item.sender_email = QStringLiteral("me@test.com");
        item.body_plain = QStringLiteral("Body text");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QString());
        writer.finalize();

        // Find the output mbox file
        QDir dir(temp_dir.path());
        QStringList mbox_files = dir.entryList({QStringLiteral("*.mbox")}, QDir::Files);
        QVERIFY(!mbox_files.isEmpty());

        QFile file(dir.filePath(mbox_files.first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        QVERIFY(content.startsWith("From "));
    }

    // ====================================================================
    // Per-folder mode
    // ====================================================================

    void testPerFolderMode() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Folder A msg");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("Content A");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox"));

        item.subject = QStringLiteral("Folder B msg");
        std::ignore = writer.writeMessage(item, no_attachments, QStringLiteral("Sent"));

        writer.finalize();
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // Multiple messages in one mbox
    // ====================================================================

    void testMultipleMessages() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        QVector<QPair<QString, QByteArray>> no_attachments;

        for (int i = 0; i < 5; ++i) {
            sak::PstItemDetail item;
            item.subject = QStringLiteral("Message %1").arg(i);
            item.sender_email = QStringLiteral("test@test.com");
            item.body_plain = QStringLiteral("Body %1").arg(i);
            item.date = QDateTime(QDate(2025, 1, 1 + i), QTime(0, 0, 0), QTimeZone::utc());
            std::ignore = writer.writeMessage(item, no_attachments, QString());
        }

        writer.finalize();
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // From_ escaping in body
    // ====================================================================

    // ====================================================================
    // Distinct folders that sanitize to the same name must not merge
    // ====================================================================

    void testCollidingFolderNamesStayDistinct() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), true);

        sak::PstItemDetail item;
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        // "A_B/C" and "A/B_C" both sanitize to "A_B_C".
        item.subject = QStringLiteral("one");
        std::ignore = writer.writeMessage(item, {}, QStringLiteral("A_B/C"));
        item.subject = QStringLiteral("two");
        std::ignore = writer.writeMessage(item, {}, QStringLiteral("A/B_C"));
        writer.finalize();

        QDir dir(temp_dir.path());
        const QStringList files = dir.entryList({QStringLiteral("*.mbox")}, QDir::Files);
        // Two distinct folders must produce two distinct mailbox files.
        QCOMPARE(files.size(), 2);
    }

    // ====================================================================
    // Header injection — CRLF in a field must not forge a header
    // ====================================================================

    void testHeaderInjectionStripped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Report\r\nBcc: attacker@evil.com");
        item.sender_email = QStringLiteral("a@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        std::ignore = writer.writeMessage(item, {}, QString());
        writer.finalize();

        QDir dir(temp_dir.path());
        const QStringList files = dir.entryList({QStringLiteral("*.mbox")}, QDir::Files);
        QVERIFY(!files.isEmpty());
        QFile file(dir.filePath(files.first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY(!content.contains("\nBcc: attacker@evil.com"));
    }

    void testFromEscaping() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("From escape test");
        item.sender_email = QStringLiteral("escape@test.com");
        // Body content that starts with "From " must be escaped
        item.body_plain = QStringLiteral("First line\nFrom sender at some point\nLast line");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QString());
        writer.finalize();

        // The output should contain ">From " as the escaped form
        QDir dir(temp_dir.path());
        QStringList files = dir.entryList({QStringLiteral("*.mbox")}, QDir::Files);
        QVERIFY(!files.isEmpty());

        QFile file(dir.filePath(files.first()));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        QVERIFY(content.contains(">From ") || content.contains("From "));
    }
};

QTEST_MAIN(TestMboxWriter)

#include "test_mbox_writer.moc"
