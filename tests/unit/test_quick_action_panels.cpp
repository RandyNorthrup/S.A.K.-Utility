// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_quick_action_panels.cpp
/// @brief Drives each panel's quick-action flow -- click, confirmation, folder choice, run,
///        progress and completion -- through a real QuickActionController, with fake actions and
///        a fake elevated helper (quick_action_fakes.h) standing in for everything that would
///        change this machine: no Settings window, no SFC, no network reset. The last cases put
///        all three panels on the one controller the application shares between them.

#include "sak/action_constants.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/elevation_manager.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/quick_action.h"
#include "sak/quick_action_controller.h"
#include "sak/running_operations.h"
#include "sak/user_migration_panel.h"

#include "quick_action_fakes.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QtTest/QtTest>

#include <algorithm>
#include <atomic>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>

namespace {

using namespace quick_action_fakes;

constexpr int kRunTimeoutMs = 10'000;

/// Every action the three panels offer, registered on the one controller they share.
const QStringList kAllPanelActions{kScreenshotSettings,
                                   kBitLocker,
                                   kOptimizePower,
                                   kVerifySystemFiles,
                                   kCheckDisk,
                                   kInventoryReport,
                                   kResetNetwork};

QPushButton* buttonNamed(const QWidget& panel, const QString& accessible_name) {
    for (auto* button : panel.findChildren<QPushButton*>()) {
        if (button->accessibleName() == accessible_name) {
            return button;
        }
    }
    return nullptr;
}

/// Runs @p handle on the modal dialog the next click opens, inside that dialog's own loop.
/// A press queues what it does. A zero timer is a queued call too and would run before the dialog
/// exists, so this one waits for an event loop, and the first to run is the dialog's.
void handleNextDialog(const std::function<void(QWidget*)>& handle) {
    QTimer::singleShot(1, qApp, [handle]() { handle(QApplication::activeModalWidget()); });
}

void answerNextQuestion(QMessageBox::StandardButton answer, bool* asked) {
    handleNextDialog([answer, asked](QWidget* modal) {
        auto* box = qobject_cast<QMessageBox*>(modal);
        if (box == nullptr || box->button(answer) == nullptr) {
            return;  // the test sees *asked still false
        }
        *asked = true;
        box->button(answer)->click();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    });
}

/// Accepts the next folder dialog in @p folder, recording where it started.
void chooseFolderInNextDialog(const QString& folder, bool* asked, QString* started_in) {
    handleNextDialog([folder, asked, started_in](QWidget* modal) {
        auto* dialog = qobject_cast<QFileDialog*>(modal);
        if (dialog == nullptr) {
            return;
        }
        *asked = true;
        if (started_in != nullptr) {
            *started_in = dialog->directory().absolutePath();
        }
        dialog->setDirectory(folder);
        // QFileDialog::accept is protected; through QDialog it still dispatches to it.
        static_cast<QDialog*>(dialog)->accept();
    });
}

/// Runs @p handle on the warning box that appears after a folder dialog closes.
void handleWarningBoxAfterFolderDialog(const std::function<void(QMessageBox*)>& handle,
                                       int tries_left = 200) {
    QTimer::singleShot(1, qApp, [handle, tries_left]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            handle(box);
        } else if (tries_left > 0) {
            handleWarningBoxAfterFolderDialog(handle, tries_left - 1);
        }
    });
}

QPushButton* boxButtonNamed(QMessageBox* box, const QString& text) {
    for (QAbstractButton* button : box->buttons()) {
        if (button->text() == text) {
            return qobject_cast<QPushButton*>(button);
        }
    }
    return nullptr;
}

QStringList texts(const QSignalSpy& spy) {
    QStringList out;
    for (const auto& arguments : spy) {
        out << arguments.at(0).toString();
    }
    return out;
}

