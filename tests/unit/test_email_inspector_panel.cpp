// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_inspector_panel.cpp
/// @brief Email Inspector search: what the technician asked for is searched, and what it finds
///        reaches the screen.

#include "sak/email_inspector_panel.h"

#include "../support/pst_fixture.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
#include <QtTest/QtTest>

namespace {

constexpr int kOpenTimeoutMs = 30'000;
constexpr int kSearchTimeoutMs = 30'000;
constexpr int kDialogPollMs = 20;
constexpr int kDialogTimeoutMs = 10'000;
constexpr uint64_t kNoSuchFolder = 0x1234;

QByteArray mailboxBytes() {
    QByteArray content;
    content += "From a@example.com Mon Jan  1 00:00:00 2024\r\n";
    content += "From: Sender One <a@example.com>\r\n";
    content += "To: Zebra Recipient <zebracontact@example.com>\r\n";
    content += "Subject: Reaches the zebra person\r\n";
    content += "\r\n";
    content += "Plain body.\r\n";
    content += "\r\n";
    content += "From b@example.com Mon Jan  2 00:00:00 2024\r\n";
    content += "From: Sender Two <b@example.com>\r\n";
    content += "To: Someone <someone@example.com>\r\n";
    content += "Subject: Carries a file\r\n";
    content += "MIME-Version: 1.0\r\n";
    content += "Content-Type: multipart/mixed; boundary=\"BOUND\"\r\n";
    content += "\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: text/plain; charset=UTF-8\r\n";
    content += "\r\n";
    content += "Body text.\r\n";
    content += "--BOUND\r\n";
    content += "Content-Type: application/octet-stream; name=\"quarterly.bin\"\r\n";
    content += "Content-Transfer-Encoding: base64\r\n";
    content += "Content-Disposition: attachment; filename=\"quarterly.bin\"\r\n";
    content += "\r\n";
    content += "SGVsbG8gQXR0YWNo\r\n";
    content += "--BOUND--\r\n";
    return content;
}

bool writeBytes(const QString& path, const QByteArray& bytes) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QStringList columnTexts(const QTableWidget& table, int column) {
    QStringList texts;
    for (int row = 0; row < table.rowCount(); ++row) {
        const QTableWidgetItem* item = table.item(row, column);
        texts << (item != nullptr ? item->text() : QString());
    }
    return texts;
}

/// Answers the next QInputDialog, recording its title. Polls, because the dialog runs its own
/// nested event loop inside the call that opened it.
class InputDialogAnswerer : public QObject {
public:
    explicit InputDialogAnswerer(QString answer, bool accept = true)
        : m_answer(std::move(answer)), m_accept(accept) {
        m_timer.setInterval(kDialogPollMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() { answerIfOpen(); });
        m_timer.start();
        QTimer::singleShot(kDialogTimeoutMs, &m_timer, &QTimer::stop);
    }
    QString title;
    bool seen{false};

private:
    void answerIfOpen() {
        auto* dialog = qobject_cast<QInputDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        m_timer.stop();
        seen = true;
        title = dialog->windowTitle();
        dialog->setTextValue(m_answer);
        dialog->done(m_accept ? QDialog::Accepted : QDialog::Rejected);
    }
    QString m_answer;
    bool m_accept;
    QTimer m_timer;
};

}  // namespace

class EmailInspectorPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void init();
    void cleanup();

    void panelSearchCoversRecipientsAndAttachmentNames();
    void searchCriteriaScopesOnlyWhenAFolderIsGiven();
    void resultsAreListedSoTheyCanBeOpened();
    void aSearchThatFindsNothingSaysSo();
    void leavingTheResultsForAFolderListsThatFolder();
    void searchInFolderSearchesOnlyThatFolder();
    void searchInFolderCancelledSearchesNothing();
    void searchInFolderWithNoFolderIdAsksNothing();
    void folderMenuItemOpensTheScopedSearch();

private:
    void openMailbox();
    void openStore();
    void runRibbonSearch(const QString& query);
    void waitForSearchToFinish();

    QTemporaryDir m_dir;
    std::unique_ptr<sak::EmailInspectorPanel> m_panel;
};

void EmailInspectorPanelTests::init() {
    QVERIFY(m_dir.isValid());
    m_panel = std::make_unique<sak::EmailInspectorPanel>();
}

void EmailInspectorPanelTests::cleanup() {
    m_panel.reset();
}

