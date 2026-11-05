// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file thermal_monitor.cpp
/// @brief Real-time thermal sensor monitoring implementation

#include "sak/thermal_monitor.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QtConcurrent>

#include <algorithm>
#include <cmath>
#include <utility>

namespace sak {

namespace {
constexpr qsizetype kDiskKeyPrefixLength = 4;
constexpr auto kSensorStatusPrefix = "status_";
constexpr double kWmiTenthsKelvinDivisor = 10.0;
constexpr double kKelvinZeroCelsius = 273.15;
// No silicon or drive sensor reports above this; a larger value is telemetry corruption,
// not a temperature, and must not reach the thresholds or the UI.
constexpr double kMaxPlausibleCelsius = 150.0;

// A sensor value must be a FINITE Celsius reading inside the plausible range.
// QString::toDouble accepts "nan"/"inf", and a NaN passes a bare `temp <= 0` test, enters
// the reading set, then silently fails every threshold comparison in processReadings
// (NaN >= x is false) -- a warning that can never fire. Reject instead of coercing.
[[nodiscard]] bool isPlausibleCelsius(double temp) {
    return std::isfinite(temp) && temp > 0.0 && temp <= kMaxPlausibleCelsius;
}

// True only for the "disk<DeviceId>" keys the combined script emits: the suffix must be a
// non-empty run of digits. Any other "disk*" key is not a device identity we can trust, so
// the line is dropped instead of becoming a component named after arbitrary text.
[[nodiscard]] bool isDiskDeviceId(const QString& device_id) {
    if (device_id.isEmpty()) {
        return false;
    }
    return std::ranges::all_of(device_id, [](const QChar ch) { return ch.isDigit(); });
}

[[nodiscard]] QStringList sensorStatusLines(const QString& output) {
    QStringList statuses;
    const auto lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString status = line.trimmed();
        if (status.startsWith(QLatin1String(kSensorStatusPrefix))) {
            statuses.append(status);
        }
    }
    return statuses;
}

// A disk identity goes into the next poll's script inside single quotes. Printable ASCII
// without an apostrophe cannot end that string (PowerShell also ends it on curly quotes, which
// are not ASCII); an identity with anything else is simply asked again on every poll.
[[nodiscard]] bool isScriptSafeIdentity(const QString& identity) {
    static const QRegularExpression safe(QStringLiteral("^[\\x20-\\x26\\x28-\\x7E]{1,200}$"));
    return !identity.trimmed().isEmpty() && safe.match(identity).hasMatch();
}

[[nodiscard]] QMap<QString, QString> diskIdentities(const QString& output) {
    const QString prefix = QStringLiteral("identity_disk");
    QMap<QString, QString> identities;
    const auto lines = output.split('\n', Qt::SkipEmptyParts);
    for (const QString& line : lines) {
        const QString trimmed = line.trimmed();
        const qsizetype eq_pos = trimmed.indexOf(QLatin1Char('='));
        if (!trimmed.startsWith(prefix) || eq_pos < 0) {
            continue;
        }
        const QString device_id = trimmed.mid(prefix.size(), eq_pos - prefix.size());
        if (isDiskDeviceId(device_id)) {
            identities.insert(device_id, trimmed.mid(eq_pos + 1));
        }
    }
    return identities;
}

void logSensorStatuses(const QStringList& statuses) {
    for (const QString& status : statuses) {
        logWarning("Thermal sensor {}", status.toStdString());
    }
}

[[nodiscard]] QString cpuThermalScript() {
    // The ACPI thermal zone, through the class that needs administrator rights and then through
    // its performance counters, which a standard user may read. What stopped both is said, in
    // words a technician can act on: "needs_admin", or "command_not_supported" for a PC that
    // reports no zone at all, as many desktops do. testpc1 answers the zone class with WMI's "not
    // supported" (category NotImplemented), which once read as a failed query. Each request has
    // its own time limit, so one slow answer cannot run the whole poll out of time.
    return QStringLiteral(
        "$cpuNote='no_reading';"
        "try{"
        "$t=Get-CimInstance -Namespace root/WMI "
        "-ClassName MSAcpi_ThermalZoneTemperature "
        "-OperationTimeoutSec 2 -ErrorAction Stop|Select-Object -First 1;"
        "if($t.CurrentTemperature -gt 0){"
        "$r['cpu']=[math]::Round(($t.CurrentTemperature/10)-273.15,1)}"
        "}catch [System.Management.Automation.CommandNotFoundException]{"
        "$cpuNote='command_not_supported'"
        "}catch{"
        "if($_.CategoryInfo.Category -eq 'PermissionDenied'){$cpuNote='needs_admin'"
        "}elseif($_.CategoryInfo.Category -eq 'ObjectNotFound' -or "
        "$_.CategoryInfo.Category -eq 'NotImplemented' -or "
        "$_.CategoryInfo.Reason -match 'NotSupported'){"
        "$cpuNote='command_not_supported'"
        // Windows denies this WMI class to a normal user. That says nothing about the GPU and disk
        // queries below, which do not need the same rights: a throw here would blank them too.
        "}else{$cpuNote='query_failed'}};"
        "if(-not $r.ContainsKey('cpu')){try{"
        "$z=Get-CimInstance -ClassName Win32_PerfFormattedData_Counters_ThermalZoneInformation "
        "-OperationTimeoutSec 2 -ErrorAction Stop|Where-Object{$_.HighPrecisionTemperature -gt 0}|"
        "Sort-Object HighPrecisionTemperature -Descending|Select-Object -First 1;"
        "if($z){$r['cpu']=[math]::Round(($z.HighPrecisionTemperature/10)-273.15,1)}"
        "}catch{if($cpuNote -eq 'no_reading'){$cpuNote='query_failed'}}};"
        "if(-not $r.ContainsKey('cpu')){"
        "Write-Output \"status_cpu=unavailable:$cpuNote\"};");
}

[[nodiscard]] QString gpuThermalScript() {
    return QStringLiteral(
        "$nvp=@("
        "'C:\\Program Files\\NVIDIA Corporation\\NVSMI\\nvidia-smi.exe',"
        "'C:\\Windows\\System32\\nvidia-smi.exe');"
        "$gpuFound=$false;"
        "foreach($p in $nvp){"
        "if(-not (Test-Path -LiteralPath $p -PathType Leaf)){continue};"
        "$gpuFound=$true;"
        "$o=&$p --query-gpu=temperature.gpu --format=csv,noheader,nounits;"
        "if($LASTEXITCODE -ne 0){throw 'nvidia-smi temperature query failed'};"
        "if($o){"
        "$g=@($o)|ForEach-Object{$_.ToString().Split(\"`n\")}|"
        "ForEach-Object{$_.Trim()}|Where-Object{$_}|ForEach-Object{[double]$_};"
        "if($g){$r['gpu']=($g|Measure-Object -Maximum).Maximum;break}};"
        "Write-Output 'status_gpu=unavailable:no_reading';break};"
        "if(-not $gpuFound){"
        "Write-Output 'status_gpu=unavailable:command_not_supported'};");
}

[[nodiscard]] QString diskThermalScript() {
    // Each disk's id is kept before its query: inside a catch, $_ is the error, and the status
    // line named no disk. A disk Windows will not read for a standard user says so instead of
    // failing the whole poll, which blanked the CPU and GPU readings with it.
    //
    // testpc1's USB NVMe enclosure took 12 to 17 s to answer a reliability-counter request, with
    // no temperature, so every poll outran its time limit and both disks that do report (36 and
    // 43 C) read N/A. Get-StorageReliabilityCounter takes no time limit; the same counters asked
    // through CIM do. A disk that runs out of time says so ("no_answer"). Each disk names its
    // identity, skipped or not, so the monitor sees a drive arrive or leave; $skip names the
    // identities an earlier poll heard nothing from, which are not asked again.
    return QStringLiteral(
        "try{$d=@(Get-PhysicalDisk -ErrorAction Stop)"
        "}catch [System.Management.Automation.CommandNotFoundException]{"
        "Write-Output 'status_disk=unavailable:command_not_supported';$d=@()"
        "}catch{"
        "if($_.CategoryInfo.Reason -match 'NotSupported'){"
        "Write-Output 'status_disk=unavailable:command_not_supported';$d=@()"
        "}else{throw}};"
        "if($d.Count -eq 0){Write-Output 'status_disk=unavailable:no_devices'};"
        "$d|ForEach-Object{$id=$_.DeviceId;"
        "$uid=([string]$_.UniqueId).Trim();if(-not $uid){$uid=([string]$_.SerialNumber).Trim()};"
        "Write-Output \"identity_disk$id=$uid\";"
        "if($uid -and $skip -contains $uid){return};try{"
        "$c=Get-CimAssociatedInstance -InputObject $_ "
        "-ResultClassName MSFT_StorageReliabilityCounter "
        "-OperationTimeoutSec 2 -ErrorAction Stop|Select-Object -First 1;"
        "if($c -and $c.Temperature -gt 0){"
        "$r[\"disk$id\"]=$c.Temperature"
        "}else{Write-Output \"status_disk$id=unavailable:no_reading\"}"
        "}catch [System.NotSupportedException]{"
        "Write-Output \"status_disk$id=unavailable:command_not_supported\""
        "}catch{"
        "if($_.CategoryInfo.Category -eq 'PermissionDenied'){"
        "Write-Output \"status_disk$id=unavailable:needs_admin\""
        // WBEM_S_TIMEDOUT: the request's own time limit ran out.
        "}elseif($_.FullyQualifiedErrorId -like 'HRESULT 0x40004,*'){"
        "Write-Output \"status_disk$id=unavailable:no_answer\""
        "}elseif($_.CategoryInfo.Category -eq 'NotImplemented' -or "
        "$_.CategoryInfo.Reason -match 'NotSupported'){"
        "Write-Output \"status_disk$id=unavailable:command_not_supported\""
        "}else{throw}}};");
}
}  // namespace

