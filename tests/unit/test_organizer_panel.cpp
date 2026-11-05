// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_organizer_panel.cpp
/// @brief The File Organizer's two buttons do what they say, whatever was pressed before.

#include "sak/organizer_panel.h"

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
#include <QTableWidget>
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
    void previewMovesNothingAndOrganizeMovesEverything();
    void aFileThatWillNotMoveIsListedWithWhatDidMove();
    void thereIsNoPreviewModeLeftToForget();
};

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
    QVERIFY2(report.contains(QStringLiteral("Moved 1 file(s).")), qPrintable(report));
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