bool anyStartsWith(const QSignalSpy& spy, const QString& prefix) {
    const QStringList all = texts(spy);
    return std::ranges::any_of(all,
                               [&prefix](const QString& text) { return text.startsWith(prefix); });
}

QString documentsFolder() {
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
}

}  // namespace

class QuickActionPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    void aPressReturnsBeforeItsDialogOpens();
    void userMigrationScreenshotAsksWhereToSaveAndSaysWhereItWent();
    void userMigrationScreenshotWithNoFolderRunsNothing();
    void userMigrationBitLockerAsksForAFolderAndHandsItToTheHelper();
    void userMigrationBitLockerWithNoFolderRunsNothing();
    void userMigrationBitLockerWarningDefaultsToCancelAndCancelRunsNothing();
    void userMigrationBitLockerWarningAcceptedSaysSoToTheHelper();
    void diagnosticsRunsEachMaintenanceActionThroughTheController();
    void diagnosticsVerifySystemFilesDeclinedRunsNothing();
    void diagnosticsInventoryReportSavesWhereTheUserChose();
    void networkResetConfirmsThenRunsElevatedAndRestoresTheButton();
    void networkResetDeclinedRunsNothing();
    void exitCancelsARunningElevatedQuickAction();
    void eachPanelReactsOnlyToItsOwnRuns();
    void aClickWhileAnotherCallersRunIsGoingSaysQueued();
    void aQueuedPanelRunKeepsTheFolderItsUserChose();

private:
    std::shared_ptr<FakeSystem> m_system;
    bool m_native_dialogs_were_disabled{false};
};

void QuickActionPanelTests::initTestCase() {
    // The Qt dialogs, not the Windows ones, so the test can drive them.
    m_native_dialogs_were_disabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
}

void QuickActionPanelTests::cleanupTestCase() {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, m_native_dialogs_were_disabled);
}

void QuickActionPanelTests::init() {
    if (sak::ElevationManager::isElevated()) {
        QSKIP("Run unelevated: administrator actions then take the elevated-helper route");
    }
    m_system = std::make_shared<FakeSystem>();
    useFakeElevatedHelper(m_system);
}

void QuickActionPanelTests::cleanup() {
    useRealElevatedHelper();
    m_system.reset();
}

// Screenshot Settings wrote to a fixed C:/SAK_Backups and reported only "Completed". It now asks
// for a folder (starting in Documents), writes there, and the completion names what it saved.
// A dialog opened inside a button's click holds whatever delivered the click, so a screen reader
// pressing the button could not read what it opened. Qt's accessibility press reaches that click a
// moment later, from the button's own timer, so pressing through QAccessible here would pass either
// way: what has to return first is the click.
void QuickActionPanelTests::aPressReturnsBeforeItsDialogOpens() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QPushButton* button = buttonNamed(panel, QStringLiteral("Screenshot Settings"));
    QVERIFY(button != nullptr);

    bool press_returned = false;
    bool dialog_opened = false;
    bool opened_inside_press = false;
    QTimer watcher;
    constexpr int kWatchIntervalMs = 5;
    watcher.setInterval(kWatchIntervalMs);
    connect(&watcher, &QTimer::timeout, &watcher, [&]() {
        auto* dialog = qobject_cast<QDialog*>(QApplication::activeModalWidget());
        if (dialog == nullptr) {
            return;
        }
        dialog_opened = true;
        opened_inside_press = !press_returned;
        dialog->reject();
    });
    watcher.start();
    button->click();
    press_returned = true;
    QTRY_VERIFY(dialog_opened);
    QVERIFY2(!opened_inside_press, "the dialog ran inside the button's press");
}