// ============================================================================
// Construction / Destruction
// ============================================================================

ThermalMonitor::ThermalMonitor(QObject* parent) : QObject(parent) {
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ThermalMonitor::onTimerTick);
    connect(&m_poll_watcher,
            &QFutureWatcher<QVector<ThermalReading>>::finished,
            this,
            &ThermalMonitor::onPollComplete);
}

ThermalMonitor::~ThermalMonitor() {
    stop();
    if (m_poll_watcher.isRunning()) {
        // SAK-ALLOW-BLOCKING: the pool task writes its readings into m_poll_watcher, which
        // dies with this object. A single sensor poll is short, bounded work; abandoning it
        // would hand the pool a dangling watcher.
        m_poll_watcher.waitForFinished();
    }
}

// ============================================================================
// Public API
// ============================================================================

int ThermalMonitor::clampPollIntervalMs(int requested) {
    return requested >= kMinThermalPollIntervalMs ? requested : kMinThermalPollIntervalMs;
}

void ThermalMonitor::start(int interval_ms) {
    if (m_timer.isActive()) {
        m_timer.stop();
    }

    // Clamp: a zero interval busy-spins the sensor query; a negative one is
    // invalid. Never arm the timer with a non-positive interval.
    m_interval_ms = clampPollIntervalMs(interval_ms);
    m_active = true;
    // m_quiet_disks is kept: the panel restarts the monitor each time it is shown, and a disk
    // that took 12 s to say nothing would be asked again at each visit, a benchmark on it or not.
    // A drive arriving or leaving, or a hardware or SMART scan, asks again.
    logInfo("Thermal monitor started ({}ms interval)", m_interval_ms);

    // Fire initial poll immediately
    onTimerTick();
}

