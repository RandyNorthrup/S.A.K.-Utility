// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_pdf_email_writer.cpp
/// @brief Unit tests for PdfEmailWriter QPdfWriter output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/pdf_email_writer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestPdfEmailWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path -- Plain text message to PDF
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

        // prefix_with_date=false + preserve_folders=false => exact deterministic destination
        QCOMPARE(result.value(), temp_dir.path() + QStringLiteral("/PDF Test.pdf"));

        QFile file(result.value());
        QVERIFY(file.exists());
        QVERIFY(file.size() > 0);

        // PDF files start with %PDF
        QVERIFY(file.open(QIODevice::ReadOnly));
        QByteArray header = file.read(5);
        QCOMPARE(header, QByteArrayLiteral("%PDF-"));  // full 5-byte magic, not just "%PDF"

        sak::PstItemDetail empty_body = item;
        empty_body.body_plain.clear();
        const auto empty_result = writer.writeMessage(empty_body, no_attachments, QString());
        QVERIFY(empty_result.has_value());
        QFile empty_file(empty_result.value());
        QVERIFY(empty_file.exists());
        QVERIFY2(file.size() > empty_file.size(),
                 "plain-text body never reached the rendered page");
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

        sak::PstItemDetail bodyless = item;
        bodyless.body_html.clear();
        QTemporaryDir bodyless_dir;
        QVERIFY(bodyless_dir.isValid());
        sak::PdfEmailWriter bodyless_writer(bodyless_dir.path(), false, false);
        const auto bodyless_result =
            bodyless_writer.writeMessage(bodyless, no_attachments, QString());
        QVERIFY(bodyless_result.has_value());
        QVERIFY2(file.size() > QFileInfo(bodyless_result.value()).size(),
                 "HTML body did not reach the rendered page");
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
        // The message must be written INTO the preserved subfolder -- a created-then-ignored
        // directory (return m_output_dir instead of *target_dir) would still satisfy exists().
        QCOMPARE(result.value(), expected + QStringLiteral("/Subfolder PDF.pdf"));
        QVERIFY(QFile::exists(result.value()));
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
        QCOMPARE(fi.fileName(), QStringLiteral("2025-04-10_000000_Dated PDF.pdf"));
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

        auto result = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result.has_value());
        // The counter is the EXACT size of the file just committed (pdf_email_writer.cpp:267),
        // not a proxy such as the HTML length and not merely "nonzero".
        QCOMPARE(writer.totalBytesWritten(), QFileInfo(result.value()).size());

        // The counter ACCUMULATES across messages, so a second write must SUM rather than
        // replace. A single message cannot tell "+=" from "="; email_export_worker reads this
        // value as a per-item delta (email_export_worker.cpp:968/983).
        item.subject = QStringLiteral("Size Two");
        item.body_plain = QStringLiteral("A different and noticeably longer second body.");
        auto second = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(second.has_value());
        QVERIFY(second.value() != result.value());
        QCOMPARE(writer.totalBytesWritten(),
                 QFileInfo(result.value()).size() + QFileInfo(second.value()).size());
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
        // Exact collision-resolution scheme: "X_1" is taken by the first message, so the
        // second "X" keeps the free name "X.pdf" and the third must skip the occupied
        // "X_1.pdf" and land on "X_2.pdf". Distinctness alone would also accept a resolver
        // that suffixes every file.
        QCOMPARE(QFileInfo(r1.value()).fileName(), QStringLiteral("X_1.pdf"));
        QCOMPARE(QFileInfo(r2.value()).fileName(), QStringLiteral("X.pdf"));
        QCOMPARE(QFileInfo(r3.value()).fileName(), QStringLiteral("X_2.pdf"));
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
        QCOMPARE(file.read(5), QByteArrayLiteral("%PDF-"));
    }

    // The success check must require a structurally-complete PDF (magic + %%EOF
    // trailer), not merely a nonzero file. A real render must satisfy both.
    void writesStructurallyCompletePdf() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::PdfEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Complete");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("hello");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());
        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("%PDF-"));
        QVERIFY(bytes.contains("%%EOF"));
    }

    // A subfolder path escaping the output directory must be refused -- and refused
    // BEFORE any directory is created. The export root is nested one level inside the
    // QTemporaryDir so "../escape" resolves to a path we own and that is auto-cleaned.
    void rejectsSubfolderTraversal() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        const QString root = temp_dir.filePath(QStringLiteral("root"));
        QVERIFY(QDir().mkpath(root));
        const QString escape_dir = temp_dir.filePath(QStringLiteral("escape"));
        QVERIFY(!QDir(escape_dir).exists());  // precondition: nothing there yet

        sak::PdfEmailWriter writer(root, false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("x");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("b");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QStringLiteral("../escape"));
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::path_traversal_attempt);
        // The lexical guard runs inside composeTargetDir(), before resolveTargetDirectory()
        // reaches mkpath(). Moving it after mkpath would still return this error code, so
        // the refusal is only proved by observing that nothing was created outside the root.
        QVERIFY(!QDir(escape_dir).exists());
        QVERIFY(QDir(root).entryList(QDir::AllEntries | QDir::NoDotAndDotDot).isEmpty());
    }
};

QTEST_MAIN(TestPdfEmailWriter)

#include "test_pdf_email_writer.moc"
