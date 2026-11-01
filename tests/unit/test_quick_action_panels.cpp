// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_quick_action_panels.cpp
/// @brief Drives each panel's quick-action flow -- click, confirmation, folder choice, run,
///        progress and completion -- through a real QuickActionController, with fake actions and
///        a fake elevated helper (quick_action_fakes.h) standing in for everything that would
///        change this machine: no Settings window, no SFC, no network reset.

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

QPushButton* buttonNamed(const QWidget& panel, const QString& accessible_name) {
    for (auto* button : panel.findChildren<QPushButton*>()) {
        if (button->accessibleName() == accessible_name) {
            return button;
        }
    }
    return nullptr;
}

/// Runs @p handle on the modal dialog the next click opens, inside that dialog's own loop.
void handleNextDialog(const std::function<void(QWidget*)>& handle) {
    QTimer::singleShot(0, qApp, [handle]() { handle(QApplication::activeModalWidget()); });
}

void answerNextQuestion(QMessageBox::StandardButton answer, bool* asked) {
    handleNextDialog([answer, asked](QWidget* modal) {
        auto* box = qobject_cast<QMessageBox*>(modal);
        if (box == nullptr || box->button(answer) == nullptr) {
            return;  // the test sees *asked still false
        }
        *asked = true;
        box->button(answer)->click();
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

    void userMigrationScreenshotRunsAndReportsCompletion();
    void userMigrationBitLockerAsksForAFolderAndHandsItToTheHelper();
    void userMigrationBitLockerWithNoFolderRunsNothing();
    void diagnosticsRunsEachMaintenanceActionThroughTheController();
    void diagnosticsVerifySystemFilesDeclinedRunsNothing();
    void diagnosticsInventoryReportSavesWhereTheUserChose();
    void networkResetConfirmsThenRunsElevatedAndRestoresTheButton();
    void networkResetDeclinedRunsNothing();
    void exitCancelsARunningElevatedQuickAction();

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

void QuickActionPanelTests::userMigrationScreenshotRunsAndReportsCompletion() {
    sak::QuickActionController controller;
    QVERIFY(registerFakes(controller, m_system, {kScreenshotSettings, kBitLocker}));
    sak::UserMigrationPanel panel(controller);
    QSignalSpy status(&panel, &sak::UserMigrationPanel::statusMessage);
    QPushButton* button = buttonNamed(panel, QStringLiteral("Screenshot Settings"));
    QVERIFY(button != nullptr);

    button->click();
    QTRY_VERIFY_WITH_TIMEOUT(
        anyStartsWith(status, QStringLiteral("Completed: ") + kScreenshotSettings), kRunTimeoutMs);
    QCOMPARE(m_system->inProcessRuns().size(), 1);
    QVERIFY(m_system->elevatedTasks().isEmpty());
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
    QVERIFY2(asked, "Verify System Files repairs Windows files and must be confirmed");
    QTRY_VERIFY_WITH_TIMEOUT(
        anyStartsWith(status, QStringLiteral("Completed: ") + kVerifySystemFiles), kRunTimeoutMs);
    QTRY_VERIFY_WITH_TIMEOUT(verify->isEnabled(), kRunTimeoutMs);

    // Check Disk Errors: elevated, no confirmation.
    QPushButton* check_disk = buttonNamed(panel, kCheckDisk);
    QVERIFY(check_disk != nullptr);
    check_disk->click();
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
    QVERIFY2(asked, "a network reset must be confirmed");
    QVERIFY(!reset->isEnabled());
    QCOMPARE(reset->text(), QStringLiteral("Resetting..."));
    QTRY_COMPARE_WITH_TIMEOUT(
        m_system->elevatedTasks(),
        QStringList{QString::fromLatin1(sak::quick_action_elevation::kResetNetworkTaskId)},
        kRunTimeoutMs);

    m_system->hold.store(false);
    QTRY_VERIFY_WITH_TIMEOUT(texts(status).contains(
                                 QStringLiteral("Network settings reset successfully")),
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

QTEST_MAIN(QuickActionPanelTests)
#include "test_quick_action_panels.moc"
