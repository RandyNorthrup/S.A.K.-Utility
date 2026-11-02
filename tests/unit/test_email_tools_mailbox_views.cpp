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
#include "../support/recurrence_blob.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QSemaphore>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QThreadPool>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QtTest/QtTest>

using namespace sak::pst_fixture;

namespace {

constexpr uint8_t kSetAppointment = 0x02;
constexpr uint8_t kSetTask = 0x03;
constexpr uint8_t kSetAddress = 0x04;
constexpr uint8_t kSetLog = 0x0A;
constexpr int kWaitMs = 5000;
constexpr int kDriverPollMs = 20;
constexpr int kDayListSubjectColumn = 1;

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

    /// Answers whatever nested event loop a panel call opens: a menu or a modal dialog. They run
    /// inside the call that opened them, so only a poll from outside can reach them.
    class ModalDriver {
    public:
        std::function<void(QMenu*)> onMenu;
        std::function<void(QWidget*)> onModal;
        bool menuSeen = false;

        ModalDriver() {
            m_timer.setInterval(kDriverPollMs);
            // Queued: answering a menu can open a dialog, whose loop runs inside the answer, and a
            // timer does not fire again while its own handler is still on the stack.
            QObject::connect(&m_timer, &QTimer::timeout, &m_timer, [this] {
                QMetaObject::invokeMethod(&m_timer, [this] { answer(); }, Qt::QueuedConnection);
            });
            m_timer.start();
        }

    private:
        void answer() {
            if (auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
                menu != nullptr && onMenu) {
                menuSeen = true;
                onMenu(menu);
                return;
            }
            if (QWidget* modal = QApplication::activeModalWidget(); modal != nullptr && onModal) {
                onModal(modal);
            }
        }

        QTimer m_timer;
    };

    static void collectTreeTexts(const QTreeWidgetItem* item, QStringList& out) {
        out.append(item->text(0));
        for (int i = 0; i < item->childCount(); ++i) {
            collectTreeTexts(item->child(i), out);
        }
    }

    static QStringList panelTreeTexts(const sak::EmailInspectorPanel& panel) {
        QStringList texts;
        for (int i = 0; i < panel.m_folder_tree->topLevelItemCount(); ++i) {
            collectTreeTexts(panel.m_folder_tree->topLevelItem(i), texts);
        }
        return texts;
    }

    static QTreeWidgetItem* treeItem(const sak::EmailInspectorPanel& panel, const QString& text) {
        const auto found = panel.m_folder_tree->findItems(text,
                                                          Qt::MatchExactly | Qt::MatchRecursive);
        return found.isEmpty() ? nullptr : found.first();
    }

    [[nodiscard]] static BuilderMessage storedMail(const QString& subject) {
        BuilderMessage msg;
        msg.subject = subject;
        msg.sender_name = QStringLiteral("Owen Lark");
        msg.sender_email = QStringLiteral("owen@harborlane.example");
        msg.date = QDateTime(QDate(2026, 3, 9), QTime(14, 5), QTimeZone::UTC);
        msg.body_plain = QStringLiteral("Body of ") + subject;
        return msg;
    }

    [[nodiscard]] static BuilderFolder journalFolder() {
        const auto logged = [](uint32_t long_id, uint16_t type, const QByteArray& value) {
            return BuilderNamedProp{
                .set_byte = kSetLog, .long_id = long_id, .type = type, .value = value};
        };
        const QDateTime start(QDate(2026, 3, 9), QTime(14, 0), QTimeZone::UTC);
        BuilderMessage entry;
        entry.message_class = QStringLiteral("IPM.Activity");
        entry.subject = QStringLiteral("Call with the oven company");
        entry.body_plain = QStringLiteral("Thermostat part on order.");
        entry.named_props = {
            logged(sak::pst::kLidLogType,
                   sak::email::kPropTypeUnicode,
                   builderUnicode(QStringLiteral("Phone call"))),
            logged(sak::pst::kLidLogStart, sak::email::kPropTypeSysTime, builderFiletime(start)),
            logged(sak::pst::kLidLogDuration, sak::email::kPropTypeInt32, builderInt32(15)),
        };
        BuilderFolder journal;
        journal.name = QStringLiteral("Journal");
        journal.container_class = QStringLiteral("IPF.Journal");
        journal.messages = {entry};
        return journal;
    }

