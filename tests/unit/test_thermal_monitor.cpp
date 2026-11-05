// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_thermal_monitor.cpp
/// @brief Unit tests for ThermalMonitor timer behavior and polling

#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/thermal_monitor.h"

#include "../support/test_hardware_guard.h"

#include <QFile>
#include <QMutex>
#include <QScopeGuard>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest/QtTest>

#include <atomic>
#include <filesystem>
#include <optional>

using namespace sak;

namespace {
ProcessResult successfulSensorQuery() {
    ProcessResult result;
    result.exit_code = 0;
    result.std_out = QStringLiteral("cpu=42.0");
    return result;
}
}  // namespace

class ThermalMonitorTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initialState();
    void startStop();
    void startFiresInitialPollImmediately();
    void pollOnceReadsWhatTheScriptPrints();
    void pollOnceReportsAFailedQuery();
    void pollOnceRunsTheRealScript();
    void sensorScriptSurfacesUnavailableQueries();
    void singleShotTimerBehavior();
    void clampPollInterval_flooring();
    void missingSensorIsLoggedOnce();
    void missingSensorWarnsOnceAcrossPolls();
    // The stress test's temperature reader passes the end of the run in, so a slow read stops
    // when the run does instead of holding it.
    void aCpuTemperatureReadAsksWhetherToGiveUp();
    void anUnavailableSensorSaysWhy();
    void aDiskWithNoReadingIsNotAskedAgain();
    void aNewDiskInTheSkippedNumberIsAsked();
    void theScriptNamesEachDisk();

private:
    // Outlives every test: the logger keeps writing into it once initialized.
    QTemporaryDir m_log_dir;
};

