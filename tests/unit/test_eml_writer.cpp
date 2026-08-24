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
    // Happy Path -- Simple Plain-Text Message
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
        // The whole message is deterministic apart from the RFC 2822 Date value, so pin it
        // byte-for-byte around that single line. Header ORDER, the CRLF terminators, the blank
        // header/body separator and the 8bit label are all load-bearing and none of them is
        // observable through contains().
        const qsizetype date_at = content.indexOf("\r\nDate: ");
        QVERIFY(date_at > 0);
        const qsizetype date_end = content.indexOf("\r\n", date_at + 2);
        QVERIFY(date_end > date_at);
        QCOMPARE(content.left(date_at + 2),
                 QByteArray("From: John Doe <john@example.com>\r\n"
                            "To: jane@example.com\r\n"
                            "Subject: Test Subject\r\n"));
        QCOMPARE(content.mid(date_end + 2),
                 QByteArray("MIME-Version: 1.0\r\n"
                            "Content-Type: text/plain; charset=utf-8\r\n"
                            "Content-Transfer-Encoding: 8bit\r\n"
                            "\r\n"
                            "Hello, this is a test email."));
    }

    // ====================================================================
    // HTML Message -- Multipart Alternative
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

        // Extract the declared boundary and prove the PARTS actually use it, in RFC 2046 order
        // (least-preferred text/plain first), and that the closing delimiter terminates the
        // entity. Membership of three Content-Type strings proves none of that.
        const QByteArray kDecl = "Content-Type: multipart/alternative; boundary=\"";
        const qsizetype decl_at = content.indexOf(kDecl);
        QVERIFY(decl_at >= 0);
        const qsizetype b_start = decl_at + kDecl.size();
        const qsizetype b_end = content.indexOf('"', b_start);
        QVERIFY(b_end > b_start);
        const QByteArray boundary = content.mid(b_start, b_end - b_start);
        QVERIFY(boundary.startsWith("----=_SAK_Part_"));

        const qsizetype plain_at = content.indexOf("--" + boundary +
                                                   "\r\nContent-Type: text/plain; charset=utf-8"
                                                   "\r\nContent-Transfer-Encoding: 8bit\r\n\r\n"
                                                   "Plain text version\r\n");
        const qsizetype html_at = content.indexOf("--" + boundary +
                                                  "\r\nContent-Type: text/html; charset=utf-8"
                                                  "\r\nContent-Transfer-Encoding: 8bit\r\n\r\n"
                                                  "<html><body><b>Bold test</b></body></html>\r\n");
        QVERIFY(plain_at > 0);
        QVERIFY(html_at > plain_at);  // RFC 2046: plain before html
        QVERIFY(content.endsWith("--" + boundary + "--\r\n"));
    }

    // ====================================================================
    // Message with Attachments -- Multipart Mixed
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

        // The MIME parameters existing proves nothing about the PART STRUCTURE or the payload.
        // Pin the whole attachment part, base64 of "fake pdf content" included, plus the body
        // part that must precede it and the closing delimiter.
        const QByteArray kDecl = "Content-Type: multipart/mixed; boundary=\"";
        const qsizetype decl_at = content.indexOf(kDecl);
        QVERIFY(decl_at >= 0);
        const qsizetype b_start = decl_at + kDecl.size();
        const qsizetype b_end = content.indexOf('"', b_start);
        QVERIFY(b_end > b_start);
        const QByteArray boundary = content.mid(b_start, b_end - b_start);

        const qsizetype body_at = content.indexOf("--" + boundary +
                                                  "\r\nContent-Type: text/plain; charset=utf-8"
                                                  "\r\nContent-Transfer-Encoding: 8bit\r\n\r\n"
                                                  "See attached.\r\n");
        QVERIFY(body_at > 0);
        const qsizetype att_at =
            content.indexOf("--" + boundary +
                            "\r\nContent-Type: application/octet-stream; name=\"document.pdf\""
                            "\r\nContent-Transfer-Encoding: base64"
                            "\r\nContent-Disposition: attachment; filename=\"document.pdf\"\r\n\r\n"
                            "ZmFrZSBwZGYgY29udGVudA==\r\n");
        QVERIFY(att_at > body_at);
        QVERIFY(content.endsWith("--" + boundary + "--\r\n"));
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
        QCOMPARE(result.value(), expected_dir + "/Subfolder Test.eml");
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
        QCOMPARE(fi.fileName(), QStringLiteral("2025-03-15_143000_Dated Message.eml"));
    }

    // ====================================================================
    // Filename Sanitization -- Invalid Characters
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
        // <, >, ':' and '"' each map to '_' via the invalid-char regex; pin the whole
        // filename so dropping any single char from that class (e.g. ':') is caught.
        QCOMPARE(fi.fileName(), QStringLiteral("Re_ Invoice _2025_ for _Project_.eml"));

        // The invalid-char class is [<>:"/\|?*\x00-\x1F]; the fixture above only covers <, >, ':'
        // and '"'. Cover every remaining member (/, \, |, ?, * and a C0 control) so dropping any
        // one of them from the class is caught -- a stray '/' or '\' turns the subject into a
        // path component instead of a filename.
        item.subject = QStringLiteral("a/b\\c|d?e*f\tg");
        auto result2 = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(result2.has_value());
        QCOMPARE(QFileInfo(result2.value()).fileName(), QStringLiteral("a_b_c_d_e_f_g.eml"));
    }

    // ====================================================================
    // Empty Subject -- Fallback Filename
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
        QCOMPARE(fi.fileName(), QStringLiteral("no_subject.eml"));

        // The fallback keys off subject.trimmed().isEmpty(), and the same trim also strips padding
        // from a NON-empty subject. An already-empty subject reaches neither behaviour, so pin
        // both arms: whitespace-only falls back (and collides with the first file), and a padded
        // subject is trimmed rather than left with spaces baked into the name.
        item.subject = QStringLiteral("   \t  ");
        auto blank = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(blank.has_value());
        QCOMPARE(QFileInfo(blank.value()).fileName(), QStringLiteral("no_subject_(1).eml"));

        item.subject = QStringLiteral("  Padded  ");
        auto padded = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(padded.has_value());
        QCOMPARE(QFileInfo(padded.value()).fileName(), QStringLiteral("Padded.eml"));
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

        // The collision resolver keeps the first name and suffixes the second "_(1)".
        QCOMPARE(QFileInfo(result1.value()).fileName(), QStringLiteral("Same Subject.eml"));
        QCOMPARE(QFileInfo(result2.value()).fileName(), QStringLiteral("Same Subject_(1).eml"));

        // Both files should exist
        QVERIFY(QFile::exists(result1.value()));
        QVERIFY(QFile::exists(result2.value()));
    }

    // ====================================================================
    // Bytes Written Tracking
    // ====================================================================

    // ====================================================================
    // Header Injection -- CRLF in header values must not forge headers
    // ====================================================================

    void testHeaderInjectionStripped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Hello\r\nBcc: attacker@evil.com");
        item.sender_name = QStringLiteral("Sender");
        item.sender_email = QStringLiteral("sender@test.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        // The CRLF is collapsed, so no forged Bcc header line appears in the
        // header block; the subject text stays on the Subject line.
        QVERIFY(!content.contains("\nBcc: attacker@evil.com"));
        // Pin the folded line WITH its terminator: a trailing CRLF proves the injected
        // text ends the Subject rather than running into the next header. (The old
        // contains("attacker@evil.com") below it was vacuous -- strictly implied by this.)
        QVERIFY(content.contains("Subject: Hello  Bcc: attacker@evil.com\r\n"));

        // sanitizeHeaderValue has three arms; only the CR/LF one is reached above. TAB
        // must fold to a space (an obs-FWS header-continuation vector) and every OTHER
        // C0 control must be dropped -- a BEL or SOH left in a header is passed through
        // verbatim to downstream MTAs.
        item.subject = QStringLiteral("A\tB\x01") + QStringLiteral("C\aD");
        auto second = writer.writeMessage(item, {}, QString());
        QVERIFY(second.has_value());
        QFile file2(second.value());
        QVERIFY(file2.open(QIODevice::ReadOnly));
        QVERIFY(file2.readAll().contains(QByteArray("Subject: A BCD\r\n")));

        // encodedDisplayName's non-ASCII arm is otherwise dead -- no fixture in this file sets a
        // non-ASCII sender_name. The display NAME must be RFC 2047 'B'-encoded while the addr-spec
        // stays RAW (RFC 2047 forbids an encoded-word inside an address), so pin the whole From
        // line.
        item.sender_name = QString::fromUtf8(
            "J\xC3\xBC"
            "rgen");  // "Jurgen" with u-umlaut
        item.sender_email = QStringLiteral("s@example.com");
        item.subject = QStringLiteral("ascii");
        auto from_result = writer.writeMessage(item, {}, QString());
        QVERIFY(from_result.has_value());
        QFile from_file(from_result.value());
        QVERIFY(from_file.open(QIODevice::ReadOnly));
        const QByteArray from_content = from_file.readAll();
        QVERIFY(from_content.startsWith("From: =?UTF-8?B?SsO8cmdlbg==?= <s@example.com>\r\n"));
        QVERIFY(!from_content.contains(QByteArray("J\xC3\xBC")));  // no raw 8-bit in From
    }

    // ====================================================================
    // Body Transfer-Encoding -- raw UTF-8 must be labelled 8bit, not QP
    // ====================================================================

    void testPlainBodyLabeled8bit() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("Encoding");
        item.sender_name = QStringLiteral("Sender");
        // A body with "=20" would be corrupted if decoded as quoted-printable.
        item.body_plain = QStringLiteral("value=20more");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());

        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();

        QVERIFY(content.contains("Content-Transfer-Encoding: 8bit"));
        QVERIFY(!content.contains("quoted-printable"));
        QVERIFY(content.contains("value=20more"));
    }

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
        QCOMPARE(writer.totalBytesWritten(), QFileInfo(result.value()).size());

        // The counter ACCUMULATES across messages, so a second write must SUM rather than
        // replace. A single message cannot tell "+=" from "=", and email_export_worker reads this
        // value as a per-item delta.
        item.subject = QStringLiteral("Track Bytes Two");
        item.body_plain = QStringLiteral("A different and noticeably longer second body.");
        auto second = writer.writeMessage(item, no_attachments, QString());
        QVERIFY(second.has_value());
        QCOMPARE(writer.totalBytesWritten(),
                 QFileInfo(result.value()).size() + QFileInfo(second.value()).size());
    }

    // A generated _(n) collision name must itself be checked for existence so a
    // distinct message is never overwritten (P05-35 residual). Writing "foo",
    // then "foo_(1)", then "foo" again must yield three distinct files -- the
    // third must not clobber the second's foo_(1).eml.
    void collisionSuffixDoesNotOverwriteDistinctMessage() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());

        sak::EmlWriter writer(temp_dir.path(), false, false);
        QVector<QPair<QString, QByteArray>> none;

        auto write = [&](const QString& subject) {
            sak::PstItemDetail item;
            item.subject = subject;
            item.sender_email = QStringLiteral("s@example.com");
            item.body_plain = QStringLiteral("body of ") + subject;
            item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());
            return writer.writeMessage(item, none, QString());
        };

        QVERIFY(write(QStringLiteral("foo")).has_value());
        auto second = write(QStringLiteral("foo_(1)"));
        QVERIFY(second.has_value());
        auto third = write(QStringLiteral("foo"));
        QVERIFY(third.has_value());

        // Three distinct files; the second file's content is intact.
        QCOMPARE(QDir(temp_dir.path()).entryList({QStringLiteral("*.eml")}, QDir::Files).size(), 3);
        QVERIFY(third.value() != second.value());
        // Pin the generated names, not merely "different": the third "foo" must SKIP the
        // already-taken foo_(1).eml and land on foo_(2).eml.
        QCOMPARE(QFileInfo(second.value()).fileName(), QStringLiteral("foo_(1).eml"));
        QCOMPARE(QFileInfo(third.value()).fileName(), QStringLiteral("foo_(2).eml"));
        QFile f(second.value());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().contains("body of foo_(1)"));
    }

    // A non-ASCII Subject must be RFC 2047 'B'-encoded, not emitted as raw 8-bit
    // (which RFC 5322 readers reject/mis-decode).
    void nonAsciiSubjectIsRfc2047Encoded() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        // Split adjacent literals so \x does not greedily absorb the following ASCII
        // hex-letters ("ber"/"50") into an out-of-range escape.
        item.subject = QString::fromUtf8(
            "Rechnung \xC3\xBC"
            "ber \xE2\x82\xAC"
            "50");  // "uber EUR50" (u-umlaut + euro sign)
        item.sender_email = QStringLiteral("s@example.com");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(result.has_value());
        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        QVERIFY(content.contains("Subject: =?UTF-8?B?UmVjaG51bmcgw7xiZXIg4oKsNTA=?="));
        QVERIFY(
            !content.contains(QByteArray("\xC3\xBC"
                                         "ber")));  // no raw 8-bit in header
    }

    // Attachment base64 must be wrapped to <=76-column lines (RFC 2045); a single
    // unbroken run is rejected by strict MIME readers.
    void attachmentBase64IsWrapped() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::EmlWriter writer(temp_dir.path(), false, false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("wrap");
        item.sender_email = QStringLiteral("s@example.com");
        item.body_plain = QStringLiteral("see attached");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        const QByteArray blob(300, 'A');
        auto result = writer.writeMessage(item, {{QStringLiteral("a.bin"), blob}}, QString());
        QVERIFY(result.has_value());
        QFile file(result.value());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray content = file.readAll();
        const QByteArray unwrapped = blob.toBase64();
        // 300 bytes -> 400 base64 chars -> five full 76-column lines plus a 20-char tail, each
        // terminated by CRLF. Pinning the whole run catches a wrap that is correct only on the
        // first line, an LF-only terminator, and a dropped tail line.
        QCOMPARE(unwrapped.size(), static_cast<qsizetype>(400));
        QVERIFY(!content.contains(unwrapped));  // not one giant line
        const QByteArray wrapped = unwrapped.mid(0, 76) + "\r\n" + unwrapped.mid(76, 76) + "\r\n" +
                                   unwrapped.mid(152, 76) + "\r\n" + unwrapped.mid(228, 76) +
                                   "\r\n" + unwrapped.mid(304, 76) + "\r\n" +
                                   unwrapped.mid(380, 20) + "\r\n";
        QVERIFY(content.contains(wrapped));
    }

    // A subfolder path that escapes the output directory must be refused, not
    // silently written outside the tree (defense-in-depth).
    void rejectsSubfolderTraversal() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::EmlWriter writer(temp_dir.path(), false, true);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("x");
        item.sender_email = QStringLiteral("s@example.com");
        item.body_plain = QStringLiteral("b");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        // Escape into a sibling of the temp dir whose name is unique to this run, so the "was
        // anything created outside?" check below cannot be confused by a leftover.
        const QString sibling = QFileInfo(temp_dir.path()).fileName() + QStringLiteral("_escaped");
        auto result = writer.writeMessage(item, {}, QStringLiteral("../") + sibling);
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::path_traversal_attempt);

        // TWO distinct guards return path_traversal_attempt: the lexical subfolderEscapes BEFORE
        // mkpath, and the junction/symlink targetWithinRoot AFTER it. Prove the lexical one
        // fired: nothing was created outside the output root, and nothing inside it either.
        QVERIFY(
            !QFileInfo::exists(QFileInfo(temp_dir.path()).path() + QStringLiteral("/") + sibling));
        QCOMPARE(QDir(temp_dir.path()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot),
                 QStringList());
    }
};

QTEST_MAIN(TestEmlWriter)
#include "test_eml_writer.moc"
