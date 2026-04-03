// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_eml_writer.cpp
/// @brief Unit tests for EmlWriter RFC 5322 output

#include "sak/email_types.h"
#include "sak/eml_writer.h"
#include "sak/error_codes.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

class TestEmlWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Simple Plain-Text Message
    // ====================================================================

    void testWritePlainTextMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Test Subject");
        item.sender_name = QStringLiteral("John Doe");
        item.sender_email = QStringLiteral("john@example.com");
        item.display_to = QStringLiteral("jane@example.com");
        item.body_plain = QStringLiteral("Hello, this is a test email.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        QVERIFY(content.contains("From: John Doe <john@example.com>"));
        QVERIFY(content.contains("To: jane@example.com"));
        QVERIFY(content.contains("Subject: Test Subject"));
        QVERIFY(content.contains("Hello, this is a test email."));
        QVERIFY(content.contains("Content-Type: text/plain"));
    }

    // ====================================================================
    // HTML Message — Multipart Alternative
    // ====================================================================

    void testWriteHtmlMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("HTML Test");
        item.sender_name = QStringLiteral("Sender");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_plain = QStringLiteral("Plain text version");
        item.body_html = QStringLiteral("<html><body><b>Bold test</b></body></html>");
        item.date = QDateTime(QDate(2025, 3, 1), QTime(8, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();

        QVERIFY(content.contains("multipart/alternative"));
        QVERIFY(content.contains("text/plain"));
        QVERIFY(content.contains("text/html"));
        QVERIFY(content.contains("<b>Bold test</b>"));
    }

    // ====================================================================
    // Message with Attachments — Multipart Mixed
    // ====================================================================

    void testWriteMessageWithAttachments() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("With Attachments");
        item.sender_name = QStringLiteral("Sender");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_plain = QStringLiteral("See attached.");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(12, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("document.pdf"), QByteArray("fake pdf content")});

        auto result = writer.writeMessage(item, attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();

        QVERIFY(content.contains("multipart/mixed"));
        QVERIFY(content.contains("document.pdf"));
        QVERIFY(content.contains("Content-Transfer-Encoding: base64"));
    }

    // ====================================================================
    // Folder Structure Preservation
    // ====================================================================

    void testPreserveFolderStructure() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Subfolder Test");
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 6, 1), QTime(9, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QStringLiteral("Inbox/Projects"));
        QVERIFY(result.has_value());

        // File should be in the subfolder
        QString expected_dir = temp_dir.path() + "/Inbox/Projects";
        QVERIFY(QDir(expected_dir).exists());
        QVERIFY(result.value().startsWith(expected_dir));
    }

    // ====================================================================
    // Date Prefix in Filename
    // ====================================================================

    void testDatePrefixFilename() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), true, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Dated Message");
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 3, 15), QTime(14, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QVERIFY(fi.fileName().startsWith("2025-03-15_"));
    }

    // ====================================================================
    // Filename Sanitization — Invalid Characters
    // ====================================================================

    void testSanitizeInvalidCharacters() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Re: Invoice <2025> for \"Project\"");
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        // Should not contain < > or "
        QVERIFY(!fi.fileName().contains('<'));
        QVERIFY(!fi.fileName().contains('>'));
        QVERIFY(!fi.fileName().contains('"'));
    }

    // ====================================================================
    // Empty Subject — Fallback Filename
    // ====================================================================

    void testEmptySubjectFallback() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        // Subject empty
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("No subject email");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QVERIFY(fi.fileName().contains("no_subject"));
    }

    // ====================================================================
    // Duplicate Filename Collision
    // ====================================================================

    void testDuplicateFilenameCollision() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Same Subject");
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("First email");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result1 = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result1.has_value());

        item.body_plain = QStringLiteral("Second email");
        auto result2 = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result2.has_value());

        // The two files should have different paths
        QVERIFY(result1.value() != result2.value());

        // Both files should exist
        QVERIFY(QFile::exists(result1.value()));
        QVERIFY(QFile::exists(result2.value()));
    }

    // ====================================================================
    // Bytes Written Tracking
    // ====================================================================

    void testBytesWrittenTracking() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), static_cast<qint64>(0));

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Track Bytes");
        item.sender_name = QStringLiteral("Test");
        item.body_plain = QStringLiteral("Some content here.");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());
        QVERIFY(writer.totalBytesWritten() > 0);
    }
};

QTEST_MAIN(TestEmlWriter)
#include "test_eml_writer.moc"
