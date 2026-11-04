// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_diagnostic_benchmark_panel.cpp
/// @brief The thermal readout polls only while the Diagnostics panel is shown, and the System
///        Inventory Report asks where to save and says where it went.

#include "sak/actions/generate_system_report_action.h"
#include "sak/diagnostic_benchmark_panel.h"
#include "sak/diagnostic_controller.h"
#include "sak/process_runner.h"
#include "sak/quick_action_controller.h"
#include "sak/thermal_monitor.h"

#include "../support/test_hardware_guard.h"

#include <QApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

namespace {
// One sensor query is in flight when the panel hides; stopping waits for it to land.
constexpr int kPollSettleTimeoutMs = 30'000;
// The real collectors (Get-ComputerInfo, storage, network) are each bounded by their own timeout;
// this is well past their sum under a loaded test run.
constexpr int kInventoryReportTimeoutMs = 300'000;
const QString kInventoryReport = QStringLiteral("System Inventory Report");
// What the sensor script prints on a PC with every sensor readable.
const QString kFakeSensorOutput = QStringLiteral("cpu=52.3\ngpu=61\ndisk0=38");

QPushButton* inventoryReportButton(const sak::DiagnosticBenchmarkPanel& panel) {
    for (auto* button : panel.findChildren<QPushButton*>()) {
        if (button->accessibleName() == kInventoryReport) {
            return button;
        }
    }
    return nullptr;
}

/// Runs @p inspect on the folder dialog the next click opens, inside that dialog's own loop.
/// A press queues what it does. A zero timer is a queued call too and would run before the dialog
/// exists, so this one waits for an event loop, and the first to run is the dialog's.
void inspectNextFolderDialog(const std::function<void(QFileDialog*)>& inspect) {
    QTimer::singleShot(1, qApp, [inspect]() {
        inspect(qobject_cast<QFileDialog*>(QApplication::activeModalWidget()));
    });
}

/// Registers the real System Inventory Report, the one quick action this suite runs for real.
void registerInventoryReport(sak::QuickActionController& quick_actions) {
    QVERIFY(
        !quick_actions.registerAction(std::make_unique<sak::GenerateSystemReportAction>(QString()))
             .isEmpty());
}

QStringList statusTexts(const QSignalSpy& spy) {
    QStringList texts;
    for (const auto& arguments : spy) {
        texts << arguments.at(0).toString();
    }
    return texts;
}
}  // namespace

class DiagnosticBenchmarkPanelTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void thermalPollsOnlyWhileShown_data();
    void thermalPollsOnlyWhileShown();
    void inventoryReportAsksWhereToSaveAndSaysWhereItWent();
    void inventoryReportRunsItsRealCollectors();
    void inventoryReportCancelledFolderRunsNothing();
    void stressStatusSaysWhenThereIsNoSensor();
    void thermalLimitTooltipSaysWhatHappens();
    void anUnreadableDriveShowsNoFakeNumbers();
    void thermalDiskRowSaysItIsTheHottestDisk();
    void exportScanReportWritesTheHtmlItSays();

private:
    void runInventoryReportAndCheckItsFile();

    bool m_nativeDialogsWereDisabled{false};
    // Every process this panel starts is answered here. The real sensor script reads every disk's
    // reliability counters and the real report queries every disk, so a run of this suite on a
    // person's PC would otherwise be asking their USB drives for those.
    std::atomic<int> m_processes_answered{0};
    std::unique_ptr<sak::ScopedProcessFaultInjector> m_fake_processes;
};

void DiagnosticBenchmarkPanelTests::initTestCase() {
    // The Qt dialog, not the Windows one, so the test can drive it.
    m_nativeDialogsWereDisabled = QApplication::testAttribute(Qt::AA_DontUseNativeDialogs);
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, true);
    m_fake_processes = std::make_unique<sak::ScopedProcessFaultInjector>(
        [this](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            m_processes_answered.fetch_add(1);
            sak::ProcessResult result;
            result.exit_code = 0;
            result.std_out = kFakeSensorOutput;
            return result;
        });
}

void DiagnosticBenchmarkPanelTests::cleanupTestCase() {
    m_fake_processes.reset();
    QApplication::setAttribute(Qt::AA_DontUseNativeDialogs, m_nativeDialogsWereDisabled);
}

void DiagnosticBenchmarkPanelTests::cleanup() {
    qApp->setProperty("sakStartupSmokeTest", QVariant());
    qApp->setProperty("sakAccessibilityAudit", QVariant());
}

void DiagnosticBenchmarkPanelTests::thermalPollsOnlyWhileShown_data() {
    QTest::addColumn<QByteArray>("mode");
    QTest::newRow("interactive") << QByteArray();
    QTest::newRow("startup smoke") << QByteArray("sakStartupSmokeTest");
    QTest::newRow("accessibility audit") << QByteArray("sakAccessibilityAudit");
}

