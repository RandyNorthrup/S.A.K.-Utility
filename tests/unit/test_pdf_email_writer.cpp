// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pdf_email_writer.cpp
/// @brief Unit tests for PdfEmailWriter QPdfWriter output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pdf_email_writer.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestPdfEmailWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path — Plain text message to PDF
    // ====================================================================

    void testWritePlainTextPdf() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PdfEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("PDF Test");
        item.sender_name = QStringLiteral("Alice");
        item.sender_email = QStringLiteral("alice@example.com");
        item.body_plain = QStringLiteral("Hello from PDF writer.");
        item.date = QDateTime(QDate(2025, 6, 15), QTime(10, 30, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.size() > 0);

        // PDF files start with %PDF
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(5);
        QVERIFY(header.startsWith("%PDF"));
    }

    // ====================================================================
    // HTML content rendered to PDF
    // ====================================================================

    void testWriteHtmlPdf() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PdfEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("HTML PDF");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_html = QStringLiteral("<html><body><h1>Title</h1><p>Paragraph</p></body></html>");
        item.date = QDateTime(QDate(2025, 3, 1), QTime(8, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.size() > 0);
    }

    // ====================================================================
    // Folder preservation
    // ====================================================================

    void testFolderPreservation() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PdfEmailWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Subfolder PDF");
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
    // Date prefix
    // ====================================================================

    void testDatePrefix() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PdfEmailWriter writer(temp_dir.path(), true, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Dated PDF");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 4, 10), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());

        QFileInfo fi(result.value());
        QVERIFY(fi.fileName().startsWith("2025-04-10_"));
    }

    // ====================================================================
    // Total bytes
    // ====================================================================

    void testTotalBytesWritten() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PdfEmailWriter writer(temp_dir.path(), false, false);
        QCOMPARE(writer.totalBytesWritten(), 0);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Size");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("Content");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> no_attachments;
        std::ignore = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(writer.totalBytesWritten() > 0);
    }
};

QTEST_MAIN(TestPdfEmailWriter)

#include "test_pdf_email_writer.moc"