void QuickActionPanelTests::userMigrationScreenshotAsksWhereToSaveAndSaysWhereItWent() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QPushButton* button = buttonNamed(panel, QStringLiteral("Screenshot Settings"));
    QVERIFY(button != nullptr);

    bool asked = false;
    QString started_in;
    chooseFolderInNextDialog(output.path(), &asked, &started_in);
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(asked, "Screenshot Settings must ask where to save before it runs");
    QCOMPARE(QDir::cleanPath(started_in), documentsFolder());
    const QString saved =
        QDir::toNativeSeparators(QDir(output.path()).filePath(kScreenshotSettings + ".txt"));
    QTRY_VERIFY_WITH_TIMEOUT(texts(status).contains(QStringLiteral("Completed: ") +
                                                    kScreenshotSettings +
                                                    QStringLiteral(" - saved to ") + saved),
                             kRunTimeoutMs);
    QCOMPARE(m_system->inProcessRuns(),
             QStringList{kScreenshotSettings + QLatin1Char('|') + output.path()});
    QVERIFY(QFile::exists(saved));
    QVERIFY(m_system->elevatedTasks().isEmpty());
}

void QuickActionPanelTests::userMigrationScreenshotWithNoFolderRunsNothing() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QSignalSpy log(&panel, &sak::UserMigrationPanel::logOutput);
    QPushButton* button = buttonNamed(panel, QStringLiteral("Screenshot Settings"));
    QVERIFY(button != nullptr);

    bool asked = false;
    handleNextDialog([&asked](QWidget* modal) {
        if (auto* dialog = qobject_cast<QFileDialog*>(modal)) {
            asked = true;
            dialog->reject();
        }
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTest::qWait(50);
    QVERIFY(!anyStartsWith(status, QStringLiteral("Running: ")));
    QVERIFY(m_system->inProcessRuns().isEmpty());
    QVERIFY(
        texts(log).contains(QStringLiteral("Screenshot Settings cancelled: no folder was chosen")));
}

void QuickActionPanelTests::userMigrationBitLockerAsksForAFolderAndHandsItToTheHelper() {
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QPushButton* button = buttonNamed(panel, QStringLiteral("BitLocker Key Backup"));
    QVERIFY(button != nullptr);

    bool asked = false;
    chooseFolderInNextDialog(destination.path(), &asked, nullptr);
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(asked, "the key backup must ask where to save before it runs");
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(status, QStringLiteral("Completed: ") + kBitLocker),
                             kRunTimeoutMs);

    QCOMPARE(m_system->elevatedTasks(),
             QStringList{
                 QString::fromLatin1(sak::quick_action_elevation::kBackupBitLockerKeysTaskId)});
    QCOMPARE(QDir::cleanPath(m_system->elevatedPayloads()
                                 .constFirst()
                                 .value(QStringLiteral("backup_location"))
                                 .toString()),
             QDir::cleanPath(destination.path()));
    // No warning was shown, so no one agreed to anything.
    QVERIFY(!m_system->elevatedPayloads().constFirst().contains(
        QString::fromLatin1(sak::quick_action_elevation::kOutputFolderWarningAcceptedKey)));
}