// The panel used to start thermal polling in its constructor and keep spawning a PowerShell
// sensor query every 2 s until exit, whether or not the Diagnostics tab was ever looked at --
// except in smoke and audit runs, which skipped it and so never exercised what users got. It
// now polls while shown, stops when hidden, and behaves the same in every run mode.
void DiagnosticBenchmarkPanelTests::thermalPollsOnlyWhileShown() {
    QFETCH(QByteArray, mode);
    if (!mode.isEmpty()) {
        qApp->setProperty(mode.constData(), true);
    }
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* monitor = panel.findChild<sak::ThermalMonitor*>();
    QVERIFY(monitor != nullptr);
    QVERIFY2(!monitor->isRunning(), "constructing the panel must not start sensor polling");

    const int answered_before = m_processes_answered.load();
    panel.show();
    QVERIFY2(monitor->isRunning(), "a shown readout polls");
    QTRY_VERIFY2(m_processes_answered.load() > answered_before, "a shown readout asks the sensors");

    panel.hide();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor->isRunning(), kPollSettleTimeoutMs);
    // ...and stays stopped: the in-flight poll landing must not re-arm the timer.
    QTest::qWait(1);
    QVERIFY(!monitor->isRunning());

    panel.show();
    QVERIFY2(monitor->isRunning(), "showing the readout again resumes polling");
    panel.hide();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor->isRunning(), kPollSettleTimeoutMs);
}

// The inventory quick action wrote to a fixed C:/SAK_Backups and reported only "Completed". It
// now asks for a folder (starting in Documents), writes there, and the completion names the file.
void DiagnosticBenchmarkPanelTests::inventoryReportAsksWhereToSaveAndSaysWhereItWent() {
    runInventoryReportAndCheckItsFile();
}

// The one run of the report's real collectors, so a PowerShell error in them cannot hide behind the
// fake. They query every disk, so it runs only in CI.
void DiagnosticBenchmarkPanelTests::inventoryReportRunsItsRealCollectors() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    const sak::ScopedProcessFaultInjector real_processes(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            return std::nullopt;
        });
    runInventoryReportAndCheckItsFile();
}

void DiagnosticBenchmarkPanelTests::runInventoryReportAndCheckItsFile() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    sak::QuickActionController quick_actions;
    registerInventoryReport(quick_actions);
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QPushButton* button = inventoryReportButton(panel);
    QVERIFY2(button != nullptr, "the System Inventory Report button must exist by that name");

    bool asked = false;
    QString started_in;
    inspectNextFolderDialog([&](QFileDialog* dialog) {
        if (dialog == nullptr) {
            return;  // no dialog: `asked` stays false and the test fails below
        }
        asked = true;
        started_in = dialog->directory().absolutePath();
        dialog->setDirectory(output.path());
        // QFileDialog::accept is protected; through QDialog it still dispatches to it.
        static_cast<QDialog*>(dialog)->accept();
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY2(asked, "the report must ask where to save before it runs");
    QCOMPARE(QDir::cleanPath(started_in),
             QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)));

    const QString finished_prefix_ok = QStringLiteral("Completed: ") + kInventoryReport;
    const QString finished_prefix_failed = QStringLiteral("Failed: ") + kInventoryReport;
    QString finished;
    const auto report_finished = [&]() {
        for (const QString& text : statusTexts(status)) {
            if (text.startsWith(finished_prefix_ok) || text.startsWith(finished_prefix_failed)) {
                finished = text;
                return true;
            }
        }
        return false;
    };
    QTRY_VERIFY_WITH_TIMEOUT(report_finished(), kInventoryReportTimeoutMs);

    const QStringList written =
        QDir(output.path()).entryList({QStringLiteral("*.txt")}, QDir::Files);
    QCOMPARE(written.size(), 1);
    QVERIFY(written.first().startsWith(QStringLiteral("SystemInventory_")));
    const QString saved = QDir::toNativeSeparators(QDir(output.path()).filePath(written.first()));
    QVERIFY2(finished.endsWith(QStringLiteral(" - saved to ") + saved), qPrintable(finished));
    QVERIFY(QFileInfo(saved).size() > 0);
}

void DiagnosticBenchmarkPanelTests::inventoryReportCancelledFolderRunsNothing() {
    sak::QuickActionController quick_actions;
    registerInventoryReport(quick_actions);
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QSignalSpy log(&panel, &sak::DiagnosticBenchmarkPanel::logOutput);
    QPushButton* button = inventoryReportButton(panel);
    QVERIFY(button != nullptr);

    bool asked = false;
    inspectNextFolderDialog([&](QFileDialog* dialog) {
        if (dialog != nullptr) {
            asked = true;
            dialog->reject();
        }
    });
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QVERIFY(asked);
    QTest::qWait(1);
    for (const QString& text : statusTexts(status)) {
        QVERIFY2(!text.startsWith(QStringLiteral("Running: ")), qPrintable(text));
    }
    QVERIFY(button->isEnabled());
    QVERIFY(statusTexts(log).contains(
        QStringLiteral("System Inventory Report cancelled: no folder was chosen")));
}

