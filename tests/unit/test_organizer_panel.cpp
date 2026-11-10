// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_organizer_panel.cpp
/// @brief The File Organizer's two buttons do what they say, whatever was pressed before.

#include "sak/file_management_explorer_panel.h"
#include "sak/organizer_panel.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QHeaderView>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QtTest/QtTest>

#include <windows.h>

namespace {

constexpr int kDialogPollMs = 20;
constexpr int kRunTimeoutMs = 10'000;

bool writeFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("content") > 0;
}

/// Finds a control by the name a screen reader reads, which is how the panel labels them.
template <typename Control>
Control* findByAccessibleName(const QWidget& parent, const QString& name) {
    for (Control* control : parent.findChildren<Control*>()) {
        if (control->accessibleName() == name) {
            return control;
        }
    }
    return nullptr;
}

/// Answers whichever dialog appears, the way a person clicking the obvious button would, and keeps
/// what it said.
class DialogAnswerer : public QObject {
public:
    explicit DialogAnswerer(QObject* parent) : QObject(parent), m_timer(this) {
        m_timer.setInterval(kDialogPollMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() { answer(); });
        m_timer.start();
    }

    QStringList seen;

private:
    void answer() {
        QWidget* modal = QApplication::activeModalWidget();
        if (modal == nullptr) {
            return;
        }
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            seen.append(box->windowTitle() + QStringLiteral(": ") + box->text());
            QAbstractButton* yes = box->button(QMessageBox::Yes);
            (yes != nullptr ? yes : box->button(QMessageBox::Ok))->click();
            return;
        }
        if (auto* dialog = qobject_cast<QDialog*>(modal)) {
            const auto* text = dialog->findChild<QTextEdit*>();
            seen.append(dialog->windowTitle() + QStringLiteral(": ") +
                        (text != nullptr ? text->toPlainText() : QString()));
            dialog->accept();
        }
    }

    QTimer m_timer;
};

}  // namespace

class OrganizerPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // UI review F42: the category table was the only one in the app with row numbers.
    void theCategoryTableHasNoRowNumbers() {
        sak::OrganizerPanel panel;
        for (const QTableWidget* candidate : panel.findChildren<QTableWidget*>()) {
            if (candidate->accessibleName() == QStringLiteral("Category Mappings Table")) {
                QVERIFY(!candidate->verticalHeader()->isVisibleTo(&panel));
                return;
            }
        }
        QFAIL("no category table");
    }
    // Review 2 (N19): the target the page starts on read "C:/Users/Username".
    void theStartingTargetReadsAsAWindowsPath() {
        sak::OrganizerPanel panel;
        auto* target = findByAccessibleName<QLineEdit>(panel,
                                                       QStringLiteral("Target Directory Path"));
        QVERIFY(target != nullptr);
        QVERIFY2(!target->text().isEmpty(), "the page starts with no target");
        QVERIFY2(!target->text().contains(QLatin1Char('/')), qPrintable(target->text()));
    }
    void previewMovesNothingAndOrganizeMovesEverything();
    void aFileThatWillNotMoveIsListedWithWhatDidMove();
    void thereIsNoPreviewModeLeftToForget();
    void theHiddenExplorerKeepsItsStatusToItself();
    void eachPageHasOnePrimaryButton();
};

// Review 4 (Y12): File Organizer had eight primary buttons (Scan, Manual, Browse, the category
// edits, Settings and Organize Files). Each page's one primary button does the page's work.
void OrganizerPanelTests::eachPageHasOnePrimaryButton() {
    sak::OrganizerPanel panel;
    auto* tabs = panel.findChild<QTabWidget*>(QString(), Qt::FindDirectChildrenOnly);
    QVERIFY(tabs != nullptr);
    for (int index = 0; index < 2; ++index) {  // File Organizer, Duplicate Finder
        QStringList primary;
        for (const auto* button : tabs->widget(index)->findChildren<QPushButton*>()) {
            if (button->styleSheet() == sak::ui::kPrimaryButtonStyle) {
                primary.append(button->text());
            }
        }
        QVERIFY2(primary.size() == 1,
                 qPrintable(tabs->tabText(index) + QStringLiteral(": ") +
                            primary.join(QStringLiteral(", "))));
    }
}