void ThermalMonitorTests::sensorScriptSurfacesUnavailableQueries() {
    const QString script = ThermalMonitor::buildCombinedThermalScript();
    // A denied CPU query must not take the GPU and disk readings with it, so the CPU section ends
    // in a status line and the rest of the script still runs.
    const qsizetype gpu_section = script.indexOf(QStringLiteral("nvidia-smi"));
    QVERIFY(gpu_section > 0);
    QVERIFY2(!script.left(gpu_section).contains(QStringLiteral("throw")),
             "the CPU section must not abort the script");
    QVERIFY(script.left(gpu_section).contains(QStringLiteral("$cpuNote='query_failed'")));
    QVERIFY(script.left(gpu_section).contains(QStringLiteral("status_cpu=unavailable:$cpuNote")));
    // testpc1 showed N/A with no reason. Denied access is named, the thermal zone is also read
    // through the counters a standard user may read, and a denied disk names its own id instead
    // of failing the whole poll.
    QVERIFY(script.left(gpu_section).contains(QStringLiteral("$cpuNote='needs_admin'")));
    QVERIFY(
        script.contains(QStringLiteral("Win32_PerfFormattedData_Counters_ThermalZoneInformation")));
    QVERIFY(script.contains(QStringLiteral("$id=$_.DeviceId;")));
    QVERIFY(script.contains(QStringLiteral("status_disk$id=unavailable:needs_admin")));
    QVERIFY2(!script.contains(QStringLiteral("status_disk$($_.DeviceId)")),
             "inside a catch, $_ is the error, not the disk");
    // testpc1 again: its thermal zone answers "not supported", and one USB NVMe took 12 to 17 s
    // to answer for its counters, so every poll ran out of time and the two disks that report
    // read N/A. Not supported is said as such, every request has its own time limit, and a disk
    // out of time says so instead of failing the poll.
    QVERIFY(script.left(gpu_section).contains(QStringLiteral("-eq 'NotImplemented'")));
    QVERIFY2(!script.contains(QStringLiteral("Get-StorageReliabilityCounter")),
             "that cmdlet takes no time limit");
    QVERIFY(script.contains(QStringLiteral("-ResultClassName MSFT_StorageReliabilityCounter")));
    const qsizetype requests = script.count(QStringLiteral("Get-Cim"));
    QVERIFY(requests >= 3);
    QCOMPARE(script.count(QStringLiteral("-OperationTimeoutSec 2 ")), requests);
    QVERIFY(script.contains(QStringLiteral("'HRESULT 0x40004,*'")));
    QVERIFY(script.contains(QStringLiteral("status_disk$id=unavailable:no_answer")));
    // A quiet disk is skipped by its identity, and names that identity even when skipped, so the
    // monitor sees drives arrive and leave.
    QVERIFY(script.startsWith(QStringLiteral("$r=@{};$skip=@();")));
    QVERIFY(script.contains(QStringLiteral("$uid=([string]$_.UniqueId).Trim();")));
    QVERIFY(script.contains(QStringLiteral(
        "Write-Output \"identity_disk$id=$uid\";if($uid -and $skip -contains $uid){return};")));
    QVERIFY(
        ThermalMonitor::buildCombinedThermalScript(
            {QStringLiteral("USBSTOR\\DISK&VEN_NVME&PROD_NS-PCNVMEHDE"),
             QStringLiteral("5000C500A1B2C3D4")})
            .startsWith(QStringLiteral(
                "$r=@{};$skip=@('USBSTOR\\DISK&VEN_NVME&PROD_NS-PCNVMEHDE','5000C500A1B2C3D4');")));
    // Nothing that could end the quoted string reaches the script: an apostrophe, PowerShell's
    // curly quotes, or a line break.
    for (const QString& unsafe : {QStringLiteral("1');throw '"),
                                  QString(u"DISK\u2019X"),
                                  QStringLiteral("DISK\nX"),
                                  QStringLiteral("  ")}) {
        QVERIFY2(ThermalMonitor::buildCombinedThermalScript({unsafe}).startsWith(
                     QStringLiteral("$r=@{};$skip=@();")),
                 qPrintable(unsafe));
    }
    QVERIFY2(!script.contains(QStringLiteral("SilentlyContinue")), qPrintable(script));
    QVERIFY2(!script.contains(QStringLiteral("2>$null")), qPrintable(script));
    QVERIFY2(!script.contains(QStringLiteral("catch{}")), qPrintable(script));
    QVERIFY(script.contains(QStringLiteral("status_cpu=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("status_gpu=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("status_disk=unavailable:")));
    QVERIFY(script.contains(QStringLiteral("-ErrorAction Stop")));
}

void ThermalMonitorTests::initialState() {
    ThermalMonitor monitor;
    QVERIFY(!monitor.isRunning());
}

// testpc1: Thermal said a bare N/A. Each sensor without a reading names why.
void ThermalMonitorTests::anUnavailableSensorSaysWhy() {
    const QStringList statuses = {QStringLiteral("status_cpu=unavailable:needs_admin"),
                                  QStringLiteral("status_gpu=unavailable:command_not_supported"),
                                  QStringLiteral("status_disk0=unavailable:no_reading"),
                                  QStringLiteral("status_disk1=unavailable:needs_admin")};
    QCOMPARE(ThermalMonitor::unavailableReason(statuses, QStringLiteral("cpu")),
             QStringLiteral("needs administrator rights"));
    QCOMPARE(ThermalMonitor::unavailableReason(statuses, QStringLiteral("gpu")),
             QStringLiteral("not reported by this PC"));
    QCOMPARE(ThermalMonitor::unavailableReason(statuses, QStringLiteral("disk")),
             QStringLiteral("needs administrator rights"));
    QCOMPARE(ThermalMonitor::unavailableReason(
                 {QStringLiteral("status_query=unavailable:query_failed")}, QStringLiteral("cpu")),
             QStringLiteral("could not be read"));
    QCOMPARE(ThermalMonitor::unavailableReason({}, QStringLiteral("disk")),
             QStringLiteral("not reported by this PC"));
    QCOMPARE(
        ThermalMonitor::unavailableReason({QStringLiteral("status_disk0=unavailable:no_reading"),
                                           QStringLiteral("status_disk1=unavailable:no_answer")},
                                          QStringLiteral("disk")),
        QStringLiteral("did not answer in time"));
    // Several disks: the most telling reason is shown, whichever disk comes last.
    QCOMPARE(
        ThermalMonitor::unavailableReason({QStringLiteral("status_disk0=unavailable:no_answer"),
                                           QStringLiteral("status_disk1=unavailable:no_reading")},
                                          QStringLiteral("disk")),
        QStringLiteral("did not answer in time"));
    QCOMPARE(
        ThermalMonitor::unavailableReason({QStringLiteral("status_disk0=unavailable:needs_admin"),
                                           QStringLiteral("status_disk1=unavailable:no_answer")},
                                          QStringLiteral("disk")),
        QStringLiteral("needs administrator rights"));
}

namespace {
// testpc1's three disks as the sensor script names them: the internal NVMe, the USB NVMe
// enclosure that gives no temperature, and the Seagate.
const QString kTestpc1Disks = QStringLiteral(
    "identity_disk0=NVME-SKHYNIX-BC501\nidentity_disk1=USB-NS-PCNVMEHDE\n"
    "identity_disk2=USB-SEAGATE-EXPANSION\n");
const QString kSkipEnclosure = QStringLiteral("$r=@{};$skip=@('USB-NS-PCNVMEHDE');");
const QString kSkipNothing = QStringLiteral("$r=@{};$skip=@();");

ThermalPollResult pollOf(const QMap<QString, QString>& identities, const QStringList& statuses) {
    ThermalPollResult poll;
    poll.query_ok = true;
    poll.disk_identities = identities;
    poll.sensor_statuses = statuses;
    return poll;
}
}  // namespace

// testpc1: the USB NVMe that took 12 s to say nothing was asked again on every poll, while a disk
// benchmark ran on it. A disk with no reading is not asked again, and still says why, until a
// hardware or SMART scan asks every disk again.
void ThermalMonitorTests::aDiskWithNoReadingIsNotAskedAgain() {
    QMutex mutex;
    QStringList scripts;
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList& args) -> std::optional<ProcessResult> {
            QMutexLocker lock(&mutex);
            scripts.append(args.constLast());
            ProcessResult result;
            result.exit_code = 0;
            // The enclosure says why only when it is asked.
            const bool asked = !args.constLast().startsWith(kSkipEnclosure);
            result.std_out = kTestpc1Disks + QStringLiteral("disk0=36\ndisk2=43") +
                             (asked ? QStringLiteral("\nstatus_disk1=unavailable:no_answer")
                                    : QString());
            return result;
        });
    const auto scriptCount = [&] {
        const QMutexLocker lock(&mutex);
        return scripts.size();
    };
    const auto scriptAt = [&](qsizetype index) {
        const QMutexLocker lock(&mutex);
        return scripts.at(index);
    };

    ThermalMonitor monitor;
    QSignalSpy statuses_spy(&monitor, &ThermalMonitor::sensorStatusesUpdated);
    QVERIFY(statuses_spy.isValid());
    monitor.start(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(statuses_spy.count() >= 2, 10'000);
    monitor.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.m_poll_watcher.isRunning(), 10'000);

    QVERIFY(scriptAt(0).startsWith(kSkipNothing));
    QVERIFY2(scriptAt(1).startsWith(kSkipEnclosure), qPrintable(scriptAt(1).left(60)));
    const QStringList expected{QStringLiteral("status_disk1=unavailable:no_answer")};
    QCOMPARE(statuses_spy.at(0).at(0).toStringList(), expected);
    QCOMPARE(statuses_spy.at(1).at(0).toStringList(), expected);

    // The panel restarts the monitor each time it is shown; that does not ask the disk again.
    qsizetype mark = scriptCount();
    monitor.start(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(scriptCount() > mark, 10'000);
    monitor.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.m_poll_watcher.isRunning(), 10'000);
    QVERIFY2(scriptAt(mark).startsWith(kSkipEnclosure), qPrintable(scriptAt(mark).left(60)));

    // A re-scan asks every disk again: a drive that was asleep may answer now.
    monitor.askEveryDiskAgain();
    mark = scriptCount();
    monitor.start(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(scriptCount() > mark, 10'000);
    monitor.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!monitor.m_poll_watcher.isRunning(), 10'000);
    QVERIFY2(scriptAt(mark).startsWith(kSkipNothing), qPrintable(scriptAt(mark).left(60)));
}

// Each disk's identity line becomes the poll's identity map and never a reading, even when the
// identity happens to look like a temperature.
void ThermalMonitorTests::theScriptNamesEachDisk() {
    ScopedProcessFaultInjector injector(
        [](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            ProcessResult result;
            result.exit_code = 0;
            result.std_out = QStringLiteral(
                "identity_disk0=NVME-A\nidentity_disk1=45\nidentity_diskX=BAD\n"
                "identity_disk2=\ndisk0=36");
            return result;
        });
    const ThermalPollResult poll = ThermalMonitor::querySensors();
    QVERIFY(poll.query_ok);
    const QMap<QString, QString> expected{{QStringLiteral("0"), QStringLiteral("NVME-A")},
                                          {QStringLiteral("1"), QStringLiteral("45")},
                                          {QStringLiteral("2"), QString()}};
    QCOMPARE(poll.disk_identities, expected);
    QCOMPARE(poll.readings.size(), 1);
    QCOMPARE(poll.readings.first().component, QStringLiteral("Disk 0"));
}

// The coordinator's case: technicians hot-plug drives, so disk 1 can be another drive ten minutes
// later. The skip follows the drive, not its number, and a drive arriving or leaving asks every
// disk again.
void ThermalMonitorTests::aNewDiskInTheSkippedNumberIsAsked() {
    ThermalMonitor::QuietDisks quiet;
    const QStringList no_answer{QStringLiteral("status_disk1=unavailable:no_answer")};
    const QMap<QString, QString> testpc1{{QStringLiteral("0"), QStringLiteral("NVME-A")},
                                         {QStringLiteral("1"), QStringLiteral("USB-NVME-B")}};
    QCOMPARE(ThermalMonitor::rememberQuietDisks(pollOf(testpc1, no_answer), &quiet), no_answer);
    QCOMPARE(quiet.status_by_identity.keys(), QStringList{QStringLiteral("USB-NVME-B")});

    // Same drives, numbered the other way round: the enclosure is still the one skipped, and
    // the reason it has no reading is given under its number now.
    const QMap<QString, QString> renumbered{{QStringLiteral("0"), QStringLiteral("USB-NVME-B")},
                                            {QStringLiteral("1"), QStringLiteral("NVME-A")}};
    QCOMPARE(ThermalMonitor::rememberQuietDisks(pollOf(renumbered, {}), &quiet),
             QStringList{QStringLiteral("status_disk0=unavailable:no_answer")});
    QCOMPARE(quiet.status_by_identity.keys(), QStringList{QStringLiteral("USB-NVME-B")});

    // The enclosure is unplugged and a different drive arrives as disk 1: it is asked, and the
    // enclosure's reason is not shown for it.
    const QMap<QString, QString> swapped{{QStringLiteral("0"), QStringLiteral("NVME-A")},
                                         {QStringLiteral("1"), QStringLiteral("SANDISK-C")}};
    QCOMPARE(ThermalMonitor::rememberQuietDisks(pollOf(swapped, {}), &quiet), QStringList());
    QVERIFY(quiet.status_by_identity.isEmpty());
    QVERIFY(ThermalMonitor::buildCombinedThermalScript(quiet.status_by_identity.keys())
                .startsWith(kSkipNothing));

    // The enclosure comes back as disk 2 and does not answer: it is skipped again from then on.
    QMap<QString, QString> back = swapped;
    back.insert(QStringLiteral("2"), QStringLiteral("USB-NVME-B"));
    const QStringList late{QStringLiteral("status_disk2=unavailable:no_answer")};
    QCOMPARE(ThermalMonitor::rememberQuietDisks(pollOf(back, late), &quiet), late);
    QCOMPARE(quiet.status_by_identity.keys(), QStringList{QStringLiteral("USB-NVME-B")});

    // A drive whose identity Windows does not give is asked on every poll.
    ThermalMonitor::QuietDisks anonymous;
    const QMap<QString, QString> no_identity{{QStringLiteral("0"), QString()}};
    (void)ThermalMonitor::rememberQuietDisks(
        pollOf(no_identity, {QStringLiteral("status_disk0=unavailable:no_reading")}), &anonymous);
    QVERIFY(anonymous.status_by_identity.isEmpty());
}

void ThermalMonitorTests::aCpuTemperatureReadAsksWhetherToGiveUp() {
    // Already told to give up: the read asks, starts no process, and answers "no reading".
    int asked = 0;
    const double temp = ThermalMonitor::queryCpuTemperature([&asked]() {
        ++asked;
        return true;
    });
    QVERIFY(asked > 0);
    QCOMPARE(temp, -1.0);
}

void ThermalMonitorTests::startStop() {
    // Hold the first query in flight so stop() always races the poll, regardless of
    // PowerShell startup time or whether this host has readable sensors.
    QSemaphore poll_entered;
    QSemaphore finish_poll;
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            poll_entered.release();
            finish_poll.acquire();
            return successfulSensorQuery();
        });

    int updates = 0;
    ThermalMonitor monitor;
    const auto unblock_poll = qScopeGuard([&] {
        finish_poll.release();
        monitor.stop();
    });
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&updates](const QVector<sak::ThermalReading>&) { ++updates; });
    QSignalSpy finished_spy(&monitor.m_poll_watcher,
                            &QFutureWatcher<sak::ThermalPollResult>::finished);
    QVERIFY(finished_spy.isValid());

    monitor.start(500);  // 500ms interval
    QVERIFY2(poll_entered.tryAcquire(1, 5000), "Initial sensor query did not start");
    QVERIFY(monitor.isRunning());

    // During the held poll the one-shot timer is inactive. Only m_active carries
    // stop intent into onPollComplete().
    QVERIFY(!monitor.m_timer.isActive());
    monitor.stop();
    finish_poll.release();
    QTRY_VERIFY_WITH_TIMEOUT(finished_spy.count() >= 1, 5000);
    QVERIFY(!monitor.isRunning());
    QCOMPARE(updates, 0);  // result of a poll that outlived stop() is dropped
}

// start() fires the initial poll IMMEDIATELY rather than merely arming the timer. The
// stop-during-poll test above depends on that -- it needs an in-flight poll for stop() to race,
// or the m_active guard it is named for is never exercised -- but it cannot prove it: a start()
// that only armed the timer satisfies its isRunning() check just as well.
//
// Proved here without racing the poll: with a 30s interval, a reading can only arrive quickly if
// start() polled at once. If it merely armed the timer, nothing can arrive for 30 seconds.
void ThermalMonitorTests::startFiresInitialPollImmediately() {
    std::atomic<int> queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            queries.fetch_add(1);
            return successfulSensorQuery();
        });

    int cycles = 0;
    ThermalMonitor monitor;
    QObject::connect(&monitor,
                     &ThermalMonitor::readingsUpdated,
                     &monitor,
                     [&cycles](const QVector<sak::ThermalReading>&) { ++cycles; });

    monitor.start(30'000);
    QTRY_VERIFY_WITH_TIMEOUT(cycles >= 1, 5000);
    QCOMPARE(queries.load(), 1);
    monitor.stop();
    QVERIFY(!monitor.isRunning());
}

