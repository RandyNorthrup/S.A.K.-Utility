// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_html_email_writer.cpp
/// @brief Unit tests for HtmlEmailWriter styled HTML output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/html_email_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestHtmlEmailWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Plain text message
    // ====================================================================

    void testWritePlainTextMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("HTML Test");
        item.sender_name = QStringLiteral("Alice");
        item.sender_email = QStringLiteral("alice@example.com");
        item.display_to = QStringLiteral("bob@example.com");
        item.body_plain = QStringLiteral("Hello, this is plain text.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly));

        QByteArray content = file.readAll();
        QVERIFY(content.contains("<!DOCTYPE html>") || content.contains("<html"));
        QVERIFY(content.contains("HTML Test"));
        QVERIFY(content.contains("alice@example.com"));
    }

    // ====================================================================
    // HTML body preserved
    // ====================================================================

    void testHtmlBodyPreserved() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Rich Content");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_html = QStringLiteral("<p>This is <b>bold</b> and <i>italic</i>.</p>");
        item.date = QDateTime(QDate(2025, 3, 1), QTime(8, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray content = file.readAll();
        QVERIFY(content.contains("<b>bold</b>"));
    }

    // ====================================================================
    // Folder preservation
    // ====================================================================

    void testFolderPreservation() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Subfolder");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Text");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QStringLiteral("Archive/2025"));
        QVERIFY(result.has_value());

        QString expected = temp_dir.path() + "/Archive/2025";
        QVERIFY(QDir(expected).exists());
    }

    // ====================================================================
    // Total bytes written
    // ====================================================================

    void testTotalBytesWritten() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), 0);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Size Test");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(writer.totalBytesWritten() > 0);
    }

    // ====================================================================
    // Date prefix in filename
    // ====================================================================

    void testDatePrefix() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::HtmlEmailWriter writer(temp_dir.path(), true, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Dated HTML");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 7, 20), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QVERIFY(fi.fileName().startsWith("2025-07-20_"));
    }
};

QTEST_MAIN(TestHtmlEmailWriter)

#include "test_html_email_writer.moc"
