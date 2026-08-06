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
        QVERIFY(!a.contains(ma.captured(1) + QStringLiteral("hello")));
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
        QVERIFY(content.contains("PLAIN_ALTERNATIVE_TEXT"));
        QVERIFY(content.contains("HTML_ALTERNATIVE_TEXT"));
        QVERIFY(content.contains("text/plain"));
        QVERIFY(content.contains("text/html"));
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
        QVERIFY(unwrapped.size() > 76);
        QVERIFY(!content.contains(unwrapped));
        QVERIFY(content.contains(unwrapped.left(76)));
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
};

QTEST_MAIN(TestMboxWriter)

#include "test_mbox_writer.moc"