    /// One series with people on it, one week deleted and one moved, and one that cannot be read.
    [[nodiscard]] static BuilderFolder repeatingCalendar(const QDateTime& start) {
        const auto named = [](uint32_t long_id, uint16_t type, const QByteArray& value) {
            return BuilderNamedProp{
                .set_byte = kSetAppointment, .long_id = long_id, .type = type, .value = value};
        };
        RecurrenceSpec spec;
        spec.start_date = QDate(2026, 11, 2);
        spec.deleted = {QDate(2026, 11, 9)};
        RecurChangedOccurrence moved;
        moved.original_start = QDateTime(QDate(2026, 11, 23), QTime(9, 0), QTimeZone::UTC);
        moved.start = QDateTime(QDate(2026, 11, 24), QTime(14, 0), QTimeZone::UTC);
        moved.end = QDateTime(QDate(2026, 11, 24), QTime(15, 0), QTimeZone::UTC);
        spec.changed = {moved};

        BuilderMessage series;
        series.message_class = QStringLiteral("IPM.Appointment");
        series.subject = QStringLiteral("Staff meeting");
        series.named_props = {
            named(sak::pst::kLidAppointmentStartWhole,
                  sak::email::kPropTypeSysTime,
                  builderFiletime(start)),
            named(sak::pst::kLidAppointmentEndWhole,
                  sak::email::kPropTypeSysTime,
                  builderFiletime(start.addSecs(3600))),
            named(sak::pst::kLidAppointmentRecur,
                  sak::email::kPropTypeBinary,
                  buildRecurrenceBlob(spec)),
            named(sak::pst::kLidTimeZoneStruct, sak::email::kPropTypeBinary, pacificZoneStruct()),
        };
        BuilderRecipient organizer{QStringLiteral("Dana Whitlock"),
                                   QStringLiteral("dana@harborlane.example")};
        organizer.organizer = true;
        BuilderRecipient required{QStringLiteral("Marisol Quint"),
                                  QStringLiteral("marisol@harborlane.example")};
        required.track_status = 3;
        BuilderRecipient optional{QStringLiteral("Owen Lark")};
        optional.recipient_type = 2;
        optional.track_status = 4;
        series.recipients = {organizer, required, optional};
        BuilderAttachment exception;
        exception.name = QStringLiteral("Changed occurrence");
        exception.data = QByteArray("stored exception");
        exception.hidden = true;
        series.attachments = {exception};

        RecurrenceSpec odd = spec;
        odd.frequency = 0x2099;
        BuilderMessage unreadable;
        unreadable.message_class = QStringLiteral("IPM.Appointment");
        unreadable.subject = QStringLiteral("Unreadable series");
        unreadable.named_props = {
            named(sak::pst::kLidAppointmentStartWhole,
                  sak::email::kPropTypeSysTime,
                  builderFiletime(start.addDays(1))),
            named(sak::pst::kLidAppointmentRecur,
                  sak::email::kPropTypeBinary,
                  buildRecurrenceBlob(odd)),
        };
        BuilderFolder calendar;
        calendar.name = QStringLiteral("Calendar");
        calendar.container_class = QStringLiteral("IPF.Appointment");
        calendar.messages = {series, unreadable};
        return calendar;
    }