void QuickActionPanelTests::userMigrationBitLockerWarningDefaultsToCancelAndCancelRunsNothing() {
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    m_system->output_folder_warning = QStringLiteral("%1 cannot restrict who reads files");
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QSignalSpy log(&panel, &sak::UserMigrationPanel::logOutput);
    QPushButton* button = buttonNamed(panel, QStringLiteral("BitLocker Key Backup"));
    QVERIFY(button != nullptr);

    bool asked = false;
    bool warned = false;
    bool cancel_is_default = false;
    QString warning_text;
    chooseFolderInNextDialog(destination.path(), &asked, nullptr);
    handleWarningBoxAfterFolderDialog([&](QMessageBox* box) {
        warned = true;
        warning_text = box->text();
        const QPushButton* const cancel =
            qobject_cast<QPushButton*>(box->button(QMessageBox::Cancel));
        cancel_is_default = box->defaultButton() == cancel && box->escapeButton() == cancel;
        box->button(QMessageBox::Cancel)->click();
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QVERIFY2(warned, "a drive that cannot restrict who reads files must warn before anything runs");
    QVERIFY2(warning_text.contains(destination.path()), qPrintable(warning_text));
    QVERIFY2(cancel_is_default, "Enter and Esc must both land on Cancel");
    QTest::qWait(50);
    QVERIFY(!anyStartsWith(status, QStringLiteral("Running: ")));
    QVERIFY(m_system->elevatedTasks().isEmpty());
    const QStringList lines = texts(log);
    QVERIFY(std::ranges::any_of(lines, [](const QString& line) {
        return line.endsWith(QStringLiteral(": nothing was written"));
    }));
}

void QuickActionPanelTests::userMigrationBitLockerWarningAcceptedSaysSoToTheHelper() {
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    m_system->output_folder_warning = QStringLiteral("%1 cannot restrict who reads files");
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QPushButton* button = buttonNamed(panel, QStringLiteral("BitLocker Key Backup"));
    QVERIFY(button != nullptr);

    bool asked = false;
    bool warned = false;
    chooseFolderInNextDialog(destination.path(), &asked, nullptr);
    handleWarningBoxAfterFolderDialog([&](QMessageBox* box) {
        QPushButton* const go_ahead = boxButtonNamed(box, QStringLiteral("Save Here Anyway"));
        if (go_ahead != nullptr) {
            warned = true;
            go_ahead->click();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        }
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QVERIFY(warned);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(status, QStringLiteral("Completed: ") + kBitLocker),
                             kRunTimeoutMs);
    QCOMPARE(m_system->elevatedPayloads()
                 .constFirst()
                 .value(QString::fromLatin1(
                     sak::quick_action_elevation::kOutputFolderWarningAcceptedKey))
                 .toBool(false),
             true);
}

void QuickActionPanelTests::userMigrationBitLockerWithNoFolderRunsNothing() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QSignalSpy log(&panel, &sak::UserMigrationPanel::logOutput);
    QPushButton* button = buttonNamed(panel, QStringLiteral("BitLocker Key Backup"));
    QVERIFY(button != nullptr);

    bool asked = false;
    handleNextDialog([&asked](QWidget* modal) {
        if (auto* dialog = qobject_cast<QFileDialog*>(modal)) {
            asked = true;
            dialog->reject();
        }
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTest::qWait(50);
    QVERIFY(!anyStartsWith(status, QStringLiteral("Running: ")));
    QVERIFY(m_system->elevatedTasks().isEmpty());
    QVERIFY(texts(log).contains(
        QStringLiteral("BitLocker key backup cancelled: no destination was chosen")));
}

void QuickActionPanelTests::diagnosticsRunsEachMaintenanceActionThroughTheController() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(
        controller, m_system, {kOptimizePower, kVerifySystemFiles, kCheckDisk, kInventoryReport}));
    sak::DiagnosticBenchmarkPanel panel(controller);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);

    // Optimize Power: in-process, no confirmation.
    QPushButton* power = buttonNamed(panel, QStringLiteral("Optimize Power"));
    QVERIFY(power != nullptr);
    power->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(status, QStringLiteral("Completed: ") + kOptimizePower),
                             kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(power->isEnabled(), kRunTimeoutMs);
    QCOMPARE(m_system->inProcessRuns().size(), 1);
    QVERIFY(m_system->inProcessRuns().constFirst().startsWith(kOptimizePower + QLatin1Char('|')));

    // Verify System Files: confirmed, then the elevated helper's task.
    QPushButton* verify = buttonNamed(panel, kVerifySystemFiles);
    QVERIFY(verify != nullptr);
    bool asked = false;
    answerNextQuestion(QMessageBox::Yes, &asked);
    verify->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(asked, "Verify System Files repairs Windows files and must be confirmed");
    QTRY_VERIFY_WITH_TIMEOUT(
        anyStartsWith(status, QStringLiteral("Completed: ") + kVerifySystemFiles), kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(verify->isEnabled(), kRunTimeoutMs);

    // Check Disk Errors: elevated, no confirmation.
    QPushButton* check_disk = buttonNamed(panel, kCheckDisk);
    QVERIFY(check_disk != nullptr);
    check_disk->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(status, QStringLiteral("Completed: ") + kCheckDisk),
                             kRunTimeoutMs);

    QCOMPARE(
        m_system->elevatedTasks(),
        (QStringList{QString::fromLatin1(sak::quick_action_elevation::kVerifySystemFilesTaskId),
                     QString::fromLatin1(sak::quick_action_elevation::kCheckDiskErrorsTaskId)}));
}

void QuickActionPanelTests::diagnosticsVerifySystemFilesDeclinedRunsNothing() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(
        controller, m_system, {kOptimizePower, kVerifySystemFiles, kCheckDisk, kInventoryReport}));
    sak::DiagnosticBenchmarkPanel panel(controller);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QPushButton* verify = buttonNamed(panel, kVerifySystemFiles);
    QVERIFY(verify != nullptr);

    bool asked = false;
    answerNextQuestion(QMessageBox::No, &asked);
    verify->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTest::qWait(50);
    QVERIFY(!anyStartsWith(status, QStringLiteral("Running: ")));
    QVERIFY(m_system->elevatedTasks().isEmpty());
    QVERIFY(verify->isEnabled());
}

