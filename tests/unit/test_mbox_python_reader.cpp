// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_mbox_python_reader.cpp
/// @brief A converted mailbox, read back by Python's mailbox module instead of the app's own
/// parser.
///
/// Importing into a real mail client stays a manual step on a VM. This is the check that does not
/// need one: another implementation of the format, reading the same bytes.

#include "sak/email_constants.h"
#include "sak/mbox_parser.h"
#include "sak/ost_conversion_worker.h"

#include "../support/pst_store_builder.h"

#include <QCryptographicHash>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

constexpr int kPythonTimeoutMs = 60'000;

const QByteArray kAttachmentBytes = QByteArray("%PDF-1.4 a real file's bytes\n").repeated(20);

BuilderMessage mail(const QString& subject, const QDateTime& date) {
    BuilderMessage msg;
    msg.subject = subject;
    msg.sender_name = QStringLiteral("Owen Lark");
    msg.sender_email = QStringLiteral("owen@harborlane.example");
    msg.display_to = QStringLiteral("Dana Whitlock");
    msg.date = date;
    msg.body_plain = QStringLiteral("Body of ") + subject;
    return msg;
}

}  // namespace

class MboxPythonReaderTests : public QObject {
    Q_OBJECT

private:
    /// Convert a store holding the shapes an MBOX has to get right. Returns the mailbox written.
    static QString convertedMailbox(QTemporaryDir& dir, const QDateTime& first_date) {
        BuilderMessage plain = mail(QStringLiteral("Oven repair quote"), first_date);
        // A line that begins "From " is what an MBOX has to protect: unescaped it splits the
        // message.
        plain.body_plain = QStringLiteral("Line one\nFrom the oven company: a quote\nLine three");
        BuilderMessage attached = mail(QStringLiteral("Invoice 1042"), first_date.addDays(1));
        attached.attachments = {{QStringLiteral("invoice.pdf"),
                                 QStringLiteral("application/pdf"),
                                 kAttachmentBytes,
                                 {}}};
        BuilderMessage accented = mail(QStringLiteral("Caf\u00E9 menu \u4F1A\u8BAE"),
                                       first_date.addDays(2));
        // Real stores give ordinary text attachments a Content-ID, which comes out inline.
        BuilderMessage inline_file = mail(QStringLiteral("Inline file"), first_date.addDays(4));
        inline_file.attachments = {{QStringLiteral("caf\u00E9 notes.txt"),
                                    QStringLiteral("text/plain"),
                                    QByteArray("the notes"),
                                    QStringLiteral("notes@example.test")}};
        BuilderMessage unsent = mail(QStringLiteral("Draft"), {});
        unsent.sender_name.clear();
        unsent.sender_email.clear();
        unsent.created = first_date.addDays(3);
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {plain, attached, accented, unsent, inline_file};

        const QString store = dir.filePath(QStringLiteral("store.pst"));
        QFile file(store);
        if (!file.open(QIODevice::WriteOnly)) {
            return {};
        }
        file.write(PstStoreBuilder{}.build({inbox}));
        file.close();

        sak::OstConversionConfig config;
        config.output_directory = dir.filePath(QStringLiteral("out"));
        QDir().mkpath(config.output_directory);
        config.one_mbox_per_folder = false;
        config.include_source_checksums = false;
        sak::OstConversionWorker worker;
        sak::OstConversionResult result;
        QObject::connect(&worker,
                         &sak::OstConversionWorker::conversionFinished,
                         &worker,
                         [&result](const sak::OstConversionResult& done) { result = done; });
        worker.convert(store, config);
        if (result.items_converted != 5) {
            return {};
        }
        QDirIterator found(config.output_directory,
                           {QStringLiteral("*.mbox")},
                           QDir::Files,
                           QDirIterator::Subdirectories);
        return found.hasNext() ? found.next() : QString();
    }

    /// What Python's mailbox module says is in @p mailbox_path. Empty when it could not say.
    static QJsonObject readWithPython(const QString& mailbox_path) {
        const QByteArray python = qgetenv("SAK_TEST_PYTHON");
        const QByteArray script = qgetenv("SAK_TEST_MBOX_READER");
        if (python.isEmpty() || script.isEmpty()) {
            qWarning("CMake sets the Python and script paths for this test");
            return {};
        }
        QProcess reader;
        reader.start(QString::fromLocal8Bit(python),
                     {QStringLiteral("-I"), QString::fromLocal8Bit(script), mailbox_path});
        if (!reader.waitForFinished(kPythonTimeoutMs) || reader.exitCode() != 0) {
            qWarning("%s", reader.readAllStandardError().constData());
            return {};
        }
        return QJsonDocument::fromJson(reader.readAllStandardOutput()).object();
    }

private Q_SLOTS:
    void pythonsMailboxModuleReadsAConvertedStore() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDateTime first_date(QDate(2026, 3, 9), QTime(14, 5), QTimeZone::UTC);
        const QString mailbox_path = convertedMailbox(dir, first_date);
        QVERIFY(!mailbox_path.isEmpty());