// The script's output is a fake: the real script asks every disk for its reliability counters.
void ThermalMonitorTests::pollOnceReadsWhatTheScriptPrints() {
    ScopedProcessFaultInjector injector(
        [](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            ProcessResult result;
            result.exit_code = 0;
            result.std_out = QStringLiteral(
                "cpu=52.3\ngpu=61\ndisk0=38\ndiskX=40\ncpu2=nan\n"
                "status_disk1=unavailable:no_reading");
            return result;
        });
    bool query_ok = false;
    const QVector<ThermalReading> readings = ThermalMonitor::pollOnce(query_ok);
    QVERIFY(query_ok);
    QCOMPARE(readings.size(), 3);
    QCOMPARE(readings.at(0).component, QStringLiteral("CPU Package"));
    QCOMPARE(readings.at(0).temperature_celsius, 52.3);
    QCOMPARE(readings.at(1).component, QStringLiteral("GPU"));
    QCOMPARE(readings.at(2).component, QStringLiteral("Disk 0"));
    QVERIFY(readings.at(2).timestamp.isValid());
}

void ThermalMonitorTests::pollOnceReportsAFailedQuery() {
    ScopedProcessFaultInjector injector(
        [](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            ProcessResult result;
            result.exit_code = 1;
            result.std_out = QStringLiteral("cpu=52.3");
            return result;
        });
    bool query_ok = true;
    QVERIFY(ThermalMonitor::pollOnce(query_ok).isEmpty());
    QVERIFY(!query_ok);
}

