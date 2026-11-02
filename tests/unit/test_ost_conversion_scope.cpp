// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_ost_conversion_scope.cpp
/// @brief What a store-to-MBOX conversion covers, on whole generated stores.

#include "sak/mbox_parser.h"
#include "sak/ost_conversion_worker.h"
#include "sak/ost_converter_controller.h"
#include "sak/ost_converter_types.h"

#include "../support/pst_store_builder.h"

#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

BuilderMessage mail(const QString& subject) {
    BuilderMessage msg;
    msg.subject = subject;
    msg.sender_name = QStringLiteral("Owen Lark");
    msg.sender_email = QStringLiteral("owen@harborlane.example");
    msg.date = QDateTime(QDate(2026, 3, 9), QTime(14, 5), QTimeZone::UTC);
    msg.body_plain = QStringLiteral("Body of ") + subject;
    return msg;
}

BuilderFolder folder(const QString& name, const QString& cls, const QVector<BuilderMessage>& msgs) {
    BuilderFolder out;
    out.name = name;
    out.container_class = cls;
    out.messages = msgs;
    return out;
}

BuilderMessage nonMail(const QString& cls, const QString& subject) {
    BuilderMessage msg;
    msg.message_class = cls;
    msg.subject = subject;
    return msg;
}

/// Messages in every .mbox under @p dir, counted by the app's own reader.
int mailboxMessageCount(const QString& dir) {
    int total = 0;
    QDirIterator it(dir, {QStringLiteral("*.mbox")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        MboxParser parser;
        parser.open(it.next());
        if (!parser.isOpen() || !parser.readMessages(0, 1).has_value()) {
            return -1;
        }
        total += parser.messageCount();
    }
    return total;
}

QByteArray allMailboxBytes(const QString& dir) {
    QByteArray bytes;
    QDirIterator it(dir, {QStringLiteral("*.mbox")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QFile file(it.next());
        if (file.open(QIODevice::ReadOnly)) {
            bytes += file.readAll();
        }
    }
    return bytes;
}

}  // namespace

class TestOstConversionScope : public QObject {
    Q_OBJECT

private:
    QTemporaryDir m_dir;
    int m_counter = 0;

    struct Run {
        sak::OstConversionResult result;
        QString output;
    };

    Run convert(const QByteArray& store, sak::OstConversionConfig config = {}) {
        const QString stem = QStringLiteral("store%1").arg(++m_counter);
        const QString path = m_dir.filePath(stem + QStringLiteral(".pst"));
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(store);
            file.close();
        }
        Run run;
        run.output = m_dir.filePath(stem + QStringLiteral("_out"));
        QDir().mkpath(run.output);
        config.output_directory = run.output;
        config.include_source_checksums = false;
        sak::OstConversionWorker worker;
        QObject::connect(&worker,
                         &sak::OstConversionWorker::conversionFinished,
                         &worker,
                         [&run](const sak::OstConversionResult& result) { run.result = result; });
        worker.convert(path, config);
        return run;
    }

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // The real shape: mail plus contacts, calendar, tasks and notes, and bookkeeping folders. A
    // clean mailbox must convert cleanly, with what MBOX cannot hold counted and not failed.
    void convertsTheMailAndCountsWhatItLeavesOut() {
        const QVector<BuilderFolder> folders{
            folder(QStringLiteral("Inbox"),
                   QStringLiteral("IPF.Note"),
                   {mail(QStringLiteral("One")), mail(QStringLiteral("Two"))}),
            folder(QStringLiteral("Contacts"),
                   QStringLiteral("IPF.Contact"),
                   {nonMail(QStringLiteral("IPM.Contact"), QStringLiteral("Theo")),
                    nonMail(QStringLiteral("IPM.DistList"), QStringLiteral("Staff"))}),
            folder(QStringLiteral("Calendar"),
                   QStringLiteral("IPF.Appointment"),
                   {nonMail(QStringLiteral("IPM.Appointment"), QStringLiteral("Inspection"))}),
            folder(QStringLiteral("Tasks"),
                   QStringLiteral("IPF.Task"),
                   {nonMail(QStringLiteral("IPM.Task"), QStringLiteral("Order flour"))}),
            folder(QStringLiteral("Notes"),
                   QStringLiteral("IPF.StickyNote"),
                   {nonMail(QStringLiteral("IPM.StickyNote"), QStringLiteral("Call back"))}),
            folder(QStringLiteral("Conversation History"),
                   QStringLiteral("IPF.Note"),
                   {mail(QStringLiteral("Chat"))}),
        };
        const Run run = convert(PstStoreBuilder{}.build(folders));

        QCOMPARE(run.result.items_converted, 3);
        QCOMPARE(run.result.items_failed, 0);
        QVERIFY(run.result.errors.isEmpty());
        QCOMPARE(run.result.skipped_contacts, 2);
        QCOMPARE(run.result.skipped_calendar, 1);
        QCOMPARE(run.result.skipped_tasks, 1);
        QCOMPARE(run.result.skipped_notes, 1);
        QCOMPARE(run.result.skipped_other, 0);
        QCOMPARE(sak::OstConverterController::classifyOutcome(run.result),
                 sak::OstConversionJob::Status::Complete);
        QCOMPARE(mailboxMessageCount(run.output), 3);
    }

    // Real mail turned up in system folders and was left out, while a contact filed in Deleted
    // Items was written out as if it were mail. What an item is decides, not where it is filed.
    void convertsEveryRealEmailWhereverItLivesAndNothingElse() {
        BuilderMessage invite = mail(QStringLiteral("Invite"));
        invite.message_class = QStringLiteral("IPM.Schedule.Meeting.Request");
        const QVector<BuilderFolder> folders{
            folder(
                QStringLiteral("Inbox"), QStringLiteral("IPF.Note"), {mail(QStringLiteral("In"))}),
            folder(QStringLiteral("Deleted Items"),
                   QString(),
                   {mail(QStringLiteral("Binned")),
                    nonMail(QStringLiteral("IPM.Contact"), QStringLiteral("Binned contact")),
                    nonMail(QStringLiteral("IPM.Appointment"), QStringLiteral("Binned event")),
                    nonMail(QStringLiteral("IPM.Task"), QStringLiteral("Binned task"))}),
            folder(QStringLiteral("Conversation History"),
                   QStringLiteral("IPF.Note"),
                   {mail(QStringLiteral("System mail")),
                    nonMail(QStringLiteral("IPM.Configuration.X"), QStringLiteral("Setting"))}),
            folder(QStringLiteral("Files"), QStringLiteral("IPF.Note"), {}),
            folder(QStringLiteral("Calendar"), QStringLiteral("IPF.Appointment"), {invite}),
        };
        PstStoreBuilder builder;
        builder.outside_folders = {folder(
            QStringLiteral("Stray"), QStringLiteral("IPF.Note"), {mail(QStringLiteral("Far"))})};
        const Run run = convert(builder.build(folders));

        QVERIFY2(run.result.errors.isEmpty(), qPrintable(run.result.errors.join(QLatin1Char('|'))));
        QCOMPARE(run.result.items_failed, 0);
        // Inbox, Deleted Items, the system folder, the invitation, and the one outside the tree.
        QCOMPARE(run.result.items_converted, 5);
        QCOMPARE(run.result.skipped_contacts, 1);
        QCOMPARE(run.result.skipped_calendar, 1);
        QCOMPARE(run.result.skipped_tasks, 1);
        QCOMPARE(run.result.skipped_other, 1);
        // Every row is accounted for, one way or the other.
        const int rows = 1 + 4 + 2 + 0 + 1 + 1;
        QCOMPARE(run.result.items_converted + run.result.items_failed + run.result.itemsSkipped(),
                 rows);
        QCOMPARE(mailboxMessageCount(run.output), 5);
        const QByteArray written = allMailboxBytes(run.output);
        QVERIFY(written.contains("Subject: System mail"));
        QVERIFY(written.contains("Subject: Far"));
        QVERIFY(!written.contains("Binned contact"));
        QVERIFY(!written.contains("Setting"));
        // A folder that held nothing to convert is not counted as one converted.
        QCOMPARE(run.result.folders_processed, 5);
    }

    // A search folder lists mail filed elsewhere. Converting it would write each message twice.
    void aSearchFolderIsNotConverted() {
        BuilderFolder search = folder(QStringLiteral("Finder"),
                                      QStringLiteral("IPF.Note"),
                                      {mail(QStringLiteral("Copy"))});
        search.search_folder = true;
        BuilderFolder inbox = folder(QStringLiteral("Inbox"),
                                     QStringLiteral("IPF.Note"),
                                     {mail(QStringLiteral("In"))});
        inbox.children = {search};
        const Run run = convert(PstStoreBuilder{}.build({inbox}));
        QCOMPARE(run.result.items_converted, 1);
        QCOMPARE(run.result.itemsSkipped(), 0);
        QCOMPARE(mailboxMessageCount(run.output), 1);
    }

    // An NST reads as an OST: same header, same layout. The real one here held no mail, so this is
    // what shows mail in one comes through.
    void anNstWithMailConverts() {
        PstStoreBuilder builder;
        builder.content_type = sak::email::kOstContentType;
        const QByteArray store = builder.build(
            {folder(QStringLiteral("Inbox"),
                    QStringLiteral("IPF.Note"),
                    {mail(QStringLiteral("Group one")), mail(QStringLiteral("Group two"))})});
        const QString path = m_dir.filePath(QStringLiteral("group-mail.nst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(store);
        file.close();

        sak::OstConversionConfig config;
        const QString out = m_dir.filePath(QStringLiteral("nst_out"));
        QVERIFY(QDir().mkpath(out));
        config.output_directory = out;
        config.include_source_checksums = false;
        sak::OstConversionWorker worker;
        sak::OstConversionResult result;
        QObject::connect(&worker,
                         &sak::OstConversionWorker::conversionFinished,
                         &worker,
                         [&result](const sak::OstConversionResult& done) { result = done; });
        worker.convert(path, config);
        QCOMPARE(result.items_converted, 2);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(mailboxMessageCount(out), 2);
        QVERIFY(sak::OstConverterController::refusalForFile(path).isEmpty());
    }

    void aStoreWithNoMessagesCompletes() {
        const Run run = convert(PstStoreBuilder{}.build(
            {folder(QStringLiteral("Inbox"), QStringLiteral("IPF.Note"), {})}));
        QCOMPARE(run.result.items_converted, 0);
        QCOMPARE(run.result.itemsSkipped(), 0);
        QVERIFY(run.result.errors.isEmpty());
        QCOMPARE(sak::OstConverterController::classifyOutcome(run.result),
                 sak::OstConversionJob::Status::Complete);
    }

    // Named after the folder, not the store's path to it: "Top of Personal Folders_Inbox" helps no
    // one importing the result.
    void namesAMailboxAfterItsFolder() {
        const Run run = convert(PstStoreBuilder{}.build({folder(
            QStringLiteral("Inbox"), QStringLiteral("IPF.Note"), {mail(QStringLiteral("One"))})}));
        QDirIterator it(
            run.output, {QStringLiteral("*.mbox")}, QDir::Files, QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        QCOMPARE(QFileInfo(it.next()).fileName(), QStringLiteral("Inbox.mbox"));
    }

    // Drafts and saved messages have no sender, and may have no send time. They are mail.
    void convertsUnsentMailWithNoSenderOrSendTime() {
        BuilderMessage draft;
        draft.subject = QStringLiteral("Draft reply");
        draft.body_plain = QStringLiteral("Not sent yet");
        draft.created = QDateTime(QDate(2026, 2, 1), QTime(10, 0), QTimeZone::UTC);
        const Run run = convert(PstStoreBuilder{}.build(
            {folder(QStringLiteral("Drafts"), QStringLiteral("IPF.Note"), {draft})}));
        QCOMPARE(run.result.items_failed, 0);
        QCOMPARE(run.result.items_converted, 1);
        QCOMPARE(mailboxMessageCount(run.output), 1);
        const QByteArray bytes = allMailboxBytes(run.output);
        QVERIFY2(bytes.startsWith("From - Sun Feb 01 10:00:00 2026"),
                 "the separator carries the unknown-sender marker and the store's own time");
        QVERIFY2(!bytes.contains("\r\nFrom: "), "no From: header is invented");
        QVERIFY2(!bytes.contains("\r\nDate: "), "the store's time is not presented as a send date");
    }

    // PR_CONTENT_COUNT can undercount. Walking up to it silently dropped the rest of the folder.
    void convertsEveryRowWhenTheDeclaredCountIsLow() {
        BuilderFolder inbox = folder(QStringLiteral("Inbox"), QStringLiteral("IPF.Note"), {});
        for (int i = 0; i < 5; ++i) {
            inbox.messages.append(mail(QStringLiteral("M%1").arg(i)));
        }
        inbox.declared_count = 1;
        const Run run = convert(PstStoreBuilder{}.build({inbox}));
        QCOMPARE(run.result.items_converted, 5);
        QCOMPARE(mailboxMessageCount(run.output), 5);
    }

    void aDeclaredCountAboveTheRowsIsNotAFailure() {
        BuilderFolder inbox = folder(QStringLiteral("Inbox"),
                                     QStringLiteral("IPF.Note"),
                                     {mail(QStringLiteral("Only"))});
        inbox.declared_count = 400;
        const Run run = convert(PstStoreBuilder{}.build({inbox}));
        QCOMPARE(run.result.items_converted, 1);
        QCOMPARE(run.result.items_failed, 0);
        QVERIFY(run.result.errors.isEmpty());
    }

    void reportsEveryUnreadableBodyAndKeepsGoing() {
        BuilderMessage damaged = mail(QStringLiteral("Damaged"));
        damaged.body_plain.clear();
        damaged.body_rtf_compressed = "not compressed rtf at all";
        const Run run =
            convert(PstStoreBuilder{}.build({folder(QStringLiteral("Inbox"),
                                                    QStringLiteral("IPF.Note"),
                                                    {damaged, mail(QStringLiteral("Fine"))})}));
        QCOMPARE(run.result.items_converted, 1);
        QCOMPARE(run.result.items_failed, 1);
        QCOMPARE(run.result.errors.size(), 1);
        QVERIFY2(run.result.errors.first().contains(QStringLiteral("Damaged")),
                 "the failed item is named in the report");
        QCOMPARE(sak::OstConverterController::classifyOutcome(run.result),
                 sak::OstConversionJob::Status::Failed);
    }

    void carriesAnAttachedEmailIntoTheMailbox() {
        BuilderMessage outer = mail(QStringLiteral("Fwd: quote"));
        outer.attached_messages.append(mail(QStringLiteral("Original quote")));
        const Run run = convert(PstStoreBuilder{}.build(
            {folder(QStringLiteral("Inbox"), QStringLiteral("IPF.Note"), {outer})}));
        QCOMPARE(run.result.items_failed, 0);
        const QByteArray bytes = allMailboxBytes(run.output);
        QVERIFY(bytes.contains("Content-Type: message/rfc822"));
        QVERIFY(bytes.contains("Original quote.eml"));
    }

    // Deep recovery is documented as the same scan and more. On its own it once did nothing.
    void deepRecoveryRunsWithoutRecoverDeletedTicked() {
        PstStoreBuilder builder;
        builder.orphan_messages.append(mail(QStringLiteral("Hard deleted")));
        sak::OstConversionConfig config;
        config.recover_deleted_items = false;
        config.recovery_mode = sak::RecoveryMode::DeepRecovery;
        const Run run = convert(builder.build({folder(QStringLiteral("Inbox"),
                                                      QStringLiteral("IPF.Note"),
                                                      {mail(QStringLiteral("Kept"))})}),
                                config);
        QCOMPARE(run.result.items_converted, 1);
        QCOMPARE(run.result.items_recovered, 1);
        QCOMPARE(mailboxMessageCount(run.output), 2);
    }

    // An NST reads as an OST to the parser. It must be refused at the door, not "converted" to
    // nothing and reported Complete.
    void theQueueRefusesAnythingThatIsNotAnOutlookStore() {
        QVERIFY(
            sak::OstConverterController::refusalForFile(QStringLiteral("C:/x/mail.nst")).isEmpty());
        QVERIFY(
            !sak::OstConverterController::refusalForFile(QStringLiteral("C:/x/cal.ics")).isEmpty());
        QVERIFY(
            sak::OstConverterController::refusalForFile(QStringLiteral("C:/x/mail.PST")).isEmpty());
        QVERIFY(
            sak::OstConverterController::refusalForFile(QStringLiteral("C:/x/mail.ost")).isEmpty());

        const QString path = m_dir.filePath(QStringLiteral("calendar.ics"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("x");
        file.close();
        sak::OstConverterController controller;
        controller.addFile(path);
        QVERIFY(controller.queue().isEmpty());

        const QString nst = m_dir.filePath(QStringLiteral("cache.nst"));
        QFile nst_file(nst);
        QVERIFY(nst_file.open(QIODevice::WriteOnly));
        nst_file.write("x");
        nst_file.close();
        controller.addFile(nst);
        QCOMPARE(controller.queue().size(), 1);
    }
};

QTEST_MAIN(TestOstConversionScope)
#include "test_ost_conversion_scope.moc"
