// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_diagnostic_panel.cpp
/// @brief The Network panel's start-up adapter scan runs, off-thread, in every run mode.

#include "sak/accessible_name_rule.h"
#include "sak/network_diagnostic_controller.h"
#include "sak/network_diagnostic_panel.h"
#include "sak/quick_action_controller.h"

#include <QAbstractButton>
#include <QApplication>
#include <QDialog>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QTableWidget>
#include <QTabWidget>
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
// A refused loopback connection or a missing tool ends at once; this bounds a slow test machine.
constexpr int kIperfRefusedTimeoutMs = 30'000;

QPushButton* reportButton(const QWidget& panel) {
    const auto buttons = panel.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Generate Report...")) {
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

QPushButton* buttonLabelled(const QWidget& panel, const QString& text) {
    const auto buttons = panel.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

/// Every label on the panel whose text holds @p fragment.
QStringList labelsHolding(const QWidget& panel, const QString& fragment) {
    QStringList texts;
    const auto labels = panel.findChildren<QLabel*>();
    for (const QLabel* label : labels) {
        if (label->text().contains(fragment)) {
            texts << label->text();
        }
    }
    return texts;
}

/// The order the controller reports a finished test in: the result, then the operation's end.
void finishSpeedTest(sak::NetworkDiagnosticController& controller,
                     double down,
                     double up,
                     double latency) {
    Q_EMIT controller.httpSpeedComplete(down, up, latency);
    Q_EMIT controller.operationFinished(
        static_cast<int>(sak::NetworkDiagnosticController::State::RunningBandwidthTest));
}

void finishIperfTest(sak::NetworkDiagnosticController& controller,
                     const sak::BandwidthTestResult& result) {
    Q_EMIT controller.bandwidthComplete(result);
    Q_EMIT controller.operationFinished(
        static_cast<int>(sak::NetworkDiagnosticController::State::RunningBandwidthTest));
}

/// Types into iPerf3's server box, the first box the panel names that way.
bool setIperfServer(const QWidget& panel, const QString& address) {
    const auto edits = panel.findChildren<QLineEdit*>();
    for (QLineEdit* edit : edits) {
        if (edit->accessibleName() == QStringLiteral("iPerf3 server address")) {
            edit->setText(address);
            return true;
        }
    }
    return false;
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
    void speedTestResultStaysOnScreenAfterTheRunEnds();
    void speedTestSaysSoWhenItCouldNotMeasureLatency();
    void stoppedSpeedTestShowsNoResultRatherThanZeros();
    void iperfResultStaysOnScreenAfterTheRunEnds();
    void stoppedIperfTestShowsNoResultRatherThanZeros();
    void onlyOneBandwidthTestCanBeStartedAtATime();
    void stoppingKeepsRunButtonsOffUntilTheWorkerHasStopped();
    void runButtonsComeBackWhenTheOperationEndsWithoutAResult();
    void refreshKeyWorksOnTheAdaptersTab();
    void diagnoseOpensSettingsWhereWindowsHasNoTroubleshooter();
    void flushCacheDoesNotClaimItNeedsAnAdministrator();
    void backupAndRestoreDoNotPromiseEthernetOnly();
    void accessibleNameHoldsLabel_data();
    void accessibleNameHoldsLabel();
    void everyButtonsNameHoldsItsLabel();

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

// Seen on a VM: the log held DL 171.31 Mbps, UL 26.98 Mbps, latency 227.0 ms and the screen was
// blank, because the operation's end cleared the label the result had just been written to.
void NetworkDiagnosticPanelTests::speedTestResultStaysOnScreenAfterTheRunEnds() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);

    finishSpeedTest(*controller, 171.31, 26.98, 227.0);

    const QStringList shown = labelsHolding(panel, QStringLiteral("171.31"));
    QCOMPARE(shown.size(), 1);
    QVERIFY(shown.first().contains(QStringLiteral("26.98")));
    QVERIFY(shown.first().contains(QStringLiteral("227.0 ms")));
}

void NetworkDiagnosticPanelTests::speedTestSaysSoWhenItCouldNotMeasureLatency() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);

    finishSpeedTest(*controller, 171.31, 26.98, -1.0);

    const QStringList shown = labelsHolding(panel, QStringLiteral("171.31"));
    QCOMPARE(shown.size(), 1);
    QVERIFY(shown.first().contains(QStringLiteral("unknown")));
    QVERIFY2(!shown.first().contains(QStringLiteral("-1")), qPrintable(shown.first()));
}

