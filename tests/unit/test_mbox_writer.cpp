// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_writer.cpp
/// @brief Unit tests for MboxWriter Unix mailbox output

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/io_write_utils.h"
#include "sak/mbox_writer.h"

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QIODevice>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTest>
#include <QTimeZone>

#include <tuple>

class TestMboxWriter : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // Happy Path -- Write single message
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
        // The message and its date are fixed, so the serialized size is deterministic. `> 0` would
        // survive a truncated write or a dropped header/body.
        QCOMPARE(writer.totalBytesWritten(), static_cast<qint64>(273));
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
        // Pin what the sanitizer actually produced: the CR and the LF each collapse to one space
        // (mbox_writer.cpp sanitizeHeaderValue, applied to the sender in formatMboxEntry), so the
        // injected text stays INSIDE the single envelope line and the addr-spec token is still
        // "a@test.com". The negative check alone is green for a sanitizer that DROPS the CR/LF
        // (fabricating "a@test.comFrom"), for one that truncates the sender away, and for a From_
        // line escaped into ">From" -- and it never proves the file holds exactly ONE record.
        QCOMPARE(content.left(content.indexOf('\n')),
                 QByteArray("From a@test.com  From attacker@evil.com Mon Jan  1 00:00:00 2000 "
                            "Wed Jan 01 00:00:00 2025"));
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

    // A message with no sender address must fail closed rather than fabricate an
    // "unknown@localhost" envelope sender (no-fallback rule).
    void failsClosedOnMissingSender() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("no sender");
        item.body_plain = QStringLiteral("body");
        item.date = QDateTime(QDate(2025, 1, 1), QTime(0, 0, 0), QTimeZone::utc());

        auto result = writer.writeMessage(item, {}, QString());
        QVERIFY(!result.has_value());
        QCOMPARE(result.error(), sak::error_code::missing_required_field);

        // The SIBLING arm of the same envelope guard is otherwise unreached in the whole suite: a
        // sender carrying its own angle brackets -- or one that is nothing but whitespace -- would
        // name a different sender than the message did, and is refused with a DIFFERENT code
        // (mbox_writer.cpp validateEnvelope -> validation_failed). Deleting that branch outright
        // left every test green.
        item.sender_email = QStringLiteral("<evil@example.com>");
        auto bracketed = writer.writeMessage(item, {}, QString());
        QVERIFY(!bracketed.has_value());
        QCOMPARE(bracketed.error(), sak::error_code::validation_failed);

        item.sender_email = QStringLiteral("   ");
        auto blank = writer.writeMessage(item, {}, QString());
        QVERIFY(!blank.has_value());
        QCOMPARE(blank.error(), sak::error_code::validation_failed);

        // A refused message must not have cost the destination: the entry is assembled BEFORE the
        // mailbox is opened, and opening truncates.
        QCOMPARE(QDir(temp_dir.path()).entryList({QStringLiteral("*.mbox")}, QDir::Files),
                 QStringList());
    }

    // A message with no valid date must fail closed rather than stamp the current
    // time into the envelope/Date header (no-fallback rule).
    void failsClosedOnMissingDate() {
        QTemporaryDir temp_dir;
        QVERIFY(temp_dir.isValid());
        sak::MboxWriter writer(temp_dir.path(), false);

        sak::PstItemDetail item;
        item.subject = QStringLiteral("no date");
        item.sender_email = QStringLiteral("s@test.com");
        item.body_plain = QStringLiteral("body");
        // item.date left invalid

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
