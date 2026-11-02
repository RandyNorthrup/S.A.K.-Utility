// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_panel.cpp
/// @brief The Network panel's start-up adapter scan runs, off-thread, in every run mode.

#include "sak/network_diagnostic_controller.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/quick_action_controller.h"

#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextEdit>
#include <QTimer>
#include <QtTest/QtTest>

#include <memory>

namespace {
// One read-only adapter enumeration (GetAdaptersAddresses plus the per-adapter details).
constexpr int kAdapterScanTimeoutMs = 30'000;
constexpr int kDialogPollMs = 20;
constexpr int kDialogTimeoutMs = 30'000;
constexpr int kReportTimeoutMs = 30'000;

QPushButton* reportButton(const QWidget& panel) {
    const auto buttons = panel.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->accessibleName() == QStringLiteral("Generate network report")) {
            return button;
        }
    }
    return nullptr;
}

/// Answers the two dialogs Generate Report opens, in order. Polls, because each one runs its own
/// nested event loop inside the click that opened it.
class ReportDialogDriver : public QObject {
public:
    explicit ReportDialogDriver(QString report_path) : m_report_path(std::move(report_path)) {
        m_timer.setInterval(kDialogPollMs);
        connect(&m_timer, &QTimer::timeout, this, [this]() { answerOpenDialog(); });
        m_timer.start();
        QTimer::singleShot(kDialogTimeoutMs, &m_timer, &QTimer::stop);
    }
    bool details_answered{false};
    bool file_answered{false};
    bool saw_message_box{false};

private:
    void answerOpenDialog() {
        QWidget* modal = QApplication::activeModalWidget();
        if (auto* file_dialog = qobject_cast<QFileDialog*>(modal)) {
            // As a technician does: the folder, then the name typed over the suggested one.
            // selectFile() leaves the suggested name in place on this dialog.
            const QFileInfo target(m_report_path);
            file_dialog->setDirectory(target.absolutePath());
            auto* name_edit = file_dialog->findChild<QLineEdit*>(QStringLiteral("fileNameEdit"));
            if (name_edit == nullptr) {
                return;
            }
            name_edit->setText(target.fileName());
            file_answered = true;
            // QFileDialog::accept is protected; through QDialog it still dispatches to it.
            static_cast<QDialog*>(file_dialog)->accept();
            return;
        }
        if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            saw_message_box = true;
            box->accept();
            return;
        }
        auto* dialog = qobject_cast<QDialog*>(modal);
        if (dialog == nullptr || details_answered) {
            return;
        }
        fillDetails(*dialog);
        details_answered = true;
        dialog->accept();
    }

    static void fillDetails(QDialog& dialog) {
        const auto edits = dialog.findChildren<QLineEdit*>();
        for (QLineEdit* edit : edits) {
            if (edit->accessibleName() == QStringLiteral("Technician Name")) {
                edit->setText(QStringLiteral("Pat Tech"));
            } else if (edit->accessibleName() == QStringLiteral("Ticket Number")) {
                edit->setText(QStringLiteral("TKT-4242"));
            }
        }
        auto* notes = dialog.findChild<QTextEdit*>();
        if (notes != nullptr) {
            notes->setPlainText(QStringLiteral("Note for the customer"));
        }
    }

    QString m_report_path;
    QTimer m_timer;
};

/// The panel queues an adapter scan as it is built, so "nothing is running" is not yet true until
/// that scan has come back.
bool waitForStartupScan(sak::NetworkDiagnosticPanel& panel) {
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    if (controller == nullptr) {
        return false;
    }
    QSignalSpy scans(controller, &sak::NetworkDiagnosticController::adaptersScanComplete);
    if (!scans.wait(kAdapterScanTimeoutMs)) {
        return false;
    }
    QElapsedTimer waited;
    waited.start();
    while (!controller->canGenerateReport() && waited.elapsed() < kAdapterScanTimeoutMs) {
        QTest::qWait(kDialogPollMs);
    }
    return controller->canGenerateReport();
}

QByteArray readAll(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
}  // namespace

class NetworkDiagnosticPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void cleanup();
    void adapterScanRunsInEveryMode_data();
    void adapterScanRunsInEveryMode();
    void teardownRightAfterConstructionJoinsTheScan();
    void generateReportWritesAnHtmlReportWithTheDetailsAsked();
    void generateReportWritesJsonWhenThePathSaysJson();
    void generateReportAddsTheExtensionItWasNotGiven();
    void generateReportWaitsForARunningDiagnostic();

private:
    static void runReportFlow(const QString& report_path, QByteArray* written);
};