// The one test that runs the script for real, so a PowerShell error in it cannot hide behind the
// fakes above. It asks every disk for its counters, so it runs only in CI. MSAcpi_ThermalZone and
// StorageReliabilityCounter both need admin, so an EMPTY result is legitimate and the reading COUNT
// cannot be asserted; every reading that IS returned must have survived parseThermalOutput.
void ThermalMonitorTests::pollOnceRunsTheRealScript() {
    SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED();
    bool queryOk = false;
    const auto readings = ThermalMonitor::pollOnce(queryOk);
    QVERIFY2(queryOk || readings.isEmpty(), "a failed sensor query must not yield readings");

    for (const auto& reading : readings) {
        const bool known_component = reading.component == QStringLiteral("CPU Package") ||
                                     reading.component == QStringLiteral("GPU") ||
                                     reading.component.startsWith(QStringLiteral("Disk "));
        QVERIFY2(known_component, qPrintable("Unexpected component: " + reading.component));
        QVERIFY2(reading.temperature_celsius > 0.0,
                 qPrintable(QString("Non-positive temperature for %1").arg(reading.component)));
        QVERIFY2(reading.timestamp.isValid(), qPrintable("No timestamp on " + reading.component));
    }
}

void ThermalMonitorTests::singleShotTimerBehavior() {
    // Exercise the real timer, watcher and result path with a controlled process result.
    // Poll time is fixed by this fixture, not by host PowerShell/WMI scheduling.
    std::atomic<int> active_queries{0};
    std::atomic<int> peak_queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            const int concurrent = active_queries.fetch_add(1) + 1;
            int peak = peak_queries.load();
            while (concurrent > peak && !peak_queries.compare_exchange_weak(peak, concurrent)) {}
            QThread::msleep(150);
            active_queries.fetch_sub(1);
            return successfulSensorQuery();
        });

    ThermalMonitor monitor;
    QSignalSpy readings_spy(&monitor, &ThermalMonitor::readingsUpdated);
    QVERIFY(readings_spy.isValid());
    monitor.start(250);
    QVERIFY(monitor.m_timer.isSingleShot());

    // Lower bound is eventual rather than a fixed 3s window. Removing the
    // onPollComplete timer re-arm leaves exactly one cycle and fails this check.
    QTRY_VERIFY_WITH_TIMEOUT(readings_spy.count() >= 3, 10'000);
    QCOMPARE(monitor.m_timer.interval(), 250);
    QCOMPARE(peak_queries.load(), 1);

    const int cycles_before_window = readings_spy.count();
    QTest::qWait(1000);
    monitor.stop();

    // A 1s window with 250ms cadence cannot accumulate unbounded cycles. The
    // separate three-cycle lower bound above keeps this upper bound non-vacuous.
    const int window_cycles = readings_spy.count() - cycles_before_window;
    QVERIFY2(window_cycles <= 5, qPrintable(QString("Runaway poll cycles: %1").arg(window_cycles)));
    QCOMPARE(peak_queries.load(), 1);
}