// Review 4 (S22): the File Explorer, listing behind the File Organizer sub-tab, put "Explorer
// loaded 32 items" in the status bar there after the tab change had cleared it.
void OrganizerPanelTests::theHiddenExplorerKeepsItsStatusToItself() {
    sak::OrganizerPanel panel;
    panel.resize(1100, 700);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    auto* tabs = panel.findChild<QTabWidget*>(QString(), Qt::FindDirectChildrenOnly);
    auto* explorer = panel.findChild<sak::FileManagementExplorerPanel*>();
    QVERIFY(tabs != nullptr && explorer != nullptr);
    QSignalSpy spy(&panel, &sak::OrganizerPanel::statusMessage);
    tabs->setCurrentIndex(0);  // File Organizer
    QCoreApplication::processEvents();
    Q_EMIT explorer->statusMessage(QStringLiteral("Explorer loaded 32 items"), 3000);
    for (const auto& call : spy) {
        QVERIFY2(call.at(0).toString() != QStringLiteral("Explorer loaded 32 items"),
                 "the hidden explorer's message reached the status bar");
    }
    tabs->setCurrentWidget(explorer);
    QCoreApplication::processEvents();
    spy.clear();
    Q_EMIT explorer->statusMessage(QStringLiteral("Explorer loaded 32 items"), 3000);
    QCOMPARE(spy.size(), 1);
}

// Organize Files used to preview too, until a tick box in Settings was turned off, and pressing
// Preview turned it back on. Pressing the two in this order is exactly what the old page did wrong.
void OrganizerPanelTests::previewMovesNothingAndOrganizeMovesEverything() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString photo = dir.filePath(QStringLiteral("photo.jpg"));
    QVERIFY(writeFile(photo));

    sak::OrganizerPanel panel;
    auto* target = findByAccessibleName<QLineEdit>(panel, QStringLiteral("Target Directory Path"));
    auto* preview = findByAccessibleName<QPushButton>(panel,
                                                      QStringLiteral("Preview Organization"));
    auto* organize = findByAccessibleName<QPushButton>(panel, QStringLiteral("Organize Files"));
    QVERIFY(target != nullptr && preview != nullptr && organize != nullptr);
    target->setText(dir.path());

    DialogAnswerer answers(this);

    preview->click();
    QTRY_VERIFY_WITH_TIMEOUT(preview->isEnabled() && !answers.seen.isEmpty(), kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(QApplication::activeModalWidget() == nullptr, kRunTimeoutMs);
    QVERIFY(QFile::exists(photo));

    answers.seen.clear();
    organize->click();
    const QString landed = dir.filePath(QStringLiteral("Images/photo.jpg"));
    QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(landed), kRunTimeoutMs);
    QVERIFY(!QFile::exists(photo));
    QVERIFY2(answers.seen.join('\n').contains(QStringLiteral("Confirm Organization")),
             qPrintable(answers.seen.join('\n')));
    QTRY_VERIFY_WITH_TIMEOUT(organize->isEnabled(), kRunTimeoutMs);
}

void OrganizerPanelTests::aFileThatWillNotMoveIsListedWithWhatDidMove() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString locked = dir.filePath(QStringLiteral("locked.jpg"));
    QVERIFY(writeFile(locked));
    QVERIFY(writeFile(dir.filePath(QStringLiteral("free.jpg"))));
    HANDLE lock = CreateFileW(QDir::toNativeSeparators(locked).toStdWString().c_str(),
                              GENERIC_READ,
                              0,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    QVERIFY(lock != INVALID_HANDLE_VALUE);

    sak::OrganizerPanel panel;
    auto* target = findByAccessibleName<QLineEdit>(panel, QStringLiteral("Target Directory Path"));
    auto* organize = findByAccessibleName<QPushButton>(panel, QStringLiteral("Organize Files"));
    QVERIFY(target != nullptr && organize != nullptr);
    target->setText(dir.path());

    DialogAnswerer answers(this);
    organize->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        answers.seen.join('\n').contains(QStringLiteral("Finished With Errors")), kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(organize->isEnabled(), kRunTimeoutMs);
    CloseHandle(lock);

    const QString report = answers.seen.join('\n');
    QVERIFY2(report.contains(QStringLiteral("Moved 1 file.")), qPrintable(report));
    QVERIFY2(report.contains(QStringLiteral("1 file could not be moved and stayed where it was")),
             qPrintable(report));
    QVERIFY2(report.contains(QStringLiteral("locked.jpg")), qPrintable(report));
    QVERIFY(QFile::exists(locked));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("Images/free.jpg"))));
}

void OrganizerPanelTests::thereIsNoPreviewModeLeftToForget() {
    const sak::OrganizerPanel panel;
    for (const QCheckBox* box : panel.findChildren<QCheckBox*>()) {
        QVERIFY2(!box->text().contains(QStringLiteral("Preview")), qPrintable(box->text()));
    }
}

QTEST_MAIN(OrganizerPanelTests)
#include "test_organizer_panel.moc"