void EmailInspectorPanelTests::openMailbox() {
    const QString path = m_dir.filePath(QStringLiteral("mail.mbox"));
    QVERIFY(writeBytes(path, mailboxBytes()));
    m_panel->m_controller->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(m_panel->m_search_edit->isEnabled(), kOpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_panel->m_controller->currentState(),
                              EmailInspectorController::State::Idle,
                              kOpenTimeoutMs);
}

void EmailInspectorPanelTests::openStore() {
    const QString path = m_dir.filePath(QStringLiteral("store.pst"));
    QVERIFY(writeBytes(path, sak::pst_fixture::buildHnidCellTcStore()));
    m_panel->m_controller->openFile(path);
    QTRY_VERIFY_WITH_TIMEOUT(m_panel->m_search_edit->isEnabled(), kOpenTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_panel->m_controller->currentState(),
                              EmailInspectorController::State::Idle,
                              kOpenTimeoutMs);
}

void EmailInspectorPanelTests::waitForSearchToFinish() {
    QTRY_COMPARE_WITH_TIMEOUT(m_panel->m_controller->currentState(),
                              EmailInspectorController::State::Idle,
                              kSearchTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(!m_panel->m_item_count_label->text().isEmpty(), kSearchTimeoutMs);
}

void EmailInspectorPanelTests::runRibbonSearch(const QString& query) {
    m_panel->m_search_edit->setText(query);
    m_panel->m_search_button->click();
    QCOMPARE(m_panel->m_controller->currentState(), EmailInspectorController::State::Searching);
    waitForSearchToFinish();
}

// The worker has always been able to search these two; the panel never asked it to.
void EmailInspectorPanelTests::panelSearchCoversRecipientsAndAttachmentNames() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    runRibbonSearch(QStringLiteral("zebracontact"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
    QCOMPARE(m_panel->m_item_list->item(0, sak::EmailInspectorPanel::ColSubject)->text(),
             QStringLiteral("Reaches the zebra person"));

    runRibbonSearch(QStringLiteral("quarterly.bin"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
    QCOMPARE(m_panel->m_item_list->item(0, sak::EmailInspectorPanel::ColSubject)->text(),
             QStringLiteral("Carries a file"));
}

void EmailInspectorPanelTests::searchCriteriaScopesOnlyWhenAFolderIsGiven() {
    const auto everywhere = sak::EmailInspectorPanel::searchCriteria(QStringLiteral("q"), 0);
    QCOMPARE(everywhere.folder_scope_id, uint64_t{0});
    QVERIFY(everywhere.search_subject);
    QVERIFY(everywhere.search_body);
    QVERIFY(everywhere.search_sender);
    QVERIFY(everywhere.search_recipients);
    QVERIFY(everywhere.search_attachment_names);

    const auto scoped = sak::EmailInspectorPanel::searchCriteria(QStringLiteral("q"), 42);
    QCOMPARE(scoped.folder_scope_id, uint64_t{42});
}

// The panel used to throw every hit away and print only a count: a search showed nothing a
// technician could open. Each hit is now a row, and clicking it opens that message.
void EmailInspectorPanelTests::resultsAreListedSoTheyCanBeOpened() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    runRibbonSearch(QStringLiteral("Carries"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
    const auto id = m_panel->itemIdForRow(0);
    QVERIFY(id.has_value());
    QCOMPARE(*id, uint64_t{1});
    QVERIFY2(m_panel->m_item_count_label->text().contains(QStringLiteral("1 search result")),
             qPrintable(m_panel->m_item_count_label->text()));
    QVERIFY(columnTexts(*m_panel->m_item_list, sak::EmailInspectorPanel::ColFrom)
                .first()
                .contains(QStringLiteral("Sender Two")));

    QSignalSpy opened(m_panel->m_controller.get(),
                      &EmailInspectorController::mboxMessageDetailLoaded);
    m_panel->onItemListCellClicked(0, sak::EmailInspectorPanel::ColSubject);
    QTRY_COMPARE_WITH_TIMEOUT(opened.count(), 1, kSearchTimeoutMs);
}

void EmailInspectorPanelTests::aSearchThatFindsNothingSaysSo() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    runRibbonSearch(QStringLiteral("no-such-text-anywhere"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 0);
    QVERIFY2(m_panel->m_item_count_label->text().contains(QStringLiteral("0 search result")),
             qPrintable(m_panel->m_item_count_label->text()));
}

void EmailInspectorPanelTests::leavingTheResultsForAFolderListsThatFolder() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    runRibbonSearch(QStringLiteral("Carries"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);

    m_panel->onFolderTreeItemClicked(m_panel->m_folder_tree->topLevelItem(0), 0);
    QTRY_COMPARE_WITH_TIMEOUT(m_panel->m_item_list->rowCount(), 2, kSearchTimeoutMs);
}

// The menu item only focused the search box, and the box then searched the whole store.
void EmailInspectorPanelTests::searchInFolderSearchesOnlyThatFolder() {
    openStore();
    if (QTest::currentTestFailed()) {
        return;
    }
    {
        InputDialogAnswerer answerer(QStringLiteral("HI"));
        m_panel->searchInFolder(uint64_t{sak::pst_fixture::kRootFolderNid}, QStringLiteral("Root"));
        QVERIFY(answerer.seen);
        QCOMPARE(answerer.title, QStringLiteral("Search in Root"));
    }
    waitForSearchToFinish();
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
    QVERIFY2(m_panel->m_item_count_label->text().contains(QStringLiteral("in Root")),
             qPrintable(m_panel->m_item_count_label->text()));

    // The same text, the same store, a folder the message is not in.
    {
        InputDialogAnswerer answerer(QStringLiteral("HI"));
        m_panel->searchInFolder(kNoSuchFolder, QStringLiteral("Elsewhere"));
        QVERIFY(answerer.seen);
    }
    waitForSearchToFinish();
    QCOMPARE(m_panel->m_item_list->rowCount(), 0);
    QVERIFY2(m_panel->m_item_count_label->text().contains(QStringLiteral("in Elsewhere")),
             qPrintable(m_panel->m_item_count_label->text()));

    // The ribbon box still means the whole store: no scope carries over from the menu.
    runRibbonSearch(QStringLiteral("HI"));
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
}

void EmailInspectorPanelTests::searchInFolderCancelledSearchesNothing() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    InputDialogAnswerer answerer(QStringLiteral("Carries"), false);
    m_panel->searchInFolder(uint64_t{0}, QStringLiteral("Inbox"));
    QVERIFY(answerer.seen);
    QCOMPARE(m_panel->m_controller->currentState(), EmailInspectorController::State::Idle);
    QVERIFY(!m_panel->m_showing_search_results);
}

void EmailInspectorPanelTests::searchInFolderWithNoFolderIdAsksNothing() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    InputDialogAnswerer answerer(QStringLiteral("Carries"));
    m_panel->searchInFolder(std::nullopt, QStringLiteral("Broken"));
    QVERIFY(!answerer.seen);
    QCOMPARE(m_panel->m_controller->currentState(), EmailInspectorController::State::Idle);
}

// The item in the folder's right-click menu, end to end: the title proves it is this folder's
// search that opened, and the list proves it ran.
void EmailInspectorPanelTests::folderMenuItemOpensTheScopedSearch() {
    openMailbox();
    if (QTest::currentTestFailed()) {
        return;
    }
    constexpr int kWindowWidth = 1200;
    constexpr int kWindowHeight = 800;
    m_panel->resize(kWindowWidth, kWindowHeight);
    m_panel->show();
    QVERIFY(QTest::qWaitForWindowExposed(m_panel.get()));
    QTreeWidgetItem* inbox = m_panel->m_folder_tree->topLevelItem(0);
    QVERIFY(inbox != nullptr);
    const QPoint position = m_panel->m_folder_tree->visualItemRect(inbox).center();

    bool chosen = false;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&chosen, &poll]() {
        auto* menu = qobject_cast<QMenu*>(QApplication::activePopupWidget());
        if (menu == nullptr) {
            return;
        }
        const auto actions = menu->actions();
        for (QAction* action : actions) {
            if (action->text() == QStringLiteral("Search in This Folder...")) {
                poll.stop();
                chosen = true;
                menu->setActiveAction(action);
                QTest::keyClick(menu, Qt::Key_Return);
                return;
            }
        }
    });
    poll.start();
    QTimer::singleShot(kDialogTimeoutMs, &poll, &QTimer::stop);

    InputDialogAnswerer answerer(QStringLiteral("Carries"));
    m_panel->onFolderTreeContextMenu(position);
    QVERIFY(chosen);
    QVERIFY(answerer.seen);
    QVERIFY2(answerer.title.startsWith(QStringLiteral("Search in Inbox")),
             qPrintable(answerer.title));
    waitForSearchToFinish();
    QCOMPARE(m_panel->m_item_list->rowCount(), 1);
}

QTEST_MAIN(EmailInspectorPanelTests)
#include "test_email_inspector_panel.moc"