void ThermalMonitor::stop() {
    // Record stop intent FIRST: during an in-flight poll the timer is inactive, so without this a
    // stop() would be a no-op and onPollComplete would re-arm the timer, resuming forever.
    m_active = false;
    if (m_timer.isActive()) {
        m_timer.stop();
        logInfo("Thermal monitor stopped");
    }
}

bool ThermalMonitor::isRunning() const {
    return m_timer.isActive() || m_poll_watcher.isRunning();
}

QVector<ThermalReading> ThermalMonitor::pollOnce() {
    bool ignored = false;
    return pollOnce(ignored);
}

QVector<ThermalReading> ThermalMonitor::pollOnce(bool& query_ok) {
    // One-shot callers get every unavailable-sensor status logged for this one query.
    ThermalPollResult result = querySensors();
    logSensorStatuses(result.sensor_statuses);
    query_ok = result.query_ok;
    return std::move(result.readings);
}

QStringList ThermalMonitor::newSensorStatuses(const QStringList& statuses,
                                              QSet<QString>* already_logged) {
    Q_ASSERT(already_logged != nullptr);
    QStringList fresh;
    for (const QString& status : statuses) {
        if (!already_logged->contains(status)) {
            already_logged->insert(status);
            fresh.append(status);
        }
    }
    return fresh;
}