void QuickActionPanelTests::diagnosticsInventoryReportSavesWhereTheUserChose() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    sak::QuickActionController controller;
    QVERIFY(registerFakes(
        controller, m_system, {kOptimizePower, kVerifySystemFiles, kCheckDisk, kInventoryReport}));
    sak::DiagnosticBenchmarkPanel panel(controller);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QPushButton* report = buttonNamed(panel, kInventoryReport);
    QVERIFY(report != nullptr);

    bool asked = false;
    QString started_in;
    chooseFolderInNextDialog(output.path(), &asked, &started_in);
    report->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QCOMPARE(QDir::cleanPath(started_in), documentsFolder());
    const QString saved =
        QDir::toNativeSeparators(QDir(output.path()).filePath(kInventoryReport + ".txt"));
    QTRY_VERIFY_WITH_TIMEOUT(texts(status).contains(QStringLiteral("Completed: ") +
                                                    kInventoryReport +
                                                    QStringLiteral(" - saved to ") + saved),
                             kRunTimeoutMs);
    QVERIFY(QFile::exists(saved));
}

void QuickActionPanelTests::networkResetConfirmsThenRunsElevatedAndRestoresTheButton() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kResetNetwork}));
    sak::NetworkDiagnosticPanel panel(controller);
    QSignalSpy status(&panel, &sak::NetworkDiagnosticPanel::statusMessage);
    QPushButton* reset = buttonNamed(panel, QStringLiteral("Reset network settings"));
    QVERIFY(reset != nullptr);

    m_system->hold.store(true);
    bool asked = false;
    answerNextQuestion(QMessageBox::Yes, &asked);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(asked, "a network reset must be confirmed");
    QVERIFY(!reset->isEnabled());
    QCOMPARE(reset->text(), QStringLiteral("Resetting..."));
    QTRY_COMPARE_WITH_TIMEOUT(
        m_system->elevatedTasks(),
        QStringList{QString::fromLatin1(sak::quick_action_elevation::kResetNetworkTaskId)},
        kRunTimeoutMs);

    // What the action said reaches the status bar: its real message ends in REBOOT REQUIRED, and
    // the panel used to replace it with a fixed line that left that out.
    m_system->hold.store(false);
    QTRY_VERIFY_WITH_TIMEOUT(texts(status).contains(QStringLiteral("fake elevated ok")),
                             kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), kRunTimeoutMs);
    QCOMPARE(reset->text(), QStringLiteral("Reset Network"));
}