// The row shows the hottest disk in the PC, not disk 0.
void DiagnosticBenchmarkPanelTests::thermalDiskRowSaysItIsTheHottestDisk() {
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    const auto labels = panel.findChildren<QLabel*>();
    QVERIFY(std::ranges::any_of(labels, [](const QLabel* label) {
        return label->text() == QStringLiteral("Hottest disk:");
    }));
    QVERIFY(std::ranges::none_of(labels, [](const QLabel* label) {
        return label->text() == QStringLiteral("Disk 0:");
    }));
}

// Its tooltip says HTML. JSON and CSV have their own buttons beside it.
void DiagnosticBenchmarkPanelTests::exportScanReportWritesTheHtmlItSays() {
    QTemporaryDir output;
    QVERIFY(output.isValid());
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    // A result to report, without running a real scan.
    QVERIFY(QMetaObject::invokeMethod(controller,
                                      "onCpuBenchmarkComplete",
                                      Qt::DirectConnection,
                                      Q_ARG(sak::CpuBenchmarkResult, sak::CpuBenchmarkResult{})));

    QPushButton* button = nullptr;
    for (auto* candidate : panel.findChildren<QPushButton*>()) {
        if (candidate->accessibleName() == QStringLiteral("Export Scan Report")) {
            button = candidate;
        }
    }
    QVERIFY(button != nullptr);
    inspectNextFolderDialog([&](QFileDialog* dialog) {
        if (dialog != nullptr) {
            dialog->setDirectory(output.path());
            static_cast<QDialog*>(dialog)->accept();
        }
    });
    // The success box blocks until answered.
    QSignalSpy generated(controller, &sak::DiagnosticController::reportsGenerated);
    QTimer closer;
    QObject::connect(&closer, &QTimer::timeout, &panel, []() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            box->accept();
        }
    });
    closer.start(50);
    button->setEnabled(true);
    button->click();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    QTRY_COMPARE(generated.count(), 1);

    const QStringList written = QDir(output.path()).entryList(QDir::Files);
    QCOMPARE(written.size(), 1);
    QVERIFY2(written.first().endsWith(QStringLiteral(".html")), qPrintable(written.first()));
}

void DiagnosticBenchmarkPanelTests::stressStatusSaysWhenThereIsNoSensor() {
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    const auto temperatureText = [&panel]() {
        for (const auto* label : panel.findChildren<QLabel*>()) {
            if (label->text().startsWith(QStringLiteral("CPU Temp:"))) {
                return label->text();
            }
        }
        return QString();
    };

    Q_EMIT controller->stressTestStatus(5, -1.0, 0);
    QVERIFY2(temperatureText().contains(QStringLiteral("not being enforced")),
             qPrintable(temperatureText()));
    QVERIFY2(!temperatureText().contains(QStringLiteral("-1")), qPrintable(temperatureText()));

    Q_EMIT controller->stressTestStatus(6, 71.5, 0);
    QCOMPARE(temperatureText(), QStringLiteral("CPU Temp: 71.5 degC"));
}

void DiagnosticBenchmarkPanelTests::thermalLimitTooltipSaysWhatHappens() {
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    QSpinBox* limit = nullptr;
    for (auto* spin : panel.findChildren<QSpinBox*>()) {
        if (spin->accessibleName() == QStringLiteral("Thermal Limit")) {
            limit = spin;
        }
    }
    QVERIFY(limit != nullptr);
    QVERIFY2(limit->toolTip().contains(QStringLiteral("stops")), qPrintable(limit->toolTip()));
    QVERIFY2(!limit->toolTip().contains(QStringLiteral("paused")), qPrintable(limit->toolTip()));
}

void DiagnosticBenchmarkPanelTests::anUnreadableDriveShowsNoFakeNumbers() {
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    sak::SmartReport unreadable;
    unreadable.model = QStringLiteral("PhysicalDrive1");
    unreadable.overall_health = sak::SmartHealthStatus::Unknown;
    unreadable.warnings = {QStringLiteral("Could not read this drive")};
    Q_EMIT controller->smartAnalysisComplete({unreadable});

    auto* table = panel.findChild<QTableWidget*>();
    bool checked = false;
    for (auto* candidate : panel.findChildren<QTableWidget*>()) {
        if (candidate->accessibleName() == QStringLiteral("SMART Health Table")) {
            table = candidate;
            checked = true;
        }
    }
    QVERIFY(checked);
    QCOMPARE(table->rowCount(), 1);
    QCOMPARE(table->item(0, 2)->text(), QStringLiteral("N/A"));
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("--"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("--"));
}

QTEST_MAIN(DiagnosticBenchmarkPanelTests)
#include "test_diagnostic_benchmark_panel.moc"
