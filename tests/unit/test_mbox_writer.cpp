// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_writer.cpp
/// @brief Unit tests for MboxWriter Unix mailbox output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/io_write_utils.h"
#include "sak/mbox_writer.h"

#include "../support/mime_tree.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

static sak::PstItemDetail nestingItem(const QString& plain, const QString& html) {
    sak::PstItemDetail item;
    item.subject = QStringLiteral("Nesting");
    item.sender_email = QStringLiteral("a@example.test");
    item.date = QDateTime(QDate(2026, 1, 2), QTime(3, 4), QTimeZone::UTC);
    item.body_plain = plain;
    item.body_html = html;
    sak::PstAttachmentInfo picture;
    picture.long_filename = QStringLiteral("front.png");
    picture.content_id = QStringLiteral("<front@x>");
    picture.mime_type = QStringLiteral("image/png");
    sak::PstAttachmentInfo file;
    file.long_filename = QStringLiteral("menu.pdf");
    file.mime_type = QStringLiteral("application/pdf");
    item.attachments = {picture, file};
    return item;
}

class TestMboxWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path -- Write single message
    // ====================================================================

    void partsNestTheWayAReaderExpects_data() {
        QTest::addColumn<QString>("plain");
        QTest::addColumn<QString>("html");
        QTest::addColumn<bool>("shown_picture");
        QTest::addColumn<bool>("attached_file");
        QTest::addColumn<QString>("tree");
        const QString plain = QStringLiteral("Plain");
        const QString shows = QStringLiteral("<img src=\"cid:front@x\">");
        const QString none = QStringLiteral("<p>No picture</p>");
        QTest::newRow("everything")
            << plain << shows << true << true
            << QStringLiteral(
                   "mixed(related(alternative(text/plain,text/html),image/png),application/pdf)");
        QTest::newRow("a picture and no file")
            << plain << shows << true << false
            << QStringLiteral("related(alternative(text/plain,text/html),image/png)");
        QTest::newRow("HTML alone with its picture")
            << QString() << shows << true << false
            << QStringLiteral("related(text/html,image/png)");
        QTest::newRow("a file and no picture")
            << plain << none << false << true
            << QStringLiteral("mixed(alternative(text/plain,text/html),application/pdf)");
        QTest::newRow("plain text and a file")
            << plain << QString() << false << true
            << QStringLiteral("mixed(text/plain,application/pdf)");
        QTest::newRow("both bodies and nothing else")
            << plain << none << false << false
            << QStringLiteral("alternative(text/plain,text/html)");
        QTest::newRow("a picture the HTML does not show is a file")
            << plain << none << true << false
            << QStringLiteral("mixed(alternative(text/plain,text/html),image/png)");
    }

    // Thunderbird, reading what this writer wrote: with a shown picture in multipart/mixed, it
    // showed the alt text in the message, warned of remote content, and put the picture after the
    // message.
    void partsNestTheWayAReaderExpects() {
        QFETCH(QString, plain);
        QFETCH(QString, html);
        QFETCH(bool, shown_picture);
        QFETCH(bool, attached_file);
        QFETCH(QString, tree);
        const sak::PstItemDetail item = nestingItem(plain, html);
        QVector<QPair<QString, QByteArray>> attachments;
        if (shown_picture) {
            attachments.append({QStringLiteral("front.png"), "png"});
        }
        if (attached_file) {
            attachments.append({QStringLiteral("menu.pdf"), "pdf"});
        }
        const auto mime =
            sak::MboxWriter::buildMimeMessage(item, item.sender_email, item.date, attachments);
        QVERIFY(mime.has_value());
        QCOMPARE(sak::test::mimeTree(*mime), tree);
    }

    // Real stores give ordinary attachments a Content-ID. Only what the body shows is inline.
    void onlyWhatTheBodyShowsIsInline() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);
        sak::PstItemDetail item;
        item.subject = QStringLiteral("Storefront");
        item.sender_email = QStringLiteral("a@example.test");
        item.date = QDateTime(QDate(2026, 1, 2), QTime(3, 4), QTimeZone::UTC);
        item.body_html = QStringLiteral("<img src=\"cid:front@x\"><p>The menu is attached</p>");
        sak::PstAttachmentInfo shown;
        shown.long_filename = QStringLiteral("front.png");
        shown.content_id = QStringLiteral("<front@x>");
        sak::PstAttachmentInfo attached;
        attached.long_filename = QStringLiteral("menu.pdf");
        attached.content_id = QStringLiteral("<menu@x>");
        item.attachments = {shown, attached};
        const auto written = writer.writeMessage(item,
                                                 {{QStringLiteral("front.png"), "png"},
                                                  {QStringLiteral("menu.pdf"), "pdf"}},
                                                 QString());
        QVERIFY(written.has_value());
        writer.finalize();
        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY2(bytes.contains("Content-Disposition: inline; filename=\"front.png\""),
                 bytes.constData());
        QVERIFY2(bytes.contains("Content-Disposition: attachment; filename=\"menu.pdf\""),
                 bytes.constData());
        QVERIFY(bytes.contains("Content-ID: <front@x>"));
        QVERIFY(bytes.contains("Content-ID: <menu@x>"));
    }

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
        // The message and its date are fixed, so the serialized size is deterministic. `> 0` would
        // survive a truncated write or a dropped header/body.
        QCOMPARE(writer.totalBytesWritten(), static_cast<qint64>(273));
    }

    void unicodeHeadersSerializeExactRfc2047Mime() {
        sak::PstItemDetail item;
        item.subject = QString::fromUtf8("\xc3\xa9");
        item.sender_name = QString::fromUtf8("Jos\xc3\xa9");
        item.sender_email = QStringLiteral("sender@example.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        const auto mime = sak::MboxWriter::buildMimeMessage(item, item.sender_email, item.date, {});
        QVERIFY(mime.has_value());
        const QByteArray expected =
            "From: =?UTF-8?B?Sm9zw6k=?= <sender@example.com>\r\n"
            "Subject: =?UTF-8?B?w6k=?=\r\n"
            "Date: 01 Jan 2025 00:00:00 +0000\r\n"
            "MIME-Version: 1.0\r\n"
            "Content-Type: text/plain; charset=utf-8\r\n"
            "Content-Transfer-Encoding: 8bit\r\n"
            "\r\n"
            "body";
        QCOMPARE(*mime, expected);
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
        // The From_ separator is deterministic for the fixed sender + UTC date. `startsWith("From
        // ")` would pass for any From_ line; pin the exact envelope (address + asctime).
        const QByteArray first_line = content.left(content.indexOf('\n'));
        QCOMPARE(first_line, QByteArray("From me@test.com Wed Jan 01 12:00:00 2025"));
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
        // Per-folder mode writes one file per named folder; assert the two expected files exist
        // rather than merely that some bytes were written.
        QDir dir(temp_dir.path());
        QCOMPARE(dir.entryList({QStringLiteral("*.mbox")}, QDir::Files, QDir::Name),
                 (QStringList{QStringLiteral("Inbox.mbox"), QStringLiteral("Sent.mbox")}));
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
        // Five messages from the same sender -> exactly five From_ separator lines in the single
        // mailbox. `> 0` could not tell five messages from one.
        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QCOMPARE(content.count("From test@test.com "), 5);
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
        // Two distinct folders must produce two distinctly NAMED mailbox files: the folder written
        // FIRST keeps the unsuffixed name and the colliding second one takes the " (2)" suffix
        // (mbox_writer.cpp resolveFilePath). entryList() is unsorted, so sort before comparing.
        QStringList sorted_files = files;
        sorted_files.sort();
        QCOMPARE(sorted_files,
                 (QStringList{QStringLiteral("A_B_C (2).mbox"), QStringLiteral("A_B_C.mbox")}));
        // ...and each folder's mail landed in ITS OWN mailbox. A size-only check was green for two
        // arbitrarily named files -- e.g. a hashed suffix a later run can never recognize -- and
        // for a scheme that put "one" in the suffixed file and "two" in the unsuffixed one.
        const auto subject_in = [&dir](const QString& name) {
            QFile f(dir.filePath(name));
            if (!f.open(QIODevice::ReadOnly)) {
                return QByteArray("<unreadable>");
            }
            const QByteArray body = f.readAll();
            for (const QByteArray& line : body.split('\n')) {
                if (line.startsWith("Subject: ")) {
                    return line.trimmed();
                }
            }
            return QByteArray("<no subject>");
        };
        QCOMPARE(subject_in(QStringLiteral("A_B_C.mbox")), QByteArray("Subject: one"));
        QCOMPARE(subject_in(QStringLiteral("A_B_C (2).mbox")), QByteArray("Subject: two"));
    }

    // ====================================================================
    // Header injection -- CRLF in a field must not forge a header
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
        // The CR and the LF each collapse to ONE space (mbox_writer.cpp sanitizeHeaderValue), so
        // the injected text must SURVIVE inside the Subject value rather than vanish. The negative
        // check alone is also green when the Subject header is dropped altogether -- silently
        // losing the subject of every message a hostile store crafted. Pin the exact folded-away
        // line, and that the only "Bcc: " in the file is that inert one.
        QCOMPARE(content.count("\r\nSubject: Report  Bcc: attacker@evil.com\r\n"), 1);
        QCOMPARE(content.count("Bcc: "), 1);
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
        // The body line "From sender at some point" must be escaped to ">From ...". The old OR was
        // vacuous: the mbox always contains "From " (its own separator line), so it passed with or
        // without escaping. Pin the exact escaped body line.
        QCOMPARE(content.count("\n>From sender at some point\n"), 1);
        // mboxrd escapes an ALREADY-escaped ">From " line too: a reader strips one '>' on read, so
        // leaving it alone hands back a bare "From " separator and splits one record into two
        // (mbox_writer.cpp isMboxFromLine skips the leading '>' run BEFORE testing the prefix).
        // Nothing else in the suite reaches that arm, so `return line.startsWith("From ")` stayed
        // green.
        QTemporaryDir quoted_dir;
        QVERIFY(quoted_dir.isValid());
        {
            sak::MboxWriter quoted_writer(quoted_dir.path(), false);
            sak::PstItemDetail quoted = item;
            quoted.body_plain = QStringLiteral("head\n>From already escaped\ntail");
            QVERIFY(quoted_writer.writeMessage(quoted, no_attachments, QString()).has_value());
            quoted_writer.finalize();
        }
        QFile quoted_file(quoted_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(quoted_file.open(QIODevice::ReadOnly));
        const QByteArray quoted_content = quoted_file.readAll();
        QCOMPARE(quoted_content.count("\n>>From already escaped\n"), 1);
        QCOMPARE(quoted_content.count("\n>From already escaped\n"), 0);
    }

    // ====================================================================
    // Rerun must not merge onto a previous run's file (B7-32)
    // ====================================================================

    void rerunTruncatesStaleOutput() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        const auto make = [](const QString& subject) {
            sak::PstItemDetail item;
            item.subject = subject;
            item.sender_email = QStringLiteral("s@test.com");
            item.body_plain = QStringLiteral("body");
            item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
            return item;
        };

        // First run writes "OLD_SUBJECT".
        {
            sak::MboxWriter writer(temp_dir.path(), false);
            std::ignore = writer.writeMessage(make(QStringLiteral("OLD_SUBJECT")), {}, QString());
            writer.finalize();
        }
        // Second run to the SAME directory writes only "NEW_SUBJECT".
        {
            sak::MboxWriter writer(temp_dir.path(), false);
            std::ignore = writer.writeMessage(make(QStringLiteral("NEW_SUBJECT")), {}, QString());
            writer.finalize();
        }

        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY(content.contains("NEW_SUBJECT"));
        QVERIFY(!content.contains("OLD_SUBJECT"));  // stale run truncated, not merged
    }

    // ====================================================================
    // Combined output namespaced by source basename so jobs don't collide
    // ====================================================================

    void combinedBasenameNamespacesOutput() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::PstItemDetail item;
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        sak::MboxWriter writer(temp_dir.path(), false, QStringLiteral("job_alpha"));
        std::ignore = writer.writeMessage(item, {}, QString());
        writer.finalize();

        // The single combined mailbox is named for the source, not the fixed mailbox.mbox, so a
        // second job in the same directory cannot silently truncate this one.
        QVERIFY(QFile::exists(temp_dir.path() + QStringLiteral("/job_alpha.mbox")));
        QVERIFY(!QFile::exists(temp_dir.path() + QStringLiteral("/mailbox.mbox")));
    }

    // ====================================================================
    // Sender CR/LF must not forge a second From_ separator line
    // ====================================================================

    void senderCrlfDoesNotForgeSeparator() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("hi");
        item.sender_email =
            QStringLiteral("a@test.com\r\nFrom attacker@evil.com Mon Jan  1 00:00:00 2000");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        std::ignore = writer.writeMessage(item, {}, QString());
        writer.finalize();

        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        // The injected CRLF was collapsed, so no forged "From " separator line appears.
        QVERIFY(!content.contains("\nFrom attacker@evil.com"));
        // A sender holding spaces, which this one does once its CR and LF collapse, cannot go on
        // the separator line, so that line carries the unknown-sender marker and none of the text.
        QCOMPARE(content.left(content.indexOf('\n')),
                 QByteArray("From - Wed Jan 01 00:00:00 2025"));
        // The From: header keeps it, as one line, so it still cannot forge anything.
        QVERIFY(content.contains(
            "From: a@test.com  From attacker@evil.com Mon Jan  1 00:00:00 2000\r\n"));
        QCOMPARE(content.count("\nFrom "), 0);
    }

    // ====================================================================
    // MIME boundary is high-entropy random, not a predictable derivation
    // ====================================================================

    void boundaryIsRandomPerMessage() {
        const auto emit_and_read = [](const QString& dir) {
            sak::MboxWriter writer(dir, false);
            sak::PstItemDetail item;
            item.sender_email = QStringLiteral("s@test.com");
            item.body_html = QStringLiteral("<p>hello world</p>");
            item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
            std::ignore = writer.writeMessage(item, {}, QString());
            writer.finalize();
            QFile f(dir + QStringLiteral("/mailbox.mbox"));
            if (!f.open(QIODevice::ReadOnly)) {
                return QString();
            }
            return QString::fromUtf8(f.readAll());
        };

        QTemporaryDir d1;
        QTemporaryDir d2;
        QVERIFY(d1.isValid() && d2.isValid());
        const QString a = emit_and_read(d1.path());
        const QString b = emit_and_read(d2.path());

        static const QRegularExpression re(QStringLiteral("boundary=\"([^\"]+)\""));
        const auto ma = re.match(a);
        const auto mb = re.match(b);
        QVERIFY(ma.hasMatch());
        QVERIFY(mb.hasMatch());
        // Identical input (same node_id 0, same timestamp) once produced identical boundaries; the
        // randomized generator must now differ, and the boundary must not appear in the body.
        QVERIFY(ma.captured(1) != mb.captured(1));
        // The boundary appears exactly three times: the Content-Type declaration plus the opening
        // and closing delimiters. The old `!contains(boundary + "hello")` was near-vacuous (a
        // random boundary essentially never precedes body text) and pinned no structure.
        QCOMPARE(a.count(ma.captured(1)), 3);
        QCOMPARE(b.count(mb.captured(1)), 3);
        // ...and those three are the declaration, the opening delimiter of the single body part and
        // the CLOSING delimiter, which must carry its terminating "--". Emitting "--<b>\r\n" in
        // place of "--<b>--\r\n" leaves the multipart unterminated -- a strict reader swallows the
        // rest of the mailbox into this message -- and the bare count of 3 is unchanged. `b` was
        // matched but its structure was asserted nowhere at all.
        QCOMPARE(a.count(QStringLiteral("--") + ma.captured(1) +
                         QStringLiteral("\r\nContent-Type: text/html; charset=utf-8\r\n")),
                 1);
        QCOMPARE(a.count(QStringLiteral("--") + ma.captured(1) + QStringLiteral("--\r\n")), 1);
    }

    // ====================================================================
    // writeFully() short-write safety (B7-18)
    // ====================================================================

    void writeFullyWritesEveryByte() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        const QString path = temp_dir.filePath(QStringLiteral("full.bin"));
        const QByteArray payload("The quick brown fox jumps over the lazy dog.", 44);
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(sak::writeFully(f, payload));
        }
        QFile r(path);
        QVERIFY(r.open(QIODevice::ReadOnly));
        QCOMPARE(r.readAll(), payload);
    }

    void writeFullyEmptyPayloadSucceeds() {
        QBuffer buf;
        QVERIFY(buf.open(QIODevice::WriteOnly));
        QVERIFY(sak::writeFully(buf, QByteArray()));
        QCOMPARE(buf.data().size(), 0);
    }

    void writeFullyFailsOnShortWrite() {
        // A device that accepts only `cap` bytes then stalls (returns 0) must make
        // writeFully report failure instead of silently truncating.
        CappedDevice dev(3);
        QVERIFY(!sak::writeFully(dev, QByteArrayLiteral("hello")));

        CappedDevice ok(100);
        QVERIFY(sak::writeFully(ok, QByteArrayLiteral("hello")));
        QCOMPARE(ok.accepted(), static_cast<qint64>(5));
    }

    // Unsent drafts and saved items name no sender. They are mail, so they are written: the
    // separator carries Thunderbird's own unknown-sender marker, and no From: header is invented.
    void writesAMessageWithNoSender() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("no sender");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        // A sender of nothing but spaces is no sender, not a malformed one.
        item.sender_email = QStringLiteral("   ");
        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        writer.finalize();

        QFile file(temp_dir.filePath(QStringLiteral("mailbox.mbox")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("From - Wed Jan 01 00:00:00 2025\n"));
        QVERIFY(!bytes.contains("From: "));
    }

    void keepsADisplayNameWhenThereIsNoAddress() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);
        sak::PstItemDetail item;
        item.sender_name = QStringLiteral("Dana Whitlock");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        writer.finalize();
        QFile file(temp_dir.filePath(QStringLiteral("mailbox.mbox")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("From: Dana Whitlock\r\n"));
    }

    // A sender carrying its own angle brackets would name someone other than the message did.
    void refusesASenderCarryingAngleBrackets() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("bracketed");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
        item.sender_email = QStringLiteral("<evil@example.com>");
        auto bracketed = writer.writeMessage(item, {}, QString());
        QVERIFY(!bracketed.has_value());
        QCOMPARE(bracketed.error(), sak::error_code::validation_failed);

        // A refused message must not have cost the destination: the entry is assembled BEFORE the
        // mailbox is opened, and opening truncates.
        QCOMPARE(QDir(temp_dir.path()).entryList({QStringLiteral("*.mbox")}, QDir::Files),
                 QStringList());
    }

    // An Exchange path holds spaces, which the separator line cannot carry; a reader would split
    // the line wrongly. The From: header keeps the sender as recorded.
    void keepsAnExchangePathOutOfTheSeparatorLine() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);
        sak::PstItemDetail item;
        item.sender_email = QStringLiteral("/O=First Organization/CN=RECIPIENTS/CN=OWEN");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        writer.finalize();
        QFile file(temp_dir.filePath(QStringLiteral("mailbox.mbox")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("From - "));
        QVERIFY(bytes.contains("From: /O=First Organization/CN=RECIPIENTS/CN=OWEN\r\n"));
    }

    // An unsent item has only the store's timestamp. It dates the separator so the record is well
    // formed, and is never presented as when the message was sent.
    void usesTheStoreTimestampOnlyOnTheSeparatorLine() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);
        sak::PstItemDetail item;
        item.subject = QStringLiteral("draft");
        item.body_plain = QStringLiteral("body");
        item.store_timestamp = QDateTime(QDate(2025, 2, 3), QTime(4, 5, 6), QTimeZone::utc());
        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        writer.finalize();
        QFile file(temp_dir.filePath(QStringLiteral("mailbox.mbox")));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        QVERIFY(bytes.startsWith("From - Mon Feb 03 04:05:06 2025\n"));
        QVERIFY(!bytes.contains("Date: "));
    }

    // A message with no time of any kind must fail closed rather than stamp the current
    // time into the envelope/Date header (no-fallback rule).
    void failsClosedOnNoTimeOfAnyKind() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("no date");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("body");
        // item.date and item.store_timestamp left invalid

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::missing_required_field);
    }

    // Both the plain-text and HTML alternatives must be preserved; the plain part
    // must not be dropped merely because an HTML body exists.
    void keepsBothPlainAndHtmlBodies() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("both");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("PLAIN_ALTERNATIVE_TEXT");
        item.body_html = QStringLiteral("<p>HTML_ALTERNATIVE_TEXT</p>");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        QVERIFY(writer.writeMessage(item, {}, QString()).has_value());
        writer.finalize();

        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        // Both alternatives survive, ONCE each, inside the container the two-arm outer-type guard
        // picks for "two bodies, no attachments": multipart/ALTERNATIVE. Emitted as
        // multipart/mixed, a client shows the message TWICE -- exactly what that guard exists to
        // prevent -- and no assertion in this file named the outer type, so deleting the ternary
        // was green.
        QCOMPARE(content.count("PLAIN_ALTERNATIVE_TEXT"), 1);
        QCOMPARE(content.count("HTML_ALTERNATIVE_TEXT"), 1);
        QCOMPARE(content.count("Content-Type: text/plain; charset=utf-8\r\n"), 1);
        QCOMPARE(content.count("Content-Type: text/html; charset=utf-8\r\n"), 1);
        QCOMPARE(content.count("multipart/alternative"), 1);
        QCOMPARE(content.count("multipart/mixed"), 0);
        static const QRegularExpression alt_boundary_re(QStringLiteral("boundary=\"([^\"]+)\""));
        const auto alt_match = alt_boundary_re.match(QString::fromUtf8(content));
        QVERIFY(alt_match.hasMatch());
        // One boundary declaration + one delimiter per body part + the closing delimiter = 4, and
        // no second (nested) boundary: there are no attachments to mix the alternatives with.
        QCOMPARE(content.count(alt_match.captured(1).toUtf8()), 4);
        // The closing delimiter must carry its terminating "--": without it the multipart is
        // unterminated and the count above is still 4.
        QCOMPARE(content.count("--" + alt_match.captured(1).toUtf8() + "--\r\n"), 1);
    }

    // Attachment base64 must be wrapped to <=76-column lines (RFC 2045).
    void attachmentBase64IsWrapped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("wrap");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("see attached");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        const QByteArray blob(300, 'A');
        QVERIFY(
            writer.writeMessage(item, {{QStringLiteral("a.bin"), blob}}, QString()).has_value());
        writer.finalize();

        QFile file(temp_dir.path() + QStringLiteral("/mailbox.mbox"));
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        const QByteArray unwrapped = blob.toBase64();
        QCOMPARE(unwrapped.size(), 400);  // 300 bytes, 3|300 -> 4/3 growth, no '=' padding
        QVERIFY(!content.contains(unwrapped));
        // base64Wrapped emits 76-column lines each terminated by CRLF, so 400 chars become five
        // full lines plus a 20-char tail -- 412 bytes -- and the closing MIME delimiter follows
        // the tail's CRLF immediately. The old contains(left(76)) was green for ANY wrap width
        // >= 76 and for a tail line left unterminated, which glues base64 onto the delimiter.
        QByteArray expected_block;
        for (qsizetype i = 0; i < unwrapped.size(); i += 76) {
            expected_block += unwrapped.mid(i, 76) + "\r\n";
        }
        QCOMPARE(expected_block.size(), 412);
        QCOMPARE(content.count(expected_block), 1);
        QVERIFY(content.contains(expected_block + "--"));
    }

