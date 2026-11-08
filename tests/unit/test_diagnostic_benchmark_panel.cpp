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
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QMap>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QSpinBox>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QThreadPool>
#include <QTimer>
#include <QtTest/QtTest>

#include <algorithm>
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

/// The first label in @p panel that reads @p text, or nullptr.
QLabel* labelReading(const QWidget& panel, const QString& text) {
    for (auto* label : panel.findChildren<QLabel*>()) {
        if (label->text() == text) {
            return label;
        }
    }
    return nullptr;
}

/// Shows the sub-tab called @p name wherever @p panel has one.
void showSubTab(const QWidget& panel, const QString& name) {
    for (auto* tabs : panel.findChildren<QTabWidget*>()) {
        for (int index = 0; index < tabs->count(); ++index) {
            if (tabs->tabText(index) == name) {
                tabs->setCurrentIndex(index);
            }
        }
    }
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
    // Review 2 on testpc1: the Benchmark Drive list named the 32 GB D: BENCH by its disk's size,
    // "238.47 GB".
    void aBenchmarkDriveIsNamedByItsVolume() {
        sak::StorageDeviceInfo disk;
        disk.model = QStringLiteral("Best Buy NS-PCNVMEHDE(-C)");
        disk.size_bytes = 256'060'514'304ULL;
        sak::PartitionInfo bench;
        bench.drive_letter = QStringLiteral("D:");
        bench.label = QStringLiteral("BENCH");
        bench.total_bytes = 34'359'738'368ULL;
        QCOMPARE(sak::DiagnosticBenchmarkPanel::benchmarkDriveText(disk, bench),
                 QStringLiteral("D: BENCH (32.00 GB volume on Best Buy NS-PCNVMEHDE(-C))"));
        bench.label.clear();
        bench.total_bytes = 0;
        QCOMPARE(sak::DiagnosticBenchmarkPanel::benchmarkDriveText(disk, bench),
                 QStringLiteral("D: (size unknown volume on Best Buy NS-PCNVMEHDE(-C))"));
    }

    // Review 2 (S17): the report's Notes box started 24 px left of the Technician field above it.
    void theReportFieldsShareALeftEdge() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        panel.resize(1280, 900);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QWidget* technician = nullptr;
        QWidget* notes = nullptr;
        for (auto* widget : panel.findChildren<QWidget*>()) {
            if (widget->accessibleName() == QStringLiteral("Technician Name")) {
                technician = widget;
            } else if (widget->accessibleName() == QStringLiteral("Diagnostic report notes")) {
                notes = widget;
            }
        }
        QVERIFY(technician != nullptr && notes != nullptr);
        // Bring the page holding them to the front, so they are laid out.
        for (QWidget* child = technician; child->parentWidget() != nullptr;
             child = child->parentWidget()) {
            if (auto* stack = qobject_cast<QStackedWidget*>(child->parentWidget())) {
                stack->setCurrentWidget(child);
            }
        }
        QApplication::processEvents();
        QVERIFY(technician->isVisible() && notes->isVisible());
        QCOMPARE(notes->mapTo(&panel, QPoint(0, 0)).x(),
                 technician->mapTo(&panel, QPoint(0, 0)).x());
    }

    // Review 2 (F40): Disk's "Sequential:" and Memory's "Bandwidth:" were bold and their other
    // measurements not. Review 3: CPU drew its two thread results as scores and had no "Score:"
    // line; now every group has its measurements in one style and one Score line in another.
    void benchmarkGroupsShareOneLayout() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        QHash<QString, QString> style_of;
        QStringList score_styles;
        for (auto* label : panel.findChildren<QLabel*>()) {
            style_of.insert(label->text(), label->styleSheet());
            if (label->text() == QStringLiteral("Score: --")) {
                score_styles.append(label->styleSheet());
            }
        }
        QCOMPARE(score_styles.size(), 3);  // CPU, Disk, Memory
        QVERIFY(!score_styles.first().isEmpty());
        QCOMPARE(score_styles.count(score_styles.first()), 3);
        const QString measurement = style_of.value(QStringLiteral("Random 4K: --"));
        QVERIFY(measurement != score_styles.first());
        for (const char* text : {"Single-Thread: --",
                                 "Multi-Thread: --",
                                 "Sequential: --",
                                 "Latency: --",
                                 "Bandwidth: --",
                                 "Random Latency: --"}) {
            QVERIFY2(style_of.contains(QString::fromLatin1(text)), text);
            QCOMPARE(style_of.value(QString::fromLatin1(text)), measurement);
        }
    }

    // Review 4 (Y26): "Score: --" stood above a bar reading "Score: 0 / 3000"; CPU's two results
    // shared a line where Disk and Memory list one a line; and the disk baseline was "Samsung 980
    // PRO" in the Score line and "980 PRO" on the bar.
    void scoresReadOneWayInEveryGroup() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        panel.resize(1280, 860);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QHash<QString, QProgressBar*> bars;
        for (auto* bar : panel.findChildren<QProgressBar*>()) {
            bars.insert(bar->accessibleName(), bar);
        }
        for (const char* name :
             {"CPU Benchmark Score", "Disk Benchmark Score", "Memory Benchmark Score"}) {
            QProgressBar* bar = bars.value(QString::fromLatin1(name));
            QVERIFY2(bar != nullptr, name);
            QVERIFY2(!bar->isTextVisible(), name);  // blank until there is a score
            QVERIFY2(!bar->format().contains(QStringLiteral("baseline")), name);
        }
        QLabel* single = labelReading(panel, QStringLiteral("Single-Thread: --"));
        QLabel* multi = labelReading(panel, QStringLiteral("Multi-Thread: --"));
        QVERIFY(single != nullptr && multi != nullptr);
        // Laid out where they show: on the Benchmarks sub-tab.
        showSubTab(panel, QStringLiteral("Benchmarks"));
        QTRY_VERIFY(single->isVisible());
        QVERIFY2(multi->mapTo(&panel, QPoint(0, 0)).y() > single->mapTo(&panel, QPoint(0, 0)).y(),
                 "CPU's two results share a line");
        auto* controller = panel.findChild<sak::DiagnosticController*>();
        QVERIFY(controller != nullptr);
        sak::DiskBenchmarkResult disk;
        disk.overall_score = 1234;
        Q_EMIT controller->diskBenchmarkComplete(disk);
        QProgressBar* disk_bar = bars.value(QStringLiteral("Disk Benchmark Score"));
        QVERIFY(disk_bar->isTextVisible());
        QVERIFY(
            labelReading(panel, QStringLiteral("Score: 1234 (baseline Samsung 980 PRO = 1000)")) !=
            nullptr);
    }

    // UI review F17: before any sensor answered, and for one that never does, Thermal read
    // "0 C" with a bar of "0 / 100 C", where the other views said N/A.
    void thermalSaysNotAvailableUntilASensorAnswers() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        QProgressBar* cpu = nullptr;
        for (auto* bar : panel.findChildren<QProgressBar*>()) {
            if (bar->accessibleName() == QStringLiteral("CPU Temperature")) {
                cpu = bar;
            }
        }
        QVERIFY(cpu != nullptr);
        QCOMPARE(cpu->text(), QStringLiteral("N/A"));
        using Readings = QVector<sak::ThermalReading>;
        sak::ThermalReading reading;
        reading.component = QStringLiteral("CPU Package");
        reading.temperature_celsius = 55.0;
        QVERIFY(QMetaObject::invokeMethod(&panel,
                                          "onThermalReadingsUpdated",
                                          Qt::DirectConnection,
                                          Q_ARG(Readings, Readings{reading})));
        QCOMPARE(cpu->text(), QStringLiteral("55 / 100 \u00B0C"));
        QVERIFY(QMetaObject::invokeMethod(
            &panel, "onThermalReadingsUpdated", Qt::DirectConnection, Q_ARG(Readings, Readings{})));
        QVERIFY2(cpu->text().startsWith(QStringLiteral("N/A - ")), qPrintable(cpu->text()));
    }
    // UI review F39: Type was blank in one view, and "N/A" and "--" both meant "not known".
    void smartTableUsesOneMarkerForNotKnown() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        sak::SmartReport unread;
        unread.model = QStringLiteral("VBOX HARDDISK");
        unread.overall_health = sak::SmartHealthStatus::Unknown;
        using Reports = QVector<sak::SmartReport>;
        QVERIFY(QMetaObject::invokeMethod(&panel,
                                          "onSmartAnalysisComplete",
                                          Qt::DirectConnection,
                                          Q_ARG(Reports, Reports{unread})));
        QTableWidget* table = nullptr;
        for (auto* candidate : panel.findChildren<QTableWidget*>()) {
            if (candidate->columnCount() >= 6 && candidate->rowCount() == 1) {
                table = candidate;
            }
        }
        QVERIFY(table != nullptr);
        for (int column = 1; column < table->columnCount(); ++column) {
            QCOMPARE(table->item(0, column)->text(), QStringLiteral("N/A"));
        }
    }
    // Review 3 (X17): the SMART table said "40 degC" where the Thermal row uses the degree sign.
    void smartTemperatureUsesTheDegreeSign() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        sak::SmartReport drive;
        drive.model = QStringLiteral("ST8000DM004-2CX188");
        drive.overall_health = sak::SmartHealthStatus::Healthy;
        drive.temperature_celsius = 40.0;
        using Reports = QVector<sak::SmartReport>;
        QVERIFY(QMetaObject::invokeMethod(&panel,
                                          "onSmartAnalysisComplete",
                                          Qt::DirectConnection,
                                          Q_ARG(Reports, Reports{drive})));
        QTableWidget* table = nullptr;
        for (auto* candidate : panel.findChildren<QTableWidget*>()) {
            if (candidate->columnCount() >= 6 && candidate->rowCount() == 1) {
                table = candidate;
            }
        }
        QVERIFY(table != nullptr);
        const int temp_column = 3;
        QCOMPARE(table->horizontalHeaderItem(temp_column)->text(), QStringLiteral("Temp"));
        QCOMPARE(table->item(0, temp_column)->text(), QStringLiteral("40 \u00B0C"));
    }
    // Review 3 (X15): before Scan Hardware the Disk I/O "Drive:" box was empty, saying nothing.
    void benchmarkDriveBoxSaysToScanFirst() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        QComboBox* drive = nullptr;
        for (auto* combo : panel.findChildren<QComboBox*>()) {
            if (combo->accessibleName() == QStringLiteral("Benchmark Drive")) {
                drive = combo;
            }
        }
        QVERIFY(drive != nullptr);
        QCOMPARE(drive->count(), 0);
        QCOMPARE(drive->currentIndex(), -1);
        QCOMPARE(drive->placeholderText(), QStringLiteral("Run Scan Hardware first"));
    }
    // Review 3 (X3): the status bar read "Ready" beside a stress run's bar at 16%.
    void aRunningStressTestSaysSoBesideItsBar() {
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        auto* controller = panel.findChild<sak::DiagnosticController*>();
        QVERIFY(controller != nullptr);
        QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
        QSignalSpy progress(&panel, &sak::DiagnosticBenchmarkPanel::progressUpdate);
        QSpinBox* duration = nullptr;
        for (auto* spin : panel.findChildren<QSpinBox*>()) {
            if (spin->accessibleName() == QStringLiteral("Stress Duration")) {
                duration = spin;
            }
        }
        QVERIFY(duration != nullptr);
        duration->setValue(1);
        Q_EMIT controller->stressTestStatus(16, 50.0, 0);
        QVERIFY(!progress.isEmpty());
        QVERIFY(!status.isEmpty());
        QCOMPARE(status.last().at(0).toString(),
                 QStringLiteral("Stress test running, 00:16 of 1 min"));
    }
    // UI review N14: in the capture VM the Battery row read "1 - health unknown, On AC power";
    // VirtualBox's battery calls itself "1".
    void batteryRowIsPlainWords() {
        using Panel = sak::DiagnosticBenchmarkPanel;
        sak::BatteryInfo battery;
        QCOMPARE(Panel::batteryText(battery), QStringLiteral("Not present (desktop)"));
        battery.present = true;
        battery.name = QStringLiteral("1");
        battery.status = QStringLiteral("On AC power");
        QCOMPARE(Panel::batteryText(battery), QStringLiteral("Health not reported, On AC power"));
        battery.name = QStringLiteral("DELL MXV9V95");
        QCOMPARE(Panel::batteryText(battery),
                 QStringLiteral("DELL MXV9V95 - health not reported, On AC power"));
        battery.health_known = true;
        battery.health_percent = 87.25;
        battery.status = QStringLiteral("Charging");
        QCOMPARE(Panel::batteryText(battery),
                 QStringLiteral("DELL MXV9V95 - 87.3% health, Charging"));
    }
    // UI review N14 and F39: the VM's SMART table listed "VBOX HARDDISK" four times with no drive
    // number while the messages under it named PhysicalDrive0 to 4, the testpc1 table's
    // unreadable drives were bare "PhysicalDrive1" rows with Type N/A, and "High number of unsafe
    // shutdowns: 630" did not say which drive it was about.
    void smartRowsNameTheirDriveAndBus() {
        using Panel = sak::DiagnosticBenchmarkPanel;
        sak::SmartReport vbox;
        vbox.device_path = QStringLiteral("\\\\.\\PhysicalDrive3");
        vbox.model = QStringLiteral("VBOX HARDDISK");
        sak::StorageDeviceInfo inventory_vbox;
        inventory_vbox.disk_number = 3;
        inventory_vbox.model = QStringLiteral("VBOX HARDDISK");
        inventory_vbox.interface_type = QStringLiteral("SATA");
        auto identity = Panel::smartRowIdentity(vbox, &inventory_vbox);
        QCOMPARE(identity.drive, QStringLiteral("PhysicalDrive3 (VBOX HARDDISK)"));
        QCOMPARE(identity.type, QStringLiteral("SATA"));
        identity = Panel::smartRowIdentity(vbox, nullptr);
        QCOMPARE(identity.drive, QStringLiteral("PhysicalDrive3 (VBOX HARDDISK)"));
        QCOMPARE(identity.type, QString());

        // An unreadable drive: smartctl gave no model and no type; the hardware scan has both.
        sak::SmartReport unreadable;
        unreadable.device_path = QStringLiteral("\\\\.\\PhysicalDrive1");
        unreadable.model = QStringLiteral("PhysicalDrive1");
        sak::StorageDeviceInfo enclosure;
        enclosure.disk_number = 1;
        enclosure.model = QStringLiteral("NS-PCNVMEHDE(-C)");
        enclosure.interface_type = QStringLiteral("USB");
        identity = Panel::smartRowIdentity(unreadable, &enclosure);
        QCOMPARE(identity.drive, QStringLiteral("PhysicalDrive1 (NS-PCNVMEHDE(-C))"));
        QCOMPARE(identity.type, QStringLiteral("USB"));
        // The inventory's bus wins over smartctl's protocol, so both views say the same.
        unreadable.interface_type = QStringLiteral("SATA");
        QCOMPARE(Panel::smartRowIdentity(unreadable, &enclosure).type, QStringLiteral("USB"));
    }
    void smartWarningsStartWithTheirDrive() {
        using Panel = sak::DiagnosticBenchmarkPanel;
        const QString hynix = QStringLiteral("PhysicalDrive0 (SK hynix BC501)");
        QCOMPARE(Panel::smartWarningText(hynix,
                                         QStringLiteral("High number of unsafe shutdowns: 630")),
                 QStringLiteral(
                     "PhysicalDrive0 (SK hynix BC501): High number of unsafe shutdowns: 630"));
        for (const QString& named :
             {QStringLiteral(
                  "PhysicalDrive0: SMART cannot be read through this drive's USB adapter"),
              QStringLiteral(
                  "PhysicalDrive0 (SK hynix BC501): SMART health could not be determined.")}) {
            QCOMPARE(Panel::smartWarningText(hynix, named), named);
        }
        QCOMPARE(Panel::smartWarningText(QStringLiteral("PhysicalDrive1"),
                                         QStringLiteral("PhysicalDrive10: no answer")),
                 QStringLiteral("PhysicalDrive1: PhysicalDrive10: no answer"));
    }
    // Through the panel: the rows carry their numbers and the warnings their drive.
    void smartTableNamesEveryRowAndWarning() {
        sak::SmartReport vbox;
        vbox.model = QStringLiteral("VBOX HARDDISK");
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        using Reports = QVector<sak::SmartReport>;
        Reports reports;
        for (int index = 0; index < 4; ++index) {
            sak::SmartReport report = vbox;
            report.device_path = QStringLiteral("\\\\.\\PhysicalDrive%1").arg(index);
            report.warnings = {QStringLiteral("High number of unsafe shutdowns: 630")};
            reports.append(report);
        }
        QVERIFY(QMetaObject::invokeMethod(
            &panel, "onSmartAnalysisComplete", Qt::DirectConnection, Q_ARG(Reports, reports)));
        QTableWidget* table = nullptr;
        for (auto* candidate : panel.findChildren<QTableWidget*>()) {
            if (candidate->columnCount() >= 6 && candidate->rowCount() == 4) {
                table = candidate;
            }
        }
        QVERIFY(table != nullptr);
        QLabel* warnings = nullptr;
        for (auto* label : panel.findChildren<QLabel*>()) {
            if (label->text().contains(QStringLiteral("unsafe shutdowns"))) {
                warnings = label;
            }
        }
        QVERIFY(warnings != nullptr);
        for (int row = 0; row < 4; ++row) {
            const QString drive = QStringLiteral("PhysicalDrive%1 (VBOX HARDDISK)").arg(row);
            QCOMPARE(table->item(row, 0)->text(), drive);
            QVERIFY2(warnings->text().contains(drive + QStringLiteral(": High number of unsafe")),
                     qPrintable(warnings->text()));
        }
    }
    // UI review F16: steps not yet run showed the hourglass that means "running".
    void pendingSuiteStepsShowTheIdleMark() {
        constexpr qsizetype kSuiteSteps = 7;
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        const auto marks = panel.findChildren<QLabel*>(QStringLiteral("suiteStepMark"));
        QCOMPARE(marks.size(), kSuiteSteps);
        for (const QLabel* mark : marks) {
            QCOMPARE(mark->text(), QString(QChar(0x25CB)));
        }
    }
    // Review 2 (F16): with each step's mark in its label, the names started at three places
    // (done 78, running 67, pending 70), because the marks are glyphs of different widths.
    void suiteStepNamesStartInOneColumn() {
        constexpr qsizetype kSuiteSteps = 7;
        sak::QuickActionController quick_actions;
        sak::DiagnosticBenchmarkPanel panel(quick_actions);
        panel.resize(1280, 900);
        panel.show();
        QVERIFY(QTest::qWaitForWindowExposed(&panel));
        QVERIFY(
            QMetaObject::invokeMethod(&panel,
                                      "onSuiteStateChanged",
                                      Qt::DirectConnection,
                                      Q_ARG(sak::DiagnosticController::SuiteState,
                                            sak::DiagnosticController::SuiteState::CpuBenchmark)));
        QApplication::processEvents();
        const auto names = panel.findChildren<QLabel*>(QStringLiteral("suiteStepName"));
        QCOMPARE(names.size(), kSuiteSteps);
        QSet<int> lefts;
        for (const QLabel* name : names) {
            lefts.insert(name->mapTo(&panel, QPoint(0, 0)).x());
        }
        QCOMPARE(lefts.size(), 1);
        const auto marks = panel.findChildren<QLabel*>(QStringLiteral("suiteStepMark"));
        QStringList shown;
        // The offscreen platform has no fonts of its own, so the four glyphs can come out one
        // width and the names line up whatever holds them. The column must not follow its glyph:
        // every mark is one fixed width, room for the widest of the four.
        int widest = 0;
        for (const char16_t glyph :
             {char16_t{0x25CB}, char16_t{0x25B6}, char16_t{0x2705}, char16_t{0x23ED}}) {
            widest = std::max(widest, marks.first()->fontMetrics().horizontalAdvance(QChar(glyph)));
        }
        for (const QLabel* mark : marks) {
            shown << mark->text();
            QCOMPARE(mark->minimumWidth(), mark->maximumWidth());
            QCOMPARE(mark->width(), marks.first()->width());
            QVERIFY2(mark->width() >= widest,
                     qPrintable(QStringLiteral("%1 < %2").arg(mark->width()).arg(widest)));
        }
        // Two done, one running, four pending.
        QCOMPARE(shown.join(QString()),
                 QString(QChar(0x2705)) + QChar(0x2705) + QChar(0x25B6) + QChar(0x25CB) +
                     QChar(0x25CB) + QChar(0x25CB) + QChar(0x25CB));
    }
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void thermalPollsOnlyWhileShown_data();
    void thermalPollsOnlyWhileShown();
    void inventoryReportAsksWhereToSaveAndSaysWhereItWent();
    void inventoryReportRunsItsRealCollectors();
    void inventoryReportCancelledFolderRunsNothing();
    void stressStatusSaysWhenThereIsNoSensor();
    void aStoppedStressTestSaysStoppedAndEndsWhereItEnded();
    void thermalLimitTooltipSaysWhatHappens();
    void anUnreadableDriveShowsNoFakeNumbers();
    void thermalDiskRowSaysItIsTheHottestDisk();
    void thermalReasonsAreShownInFull();
    void anUnreadDiskSaysSoNotThatThePcCannot();
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
    // The first poll runs on the thread pool and can finish before its result reaches this thread.
    // Round 4's flake was that moment under load: with the poll done and its completion not yet
    // delivered, a started monitor read as stopped. Wait for the pool without the event loop, so
    // the moment is always the one checked.
    QVERIFY(QThreadPool::globalInstance()->waitForDone(kPollSettleTimeoutMs));
    QVERIFY2(m_processes_answered.load() > answered_before, "a shown readout asks the sensors");
    QVERIFY2(monitor->isRunning(), "a shown readout polls");

    panel.hide();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor->isRunning(), kPollSettleTimeoutMs);
    // ...and stays stopped: the in-flight poll landing must not re-arm the timer.
    QTest::qWait(1);
    QVERIFY(!monitor->isRunning());

    panel.show();
    QVERIFY(QThreadPool::globalInstance()->waitForDone(kPollSettleTimeoutMs));
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
    // Review 3: in the log alone, a run with no folder looked like one that never finished.
    QVERIFY(statusTexts(status).contains(
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

// testpc1: the reason a sensor has no reading sat in the 60 px value column and read "N/A - not".
// Each row's value fits its column, and the reason, or the note that a drive's temperature came
// from the SMART scan, is on the bar in full.
// Review 4 (Y23): before any SMART scan, "Hottest disk: N/A - not reported by this PC" stood beside
// drives the SMART table read at 42-54 degrees. Until SMART has run, the drive is not read yet.
void DiagnosticBenchmarkPanelTests::anUnreadDiskSaysSoNotThatThePcCannot() {
    const sak::ScopedProcessFaultInjector no_sensors(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            sak::ProcessResult result;
            result.exit_code = 0;
            result.std_out = QStringLiteral("status_cpu=unavailable:command_not_supported\n");
            return result;
        });
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    Q_EMIT controller->thermalSensorStatusesUpdated(
        {QStringLiteral("status_cpu=unavailable:command_not_supported")});
    using Readings = QVector<sak::ThermalReading>;
    QVERIFY(QMetaObject::invokeMethod(
        &panel, "onThermalReadingsUpdated", Qt::DirectConnection, Q_ARG(Readings, Readings{})));
    QProgressBar* disk = nullptr;
    for (auto* bar : panel.findChildren<QProgressBar*>()) {
        disk = bar->accessibleName() == QStringLiteral("Hottest disk Temperature") ? bar : disk;
    }
    QVERIFY(disk != nullptr);
    QCOMPARE(disk->text(), QStringLiteral("N/A - not read yet; Scan SMART reads it"));
    // Once SMART has read the drives and none reported a temperature, the PC does not report it.
    Q_EMIT controller->smartAnalysisComplete({});
    QVERIFY(QMetaObject::invokeMethod(
        &panel, "onThermalReadingsUpdated", Qt::DirectConnection, Q_ARG(Readings, Readings{})));
    QCOMPARE(disk->text(), QStringLiteral("N/A - not reported by this PC"));
}

void DiagnosticBenchmarkPanelTests::thermalReasonsAreShownInFull() {
    // Shown, the panel polls its sensors; each poll says what testpc1's did.
    const sak::ScopedProcessFaultInjector testpc1_sensors(
        [](const QString&, const QStringList&) -> std::optional<sak::ProcessResult> {
            sak::ProcessResult result;
            result.exit_code = 0;
            result.std_out = QStringLiteral(
                "status_cpu=unavailable:command_not_supported\n"
                "status_gpu=unavailable:command_not_supported\n"
                "identity_disk1=USB-NVME-B\nstatus_disk1=unavailable:no_answer");
            return result;
        });
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    Q_EMIT controller->thermalSensorStatusesUpdated(
        {QStringLiteral("status_cpu=unavailable:command_not_supported"),
         QStringLiteral("status_gpu=unavailable:command_not_supported"),
         QStringLiteral("status_disk1=unavailable:no_answer")});
    using Readings = QVector<sak::ThermalReading>;
    QVERIFY(QMetaObject::invokeMethod(
        &panel, "onThermalReadingsUpdated", Qt::DirectConnection, Q_ARG(Readings, Readings{})));
    panel.resize(1280, 860);
    panel.show();
    QVERIFY(QTest::qWaitForWindowExposed(&panel));
    QCoreApplication::processEvents();

    QMap<QString, QProgressBar*> bars;
    for (auto* bar : panel.findChildren<QProgressBar*>()) {
        bars.insert(bar->accessibleName(), bar);
    }
    QProgressBar* cpu = bars.value(QStringLiteral("CPU Temperature"));
    QProgressBar* disk = bars.value(QStringLiteral("Hottest disk Temperature"));
    QVERIFY(cpu != nullptr && disk != nullptr);
    QCOMPARE(cpu->text(), QStringLiteral("N/A - not reported by this PC"));
    QCOMPARE(disk->text(), QStringLiteral("N/A - did not answer in time"));
    QVERIFY2(cpu->fontMetrics().horizontalAdvance(cpu->text()) < cpu->width(),
             qPrintable(QString::number(cpu->width())));

    const auto rowValueFits = [&panel](QProgressBar* bar) {
        // The value label sits just left of its bar, on the same row.
        for (auto* label : panel.findChildren<QLabel*>()) {
            const QPoint at = label->mapTo(&panel, QPoint(0, label->height() / 2));
            const QPoint bar_at = bar->mapTo(&panel, QPoint(0, bar->height() / 2));
            if (qAbs(at.y() - bar_at.y()) < bar->height() && at.x() < bar_at.x() &&
                bar_at.x() - (at.x() + label->width()) < 24) {
                return label->sizeHint().width() <= label->width();
            }
        }
        return false;
    };
    QVERIFY2(rowValueFits(cpu), "the CPU row's value is cut");

    // A drive temperature from the SMART scan: the number in the column, the note on the bar.
    sak::SmartReport drive;
    drive.model = QStringLiteral("ST8000DM004-2CX188");
    drive.overall_health = sak::SmartHealthStatus::Warning;
    drive.temperature_celsius = 45.0;
    using Reports = QVector<sak::SmartReport>;
    QVERIFY(QMetaObject::invokeMethod(
        &panel, "onSmartAnalysisComplete", Qt::DirectConnection, Q_ARG(Reports, Reports{drive})));
    QVERIFY(QMetaObject::invokeMethod(
        &panel, "onThermalReadingsUpdated", Qt::DirectConnection, Q_ARG(Readings, Readings{})));
    QCoreApplication::processEvents();
    QCOMPARE(disk->text(), QStringLiteral("45 / 70 \u00B0C, from the SMART scan"));
    QVERIFY2(rowValueFits(disk), "the disk row's value is cut");
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
    QCOMPARE(temperatureText(), QStringLiteral("CPU Temp: 71.5 \u00B0C"));
}

// Review 3 (testpc1): closing the app 13 s into a run was logged "Stress test FAILED", and a 60.6 s
// run ended on "Elapsed: 00:55" with the status bar held at 91% beside "Stress test passed".
void DiagnosticBenchmarkPanelTests::aStoppedStressTestSaysStoppedAndEndsWhereItEnded() {
    sak::QuickActionController quick_actions;
    sak::DiagnosticBenchmarkPanel panel(quick_actions);
    auto* controller = panel.findChild<sak::DiagnosticController*>();
    QVERIFY(controller != nullptr);
    QSignalSpy status(&panel, &sak::DiagnosticBenchmarkPanel::statusMessage);
    QSignalSpy log(&panel, &sak::DiagnosticBenchmarkPanel::logOutput);
    QSignalSpy progress(&panel, &sak::DiagnosticBenchmarkPanel::progressUpdate);
    const auto labelStartingWith = [&panel](const QString& prefix) {
        for (const auto* label : panel.findChildren<QLabel*>()) {
            if (label->text().startsWith(prefix)) {
                return label->text();
            }
        }
        return QString();
    };

    sak::StressTestResult stopped;
    stopped.stopped = true;
    stopped.abort_reason = QStringLiteral("Cancelled by user");
    stopped.duration_seconds = 13;
    Q_EMIT controller->stressTestComplete(stopped);
    QCOMPARE(labelStartingWith(QStringLiteral("Status:")),
             QStringLiteral("Status: STOPPED - Cancelled by user"));
    QCOMPARE(labelStartingWith(QStringLiteral("Elapsed:")), QStringLiteral("Elapsed: 00:13"));
    QVERIFY(statusTexts(status).contains(QStringLiteral("Stress test stopped")));
    for (const QString& line : statusTexts(log)) {
        QVERIFY2(!line.contains(QStringLiteral("FAILED")), qPrintable(line));
    }

    sak::StressTestResult passed;
    passed.passed = true;
    passed.duration_seconds = 60;
    Q_EMIT controller->stressTestComplete(passed);
    QCOMPARE(labelStartingWith(QStringLiteral("Elapsed:")), QStringLiteral("Elapsed: 01:00"));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().at(0).toInt(), progress.last().at(1).toInt());
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
    QCOMPARE(table->item(0, 3)->text(), QStringLiteral("N/A"));
    QCOMPARE(table->item(0, 4)->text(), QStringLiteral("N/A"));
    // Review 4 (Y8): the last column, Wear, stretched to 378 px beside a cut drive name. The
    // name takes the room and every other column its contents' width.
    QHeaderView* header = table->horizontalHeader();
    QVERIFY(!header->stretchLastSection());
    QCOMPARE(header->sectionResizeMode(0), QHeaderView::Stretch);
    for (int column = 1; column < table->columnCount(); ++column) {
        QCOMPARE(header->sectionResizeMode(column), QHeaderView::ResizeToContents);
    }
}

QTEST_MAIN(DiagnosticBenchmarkPanelTests)
#include "test_diagnostic_benchmark_panel.moc"