    [[nodiscard]] static const sak::PstFolder* findByClass(const sak::PstFolderTree& tree,
                                                           const QString& container_class) {
        for (const auto& folder : tree) {
            if (folder.container_class == container_class) {
                return &folder;
            }
            if (const auto* found = findByClass(folder.children, container_class)) {
                return found;
            }
        }
        return nullptr;
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

    // The real NST here held no mail, so this is what shows mail in one is listed and opens.
    void anNstWithMailListsAndOpensIt() {
        PstStoreBuilder builder;
        builder.content_type = sak::email::kOstContentType;
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {storedMail(QStringLiteral("Group note"))};
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".nst"), builder.build({inbox})));
        QVERIFY(panel.m_controller->isFileOpen());
        QCOMPARE(panel.m_controller->fileInfo().total_items, 1);
        QVERIFY(panel.m_controller->fileInfo().is_ost);
        QVERIFY(panel.m_search_edit->isEnabled());
        QVERIFY(panelTreeTexts(panel).contains(QStringLiteral("Inbox (1)")));
    }

    // Empty is an answer, not a broken file.
    void aStoreWithNoMessagesSaysSo() {
        PstStoreBuilder builder;
        builder.content_type = sak::email::kOstContentType;
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        sak::EmailInspectorPanel panel;
        QSignalSpy errors(panel.m_controller.get(), &EmailInspectorController::errorOccurred);
        openAndWait(panel, writeFile(QStringLiteral(".nst"), builder.build({inbox})));
        QVERIFY(panel.m_controller->isFileOpen());
        QCOMPARE(errors.count(), 0);
        QVERIFY2(panel.m_status_label->text().contains(QStringLiteral("holds no messages")),
                 qPrintable(panel.m_status_label->text()));
        QVERIFY(panelTreeTexts(panel).contains(QStringLiteral("Inbox (0)")));

        // And a click on its folder keeps saying so, rather than asking for a folder again.
        panel.onFolderTreeItemClicked(treeItem(panel, QStringLiteral("Inbox (0)")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_controller->currentState(),
                                  EmailInspectorController::State::Idle,
                                  kWaitMs);
        QCOMPARE(panel.m_item_list->rowCount(), 0);
    }

    void aFileThatIsNoStoreStillSaysSo() {
        const QString path = writeFile(QStringLiteral(".nst"), "anything");
        sak::EmailInspectorPanel panel;
        QSignalSpy errors(panel.m_controller.get(), &EmailInspectorController::errorOccurred);
        panel.m_controller->openFile(path);
        QVERIFY(errors.count() >= 1);
        QVERIFY(!panel.m_controller->isFileOpen());
    }

    // Real mail in a system folder is reachable. The system folders with nothing in them stay gone.
    void systemFoldersShowOnlyWhenTheyHoldEmail() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {storedMail(QStringLiteral("Ordinary"))};
        BuilderFolder history;
        history.name = QStringLiteral("Conversation History");
        history.messages = {storedMail(QStringLiteral("Saved chat"))};
        BuilderFolder files;
        files.name = QStringLiteral("Files");
        BuilderFolder sync;
        sync.name = QStringLiteral("Sync Issues");
        BuilderFolder conflicts;
        conflicts.name = QStringLiteral("Conflicts");
        conflicts.messages = {storedMail(QStringLiteral("Conflicted"))};
        BuilderFolder failures;
        failures.name = QStringLiteral("Local Failures");
        sync.children = {conflicts, failures};
        PstStoreBuilder builder;
        BuilderFolder stray;
        stray.name = QStringLiteral("Stray");
        stray.messages = {storedMail(QStringLiteral("Far"))};
        BuilderFolder bare;
        bare.name = QStringLiteral("Bare");
        builder.outside_folders = {stray, bare};

        sak::EmailInspectorPanel panel;
        openAndWait(
            panel, writeFile(QStringLiteral(".pst"), builder.build({inbox, history, files, sync})));
        const QStringList tree = panelTreeTexts(panel);
        QVERIFY2(tree.contains(QStringLiteral("Conversation History (1)")),
                 qPrintable(tree.join(QLatin1Char('|'))));
        QVERIFY(tree.contains(QStringLiteral("Sync Issues (0)")));  // The way down to its child
        QVERIFY(tree.contains(QStringLiteral("Conflicts (1)")));
        QVERIFY(!tree.contains(QStringLiteral("Files (0)")));
        QVERIFY(!tree.contains(QStringLiteral("Local Failures (0)")));
        QVERIFY(tree.contains(QStringLiteral("Other Folders")));
        QVERIFY(tree.contains(QStringLiteral("Stray (1)")));
        QVERIFY(!tree.contains(QStringLiteral("Bare (0)")));

        // And a click on one lists its mail.
        QTreeWidgetItem* item = treeItem(panel, QStringLiteral("Conflicts (1)"));
        QVERIFY(item != nullptr);
        panel.onFolderTreeItemClicked(item, 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 1, kWaitMs);
        QCOMPARE(panel.m_item_list->item(0, sak::EmailInspectorPanel::ColSubject)->text(),
                 QStringLiteral("Conflicted"));

        // The group's own row names no folder, and a click on it does nothing.
        panel.m_current_folder_id = 0;
        panel.onFolderTreeItemClicked(treeItem(panel, QStringLiteral("Other Folders")), 0);
        QCOMPARE(panel.m_current_folder_id, uint64_t{0});
    }

    void aStoreWithoutStrayMailHasNoOtherFoldersRow() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {storedMail(QStringLiteral("Ordinary"))};
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({inbox})));
        QVERIFY(!panelTreeTexts(panel).contains(QStringLiteral("Other Folders")));
    }

    void journalFoldersOpenAndExportLikeTasksAndNotes() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({journalFolder()})));
        QVERIFY(panelTreeTexts(panel).contains(QStringLiteral("Journal (1)")));

        panel.onFolderTreeItemClicked(treeItem(panel, QStringLiteral("Journal (1)")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 1, kWaitMs);
        QCOMPARE(panel.m_item_list->item(0, sak::EmailInspectorPanel::ColType)->text(),
                 QStringLiteral("Journal"));
        QSignalSpy opened(panel.m_controller.get(), &EmailInspectorController::itemDetailLoaded);
        panel.onItemListCellClicked(0, sak::EmailInspectorPanel::ColSubject);
        QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, kWaitMs);
        const QString shown = panel.m_content_browser->toPlainText();
        QVERIFY2(shown.contains(QStringLiteral("Phone call")), qPrintable(shown));
        QVERIFY(shown.contains(QStringLiteral("15 minutes")));
        QVERIFY(shown.contains(QStringLiteral("Thermostat part on order.")));

        sak::PstFolder folder;
        folder.container_class = QStringLiteral("IPF.Journal");
        QMenu menu;
        panel.addFolderExportActions(menu, &folder, std::optional<uint64_t>(1));
        QCOMPARE(menu.actions().size(), 1);
        QCOMPARE(menu.actions().first()->text(), QStringLiteral("Export Journal as CSV..."));

        PstParser parser;
        parser.open(writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({journalFolder()})));
        const QString out = m_dir.filePath(QStringLiteral("journal_csv"));
        exportFirstFolder(parser, sak::ExportFormat::CsvJournal, out);
        QFile csv(out + QStringLiteral("/journal_export.csv"));
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(csv.readAll());
        QVERIFY2(text.contains(QStringLiteral("Phone call")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("Call with the oven company")));
        QVERIFY(text.contains(QStringLiteral(",15,")));
    }

    // Through the panel, from the right-click to the file on disk: nothing here calls the export
    // directly, so a menu that names the wrong format or the wrong folder shows up.
    void aRightClickExportWritesTheFolderItWasOpenedOn() {
        sak::EmailInspectorPanel panel;
        panel.resize(1200, 800);
        panel.show();
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        inbox.messages = {storedMail(QStringLiteral("Ordinary"))};
        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"),
                              PstStoreBuilder{}.build({inbox, tasksFolder()})));
        // Selected: the Inbox. Right-clicked: Tasks. It must be Tasks that comes out.
        panel.onFolderTreeItemClicked(treeItem(panel, QStringLiteral("Inbox (1)")), 0);
        QTRY_COMPARE_WITH_TIMEOUT(panel.m_item_list->rowCount(), 1, kWaitMs);

        QTreeWidgetItem* tasks = treeItem(panel, QStringLiteral("Tasks (1)"));
        QVERIFY(tasks != nullptr);
        const QPoint at = panel.m_folder_tree->visualItemRect(tasks).center();
        const QString out = m_dir.filePath(QStringLiteral("right_click"));
        QVERIFY(QDir().mkpath(out));

        QSignalSpy finished(panel.m_controller.get(), &EmailInspectorController::exportComplete);
        ModalDriver driver;
        driver.onMenu = [](QMenu* menu) {
            for (QAction* action : menu->actions()) {
                if (action->text() == QStringLiteral("Export Tasks as CSV...")) {
                    menu->setActiveAction(action);
                    QTest::keyClick(menu, Qt::Key_Return);
                    return;
                }
            }
            menu->close();
        };
        driver.onModal = [&out](QWidget* modal) {
            if (auto* dialog = qobject_cast<QFileDialog*>(modal)) {
                // The folder the dialog is showing is its answer. Selecting it as well would read
                // as a folder of that name inside itself.
                dialog->setDirectory(out);
                static_cast<QDialog*>(dialog)->accept();
            }
        };
        panel.onFolderTreeContextMenu(at);
        QVERIFY(driver.menuSeen);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);

        QFile csv(out + QStringLiteral("/tasks_export.csv"));
        QVERIFY2(csv.open(QIODevice::ReadOnly), qPrintable(csv.fileName()));
        const QString text = QString::fromUtf8(csv.readAll());
        QVERIFY2(text.contains(QStringLiteral("Order flour")), qPrintable(text));
        QVERIFY(!text.contains(QStringLiteral("Ordinary")));
    }

    // "All mail folders" means all the mail: the system folder's, and none of a filed contact.
    void exportAllTakesEveryEmailAndNothingElse() {
        BuilderFolder inbox;
        inbox.name = QStringLiteral("Inbox");
        BuilderMessage filed;
        filed.message_class = QStringLiteral("IPM.Contact");
        filed.subject = QStringLiteral("Filed contact");
        inbox.messages = {storedMail(QStringLiteral("Ordinary")), filed};
        BuilderFolder history;
        history.name = QStringLiteral("Conversation History");
        history.messages = {storedMail(QStringLiteral("Saved chat"))};
        sak::EmailInspectorPanel panel;
        openAndWait(panel,
                    writeFile(QStringLiteral(".pst"), PstStoreBuilder{}.build({inbox, history})));

        const QString out = m_dir.filePath(QStringLiteral("export_all"));
        QVERIFY(QDir().mkpath(out));
        QSignalSpy finished(panel.m_controller.get(), &EmailInspectorController::exportComplete);
        ModalDriver driver;
        driver.onModal = [&out](QWidget* modal) {
            if (auto* dialog = qobject_cast<QFileDialog*>(modal)) {
                // The folder the dialog is showing is its answer. Selecting it as well would read
                // as a folder of that name inside itself.
                dialog->setDirectory(out);
                static_cast<QDialog*>(dialog)->accept();
            }
        };
        panel.exportAllMailFoldersAs(sak::ExportFormat::CsvEmails);
        QTRY_COMPARE_WITH_TIMEOUT(finished.count(), 1, kWaitMs);
        const auto result = finished.first().first().value<sak::EmailExportResult>();
        QCOMPARE(result.items_exported, 2);
        QFile csv(out + QStringLiteral("/emails_export.csv"));
        QVERIFY(csv.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(csv.readAll());
        QVERIFY(text.contains(QStringLiteral("Saved chat")));
        QVERIFY(!text.contains(QStringLiteral("Filed contact")));
    }

    // Counting a mailbox reads all of it. Stop has to leave nothing half-open behind.
    void stoppingWhileAMailboxIsCountedAbandonsIt() {
        const QString path = writeFile(QStringLiteral(".mbox"), sixMessageMailbox());
        sak::EmailInspectorPanel panel;
        QSignalSpy opened(panel.m_controller.get(), &EmailInspectorController::fileOpened);
        panel.m_controller->openFile(path);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Opening);
        QVERIFY(panel.m_cancel_operation_button->isEnabled());

        panel.m_cancel_operation_button->click();
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Idle);
        QVERIFY(!panel.m_controller->isFileOpen());
        QVERIFY(!panel.m_cancel_operation_button->isEnabled());

        // A count that finished just before the Stop must not announce a file that is gone.
        QCoreApplication::processEvents();
        QCOMPARE(opened.count(), 0);
        QVERIFY(!panel.m_mbox_active);
        QVERIFY(!panel.m_search_edit->isEnabled());

        // And the same mailbox opens afterwards.
        openAndWait(panel, path);
        QVERIFY(panel.m_controller->isFileOpen());
        QCOMPARE(panel.m_controller->fileInfo().total_items, 6);
    }

    // The count is done and its answer is waiting for the event loop when Stop arrives. It must not
    // announce a file that is gone.
    void aCountThatFinishedJustBeforeTheStopAnnouncesNothing() {
        const QString path = writeFile(QStringLiteral(".mbox"), sixMessageMailbox());
        sak::EmailInspectorPanel panel;
        QSignalSpy opened(panel.m_controller.get(), &EmailInspectorController::fileOpened);
        panel.m_controller->openFile(path);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Opening);
        // No event loop runs here, so whatever the finished count sent is still queued.
        QVERIFY(QThreadPool::globalInstance()->waitForDone(kWaitMs));
        QCOMPARE(opened.count(), 0);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Opening);

        panel.m_cancel_operation_button->click();
        QCoreApplication::processEvents();
        QCOMPARE(opened.count(), 0);
        QVERIFY(!panel.m_mbox_active);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Idle);
    }

    // With every pool thread busy the count has not begun, so waiting for it runs it right there on
    // this thread. A Stop that was forgotten then counts the whole file and announces it mid-close.
    void stoppingBeforeTheCountHasBegunAbandonsIt() {
        const QString path = writeFile(QStringLiteral(".mbox"), sixMessageMailbox());
        sak::EmailInspectorPanel panel;
        QSignalSpy opened(panel.m_controller.get(), &EmailInspectorController::fileOpened);

        QThreadPool* pool = QThreadPool::globalInstance();
        QSemaphore release;
        QSemaphore started;
        const int threads = pool->maxThreadCount();
        for (int i = 0; i < threads; ++i) {
            pool->start([&release, &started] {
                started.release();
                release.acquire();
            });
        }
        started.acquire(threads);

        panel.m_controller->openFile(path);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Opening);
        panel.m_cancel_operation_button->click();
        release.release(threads);
        pool->waitForDone();

        QCOMPARE(opened.count(), 0);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Idle);
        QVERIFY(!panel.m_controller->isFileOpen());
        QCoreApplication::processEvents();
        QCOMPARE(opened.count(), 0);
        QVERIFY(!panel.m_mbox_active);
    }

    // In an MBOX both ids are legitimately 0: the one folder, and the first message. Treating 0 as
    // "nothing chosen" made View Containing Email do nothing for the first message in a mailbox.
    void viewingAnAttachmentsEmailOpensTheFirstMessageOfAnMbox() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".mbox"), mailboxWithAttachment()));
        QSignalSpy listed(panel.m_controller.get(), &EmailInspectorController::mboxMessagesLoaded);
        ModalDriver driver;
        bool chose = false;
        driver.onModal = [&chose](QWidget* modal) {
            auto* dialog = qobject_cast<sak::EmailAttachmentsBrowserDialog*>(modal);
            if (dialog == nullptr || !dialog->m_scan_complete) {
                return;
            }
            chose = true;
            dialog->m_navigate_folder_id = dialog->m_all_attachments.first().source_folder_id;
            dialog->m_navigate_message_id = dialog->m_all_attachments.first().message_node_id;
            dialog->accept();
        };
        panel.onExportAttachmentsClicked();
        QVERIFY(chose);
        // The panel takes no message from the browser's own scan, only from where it navigates.
        QTRY_VERIFY_WITH_TIMEOUT(panel.m_current_item_id.has_value(), kWaitMs);
        QCOMPARE(panel.m_current_item_id, std::optional<uint64_t>(0));
        QCOMPARE(panel.m_current_detail.subject, QStringLiteral("First, with a file"));
        QTRY_VERIFY_WITH_TIMEOUT(listed.count() >= 1, kWaitMs);
    }

    // In a PST or OST a zero id names nothing, so closing the browser that way navigates nowhere.
    void closingTheAttachmentsBrowserOfAStoreGoesNowhere() {
        sak::EmailInspectorPanel panel;
        openAndWait(panel, writeFile(QStringLiteral(".pst"), storeWithInlinePicture()));
        QSignalSpy failed(panel.m_controller.get(), &EmailInspectorController::itemDetailFailed);
        QSignalSpy errors(panel.m_controller.get(), &EmailInspectorController::errorOccurred);
        ModalDriver driver;
        bool closed = false;
        driver.onModal = [&closed](QWidget* modal) {
            auto* dialog = qobject_cast<sak::EmailAttachmentsBrowserDialog*>(modal);
            if (dialog != nullptr && dialog->m_scan_complete) {
                closed = true;
                dialog->accept();
            }
        };
        panel.onExportAttachmentsClicked();
        QVERIFY(closed);
        // Asking for item 0 fails: nothing is there. So nothing may have asked.
        QCoreApplication::processEvents();
        QCOMPARE(failed.count(), 0);
        QCOMPARE(errors.count(), 0);
        QCOMPARE(panel.m_controller->currentState(), EmailInspectorController::State::Idle);
        QVERIFY(!panel.m_current_item_id.has_value());
        QCOMPARE(panel.m_current_folder_id, uint64_t{0});
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

    void anEventShowsItsPeopleAndHowItRepeats() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("series.pst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QDateTime start(QDate(2026, 11, 2), QTime(17, 0), QTimeZone::UTC);
        file.write(PstStoreBuilder{}.build({repeatingCalendar(start)}));
        file.close();

        EmailInspectorController controller;
        sak::PstFolderTree tree;
        QObject::connect(&controller,
                         &EmailInspectorController::folderTreeLoaded,
                         &controller,
                         [&tree](const sak::PstFolderTree& loaded) { tree = loaded; });
        controller.openFile(path);
        QVERIFY(controller.isFileOpen());
        const sak::PstFolder* calendar = findByClass(tree, QStringLiteral("IPF.Appointment"));
        QVERIFY(calendar != nullptr);

        sak::EmailCalendarDialog dialog(&controller, {calendar->node_id});
        QTRY_COMPARE_WITH_TIMEOUT(dialog.m_all_events.size(), 2, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.m_all_events.first().detail_loaded, kWaitMs);

        dialog.displayEventSummary(dialog.m_all_events[0]);
        // Its changed occurrence is stored as a hidden attachment, and is no file anyone attached.
        QCOMPARE(dialog.m_all_events[0].attachment_count, 0);
        const QString shown = dialog.m_detail_browser->toPlainText();
        QVERIFY2(shown.contains(QStringLiteral("Every week on Monday")), qPrintable(shown));
        QVERIFY2(shown.contains(QStringLiteral("1 deleted, 1 changed")), qPrintable(shown));
        QVERIFY2(shown.contains(QStringLiteral("Marisol Quint <marisol@harborlane.example> "
                                               "(Required, Accepted)")),
                 qPrintable(shown));
        QVERIFY(
            shown.contains(QStringLiteral("Dana Whitlock <dana@harborlane.example> "
                                          "(Organizer)")));
        QVERIFY(shown.contains(QStringLiteral("Owen Lark (Optional, Declined)")));

        // Marked wherever its title is drawn.
        dialog.populateDayEventList(start.toLocalTime().date());
        QVERIFY(dialog.m_day_event_list->rowCount() >= 1);
        QVERIFY2(
            dialog.m_day_event_list->item(0, kDayListSubjectColumn)->text().contains(QChar(0x21BB)),
            "the day list does not mark a repeating event");

        // A pattern that cannot be read says so, on the event.
        dialog.displayEventSummary(dialog.m_all_events[1]);
        const QString unreadable = dialog.m_detail_browser->toPlainText();
        QVERIFY2(unreadable.contains(QStringLiteral("could not be read")), qPrintable(unreadable));
    }

    // The right-click export carries all of it, through the same writer as the whole calendar.
    void aSingleEventExportCarriesItsPeopleAndPattern() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("series.pst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QDateTime start(QDate(2026, 11, 2), QTime(17, 0), QTimeZone::UTC);
        file.write(PstStoreBuilder{}.build({repeatingCalendar(start)}));
        file.close();
        EmailInspectorController controller;
        sak::PstFolderTree tree;
        QObject::connect(&controller,
                         &EmailInspectorController::folderTreeLoaded,
                         &controller,
                         [&tree](const sak::PstFolderTree& loaded) { tree = loaded; });
        controller.openFile(path);
        const sak::PstFolder* calendar = findByClass(tree, QStringLiteral("IPF.Appointment"));
        QVERIFY(calendar != nullptr);
        sak::EmailCalendarDialog dialog(&controller, {calendar->node_id});
        QTRY_COMPARE_WITH_TIMEOUT(dialog.m_all_events.size(), 2, kWaitMs);
        QTRY_VERIFY_WITH_TIMEOUT(dialog.m_all_events.first().detail_loaded, kWaitMs);

        const QString out = dir.filePath(QStringLiteral("Staff meeting.ics"));
        ModalDriver driver;
        driver.onModal = [&dir](QWidget* modal) {
            if (auto* save = qobject_cast<QFileDialog*>(modal)) {
                save->setDirectory(dir.path());
                static_cast<QDialog*>(save)->accept();
            }
        };
        dialog.exportSingleEventIcs(dialog.m_all_events[0]);
        QFile ics(out);
        QVERIFY2(ics.open(QIODevice::ReadOnly),
                 qPrintable(QDir(dir.path()).entryList(QDir::Files).join(QLatin1Char('|'))));
        const QByteArray text = ics.readAll();
        QVERIFY2(text.contains("RRULE:FREQ=WEEKLY"), text.constData());
        QVERIFY(text.contains("ATTENDEE;CN=\"Marisol Quint\""));
        QVERIFY(text.contains("EXDATE;TZID="));
        QVERIFY(text.contains("RECURRENCE-ID;TZID="));
        // The old single-event writer left out what RFC 5545 requires, and doubled its line ends.
        QVERIFY(text.contains("UID:"));
        QVERIFY(text.contains("DTSTAMP:"));
        QVERIFY(!text.contains("\r\r\n"));
    }

    void theWholeCalendarExportSaysWhatItCouldNotCarry() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("series.pst"));
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        const QDateTime start(QDate(2026, 11, 2), QTime(17, 0), QTimeZone::UTC);
        file.write(PstStoreBuilder{}.build({repeatingCalendar(start)}));
        file.close();

        PstParser parser;
        parser.open(path);
        QVERIFY(parser.isOpen());
        const sak::PstFolderTree tree = parser.folderTree();
        const sak::PstFolder* calendar = findByClass(tree, QStringLiteral("IPF.Appointment"));
        QVERIFY(calendar != nullptr);
        sak::EmailExportConfig config;
        config.format = sak::ExportFormat::Ics;
        config.output_path = dir.path();
        config.has_folder = true;
        config.folder_id = calendar->node_id;
        EmailExportWorker worker;
        QSignalSpy done(&worker, &EmailExportWorker::exportComplete);
        worker.exportItems(&parser, config);
        QCOMPARE(done.count(), 1);
        const auto result = done.first().first().value<sak::EmailExportResult>();
        QCOMPARE(result.items_exported, 2);
        QVERIFY(result.errors.isEmpty());
        QCOMPARE(result.items_failed, 0);
        QCOMPARE(result.notes.size(), 1);
        QVERIFY2(result.notes.first().contains(QStringLiteral("Unreadable series")),
                 qPrintable(result.notes.first()));

        // The log and the status line carry it through the panel.
        sak::EmailInspectorPanel panel;
        QSignalSpy logged(&panel, &sak::EmailInspectorPanel::logOutput);
        panel.onExportComplete(result);
        QVERIFY2(panel.m_status_label->text().contains(QStringLiteral("note")),
                 qPrintable(panel.m_status_label->text()));
        bool in_log = false;
        for (const auto& line : logged) {
            in_log = in_log ||
                     line.first().toString().contains(QStringLiteral("Unreadable series"));
        }
        QVERIFY(in_log);
    }
};

QTEST_MAIN(EmailToolsViewsTests)
#include "test_email_tools_mailbox_views.moc"