ThermalPollResult ThermalMonitor::querySensors(const QStringList& quiet_identities) {
    ThermalPollResult poll;
    // System32-qualified interpreter, never a bare "powershell.exe": the poll can run
    // from an elevated session, so a PATH/CWD-planted powershell must not satisfy it.
    // Unresolvable -> FAILED query (query_ok stays false).
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return poll;
    }
    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NoLogo"),
                                         QStringLiteral("-Command"),
                                         buildCombinedThermalScript(quiet_identities)},
                                        sak::kTimeoutThermalQueryMs);
    if (!result.succeeded()) {
        const QString detail =
            result.std_err.trimmed().isEmpty()
                ? QStringLiteral("exit=%1 timeout=%2").arg(result.exit_code).arg(result.timed_out)
                : result.std_err.trimmed();
        logWarning("Thermal sensor query failed: {}", detail.toStdString());
        return poll;  // query FAILED (non-zero exit / timeout) -> query_ok stays false
    }

    // The script runs to completion and emits key=value lines only for available sensors; an
    // empty parse is a genuine "no readable sensors", not a failure.
    poll.sensor_statuses = sensorStatusLines(result.std_out);
    poll.disk_identities = diskIdentities(result.std_out);
    poll.query_ok = true;
    poll.readings = parseThermalOutput(result.std_out);
    return poll;
}

// ============================================================================
// Timer Callback (main thread)
// ============================================================================

void ThermalMonitor::onTimerTick() {
    if (m_poll_watcher.isRunning()) {
        m_timer.start(m_interval_ms);
        return;
    }

    // Run pollOnce() on the thread pool -- no UI blocking.
    // Lambda (not &pollOnce) to disambiguate the overload set: pollOnce() vs pollOnce(bool&).
    // The bool& overload is taken deliberately: a FAILED query (unresolvable interpreter,
    // non-zero exit, timeout) also yields an empty vector, so without this the periodic path
    // would report a sensor failure as a normal "no readable sensors" tick and say nothing.
    m_poll_watcher.setFuture(QtConcurrent::run([quiet = m_quiet_disks.status_by_identity.keys()] {
        return ThermalMonitor::querySensors(quiet);
    }));
}

void ThermalMonitor::askEveryDiskAgain() {
    m_quiet_disks = {};
}

QStringList ThermalMonitor::rememberQuietDisks(const ThermalPollResult& poll, QuietDisks* quiet) {
    Q_ASSERT(quiet != nullptr);
    QStringList present = poll.disk_identities.values();
    present.sort();
    if (present != quiet->present) {
        // A drive arrived or left. Disk 1 may now be another drive, and one that was asleep may
        // answer now, so every disk is asked again.
        quiet->status_by_identity.clear();
        quiet->present = present;
    }
    static const QRegularExpression disk_status(QStringLiteral("^status_disk(\\d+)=unavailable:"));
    for (const QString& status : poll.sensor_statuses) {
        const QRegularExpressionMatch match = disk_status.match(status);
        if (!match.hasMatch()) {
            continue;
        }
        const QString identity = poll.disk_identities.value(match.captured(1));
        if (isScriptSafeIdentity(identity)) {
            quiet->status_by_identity.insert(identity, status);
        }
    }
    // Not asked this time, each quiet disk still gives the reason it has no reading, under the
    // number it has now.
    QStringList all = poll.sensor_statuses;
    for (auto it = quiet->status_by_identity.cbegin(); it != quiet->status_by_identity.cend();
         ++it) {
        const QString number = poll.disk_identities.key(it.key());
        if (number.isEmpty()) {
            continue;
        }
        QString remembered = it.value();
        remembered.replace(disk_status, QStringLiteral("status_disk%1=unavailable:").arg(number));
        if (!all.contains(remembered)) {
            all.append(remembered);
        }
    }
    return all;
}