private:
    // Test double: accepts at most `cap` bytes, then every further write stalls at 0.
    class CappedDevice : public QIODevice {
    public:
        explicit CappedDevice(qint64 cap) : m_cap(cap) { open(QIODevice::WriteOnly); }
        [[nodiscard]] qint64 accepted() const { return m_written; }

    protected:
        qint64 writeData(const char* /*data*/, qint64 len) override {
            if (m_written >= m_cap) {
                return 0;  // stalled
            }
            const qint64 n = qMin(len, m_cap - m_written);
            m_written += n;
            return n;
        }
        qint64 readData(char* /*data*/, qint64 /*len*/) override { return -1; }

    private:
        qint64 m_cap;
        qint64 m_written = 0;
    };

private Q_SLOTS:

    // ====================================================================
    // Folder-name sanitisation
    // ====================================================================

    // FOUND BY A MUTATION DRILL, not by review. When the five private copies of the reserved
    // device-name catalogue were migrated onto the shared sak::isWindowsReservedName, breaking
    // that helper turned three of the four migrated suites RED -- and this one stayed GREEN,
    // because sanitizeFolderName had NO test at all. Its reserved-name arm was therefore
    // unproven: an "arm coverage proves never taken" hole.
    //
    // It matters here more than most places. A mail folder legitimately called "Con" or "Prn"
    // is ordinary, and on Windows opening "NUL.mbox" SUCCEEDS while silently discarding every
    // message written to it -- an export that reports success and produces nothing.
    void testSanitizeFolderNameEscapesReservedDeviceNames() {
        for (const QString& reserved : {QStringLiteral("CON"),
                                        QStringLiteral("con"),
                                        QStringLiteral("Aux"),
                                        QStringLiteral("NUL"),
                                        QStringLiteral("COM1"),
                                        QStringLiteral("lpt9"),
                                        QStringLiteral("NUL.mbox")}) {
            const QString safe = sak::MboxWriter::sanitizeFolderName(reserved);
            QVERIFY2(!safe.isEmpty(), qPrintable(reserved));
            // The name must no longer BE the device: the guard appends a suffix, so the result
            // differs from the input and is not itself reserved.
            QVERIFY2(safe != reserved, qPrintable(reserved + QStringLiteral(" -> ") + safe));
            QVERIFY2(safe.endsWith(QLatin1Char('_')),
                     qPrintable(reserved + QStringLiteral(" -> ") + safe));
        }

        // Near-misses are ordinary folder names and must pass through unchanged, or the guard is
        // just mangling legitimate mail folders.
        for (const QString& ordinary : {QStringLiteral("Console"),
                                        QStringLiteral("COM0"),
                                        QStringLiteral("COM12"),
                                        QStringLiteral("Inbox"),
                                        QStringLiteral("Auxiliary")}) {
            QCOMPARE(sak::MboxWriter::sanitizeFolderName(ordinary), ordinary);
        }

        // The trailing dot/space rule that sits immediately above the device guard: Windows
        // strips these, so two distinct folders would otherwise resolve to one file.
        QVERIFY(!sak::MboxWriter::sanitizeFolderName(QStringLiteral("Inbox "))
                     .endsWith(QLatin1Char(' ')));
        QVERIFY(!sak::MboxWriter::sanitizeFolderName(QStringLiteral("Inbox."))
                     .endsWith(QLatin1Char('.')));
        QVERIFY(sak::MboxWriter::sanitizeFolderName(QStringLiteral("Inbox ")) !=
                sak::MboxWriter::sanitizeFolderName(QStringLiteral("Inbox")));
    }
};

QTEST_MAIN(TestMboxWriter)

#include "test_mbox_writer.moc"