void NetworkDiagnosticPanelTests::stoppedSpeedTestShowsNoResultRatherThanZeros() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);

    finishSpeedTest(*controller, 0.0, 0.0, 0.0);

    QCOMPARE(labelsHolding(panel, QStringLiteral("No result")).size(), 1);
    QVERIFY(labelsHolding(panel, QStringLiteral("0.00 Mbps")).isEmpty());
}

void NetworkDiagnosticPanelTests::iperfResultStaysOnScreenAfterTheRunEnds() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);

    sak::BandwidthTestResult result;
    result.target = QStringLiteral("192.0.2.10");
    result.downloadMbps = 940.25;
    result.uploadMbps = 912.5;
    finishIperfTest(*controller, result);

    const QStringList shown = labelsHolding(panel, QStringLiteral("940.25"));
    QCOMPARE(shown.size(), 1);
    QVERIFY(shown.first().contains(QStringLiteral("912.50")));
}

void NetworkDiagnosticPanelTests::stoppedIperfTestShowsNoResultRatherThanZeros() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);

    finishIperfTest(*controller, sak::BandwidthTestResult{});

    QCOMPARE(labelsHolding(panel, QStringLiteral("No result")).size(), 1);
    QVERIFY(labelsHolding(panel, QStringLiteral("0.00 Mbps")).isEmpty());
}

// Both tests share one worker and the controller refuses a second, so offering the other one left
// its label saying "Running..." for a run that never started. Loopback, where nothing listens:
// iPerf3 is refused at once (or is not bundled beside the test), with no traffic off this PC.
void NetworkDiagnosticPanelTests::onlyOneBandwidthTestCanBeStartedAtATime() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    QPushButton* speed = buttonLabelled(panel, QStringLiteral("Run Speed Test"));
    QPushButton* iperf = buttonLabelled(panel, QStringLiteral("Run Test"));
    QVERIFY(speed != nullptr);
    QVERIFY(iperf != nullptr);
    QVERIFY(setIperfServer(panel, QStringLiteral("127.0.0.1")));

    iperf->click();
    QVERIFY(!speed->isEnabled());
    QVERIFY(!iperf->isEnabled());

    QTRY_VERIFY_WITH_TIMEOUT(speed->isEnabled() && iperf->isEnabled(), kIperfRefusedTimeoutMs);
    QCOMPARE(labelsHolding(panel, QStringLiteral("No result")).size(), 1);
    QVERIFY(labelsHolding(panel, QStringLiteral("Running")).isEmpty());
}

// Stop used to hand the run buttons back while the worker was still unwinding, and starting again
// in that window was refused with its label left saying "Running...".
void NetworkDiagnosticPanelTests::stoppingKeepsRunButtonsOffUntilTheWorkerHasStopped() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    QPushButton* speed = buttonLabelled(panel, QStringLiteral("Run Speed Test"));
    QPushButton* iperf = buttonLabelled(panel, QStringLiteral("Run Test"));
    QVERIFY(speed != nullptr);
    QVERIFY(iperf != nullptr);
    QVERIFY(setIperfServer(panel, QStringLiteral("127.0.0.1")));
    QList<QPushButton*> stops;
    const auto buttons = panel.findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Stop") && button->isEnabled()) {
            stops << button;
        }
    }

    iperf->click();
    QPushButton* stop = nullptr;
    for (QPushButton* button : buttons) {
        if (button->text() == QStringLiteral("Stop") && button->isEnabled() &&
            !stops.contains(button)) {
            stop = button;
        }
    }
    QVERIFY2(stop != nullptr, "the run should have enabled its own Stop button");

    stop->click();
    QVERIFY(!stop->isEnabled());
    QVERIFY2(!speed->isEnabled() && !iperf->isEnabled(),
             "the worker is still stopping, so no run can start yet");

    QTRY_VERIFY_WITH_TIMEOUT(speed->isEnabled() && iperf->isEnabled(), kIperfRefusedTimeoutMs);
    QVERIFY(!stop->isEnabled());
}