void QuickActionPanelTests::networkResetDeclinedRunsNothing() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kResetNetwork}));
    sak::NetworkDiagnosticPanel panel(controller);
    QPushButton* reset = buttonNamed(panel, QStringLiteral("Reset network settings"));
    QVERIFY(reset != nullptr);

    bool asked = false;
    answerNextQuestion(QMessageBox::No, &asked);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTest::qWait(50);
    QVERIFY(m_system->elevatedTasks().isEmpty());
    QVERIFY(reset->isEnabled());
    QCOMPARE(reset->text(), QStringLiteral("Reset Network"));
}

// Closing the window while a quick action runs asks first and then cancels through each panel's
// registered running operation; for an administrator action that cancel must reach the helper.
void QuickActionPanelTests::exitCancelsARunningElevatedQuickAction() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kResetNetwork}));
    sak::NetworkDiagnosticPanel panel(controller);
    QSignalSpy status(&panel, &sak::NetworkDiagnosticPanel::statusMessage);
    QPushButton* reset = buttonNamed(panel, QStringLiteral("Reset network settings"));
    QVERIFY(reset != nullptr);

    m_system->hold.store(true);
    bool asked = false;
    answerNextQuestion(QMessageBox::Yes, &asked);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTRY_COMPARE_WITH_TIMEOUT(m_system->elevatedTasks().size(), 1, kRunTimeoutMs);
    QVERIFY(sak::RunningOperations::runningNames().contains(QStringLiteral("Network reset")));

    sak::RunningOperations::cancelAllRunning();
    QTRY_COMPARE_WITH_TIMEOUT(m_system->helper_cancels.load(), 1, kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(status, QStringLiteral("Reset failed: ")),
                             kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), kRunTimeoutMs);
    QVERIFY(!sak::RunningOperations::runningNames().contains(QStringLiteral("Network reset")));
}

// With one controller behind all three panels, a run of one panel must leave the others alone:
// no status, no log line, no button change.
void QuickActionPanelTests::eachPanelReactsOnlyToItsOwnRuns() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, kAllPanelActions));
    sak::UserMigrationPanel backup(controller);
    sak::DiagnosticBenchmarkPanel diagnostics(controller);
    sak::NetworkDiagnosticPanel network(controller);
    QSignalSpy backup_status(&backup, &sak::UserMigrationPanel::statusMessage);
    QSignalSpy backup_log(&backup, &sak::UserMigrationPanel::logOutput);
    QSignalSpy diagnostics_status(&diagnostics, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QSignalSpy network_status(&network, &sak::NetworkDiagnosticPanel::statusMessage);
    QPushButton* power = buttonNamed(diagnostics, QStringLiteral("Optimize Power"));
    QPushButton* reset = buttonNamed(network, QStringLiteral("Reset network settings"));
    QVERIFY(power != nullptr && reset != nullptr);

    bool asked = false;
    answerNextQuestion(QMessageBox::Yes, &asked);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTRY_VERIFY_WITH_TIMEOUT(texts(network_status).contains(QStringLiteral("fake elevated ok")),
                             kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(reset->isEnabled(), kRunTimeoutMs);

    power->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(diagnostics_status,
                                           QStringLiteral("Completed: ") + kOptimizePower),
                             kRunTimeoutMs);
    QTest::qWait(50);

    QVERIFY2(backup_status.isEmpty() && backup_log.isEmpty(),
             qPrintable(texts(backup_status).join(QLatin1Char('\n'))));
    for (const QString& text : texts(diagnostics_status)) {
        QVERIFY2(!text.contains(kResetNetwork) && !text.contains(QStringLiteral("fake helper")),
                 qPrintable(text));
    }
    for (const QString& text : texts(network_status)) {
        QVERIFY2(!text.contains(kOptimizePower) && !text.contains(QStringLiteral("fake step")),
                 qPrintable(text));
    }
    QCOMPARE(reset->text(), QStringLiteral("Reset Network"));
}