void ThermalMonitor::onPollComplete() {
    // If stop() was called while this poll was in flight, drop the stale result and do NOT re-arm
    // the timer (this also stops the dtor's stop()+waitForFinished() teardown from resurrecting
    // it).
    if (!m_active) {
        return;
    }
    ThermalPollResult poll = m_poll_watcher.result();
    if (poll.query_ok) {
        poll.sensor_statuses = rememberQuietDisks(poll, &m_quiet_disks);
    }
    // A sensor that is not there stays not there: say so once, not on every poll.
    logSensorStatuses(newSensorStatuses(poll.sensor_statuses, &m_logged_sensor_statuses));
    if (!poll.query_ok && !m_query_failure_logged) {
        logWarning("Thermal sensor query FAILED; polls report no readings until it succeeds");
        m_query_failure_logged = true;
    } else if (poll.query_ok) {
        m_query_failure_logged = false;
    }
    Q_EMIT sensorStatusesUpdated(
        poll.query_ok ? poll.sensor_statuses
                      : QStringList{QStringLiteral("status_query=unavailable:query_failed")});
    processReadings(poll.readings);
    m_timer.start(m_interval_ms);
}

namespace {
// The reasons a sensor gives no reading, least telling first. When several lines speak of one
// sensor (several disks), the most telling one is shown.
enum class MissingReading {
    NotReported,
    NoAnswer,
    QueryFailed,
    NeedsAdmin
};

[[nodiscard]] MissingReading missingReadingOf(const QString& status) {
    const QString code = status.section(QLatin1Char(':'), -1);
    if (code == QLatin1String("needs_admin")) {
        return MissingReading::NeedsAdmin;  // the one a technician can act on at once
    }
    if (code == QLatin1String("query_failed")) {
        return MissingReading::QueryFailed;
    }
    return code == QLatin1String("no_answer") ? MissingReading::NoAnswer
                                              : MissingReading::NotReported;
}

[[nodiscard]] bool statusIsAbout(const QString& status, const QString& sensor) {
    const QString key =
        status.section(QLatin1Char('='), 0, 0).mid(QString::fromLatin1(kSensorStatusPrefix).size());
    return key == sensor || key == QLatin1String("query") ||
           (sensor == QLatin1String("disk") && key.startsWith(sensor));
}
}  // namespace

QString ThermalMonitor::unavailableReason(const QStringList& statuses, const QString& sensor) {
    MissingReading worst = MissingReading::NotReported;
    for (const QString& status : statuses) {
        if (statusIsAbout(status, sensor)) {
            worst = std::max(worst, missingReadingOf(status));
        }
    }
    switch (worst) {
    case MissingReading::NeedsAdmin:
        return tr("needs administrator rights");
    case MissingReading::QueryFailed:
        return tr("could not be read");
    case MissingReading::NoAnswer:
        return tr("did not answer in time");
    case MissingReading::NotReported:
        break;
    }
    return tr("not reported by this PC");
}

// ============================================================================
// Combined Thermal Script
// ============================================================================