// A worker that ends without reporting anything must not strand the buttons: the operation's end
// is what hands them back, whatever came before it.
void NetworkDiagnosticPanelTests::runButtonsComeBackWhenTheOperationEndsWithoutAResult() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    QPushButton* speed = buttonLabelled(panel, QStringLiteral("Run Speed Test"));
    QPushButton* iperf = buttonLabelled(panel, QStringLiteral("Run Test"));
    QVERIFY(speed != nullptr);
    QVERIFY(iperf != nullptr);
    QVERIFY(setIperfServer(panel, QStringLiteral("127.0.0.1")));
    iperf->click();
    QVERIFY(!speed->isEnabled() && !iperf->isEnabled());

    Q_EMIT controller->operationFinished(
        static_cast<int>(sak::NetworkDiagnosticController::State::RunningBandwidthTest));
    QVERIFY(speed->isEnabled() && iperf->isEnabled());

    QTRY_VERIFY_WITH_TIMEOUT(!controller->isOperationActive(
                                 sak::NetworkDiagnosticController::State::RunningBandwidthTest),
                             kIperfRefusedTimeoutMs);
}

// MainWindow moves the adapter section out into a tab of its own, which took it out from under the
// panel's shortcuts. Placed the same way here, with a real key press.
void NetworkDiagnosticPanelTests::refreshKeyWorksOnTheAdaptersTab() {
    sak::QuickActionController quick_actions;
    QTabWidget tabs;
    auto* panel = new sak::NetworkDiagnosticPanel(quick_actions);
    tabs.addTab(panel, QStringLiteral("Network Diagnostics"));
    tabs.addTab(panel->adapterWidget(), QStringLiteral("Network Adapters"));
    auto* controller = panel->findChild<sak::NetworkDiagnosticController*>();
    QVERIFY(controller != nullptr);
    QVERIFY(waitForStartupScan(*panel));

    tabs.setCurrentWidget(panel->adapterWidget());
    tabs.show();
    QVERIFY(QTest::qWaitForWindowActive(&tabs));
    auto* table = panel->adapterWidget()->findChild<QTableWidget*>();
    QVERIFY(table != nullptr);
    table->setFocus();
    QTRY_VERIFY(table->hasFocus());

    QSignalSpy scans(controller, &sak::NetworkDiagnosticController::adaptersScanComplete);
    QTest::keyClick(table, Qt::Key_F5);
    QTRY_COMPARE_WITH_TIMEOUT(scans.count(), 1, kAdapterScanTimeoutMs);
}

void NetworkDiagnosticPanelTests::diagnoseOpensSettingsWhereWindowsHasNoTroubleshooter() {
    const sak::AdapterDiagnosePlan gone = sak::adapterDiagnosePlan(QStringLiteral("Ethernet"),
                                                                   false);
    QVERIFY(gone.use_settings);
    QCOMPARE(gone.settings_page, QStringLiteral("ms-settings:troubleshoot"));
    QVERIFY(gone.diagnostic_id.isEmpty());

    const auto idFor = [](const char* type) {
        const auto plan = sak::adapterDiagnosePlan(QString::fromLatin1(type), true);
        return plan.use_settings ? QString() : plan.diagnostic_id;
    };
    QCOMPARE(idFor("Ethernet"), QStringLiteral("NetworkDiagnosticsNetworkAdapter"));
    QCOMPARE(idFor("WiFi"), QStringLiteral("NetworkDiagnosticsNetworkAdapter"));
    QCOMPARE(idFor("Bluetooth"), QStringLiteral("DeviceDiagnostic"));
    QCOMPARE(idFor("VPN"), QStringLiteral("NetworkDiagnosticsWeb"));
}