void NetworkDiagnosticPanelTests::cleanup() {
    qApp->setProperty("sakStartupSmokeTest", QVariant());
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void NetworkDiagnosticPanelTests::adapterScanRunsInEveryMode_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("interactive") << QByteArray();
    QTest::newRow("startup smoke") << QByteArray("sakStartupSmokeTest");
    QTest::newRow("accessibility audit") << QByteArray("sakAccessibilityAudit");
}

// Smoke and audit runs used to skip the adapter scan users get when the panel is created, so the
// gates that exist to exercise start-up work never ran it. It now runs in every mode, on the
// controller's worker thread.
void NetworkDiagnosticPanelTests::adapterScanRunsInEveryMode() {
    QFETCH(QByteArray, mode);
    if (!mode.isEmpty()) {
        qApp->setProperty(mode.constData(), true);
    }
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    int scans = 0;
    QObject::connect(controller,
                     &sak::NetworkDiagnosticController::adaptersScanComplete,
                     &panel,
                     [&scans](const QVector<sak::NetworkAdapterInfo>&) { ++scans; });
    QTRY_VERIFY_WITH_TIMEOUT(scans >= 1, kAdapterScanTimeoutMs);
}

// What the old skip guarded against: a gate tears the panel down moments after building it,
// with the scan still running. The controller's destructor joins its worker, so this returns
// cleanly instead of racing a scan into freed state.
void NetworkDiagnosticPanelTests::teardownRightAfterConstructionJoinsTheScan() {
    qApp->setProperty("sakStartupSmokeTest", true);
    sak::QuickActionController quick_actions;
    for (int round = 0; round < 3; ++round) {
        auto panel = std::make_unique<sak::NetworkDiagnosticPanel>(quick_actions);
        QCoreApplication::processEvents();  // dispatch the queued scan
        panel.reset();
    }
}

// Drives the button the technician clicks, through both dialogs, to the file on disk. The
// generator and its controller had tests and no route: nothing could reach them.
void NetworkDiagnosticPanelTests::runReportFlow(const QString& report_path, QByteArray* written) {
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    QVERIFY(waitForStartupScan(panel));
    QPushButton* button = reportButton(panel);
    QVERIFY(button != nullptr);

    QSignalSpy generated(controller, &sak::NetworkDiagnosticController::reportGenerated);
    ReportDialogDriver driver(report_path);
    button->click();
    QVERIFY(driver.details_answered);
    QVERIFY(driver.file_answered);
    // The finished report also says where it went, in a box of its own that the driver answers.
    QTRY_VERIFY_WITH_TIMEOUT(generated.count() == 1, kReportTimeoutMs);
    QVERIFY(driver.saw_message_box);
    *written = readAll(generated.first().first().toString());
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, false);
}

void NetworkDiagnosticPanelTests::generateReportWritesAnHtmlReportWithTheDetailsAsked() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray written;
    runReportFlow(dir.filePath(QStringLiteral("net.html")), &written);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(written.contains("<!DOCTYPE html>"));
    QVERIFY(written.contains("Pat Tech"));
    QVERIFY(written.contains("TKT-4242"));
    QVERIFY(written.contains("Note for the customer"));
}

void NetworkDiagnosticPanelTests::generateReportWritesJsonWhenThePathSaysJson() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray written;
    runReportFlow(dir.filePath(QStringLiteral("net.json")), &written);
    if (QTest::currentTestFailed()) {
        return;
    }
    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(written, &error);
    QVERIFY2(error.error == QJsonParseError::NoError, written.left(200).constData());
    QVERIFY(document.isObject());
    QVERIFY(written.contains("Pat Tech"));
    QVERIFY(written.contains("TKT-4242"));
}

void NetworkDiagnosticPanelTests::generateReportAddsTheExtensionItWasNotGiven() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    QByteArray written;
    runReportFlow(dir.filePath(QStringLiteral("net")), &written);
    if (QTest::currentTestFailed()) {
        return;
    }
    QVERIFY(written.contains("<!DOCTYPE html>"));
    QVERIFY(QFile::exists(dir.filePath(QStringLiteral("net.html"))));
}

// The first thing the panel does is scan adapters. A click during that says so, and does not ask
// for details the controller would then refuse.
void NetworkDiagnosticPanelTests::generateReportWaitsForARunningDiagnostic() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    QVERIFY(waitForStartupScan(panel));
    controller->scanAdapters();  // marks the operation active before it returns
    QVERIFY(!controller->canGenerateReport());
    QPushButton* button = reportButton(panel);
    QVERIFY(button != nullptr);
    ReportDialogDriver driver(QStringLiteral("unused"));
    button->click();
    QVERIFY(driver.saw_message_box);
    QVERIFY(!driver.details_answered);
}

QTEST_MAIN(NetworkDiagnosticPanelTests)
#include "test_network_diagnostic_panel.moc"
