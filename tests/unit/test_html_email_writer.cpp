// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_html_email_writer.cpp
/// @brief Unit tests for HtmlEmailWriter styled HTML output

#include "sak/email_html_sanitizer.h"
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

    // ====================================================================
    // Attachment path traversal must be contained
    // ====================================================================

    void testAttachmentTraversalContained() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        // A canary file three levels above where a "../../../" name would land.
        const QString canary = temp_dir.path() + QStringLiteral("/canary.html");
        {
            QFile f(canary);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("ORIGINAL");
            f.close();
        }

        sak::HtmlEmailWriter writer(temp_dir.path() + QStringLiteral("/a/b/c"), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Evil");
        item.sender_email = QStringLiteral("test@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVector<QPair<QString, QByteArray>> attachments;
        attachments.append({QStringLiteral("../../../../canary.html"), QByteArray("PWNED")});

        auto result = writer.writeMessage(item, attachments, QString());
        QVERIFY(result.has_value());

        // The canary outside the export tree must be untouched.
        QFile c(canary);
        QVERIFY(c.open(QIODevice::ReadOnly));
        QCOMPARE(c.readAll(), QByteArray("ORIGINAL"));
    }

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

    // Regression: colliding subjects must never overwrite an already-written message. Order crafted
    // to trip the old counter-only scheme: "X_2" first, then two "X" (the 2nd "X" used to generate
    // "X_2" and clobber the first message's file). The fix loops until the suffixed name is free.
    void collidingSubjectsNeverClobber() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(),
                                    false,
                                    false);  // no date prefix, no subfolders
        const QVector<QPair<QString, QByteArray>> none;
        const auto make = [](const QString& subj, const QString& body) {
            sak::PstItemDetail item;
            item.subject = subj;
            item.body_plain = body;
            return item;
        };
        const auto r1 = writer.writeMessage(make(QStringLiteral("X_2"), QStringLiteral("BODY-ONE")),
                                            none,
                                            QString());
        const auto r2 = writer.writeMessage(make(QStringLiteral("X"), QStringLiteral("BODY-TWO")),
                                            none,
                                            QString());
        const auto r3 = writer.writeMessage(make(QStringLiteral("X"), QStringLiteral("BODY-THREE")),
                                            none,
                                            QString());
        QVERIFY(r1.has_value());
        QVERIFY(r2.has_value());
        QVERIFY(r3.has_value());
        // Three DISTINCT files, all present at once.
        QVERIFY(r1.value() != r2.value());
        QVERIFY(r2.value() != r3.value());
        QVERIFY(r1.value() != r3.value());
        QVERIFY(QFile::exists(r1.value()));
        QVERIFY(QFile::exists(r2.value()));
        QVERIFY(QFile::exists(r3.value()));
        // The first message's file was NOT overwritten by the third.
        QFile f1(r1.value());
        QVERIFY(f1.open(QIODevice::ReadOnly));
        QVERIFY(f1.readAll().contains("BODY-ONE"));
    }

    // ====================================================================
    // B7-05: untrusted email HTML must not carry active content into the
    // saved page, and the page must ship a strict CSP.
    // ====================================================================

    void sanitizerStripsActiveContent() {
        const QString dirty = QStringLiteral(
            "<p>hi</p>"
            "<script>fetch('http://evil')</script>"
            "<img src=x onerror=\"steal()\">"
            "<iframe src=\"http://evil\"></iframe>"
            "<a href=\"javascript:evil()\">go</a>");
        const QString clean = sak::sanitizeEmailBodyHtml(dirty);

        QVERIFY(clean.contains(QStringLiteral("<p>hi</p>")));  // benign markup preserved
        QVERIFY(!clean.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("onerror"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("<iframe"), Qt::CaseInsensitive));
        QVERIFY(!clean.contains(QStringLiteral("javascript:"), Qt::CaseInsensitive));
    }

    void savedHtmlHasCspAndStripsScript() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::HtmlEmailWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Hostile");
        item.sender_email = QStringLiteral("evil@test.com");
        item.body_html = QStringLiteral(
            "<p>Body</p><script>fetch('http://evil')</script>"
            "<img src=\"http://tracker/beacon.gif\" onerror=\"alert(1)\">");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        QVERIFY(content.contains("Content-Security-Policy"));
        QVERIFY(content.contains("default-src 'none'"));
        QVERIFY(!content.toLower().contains("<script"));
        QVERIFY(!content.toLower().contains("onerror"));
        QVERIFY(content.contains("<p>Body</p>"));  // real body still rendered
    }
};

QTEST_MAIN(TestHtmlEmailWriter)

#include "test_html_email_writer.moc"