// B5 tail: a non-positive poll interval must never arm the timer (0 busy-spins,
// negative is invalid). start() clamps through clampPollIntervalMs.
void ThermalMonitorTests::clampPollInterval_flooring() {
    // Every assertion below is written RELATIVE to the symbol, so changing the floor's
    // magnitude keeps them all true while the anti-busy-spin guarantee moves. Pin the value.
    QCOMPARE(sak::kMinThermalPollIntervalMs, 250);
    // The DEFAULT cadence has its own constant (it used to borrow the peer-discovery broadcast
    // interval, 2 s). Each poll launches a PowerShell process, so a glanceable readout polls
    // no faster than every 5 s. Pin the value and pin that it sits ABOVE the floor.
    QCOMPARE(sak::kDefaultThermalPollIntervalMs, sak::kThermalReadoutPollIntervalMs);
    QCOMPARE(sak::kThermalReadoutPollIntervalMs, 5000);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(sak::kDefaultThermalPollIntervalMs),
             sak::kThermalReadoutPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(1000), 1000);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(sak::kMinThermalPollIntervalMs),
             sak::kMinThermalPollIntervalMs);
    // Below the floor / zero / negative all clamp up to the floor.
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(10), sak::kMinThermalPollIntervalMs);
    // Both sides of the EXACT boundary. Without them a floor written against the wrong
    // constant -- `requested >= 100 ? requested : kMinThermalPollIntervalMs` -- satisfies
    // every other case here (1000, 250, 10, 0, -5 all stay green) while start(150) arms a
    // 150ms sensor poll, killing the contract at thermal_monitor.h:64-67. A `>= 500` variant
    // passes them too, and dies on the 251 probe.
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(249), 250);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(251), 251);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(0), sak::kMinThermalPollIntervalMs);
    QCOMPARE(ThermalMonitor::clampPollIntervalMs(-5), sak::kMinThermalPollIntervalMs);
}

