// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_tools_mailbox_views.cpp
/// @brief The Email Inspector's views, driven by a real controller over whole generated stores.

#include "sak/email_attachments_browser_dialog.h"
#include "sak/email_calendar_dialog.h"
#include "sak/email_contacts_dialog.h"
#include "sak/email_export_worker.h"
#include "sak/email_file_scanner_dialog.h"
#include "sak/email_folder_selection.h"
#include "sak/email_inspector_controller.h"
#include "sak/email_inspector_panel.h"
#include "sak/pst_named_properties.h"
#include "sak/pst_parser.h"

#include "../support/pst_store_builder.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

constexpr uint8_t kSetAppointment = 0x02;
constexpr uint8_t kSetTask = 0x03;
constexpr uint8_t kSetAddress = 0x04;
constexpr int kWaitMs = 5000;

QByteArray sixMessageMailbox() {
    QByteArray content;
    for (int i = 0; i < 6; ++i) {
        content += "From sender@example.com Mon Jan  1 00:00:00 2024\n";
        content += "From: Sender <sender@example.com>\n";
        content += "Subject: Message " + QByteArray::number(i) + "\n";
        content += "\n";
        // A long body: the old count was the file's size divided by a guessed 5 KB.
        content += QByteArray(10'000, 'x') + "\n\n";
    }
    return content;
}

QByteArray mailboxWithAttachment() {
    QByteArray content;
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\n";
    content += "From: Sender <sender@example.com>\n";
    content += "Subject: First, with a file\n";
    content += "MIME-Version: 1.0\n";
    content += "Content-Type: multipart/mixed; boundary=\"B\"\n";
    content += "\n";
    content += "--B\n";
    content += "Content-Type: text/plain\n\nBody\n";
    content += "--B\n";
    content += "Content-Type: application/pdf\n";
    content += "Content-Disposition: attachment; filename=\"menu.pdf\"\n";
    content += "Content-Transfer-Encoding: base64\n\n";
    content += "SGVsbG8=\n";
    content += "--B--\n\n";
    content += "From sender@example.com Mon Jan  1 00:00:00 2024\n";
    content += "From: Sender <sender@example.com>\n";
    content += "Subject: Second, plain\n\nNo file.\n\n";
    return content;
}

BuilderFolder contactsFolder() {
    BuilderMessage theo;
    theo.message_class = QStringLiteral("IPM.Contact");
    theo.subject = QStringLiteral("Theo Brandt");
    theo.extra_props = {
        {sak::email::kPropIdCompanyName,
         sak::email::kPropTypeUnicode,
         builderUnicode(QStringLiteral("Oven Works"))},
        {sak::email::kPropIdBusinessPhone,
         sak::email::kPropTypeUnicode,
         builderUnicode(QStringLiteral("555-0101"))},
    };
    theo.named_props = {{kSetAddress,
                         sak::pst::kLidEmail1EmailAddress,
                         sak::email::kPropTypeUnicode,
                         builderUnicode(QStringLiteral("theo@ovenworks.example"))}};

    BuilderMessage ines;
    ines.message_class = QStringLiteral("IPM.Contact");
    ines.subject = QStringLiteral("Ines Caldera");
    ines.extra_props = {{sak::email::kPropIdMobilePhone,
                         sak::email::kPropTypeUnicode,
                         builderUnicode(QStringLiteral("555-0202"))}};

    BuilderFolder folder;
    folder.name = QStringLiteral("Contacts");
    folder.container_class = QStringLiteral("IPF.Contact");
    folder.messages = {theo, ines};
    return folder;
}

BuilderFolder calendarFolder(const QDateTime& start) {
    BuilderMessage event;
    event.message_class = QStringLiteral("IPM.Appointment");
    event.subject = QStringLiteral("Health inspection");
    // Delivered weeks before it happens: the day it arrived is not its day.
    event.date = start.addDays(-21);
    event.named_props = {
        {kSetAppointment,
         sak::pst::kLidAppointmentStartWhole,
         sak::email::kPropTypeSysTime,
         builderFiletime(start)},
        {kSetAppointment,
         sak::pst::kLidAppointmentEndWhole,
         sak::email::kPropTypeSysTime,
         builderFiletime(start.addSecs(3600))},
        {kSetAppointment,
         sak::pst::kLidAppointmentLocation,
         sak::email::kPropTypeUnicode,
         builderUnicode(QStringLiteral("Front kitchen"))},
    };
    BuilderFolder folder;
    folder.name = QStringLiteral("Calendar");
    folder.container_class = QStringLiteral("IPF.Appointment");
    folder.messages = {event};
    return folder;
}

BuilderFolder tasksFolder() {
    BuilderMessage task;
    task.message_class = QStringLiteral("IPM.Task");
    task.subject = QStringLiteral("Order flour");
    task.named_props = {
        {kSetTask, sak::pst::kLidTaskStatus, sak::email::kPropTypeInt32, builderInt32(1)},
        {kSetTask,
         sak::pst::kLidTaskPercentComplete,
         sak::email::kPropTypeFloat64,
         builderDouble(0.5)},
    };
    BuilderFolder folder;
    folder.name = QStringLiteral("Tasks");
    folder.container_class = QStringLiteral("IPF.Task");
    folder.messages = {task};
    return folder;
}

QStringList treeTexts(const QTreeWidget& tree) {
    QStringList texts;
    for (int i = 0; i < tree.topLevelItemCount(); ++i) {
        texts.append(tree.topLevelItem(i)->text(0));
    }
    return texts;
}

}  // namespace