// DnsFlushResolverCache needs no elevation, and nothing in the path asks for it.
void NetworkDiagnosticPanelTests::flushCacheDoesNotClaimItNeedsAnAdministrator() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    QPushButton* flush = buttonLabelled(panel, QStringLiteral("Flush Cache"));
    QVERIFY(flush != nullptr);
    QVERIFY2(!flush->toolTip().contains(QStringLiteral("admin"), Qt::CaseInsensitive),
             qPrintable(flush->toolTip()));
}

// Capture and restore work on any adapter, and the list they offer holds Wi-Fi too.
void NetworkDiagnosticPanelTests::backupAndRestoreDoNotPromiseEthernetOnly() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    for (const QString& label :
         {QStringLiteral("Backup Settings"), QStringLiteral("Restore Settings")}) {
        QPushButton* button = nullptr;
        const auto buttons = panel.adapterWidget()->findChildren<QPushButton*>();
        for (QPushButton* candidate : buttons) {
            if (candidate->text() == label) {
                button = candidate;
            }
        }
        QVERIFY2(button != nullptr, qPrintable(label));
        for (const QString& text :
             {button->toolTip(), button->accessibleName(), button->accessibleDescription()}) {
            QVERIFY2(!text.contains(QStringLiteral("Ethernet"), Qt::CaseInsensitive),
                     qPrintable(text));
        }
    }
}

void NetworkDiagnosticPanelTests::accessibleNameHoldsLabel_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QString>("label");
    QTest::addColumn<bool>("holds");
    QTest::newRow("the same") << "Run Test" << "Run Test" << true;
    QTest::newRow("adds to it") << "Stop HTTP speed test" << "Stop" << true;
    QTest::newRow("ignores case") << "Start search" << "Search" << true;
    QTest::newRow("access key") << "Save" << "&Save" << true;
    QTest::newRow("trailing dots") << "Generate Report" << "Generate Report..." << true;
    QTest::newRow("leading symbol") << "Add Files" << "+ Add Files" << true;
    QTest::newRow("symbols only") << "Previous match" << QString::fromUtf8("\xE2\x97\x84") << true;
    QTest::newRow("no label") << "Settings" << "" << true;
    QTest::newRow("words changed") << "Add Scan Directory" << "Add Directory" << false;
    QTest::newRow("words moved") << "HTTP speed test" << "Run Speed Test" << false;
    QTest::newRow("no name") << "" << "Run Test" << false;
}

void NetworkDiagnosticPanelTests::accessibleNameHoldsLabel() {
    QFETCH(QString, name);
    QFETCH(QString, label);
    QFETCH(bool, holds);
    QCOMPARE(sak::accessibleNameHoldsLabel(name, label), holds);
}

// Found on a VM: buttons whose accessible names used other words than the ones on them. The
// start-up audit holds this for every panel built with the main window; these two sections are
// checked here as well because this is where most of them were.
void NetworkDiagnosticPanelTests::everyButtonsNameHoldsItsLabel() {
    sak::QuickActionController quick_actions;
    sak::NetworkDiagnosticPanel panel(quick_actions);
    auto buttons = panel.findChildren<QAbstractButton*>();
    buttons += panel.adapterWidget()->findChildren<QAbstractButton*>();
    QVERIFY(buttons.size() > 20);
    for (const QAbstractButton* button : std::as_const(buttons)) {
        if (button->accessibleName().isEmpty()) {
            continue;  // a Qt internal, or a helper the audit holds to its own rule
        }
        QVERIFY2(sak::accessibleNameHoldsLabel(button->accessibleName(), button->text()),
                 qPrintable(button->accessibleName() + QStringLiteral(" for ") + button->text()));
    }
}

QTEST_MAIN(NetworkDiagnosticPanelTests)
#include "test_network_diagnostic_panel.moc"