// A sensor that is not there reports the same status line on every poll. The monitor used to
// log a warning per missing sensor per poll -- every 2 s for the life of the app. Each distinct
// status is now logged once; a new status (another sensor, another reason) still is.
void ThermalMonitorTests::missingSensorIsLoggedOnce() {
    QSet<QString> logged;
    const QStringList first = {QStringLiteral("status_gpu=unavailable:command_not_supported"),
                               QStringLiteral("status_disk=unavailable:no_devices")};
    QCOMPARE(ThermalMonitor::newSensorStatuses(first, &logged), first);
    QCOMPARE(ThermalMonitor::newSensorStatuses(first, &logged), QStringList());
    const QStringList changed = {QStringLiteral("status_gpu=unavailable:command_not_supported"),
                                 QStringLiteral("status_cpu=unavailable:no_reading")};
    QCOMPARE(ThermalMonitor::newSensorStatuses(changed, &logged),
             QStringList{QStringLiteral("status_cpu=unavailable:no_reading")});
    QCOMPARE(logged.size(), 3);
}

// The same, end to end: several real poll cycles that each report the missing GPU leave one
// warning in the application log, not one per poll.
void ThermalMonitorTests::missingSensorWarnsOnceAcrossPolls() {
    QVERIFY(m_log_dir.isValid());
    auto& log = logger::instance();
    QVERIFY(log.initialize(std::filesystem::path(m_log_dir.path().toStdWString()), "thermal")
                .has_value());
    const QString missing = QStringLiteral("status_gpu=unavailable:command_not_supported");
    std::atomic<int> queries{0};
    ScopedProcessFaultInjector injector(
        [&](const QString&, const QStringList&) -> std::optional<ProcessResult> {
            queries.fetch_add(1);
            ProcessResult result = successfulSensorQuery();
            result.std_out += QStringLiteral("\n") + missing;
            return result;
        });

    ThermalMonitor monitor;
    QSignalSpy readings_spy(&monitor, &ThermalMonitor::readingsUpdated);
    monitor.start(kMinThermalPollIntervalMs);
    QTRY_VERIFY_WITH_TIMEOUT(readings_spy.count() >= 3, 10'000);
    monitor.stop();
    log.flush();

    QFile file(QString::fromStdWString(log.getLogFile().wstring()));
    QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.fileName()));
    const QString text = QString::fromUtf8(file.readAll());
    QVERIFY2(queries.load() >= 3, "three polls ran");
    QCOMPARE(text.count(missing), 1);
}

QTEST_MAIN(ThermalMonitorTests)
#include "test_thermal_monitor.moc"
