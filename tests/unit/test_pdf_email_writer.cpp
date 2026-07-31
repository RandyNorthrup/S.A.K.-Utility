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

    // Regression: colliding subjects must never overwrite an already-written .pdf. The old
    // counter-only scheme could make the 2nd "X" resolve to "X_1"/"X_2" and clobber a distinct
    // file; the fix loops until the suffixed name is free.
    void collidingSubjectsNeverClobber() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::PdfEmailWriter writer(temp_dir.path(), false, false);  // no date prefix, no subfolders
        const QVector<QPair<QString, QByteArray>> none;
        const auto make = [](const QString& subj) {
            sak::PstItemDetail item;
            item.subject = subj;
            item.body_plain = QStringLiteral("body of ") + subj;
            return item;
        };
        const auto r1 = writer.writeMessage(make(QStringLiteral("X_1")), none, QString());
        const auto r2 = writer.writeMessage(make(QStringLiteral("X")), none, QString());
        const auto r3 = writer.writeMessage(make(QStringLiteral("X")), none, QString());
        QVERIFY(r1.has_value());
        QVERIFY(r2.has_value());
        QVERIFY(r3.has_value());
        // Three DISTINCT files, all present at once (no clobber).
        QVERIFY(r1.value() != r2.value());
        QVERIFY(r2.value() != r3.value());
        QVERIFY(r1.value() != r3.value());
        QVERIFY(QFile::exists(r1.value()));
        QVERIFY(QFile::exists(r2.value()));
        QVERIFY(QFile::exists(r3.value()));
    }

    // ====================================================================
    // B7-06: a hostile body that references a local file (or remote URL) must
    // render through the resource-denying document -- no disk/network load, no
    // hang, still a valid PDF. Disclosure prevention itself is via the
    // loadResource() override; this proves the render path is exercised safely.
    // ====================================================================

    void hostileBodyRendersWithoutResourceLoad() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        // A "secret" the body tries to pull into the PDF.
        const QString secret = temp_dir.path() + QStringLiteral("/secret.txt");
        {
            QFile f(secret);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("TOP-SECRET-CONTENTS");
            f.close();
        }

        sak::PdfEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Exfil");
        item.sender_email = QStringLiteral("evil@test.com");
        item.body_html = QStringLiteral(
                             "<p>See image</p>"
                             "<img src=\"file:///%1\">"
                             "<img src=\"http://tracker/beacon.gif\">"
                             "<script>fetch('http://evil')</script>")
                             .arg(secret);
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.size() > 0);
        QVERIFY(file.read(5).startsWith("%PDF"));
    }
};

QTEST_MAIN(TestPdfEmailWriter)

#include "test_pdf_email_writer.moc"