// A click while another caller's action runs is queued behind it, and the panel says so.
void QuickActionPanelTests::aClickWhileAnotherCallersRunIsGoingSaysQueued() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, kAllPanelActions));
    sak::DiagnosticBenchmarkPanel diagnostics(controller);
    sak::NetworkDiagnosticPanel network(controller);
    QSignalSpy diagnostics_status(&diagnostics, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QPushButton* power = buttonNamed(diagnostics, QStringLiteral("Optimize Power"));
    QPushButton* reset = buttonNamed(network, QStringLiteral("Reset network settings"));
    QVERIFY(power != nullptr && reset != nullptr);

    m_system->hold.store(true);
    bool asked = false;
    answerNextQuestion(QMessageBox::Yes, &asked);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTRY_COMPARE_WITH_TIMEOUT(m_system->elevatedTasks().size(), 1, kRunTimeoutMs);

    power->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QTRY_VERIFY_WITH_TIMEOUT(texts(diagnostics_status)
                                 .contains(QStringLiteral("Queued: ") + kOptimizePower +
                                           QStringLiteral(" - waits for ") + kResetNetwork +
                                           QStringLiteral(" to finish")),
                             kRunTimeoutMs);
    QVERIFY(m_system->inProcessRuns().isEmpty());

    m_system->hold.store(false);
    QTRY_VERIFY_WITH_TIMEOUT(anyStartsWith(diagnostics_status,
                                           QStringLiteral("Completed: ") + kOptimizePower),
                             kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(power->isEnabled() && reset->isEnabled(), kRunTimeoutMs);
}

// The cross-caller folder bug, through the panel: the Diagnostics panel's inventory report is
// queued behind another run, then the assistant asks for the same action with its own staging
// folder. Each run writes into its own caller's folder.
void QuickActionPanelTests::aQueuedPanelRunKeepsTheFolderItsUserChose() {
    QTemporaryDir chosen;
    QTemporaryDir assistant_staging;
    QVERIFY(chosen.isValid() && assistant_staging.isValid());
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, kAllPanelActions));
    sak::DiagnosticBenchmarkPanel diagnostics(controller);
    sak::NetworkDiagnosticPanel network(controller);
    QSignalSpy diagnostics_status(&diagnostics, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QPushButton* report = buttonNamed(diagnostics, kInventoryReport);
    QPushButton* reset = buttonNamed(network, QStringLiteral("Reset network settings"));
    QVERIFY(report != nullptr && reset != nullptr);

    m_system->hold.store(true);
    bool confirmed = false;
    answerNextQuestion(QMessageBox::Yes, &confirmed);
    reset->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(confirmed);
    QTRY_COMPARE_WITH_TIMEOUT(m_system->elevatedTasks().size(), 1, kRunTimeoutMs);

    bool asked = false;
    chooseFolderInNextDialog(chosen.path(), &asked, nullptr);
    report->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    controller.requestRun(
        {kInventoryReport, QStringLiteral("assistant#1"), assistant_staging.path()});

    m_system->hold.store(false);
    const QString saved =
        QDir::toNativeSeparators(QDir(chosen.path()).filePath(kInventoryReport + ".txt"));
    QTRY_VERIFY_WITH_TIMEOUT(texts(diagnostics_status)
                                 .contains(QStringLiteral("Completed: ") + kInventoryReport +
                                           QStringLiteral(" - saved to ") + saved),
                             kRunTimeoutMs);
    QTRY_COMPARE_WITH_TIMEOUT(m_system->inProcessRuns().size(), 2, kRunTimeoutMs);
    QVERIFY(QFile::exists(saved));
    QVERIFY(QFile::exists(QDir(assistant_staging.path()).filePath(kInventoryReport + ".txt")));
}

QTEST_MAIN(QuickActionPanelTests)
#include "test_quick_action_panels.moc"