QString ThermalMonitor::buildCombinedThermalScript(const QStringList& quiet_identities) {
    // Single PowerShell invocation that queries CPU, GPU, and all disks.
    // Output format: key=value lines (e.g. "cpu=52.3", "gpu=61", "disk0=38")
    // Unsupported/not-present is a legitimate unavailable state. Any other failure escapes
    // the script and makes the process fail, so pollOnce cannot report it as "no sensors".
    QStringList skip;
    for (const QString& identity : quiet_identities) {
        if (isScriptSafeIdentity(identity)) {
            skip.append(QLatin1Char('\'') + identity + QLatin1Char('\''));
        }
    }
    return QStringLiteral("$r=@{};$skip=@(") + skip.join(QLatin1Char(',')) + QStringLiteral(");") +
           cpuThermalScript() + gpuThermalScript() + diskThermalScript() +
           QStringLiteral("$r.GetEnumerator()|ForEach-Object{\"$($_.Key)=$($_.Value)\"}");
}

QVector<ThermalReading> ThermalMonitor::parseThermalOutput(const QString& output) {
    QVector<ThermalReading> readings;
    const QDateTime now = QDateTime::currentDateTime();

    const auto lines = output.split('\n', Qt::SkipEmptyParts);
    for (const auto& line : lines) {
        const auto trimmed = line.trimmed();
        const int eq_pos = static_cast<int>(trimmed.indexOf('='));
        if (eq_pos <= 0) {
            continue;
        }

        const auto key = trimmed.left(eq_pos).toLower();
        bool ok = false;
        const double temp = trimmed.mid(eq_pos + 1).toDouble(&ok);
        if (!ok || !isPlausibleCelsius(temp)) {
            continue;
        }

        if (key == QLatin1String("cpu")) {
            readings.append(
                {.component = "CPU Package", .temperature_celsius = temp, .timestamp = now});
        } else if (key == QLatin1String("gpu")) {
            readings.append({.component = "GPU", .temperature_celsius = temp, .timestamp = now});
        } else if (key.startsWith(QLatin1String("disk"))) {
            const QString device_id = key.mid(kDiskKeyPrefixLength);
            if (!isDiskDeviceId(device_id)) {
                continue;
            }
            readings.append({.component = QString("Disk %1").arg(device_id),
                             .temperature_celsius = temp,
                             .timestamp = now});
        }
    }
    return readings;
}

// ============================================================================
// Process Readings (thresholds, signals)
// ============================================================================

void ThermalMonitor::processReadings(const QVector<ThermalReading>& readings) {
    for (const auto& reading : readings) {
        const double temp = reading.temperature_celsius;
        if (reading.component == "CPU Package" && temp >= kCpuWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        } else if (reading.component == "GPU" && temp >= kGpuWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        } else if (reading.component.startsWith("Disk") && temp >= kDiskWarningThreshold) {
            Q_EMIT temperatureWarning(reading.component, temp);
        }
    }

    Q_EMIT readingsUpdated(readings);
}

// ============================================================================
// Standalone CPU Query (used by StressTestWorker)
// ============================================================================

double ThermalMonitor::queryCpuTemperature(const std::function<bool()>& should_cancel) {
    // System32-qualified interpreter only; unresolvable -> no reading (-1.0), never a
    // PATH/CWD-resolved launch.
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return -1.0;
    }
    const auto result = sak::runProcess(powershell,
                                        {QStringLiteral("-NoProfile"),
                                         QStringLiteral("-NoLogo"),
                                         QStringLiteral("-Command"),
                                         QStringLiteral("Get-CimInstance -Namespace root/WMI "
                                                        "-ClassName MSAcpi_ThermalZoneTemperature "
                                                        "| Select-Object -First 1 "
                                                        "-ExpandProperty CurrentTemperature")},
                                        sak::kTimeoutProcessShortMs,
                                        should_cancel);

    if (!result.succeeded()) {
        return -1.0;
    }

    const QString output = result.std_out.trimmed();
    bool ok = false;
    const double raw_value = output.toDouble(&ok);
    if (!ok || !std::isfinite(raw_value) || raw_value <= 0) {
        return -1.0;
    }

    // WMI returns temperature in tenths of Kelvin
    const double celsius = (raw_value / kWmiTenthsKelvinDivisor) - kKelvinZeroCelsius;
    // Fail closed: a non-finite or out-of-range value would sail through the caller's
    // "temp >= abort threshold" test (NaN compares false) and defeat thermal protection.
    return isPlausibleCelsius(celsius) ? celsius : -1.0;
}

}  // namespace sak