        const QJsonObject report = readWithPython(mailbox_path);
        QCOMPARE(report.value(QStringLiteral("count")).toInt(), 5);
        const QJsonArray messages = report.value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 5);

        const auto field = [&messages](int index, const char* name) {
            return messages.at(index).toObject().value(QLatin1String(name));
        };
        for (int i = 0; i < messages.size(); ++i) {
            QVERIFY(field(i, "has_separator").toBool());
        }

        QCOMPARE(field(0, "subject").toString(), QStringLiteral("Oven repair quote"));
        QVERIFY2(field(0, "from").toString().contains(QStringLiteral("owen@harborlane.example")),
                 qPrintable(field(0, "from").toString()));
        QCOMPARE(static_cast<qint64>(field(0, "date_utc").toDouble()),
                 first_date.toSecsSinceEpoch());
        // The stuffed line comes back whole inside the one message, on one reader's own terms.
        const QString body = field(0, "body").toString();
        QVERIFY2(body.contains(QStringLiteral("Line one")) &&
                     body.contains(QStringLiteral("From the oven company: a quote")) &&
                     body.contains(QStringLiteral("Line three")),
                 qPrintable(body));

        QCOMPARE(field(1, "subject").toString(), QStringLiteral("Invoice 1042"));
        const QJsonArray attachments = field(1, "attachments").toArray();
        QCOMPARE(attachments.size(), 1);
        const QJsonObject attachment = attachments.first().toObject();
        QCOMPARE(attachment.value(QStringLiteral("name")).toString(),
                 QStringLiteral("invoice.pdf"));
        QCOMPARE(attachment.value(QStringLiteral("size")).toInt(), kAttachmentBytes.size());
        QCOMPARE(attachment.value(QStringLiteral("sha256")).toString().toLatin1(),
                 QCryptographicHash::hash(kAttachmentBytes, QCryptographicHash::Sha256).toHex());

        QCOMPARE(field(2, "subject").toString(), QStringLiteral("Caf\u00E9 menu \u4F1A\u8BAE"));

        // Never sent: no From and no Date header, though the separator line still has a time.
        QCOMPARE(field(3, "subject").toString(), QStringLiteral("Draft"));
        QVERIFY(field(3, "date_utc").isNull());
        QCOMPARE(field(4, "subject").toString(), QStringLiteral("Inline file"));
    }

    // The app's own reader and Python's agree on what a message carries, message by message.
    void theAppsReaderAndPythonsAgreeOnAttachments() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QDateTime first_date(QDate(2026, 3, 9), QTime(14, 5), QTimeZone::UTC);
        const QString mailbox_path = convertedMailbox(dir, first_date);
        QVERIFY(!mailbox_path.isEmpty());
        const QJsonArray messages =
            readWithPython(mailbox_path).value(QStringLiteral("messages")).toArray();
        QCOMPARE(messages.size(), 5);

        MboxParser parser;
        parser.open(mailbox_path);
        QVERIFY(parser.isOpen());
        parser.indexMessages();
        QCOMPARE(parser.messageCount(), 5);
        for (int i = 0; i < messages.size(); ++i) {
            const auto detail = parser.readMessageDetail(i);
            QVERIFY(detail.has_value());
            const QJsonArray theirs =
                messages.at(i).toObject().value(QStringLiteral("attachments")).toArray();
            QCOMPARE(detail->attachments.size(), theirs.size());
            for (int a = 0; a < theirs.size(); ++a) {
                QCOMPARE(detail->attachments.at(a).long_filename,
                         theirs.at(a).toObject().value(QStringLiteral("name")).toString());
            }
        }
        const auto inline_detail = parser.readMessageDetail(4);
        QVERIFY(inline_detail.has_value());
        QCOMPARE(inline_detail->attachments.size(), 1);
        QCOMPARE(inline_detail->attachments.first().long_filename,
                 QStringLiteral("caf\u00E9 notes.txt"));
        QVERIFY(inline_detail->body_plain.contains(QStringLiteral("Body of Inline file")));
        parser.close();
    }
};

QTEST_MAIN(MboxPythonReaderTests)
#include "test_mbox_python_reader.moc"