class EmailToolsViewsTests : public QObject {
    Q_OBJECT
private:
    QTemporaryDir m_dir;
    int m_counter = 0;

    QString writeFile(const QString& suffix, const QByteArray& bytes) {
        const QString path = m_dir.filePath(QStringLiteral("file%1").arg(++m_counter) + suffix);
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) {
            return {};
        }
        return path;
    }

    static void openAndWait(sak::EmailInspectorPanel& panel, const QString& path) {
        panel.m_controller->openFile(path);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_controller->currentState(),
                                  EmailInspectorController::State::Idle,
                                  kWaitMs);
    }

    /// A message with a picture its body shows and a file it carries.
    [[nodiscard]] static QByteArray storeWithInlinePicture() {
        const QByteArray png("\x89PNG\r\n\x1a\nfixture bytes", 21);
        BuilderMessage msg;
        msg.subject = QStringLiteral("With picture");
        msg.sender_email = QStringLiteral("a@example.test");
        msg.date = QDateTime(QDate(2026, 3, 1), QTime(9, 0), QTimeZone::UTC);
        msg.body_html = QStringLiteral("<p><img src=\"cid:logo@x\"></p>");
        msg.attachments = {{QStringLiteral("logo.png"),
                            QStringLiteral("image/png"),
                            png,
                            QStringLiteral("logo@x")},
                           {QStringLiteral("menu.pdf"),
                            QStringLiteral("application/pdf"),
                            QByteArray("%PDF-"),
                            {}}};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {msg};
        return PstStoreBuilder{}.build({inbox});
    }

    static void exportFirstFolder(PstParser& parser, sak::ExportFormat format, const QString& out) {
        const sak::PstFolder* subtree = sak::findIpmSubtree(parser.folderTree());
        QVERIFY(subtree != nullptr);
        QVERIFY(QDir().mkpath(out));
        sak::EmailExportConfig config;
        config.format = format;
        config.output_path = out;
        config.has_folder = true;
        config.folder_id = subtree->children.first().node_id;
        EmailExportWorker worker;
        QSignalSpy done(&worker, &EmailExportWorker::exportComplete);
        worker.exportItems(&parser, config);
        QCOMPARE(done.count(), 1);
        const auto result = done.first().first().value<sak::EmailExportResult>();
        QVERIFY2(result.errors.isEmpty(), qPrintable(result.errors.join(QLatin1Char('|'))));
    }

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // The screen once said "Inbox (65 messages)" and "0 B" for a mailbox of six.
    void showsAnMboxsRealMessageCountAndSize() {
        const QByteArray bytes = sixMessageMailbox();
        const QString path = writeFile(QStringLiteral(".mbox"), bytes);
        sak::EmailInspectorPanel panel;
        openAndWait(panel, path);
        QCOMPARE(panel.m_folder_tree->topLevelItem(0)->text(0),
                 QStringLiteral("Inbox (6 messages)"));
        QCOMPARE(panel.m_controller->fileInfo().total_items, 6);
        QCOMPARE(panel.m_controller->fileInfo().file_size_bytes, static_cast<qint64>(bytes.size()));
    }

    void refusesAFileThatIsNotAMailboxAfterOpening() {
        const QString path = writeFile(QStringLiteral(".mbox"), "not a mailbox\n");
        sak::EmailInspectorPanel panel;
        QSignalSpy errors(panel.m_controller.get(), &EmailInspectorController::errorOccurred);
        openAndWait(panel, path);
        QVERIFY(!errors.isEmpty());
        QVERIFY(!panel.m_controller->isFileOpen());
    }

    // Thunderbird's mailboxes have no extension.
    void opensAMailboxWithNoExtension() {
        const QString path = m_dir.filePath(QStringLiteral("Inbox"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(sixMessageMailbox());
        file.close();
        sak::EmailInspectorPanel panel;
        openAndWait(panel, path);
        QVERIFY(panel.m_controller->isFileOpen());
        QCOMPARE(panel.m_controller->fileInfo().total_items, 6);
    }

    void refusesAnNstWithItsOwnMessage() {
        const QString path = writeFile(QStringLiteral(".nst"), "anything");
        sak::EmailInspectorPanel panel;
        QSignalSpy errors(panel.m_controller.get(), &EmailInspectorController::errorOccurred);
        panel.m_controller->openFile(path);
        QCOMPARE(errors.count(), 1);
        QVERIFY(errors.first().first().toString().contains(QStringLiteral("NST")));
        QVERIFY(!panel.m_controller->isFileOpen());
    }

    // An MBOX holds only mail. The buttons lit up anyway, because fileOpened followed mboxOpened.
    void contactsAndCalendarAreOffForAnMboxAndBackOnForAPst() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".mbox"), sixMessageMailbox()));
        QVERIFY(!panel.m_contacts_button->isEnabled());
        QVERIFY(!panel.m_calendar_button->isEnabled());

        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({contactsFolder()})));
        QVERIFY(panel.m_contacts_button->isEnabled());
        QVERIFY(panel.m_calendar_button->isEnabled());
    }

    // Per page did nothing, because an MBOX's one folder has id 0.
    void perPageReloadsAnMbox() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".mbox"), sixMessageMailbox()));
        panel.onFolderTreeItemClicked(panel.m_folder_tree->topLevelItem(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 6, kWaitMs);

        // The list must be read again at the new size, so it is emptied first to see that happen.
        panel.m_item_list->setRowCount(0);
        panel.m_page_size_combo->setCurrentText(QStringLiteral("250"));
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 6, kWaitMs);
    }

    // Tasks and notes were promised by the header, and hidden with contacts and calendars.
    void tasksAndNotesAreBrowsableAndContactsStayInTheirWindow() {
        BuilderMessage note;
        note.message_class = QStringLiteral("IPM.StickyNote");
        note.subject = QStringLiteral("Call back");
        BuilderFolder notes;
        notes.name = QStringLiteral("Notes");
        notes.container_class = QStringLiteral("IPF.StickyNote");
        notes.messages = {note};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");

        sak::EmailInspectorPanel panel;
        openAndWait(
            panel,
            writeFile(QStringLiteral(".pst"),
                      PstStoreBuilder{}.build({inbox, tasksFolder(), notes, contactsFolder()})));
        const QStringList rows = treeTexts(*panel.m_folder_tree);
        QVERIFY2(rows.contains(QStringLiteral("Tasks (1)")),
                 qPrintable(rows.join(QLatin1Char('|'))));
        QVERIFY(rows.contains(QStringLiteral("Notes (1)")));
        QVERIFY(rows.contains(QStringLiteral("Inbox (0)")));
        QVERIFY(!rows.contains(QStringLiteral("Contacts (2)")));
    }

    void aTaskShowsItsRealStatus() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({tasksFolder()})));
        panel.onFolderTreeItemClicked(panel.m_folder_tree->topLevelItem(0), 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 1, kWaitMs);
        panel.onItemListCellClicked(0, sak::EmailInspectorPanel::ColSubject);
        QTRY_VERIFY_WITH_TIMEOUT(panel.m_content_browser->toPlainText().contains(
                                     QStringLiteral("In Progress (50%)")),
                                 kWaitMs);
    }

    void aFoldersRightClickExportMatchesWhatItHolds() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({tasksFolder()})));
        const auto labels = [&panel](const QString& cls) {
            sak::PstFolder folder;
            folder.container_class = cls;
            QMenu menu;
            panel.addFolderExportActions(menu, &folder, std::optional<uint64_t>(1));
            QStringList texts;
            for (const QAction* action : menu.actions()) {
                texts.append(action->text());
            }
            return texts;
        };
        QCOMPARE(labels(QStringLiteral("IPF.Task")),
                 QStringList{QStringLiteral("Export Tasks as CSV...")});
        QCOMPARE(labels(QStringLiteral("IPF.StickyNote")),
                 QStringList{QStringLiteral("Export Notes as Text...")});
        QCOMPARE(labels(QStringLiteral("IPF.Note")),
                 QStringList({QStringLiteral("Export Folder as EML..."),
                              QStringLiteral("Export Folder as CSV...")}));
    }

    // The browser was handed an empty folder tree for an MBOX, so it found nothing, and it listens
    // for PST-shaped signals an MBOX never sends.
    void theAttachmentsBrowserFindsAnMboxsAttachments() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".mbox"), mailboxWithAttachment()));
        QCOMPARE(panel.m_cached_folder_tree.size(), 1);
        QCOMPARE(panel.m_cached_folder_tree.first().content_count, 2);

        sak::EmailAttachmentsBrowserDialog dialog(panel.m_controller.get(),
                                                  panel.m_cached_folder_tree);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.m_scan_complete, kWaitMs);
        QCOMPARE(dialog.m_all_attachments.size(), 1);
        QCOMPARE(dialog.m_all_attachments.first().filename, QStringLiteral("menu.pdf"));
        QCOMPARE(dialog.m_all_attachments.first().source_subject,
                 QStringLiteral("First, with a file"));
        QCOMPARE(dialog.m_all_attachments.first().message_node_id, 0ULL);
    }

    void theAttachmentsBrowserListsAnAttachedEmail() {
        BuilderMessage inner;
        inner.subject = QStringLiteral("Original");
        BuilderMessage outer;
        outer.subject = QStringLiteral("Fwd");
        outer.attached_messages = {inner};
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {outer};
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({inbox})));
        sak::EmailAttachmentsBrowserDialog dialog(panel.m_controller.get(),
                                                  panel.m_cached_folder_tree);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.m_scan_complete, kWaitMs);
        QCOMPARE(dialog.m_all_attachments.size(), 1);
        QCOMPARE(dialog.m_all_attachments.first().filename, QStringLiteral("Original.eml"));
    }

    // Inline pictures were skipped before any writer saw them, so an HTML or PDF export of a
    // message showed blanks where its pictures were.
    void htmlExportsCarryInlinePictures() {
        PstParser parser;
        parser.open(writeFile(QStringLiteral(".pst"), storeWithInlinePicture()));
        QVERIFY(parser.isOpen());
        const QString out = m_dir.filePath(QStringLiteral("inline_html"));
        exportFirstFolder(parser, sak::ExportFormat::Html, out);
        const QStringList pages = QDir(out).entryList({QStringLiteral("*.html")}, QDir::Files);
        QCOMPARE(pages.size(), 1);
        QFile page(out + QLatin1Char('/') + pages.first());
        QVERIFY(page.open(QIODevice::ReadOnly));
        const QString html = QString::fromUtf8(page.readAll());
        QVERIFY(html.contains(QStringLiteral("data:image/png;base64,")));
        QVERIFY(html.contains(QStringLiteral("menu.pdf")));
        QVERIFY(!html.contains(QStringLiteral("logo.png")));
    }

    // Saved attachments and text have nowhere to put a picture: a folder of every logo and
    // signature is not what those are for.
    void savedFilesLeaveInlinePicturesOut() {
        PstParser parser;
        parser.open(writeFile(QStringLiteral(".pst"), storeWithInlinePicture()));
        QVERIFY(parser.isOpen());
        const auto savedNames = [](const QString& dir) {
            QDirIterator files(dir, QDir::Files, QDirIterator::Subdirectories);
            QStringList names;
            while (files.hasNext()) {
                names.append(QFileInfo(files.next()).fileName());
            }
            return names;
        };
        const QString saved_out = m_dir.filePath(QStringLiteral("inline_attachments"));
        exportFirstFolder(parser, sak::ExportFormat::Attachments, saved_out);
        QCOMPARE(savedNames(saved_out), QStringList{QStringLiteral("menu.pdf")});

        const QString text_out = m_dir.filePath(QStringLiteral("inline_text"));
        exportFirstFolder(parser, sak::ExportFormat::Text, text_out);
        QCOMPARE(savedNames(text_out + QStringLiteral("/attachments")),
                 QStringList{QStringLiteral("menu.pdf")});
    }

    // What the folder menu's task and note exports write, with the fields only the store's
    // Name-to-ID map can supply.
    void tasksExportTheirRealStatusAndNotesTheirText() {
        BuilderMessage note;
        note.message_class = QStringLiteral("IPM.StickyNote");
        note.subject = QStringLiteral("Call back");
        note.body_plain = QStringLiteral("Call the sign painter");
        BuilderFolder notes;
        notes.name = QStringLiteral("Notes");
        notes.container_class = QStringLiteral("IPF.StickyNote");
        notes.messages = {note};
        const QString path = writeFile(QStringLiteral(".pst"),
                                       PstStoreBuilder{}.build({tasksFolder(), notes}));
        PstParser parser;
        parser.open(path);
        QVERIFY(parser.isOpen());
        const sak::PstFolder* subtree = sak::findIpmSubtree(parser.folderTree());
        QVERIFY(subtree != nullptr);
        QCOMPARE(subtree->children.size(), 2);

        const auto exportFolder =
            [&](sak::ExportFormat format, uint64_t folder_id, const QString& out) {
                QVERIFY(QDir().mkpath(out));
                sak::EmailExportConfig config;
                config.format = format;
                config.output_path = out;
                config.has_folder = true;
                config.folder_id = folder_id;
                EmailExportWorker worker;
                QSignalSpy done(&worker, &EmailExportWorker::exportComplete);
                worker.exportItems(&parser, config);
                QCOMPARE(done.count(), 1);
                const auto result = done.first().first().value<sak::EmailExportResult>();
                QCOMPARE(result.items_exported, 1);
                QVERIFY(result.errors.isEmpty());
            };

        const QString tasks_out = m_dir.filePath(QStringLiteral("tasks_csv"));
        exportFolder(sak::ExportFormat::CsvTasks, subtree->children.at(0).node_id, tasks_out);
        const QStringList csvs = QDir(tasks_out).entryList({QStringLiteral("*.csv")}, QDir::Files);
        QCOMPARE(csvs.size(), 1);
        QFile csv(tasks_out + QLatin1Char('/') + csvs.first());
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const QByteArray csv_bytes = csv.readAll();
        QVERIFY(csv_bytes.contains("Order flour"));
        QVERIFY(csv_bytes.contains("In Progress"));

        const QString notes_out = m_dir.filePath(QStringLiteral("notes_text"));
        exportFolder(sak::ExportFormat::PlainTextNotes, subtree->children.at(1).node_id, notes_out);
        const QStringList texts = QDir(notes_out).entryList({QStringLiteral("*.txt")}, QDir::Files);
        QCOMPARE(texts.size(), 1);
        QFile text(notes_out + QLatin1Char('/') + texts.first());
        QVERIFY(text.open(QIODevice::ReadOnly));
        QVERIFY(text.readAll().contains("Call the sign painter"));
    }

    void thunderbirdMailboxesAreFoundByWhatTheyAre() {
        QTemporaryDir profiles;
        QVERIFY(profiles.isValid());
        const QString account = profiles.filePath(QStringLiteral("abc.default/Mail/Local Folders"));
        QVERIFY(QDir().mkpath(account + QStringLiteral("/Archives.sbd")));
        const auto write = [](const QString& path, const QByteArray& bytes) {
            QFile file(path);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(bytes);
        };
        const QByteArray mailbox = sixMessageMailbox();
        write(account + QStringLiteral("/Inbox"), mailbox);
        write(account + QStringLiteral("/Inbox.msf"), "index");
        write(account + QStringLiteral("/Trash"), QByteArray());
        write(account + QStringLiteral("/msgFilterRules.dat"), "rules");
        write(account + QStringLiteral("/notes"), "plain text, not a mailbox\n");
        write(account + QStringLiteral("/Archives.sbd/2024"), mailbox);

        QStringList found = sak::EmailFileScannerDialog::thunderbirdMailboxes(profiles.path());
        for (QString& path : found) {
            path = QDir(account).relativeFilePath(path);
        }
        found.sort();
        QCOMPARE(found,
                 QStringList({QStringLiteral("Archives.sbd/2024"), QStringLiteral("Inbox")}));
    }
    // Phone was always blank, and Email and Company came from fields a contact does not fill.
    void theTableShowsEachContactsRealFields() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("contacts.pst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(PstStoreBuilder{}.build({contactsFolder()}));
        file.close();

        EmailInspectorController controller;
        sak::PstFolderTree tree;
        QObject::connect(&controller,
                         &EmailInspectorController::folderTreeLoaded,
                         &controller,
                         [&tree](const sak::PstFolderTree& loaded) { tree = loaded; });
        controller.openFile(path);
        QVERIFY(controller.isFileOpen());

        const std::function<uint64_t(const sak::PstFolderTree&)> find =
            [&find](const sak::PstFolderTree& folders) -> uint64_t {
            for (const auto& folder : folders) {
                if (folder.container_class == QStringLiteral("IPF.Contact")) {
                    return folder.node_id;
                }
                if (const uint64_t id = find(folder.children); id != 0) {
                    return id;
                }
            }
            return 0;
        };
        sak::EmailContactsDialog dialog(&controller, {find(tree)});
        QCOMPARE(dialog.m_contact_table->rowCount(), 2);

        QHash<QString, int> row_of;
        for (int row = 0; row < 2; ++row) {
            row_of.insert(dialog.m_contact_table->item(row, 0)->text(), row);
        }
        const int theo = row_of.value(QStringLiteral("Theo Brandt"), -1);
        const int ines = row_of.value(QStringLiteral("Ines Caldera"), -1);
        QVERIFY(theo >= 0 && ines >= 0);
        QCOMPARE(dialog.m_contact_table->item(theo, 1)->text(),
                 QStringLiteral("theo@ovenworks.example"));
        QCOMPARE(dialog.m_contact_table->item(theo, 2)->text(), QStringLiteral("Oven Works"));
        QCOMPARE(dialog.m_contact_table->item(theo, 3)->text(), QStringLiteral("555-0101"));
        QCOMPARE(dialog.m_contact_table->item(ines, 3)->text(), QStringLiteral("555-0202"));

        // Search reaches the real fields too.
        dialog.filterContacts(QStringLiteral("oven works"));
        QCOMPARE(dialog.m_contact_table->rowCount(), 1);
    }
    // An event went on the day its invitation arrived, with no end or location. An event with no
    // delivery date, most of a real calendar, was not shown at all.
    void anEventGoesOnTheDayItHappensWithItsDetails() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("calendar.pst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QDateTime start(QDate(2026, 4, 22), QTime(9, 0), QTimeZone::UTC);
        file.write(PstStoreBuilder{}.build({calendarFolder(start)}));
        file.close();

        EmailInspectorController controller;
        sak::PstFolderTree tree;
        QObject::connect(&controller,
                         &EmailInspectorController::folderTreeLoaded,
                         &controller,
                         [&tree](const sak::PstFolderTree& loaded) { tree = loaded; });
        controller.openFile(path);
        QVERIFY(controller.isFileOpen());
        const std::function<uint64_t(const sak::PstFolderTree&)> find =
            [&find](const sak::PstFolderTree& folders) -> uint64_t {
            for (const auto& folder : folders) {
                if (folder.container_class == QStringLiteral("IPF.Appointment")) {
                    return folder.node_id;
                }
                if (const uint64_t id = find(folder.children); id != 0) {
                    return id;
                }
            }
            return 0;
        };

        sak::EmailCalendarDialog dialog(&controller, {find(tree)});
        QTRY_COMPARE_WITH_TIMEOUT(dialog.m_all_events.size(), 1, kWaitMs);
        const auto& event = dialog.m_all_events.first();
        QCOMPARE(event.start_time, start);
        QCOMPARE(event.end_time, start.addSecs(3600));
        QCOMPARE(event.location, QStringLiteral("Front kitchen"));
        QVERIFY(dialog.m_date_index.contains(start.toLocalTime().date()));
        QVERIFY(!dialog.m_date_index.contains(start.addDays(-21).toLocalTime().date()));
    }
};

QTEST_MAIN(EmailToolsViewsTests)
#include "test_email_tools_mailbox_views.moc"
