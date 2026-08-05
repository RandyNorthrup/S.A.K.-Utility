// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bandwidth_tester.cpp
/// @brief LAN bandwidth via iPerf3 and HTTP-based internet speed testing

#include "sak/bandwidth_tester.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/network_transfer_runner.h"
#include "sak/process_runner.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTimer>
#include <QUuid>

#include <chrono>
#include <memory>
#include <thread>

namespace sak {

namespace {
constexpr int kProcessTimeout = 120'000;         // 2 minutes max for iPerf3
constexpr int kServerStartTimeout = 3000;
constexpr int kSpeedTestChunkBytes = 1'048'576;  // 1 MB
constexpr int kSpeedTestDurationMs = 10'000;     // 10 seconds
constexpr int kIperfProgressPollMs = kTimerRetryBaseMs;
constexpr int kCloudflareLatencyTimeoutMs = kTimeoutProcessShortMs;
constexpr double kBitsPerByte = 8.0;
constexpr double kMegabit = 1'000'000.0;
// Upload ACK / HEAD responses carry no meaningful body; cap them small so a hostile
// server cannot make us buffer a large reply on the control legs of the speed test.
constexpr qint64 kSpeedTestControlResponseCap = 64 * 1024;  // 64 KB

const auto kIperf3Exe = QStringLiteral("iperf3.exe");

QStringList extractAcceptedClientLines(const QString& rawText) {
    const auto text = rawText.trimmed();
    if (!text.contains(QStringLiteral("accepted connection"))) {
        return {};
    }

    QStringList acceptedLines;
    const auto lines = text.split(QLatin1Char('\n'));
    for (const auto& line : lines) {
        if (!line.contains(QStringLiteral("accepted"))) {
            continue;
        }
        acceptedLines.append(line.trimmed());
    }

    return acceptedLines;
}

struct HttpSample {
    double bytes = 0.0;
    double timeMs = 0.0;
};

struct IperfProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;
    QByteArray std_out;
    QByteArray std_err;
    QString error_message;
};

IperfProcessResult runIperfClientProcess(const QString& program,
                                         const QStringList& args,
                                         std::atomic<bool>& cancelled,
                                         int durationSec,
                                         BandwidthTester* tester) {
    IperfProcessResult result;
    std::atomic_bool done{false};
    std::atomic_bool started{false};

    std::thread progress_thread([tester, durationSec, &done, &started]() {
        qint64 elapsed_ms = 0;
        while (!done.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kIperfProgressPollMs));
            if (!started.load() || done.load()) {
                continue;
            }
            elapsed_ms += kIperfProgressPollMs;
            QMetaObject::invokeMethod(
                tester,
                [tester, durationSec, elapsed_ms]() {
                    Q_EMIT tester->testProgress(0.0,
                                                elapsed_ms / kMillisecondsPerSecondF,
                                                static_cast<double>(durationSec));
                },
                Qt::QueuedConnection);
        }
    });

    const ProcessResult process =
        runProcessStreaming({.program = program,
                             .args = args,
                             .timeout_ms = kProcessTimeout,
                             .on_started = [&](qint64) { started = true; },
                             .should_cancel = [&cancelled]() { return cancelled.load(); }});

    done = true;
    if (progress_thread.joinable()) {
        progress_thread.join();
    }

    result.started = started.load();
    result.timed_out = process.timed_out;
    result.cancelled = process.cancelled;
    result.exit_code = process.exit_code;
    result.std_out = process.std_out.toLocal8Bit();
    result.std_err = process.std_err.toLocal8Bit();
    if (process.timed_out) {
        result.error_message = QStringLiteral("iPerf3 process timed out");
    } else if (!process.std_err.isEmpty() && process.exit_code != 0) {
        result.error_message = process.std_err.trimmed();
    }
    return result;
}

HttpSample downloadHttpSample(const QString& url, qint64 maxResponseBytes) {
    // Sole caller runHttpSpeedTest() passes a literal URL and the compile-time
    // kDownloadResponseCap (kDownloadBytes + 1 MiB).
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(maxResponseBytes > 0);
    HttpSample sample;
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.timeout_ms = kSpeedTestDurationMs;
    // Bound the in-memory body: a hostile/oversized response must abort, not be
    // buffered whole. The download URL requests a known byte count, so the cap is
    // that count plus a small margin.
    request.max_response_bytes = maxResponseBytes;
    const auto transfer = runNetworkTransfer(request);
    if (transfer.success) {
        sample.bytes = static_cast<double>(transfer.body.size());
        sample.timeMs = static_cast<double>(transfer.elapsed_ms);
    }
    return sample;
}

HttpSample uploadHttpSample(const QString& url, int payloadBytes) {
    // Sole caller runHttpSpeedTest() passes a literal URL and the compile-time kUploadBytes.
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(payloadBytes >= 0);

    const QByteArray payload(payloadBytes, '\0');
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.method = NetworkTransferMethod::Post;
    request.body = payload;
    request.timeout_ms = kSpeedTestDurationMs;
    request.raw_headers.append(QPair<QByteArray, QByteArray>{
        QByteArrayLiteral("Content-Type"), QByteArrayLiteral("application/octet-stream")});
    request.max_response_bytes = kSpeedTestControlResponseCap;
    const auto transfer = runNetworkTransfer(request);
    HttpSample sample;
    if (transfer.success) {
        sample.bytes = static_cast<double>(payloadBytes);
        sample.timeMs = static_cast<double>(transfer.elapsed_ms);
    }
    return sample;
}

double measureHttpHeadLatencyMs(const QString& url, int timeoutMs) {
    // Sole caller runHttpSpeedTest() passes a literal URL and the compile-time
    // kCloudflareLatencyTimeoutMs.
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(timeoutMs >= 0);
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.method = NetworkTransferMethod::Head;
    request.timeout_ms = timeoutMs;
    request.max_response_bytes = kSpeedTestControlResponseCap;
    const auto transfer = runNetworkTransfer(request);
    if (!transfer.success) {
        // A failed HEAD (timeout / DNS / refused) elapses the full timeout; returning
        // that as the latency would misreport a dead link as a very high RTT. Signal
        // "no measurement" instead.
        return -1.0;
    }
    return static_cast<double>(transfer.elapsed_ms);
}
}  // namespace

BandwidthTester::BandwidthTester(QObject* parent)
    : QObject(parent), m_iperf3Path(findIperf3Path()) {}

BandwidthTester::~BandwidthTester() {
    stopIperfServer();
}

void BandwidthTester::cancel() {
    m_cancelled.store(true);
}

std::optional<double> BandwidthTester::measureTransferMbps(
    int sample_count, const std::function<std::pair<double, double>()>& sampler) {
    double total_bytes = 0.0;
    double total_time_ms = 0.0;

    for (int idx = 0; idx < sample_count; ++idx) {
        if (m_cancelled.load()) {
            return std::nullopt;
        }
        const auto [bytes, time_ms] = sampler();
        total_bytes += bytes;
        total_time_ms += time_ms;
    }

    if (total_time_ms <= 0.0) {
        return 0.0;
    }
    return (total_bytes * kBitsPerByte) / (total_time_ms / kMillisecondsPerSecondF) / kMegabit;
}

bool BandwidthTester::isIperf3Available() const {
    return !m_iperf3Path.isEmpty() && QFileInfo::exists(m_iperf3Path);
}

bool BandwidthTester::isServerRunning() const {
    return m_serverProcess != nullptr && m_serverProcess->state() == QProcess::Running;
}

QString BandwidthTester::findIperf3Path() const {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/tools/iperf3/") + kIperf3Exe,
        appDir + QStringLiteral("/../tools/iperf3/") + kIperf3Exe,
        appDir + QStringLiteral("/") + kIperf3Exe,
    };

    for (const auto& path : candidates) {
        if (QFileInfo::exists(path)) {
            return QDir::cleanPath(path);
        }
    }

    return {};
}

void BandwidthTester::startIperfServer(uint16_t port) {
    if (!isIperf3Available()) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 not found. Place iperf3.exe in tools/iperf3/"));
        return;
    }

    if (isServerRunning()) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 server is already running"));
        return;
    }

    // Unique per-instance rule name: two concurrent servers must not share one, or
    // one stopping would tear down the other's still-needed inbound rule.
    m_firewallRuleName = composeFirewallRuleName(port, QUuid::createUuid().toString(QUuid::Id128));
    createFirewallRule(m_firewallRuleName, port);
    const QString ruleName = m_firewallRuleName;

    m_serverProcess = new QProcess(this);
    m_serverProcess->setProgram(m_iperf3Path);
    m_serverProcess->setArguments({
        QStringLiteral("-s"),
        QStringLiteral("-p"),
        QString::number(port),
        QStringLiteral("--one-off"),
        QStringLiteral("-J"),
    });

    connectServerProcessSignals(ruleName, port);
    m_serverProcess->start();
}

void BandwidthTester::connectServerProcessSignals(const QString& ruleName, uint16_t port) {
    connect(m_serverProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto data = m_serverProcess->readAllStandardOutput();
        for (const auto& line : extractAcceptedClientLines(QString::fromUtf8(data))) {
            Q_EMIT serverClientConnected(line);
        }
    });

    connect(m_serverProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, ruleName]([[maybe_unused]] int exitCode,
                             [[maybe_unused]] QProcess::ExitStatus status) {
                auto* process = qobject_cast<QProcess*>(sender());
                if (process != nullptr && m_serverProcess == process) {
                    m_serverProcess = nullptr;
                    process->deleteLater();
                }
                // Remove THIS server's own rule (captured), never whatever a later
                // server may have installed into m_firewallRuleName.
                removeFirewallRule(ruleName);
                Q_EMIT serverStopped();
            });

    connect(m_serverProcess, &QProcess::started, this, [this, port]() {
        Q_EMIT serverStarted(port);
    });

    connect(
        m_serverProcess,
        &QProcess::errorOccurred,
        this,
        [this, ruleName](QProcess::ProcessError error) {
            // Mirror the finished-handler: act only on the specific process that
            // errored, and only when it is still our current server process. A
            // stale signal from a superseded process must not touch m_serverProcess.
            auto* process = qobject_cast<QProcess*>(sender());
            if (error != QProcess::FailedToStart || process == nullptr ||
                m_serverProcess != process) {
                return;
            }
            Q_EMIT errorOccurred(
                QStringLiteral("Failed to start iPerf3 server: %1").arg(process->errorString()));
            m_serverProcess = nullptr;
            process->deleteLater();
            // FailedToStart never emits finished(), so the finished-handler's cleanup never
            // runs; tear down the inbound firewall rule here or it leaks (idempotent:
            // delete-by-name is harmless if the add had not completed). removeFirewallRule
            // uses its own QProcess, not m_serverProcess.
            removeFirewallRule(ruleName);
        });
}

void BandwidthTester::stopIperfServer() {
    if (m_serverProcess == nullptr) {
        return;
    }

    if (m_serverProcess->state() == QProcess::Running) {
        m_serverProcess->terminate();
        QTimer::singleShot(kServerStartTimeout, m_serverProcess, [process = m_serverProcess]() {
            if (process->state() != QProcess::NotRunning) {
                process->kill();
            }
        });
    }

    removeFirewallRule(m_firewallRuleName);
    m_serverProcess->deleteLater();
    m_serverProcess = nullptr;
}

static QStringList buildIperfClientArgs(const BandwidthTester::IperfConfig& config) {
    QStringList args = {
        QStringLiteral("-c"),
        config.serverAddress,
        QStringLiteral("-p"),
        QString::number(config.port),
        QStringLiteral("-t"),
        QString::number(config.durationSec),
        QStringLiteral("-P"),
        QString::number(config.parallelStreams),
        QStringLiteral("-J"),
    };
    if (config.bidirectional) {
        args.append(QStringLiteral("--bidir"));
    }
    if (config.udpMode) {
        args.append(QStringLiteral("-u"));
        args.append(QStringLiteral("-b"));
        args.append(QStringLiteral("%1M").arg(config.udpBandwidthMbps));
    }
    return args;
}

void BandwidthTester::runIperfTest(const IperfConfig& config) {
    m_cancelled.store(false);

    if (!isIperf3Available()) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 not found. Place iperf3.exe in tools/iperf3/"));
        Q_EMIT testComplete({});
        return;
    }

    if (config.serverAddress.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Server address cannot be empty"));
        Q_EMIT testComplete({});
        return;
    }

    Q_EMIT testStarted(config.serverAddress);

    QStringList args = buildIperfClientArgs(config);

    const auto process =
        runIperfClientProcess(m_iperf3Path, args, m_cancelled, config.durationSec, this);

    if (!process.started) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to start iPerf3 client: %1").arg(process.error_message));
        Q_EMIT testComplete({});
        return;
    }

    if (process.cancelled) {
        Q_EMIT testComplete({});
        return;
    }

    if (process.timed_out) {
        sak::logError("iPerf3 process timed out");
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 process timed out"));
        Q_EMIT testComplete({});
        return;
    }

    if (process.exit_code != 0) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 failed (exit %1): %2")
                                 .arg(process.exit_code)
                                 .arg(QString::fromUtf8(process.std_err)));
        Q_EMIT testComplete({});
        return;
    }

    const auto parsed = parseIperfJson(process.std_out);
    if (!parsed.has_value()) {
        // Exit 0 but the output was not usable iPerf3 JSON: report an error rather
        // than a bogus all-zero "successful" result.
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 completed but returned unparseable output"));
        Q_EMIT testComplete({});
        return;
    }

    auto result = *parsed;
    result.target = config.serverAddress;
    result.mode = BandwidthTestResult::TestMode::LanIperf3;
    result.durationSec = config.durationSec;
    result.parallelStreams = config.parallelStreams;
    result.reverseMode = config.bidirectional;
    result.timestamp = QDateTime::currentDateTime();

    Q_EMIT testComplete(result);
}

std::optional<BandwidthTestResult> BandwidthTester::parseIperfJson(const QByteArray& json) {
    QJsonParseError parseErr;
    const auto doc = QJsonDocument::fromJson(json, &parseErr);
    if (doc.isNull() || !doc.isObject()) {
        return std::nullopt;  // not valid JSON (or an error object) -> not a successful test
    }

    const auto root = doc.object();
    const auto end = root.value(QStringLiteral("end")).toObject();

    // A genuine iperf3 result always carries an `end` summary with a throughput sum
    // (TCP: sum_sent/sum_received; UDP: sum). Its absence means the output was not a
    // real result, which must NOT be surfaced as a successful zero-throughput test.
    const bool hasThroughput = end.contains(QStringLiteral("sum_sent")) ||
                               end.contains(QStringLiteral("sum_received")) ||
                               end.contains(QStringLiteral("sum"));
    if (!hasThroughput) {
        return std::nullopt;
    }

    BandwidthTestResult result;

    // Parse sent/received sum
    const auto sumSent = end.value(QStringLiteral("sum_sent")).toObject();
    const auto sumRecv = end.value(QStringLiteral("sum_received")).toObject();

    result.uploadMbps = sumSent.value(QStringLiteral("bits_per_second")).toDouble() / kMegabit;
    result.downloadMbps = sumRecv.value(QStringLiteral("bits_per_second")).toDouble() / kMegabit;
    result.retransmissions = sumSent.value(QStringLiteral("retransmits")).toDouble();

    // Parse TCP window size from start
    const auto start = root.value(QStringLiteral("start")).toObject();
    const auto connecting = start.value(QStringLiteral("connecting_to")).toObject();
    result.target = connecting.value(QStringLiteral("host")).toString();

    // Parse intervals
    const auto intervals = root.value(QStringLiteral("intervals")).toArray();
    for (const auto& intervalVal : intervals) {
        const auto interval = intervalVal.toObject();
        const auto sum = interval.value(QStringLiteral("sum")).toObject();

        BandwidthTestResult::IntervalData data;
        data.startSec = sum.value(QStringLiteral("start")).toDouble();
        data.endSec = sum.value(QStringLiteral("end")).toDouble();
        data.bitsPerSecond = sum.value(QStringLiteral("bits_per_second")).toDouble();
        data.retransmits = sum.value(QStringLiteral("retransmits")).toInt();

        result.intervals.append(data);
    }

    // Parse UDP specific fields
    if (end.contains(QStringLiteral("sum"))) {
        const auto udpSum = end.value(QStringLiteral("sum")).toObject();
        result.jitterMs = udpSum.value(QStringLiteral("jitter_ms")).toDouble();
        result.packetLossPercent = udpSum.value(QStringLiteral("lost_percent")).toDouble();
    }

    return result;
}

void BandwidthTester::runHttpSpeedTest() {
    m_cancelled.store(false);

    constexpr int kDownloadBytes = 10'000'000;  // 10 MB
    constexpr int kUploadBytes = 2'000'000;     // 2 MB
    // Cap the download body at the requested count plus a 1 MB margin: enough for
    // the expected payload, but bounded so a server returning more aborts.
    constexpr qint64 kDownloadResponseCap = kDownloadBytes + 1'048'576;

    const QString downloadUrl =
        QStringLiteral("https://speed.cloudflare.com/__down?bytes=%1").arg(kDownloadBytes);
    const QString uploadUrl = QStringLiteral("https://speed.cloudflare.com/__up");

    // measureHttpHeadLatencyMs returns a negative value when it could not measure at all.
    // That sentinel is passed through unchanged: it used to be coerced to 0.0, which the
    // controller then logged as "latency 0.0 ms" -- presenting a measurement that failed
    // as a perfect one. Consumers must test for a negative value and say "unknown".
    const double latencyMs = measureHttpHeadLatencyMs(
        QStringLiteral("https://speed.cloudflare.com/__down?bytes=0"), kCloudflareLatencyTimeoutMs);

    if (m_cancelled.load()) {
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
        return;
    }

    constexpr int kSampleCount = 2;
    auto downloadMbps = measureTransferMbps(kSampleCount, [&]() -> std::pair<double, double> {
        auto sample = downloadHttpSample(downloadUrl, kDownloadResponseCap);
        return {sample.bytes, sample.timeMs};
    });
    if (!downloadMbps.has_value()) {
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
        return;
    }

    Q_EMIT httpSpeedTestProgress(*downloadMbps, 0.0);

    if (m_cancelled.load()) {
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
        return;
    }

    auto uploadMbps = measureTransferMbps(kSampleCount, [&]() -> std::pair<double, double> {
        auto sample = uploadHttpSample(uploadUrl, kUploadBytes);
        return {sample.bytes, sample.timeMs};
    });
    if (!uploadMbps.has_value()) {
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
        return;
    }

    if (*downloadMbps > 0.0 || *uploadMbps > 0.0) {
        Q_EMIT httpSpeedTestComplete(*downloadMbps, *uploadMbps, latencyMs);
    } else {
        Q_EMIT errorOccurred(QStringLiteral("HTTP speed test failed: no data transferred"));
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
    }
}

QString BandwidthTester::composeFirewallRuleName(uint16_t port, const QString& uniqueToken) {
    return QStringLiteral("SAK_Utility_iPerf3_%1_%2").arg(port).arg(uniqueToken);
}

QString BandwidthTester::composeNetshPath(const QString& systemRoot) {
    // Fail closed: with no known Windows root we cannot resolve a trusted netsh, and
    // invoking a bare "netsh" would let a PATH/CWD-planted binary run with our (often
    // elevated) rights. Callers treat empty as "do not run".
    if (systemRoot.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(systemRoot + QStringLiteral("/System32/netsh.exe"));
}

QString BandwidthTester::resolveNetshPath() {
    return composeNetshPath(qEnvironmentVariable("SystemRoot"));
}

void BandwidthTester::createFirewallRule(const QString& ruleName, uint16_t port) {
    const QString netsh = resolveNetshPath();
    if (netsh.isEmpty()) {
        sak::logWarning("Cannot resolve netsh.exe path; skipping inbound rule for port {}", port);
        return;
    }
    auto* proc = new QProcess(this);
    // Track the in-flight add so a remove requested before it finishes serializes
    // behind it (see removeFirewallRule) instead of racing and leaking a rule.
    m_firewallAddProcess = proc;
    connect(proc, &QProcess::errorOccurred, proc, [this, port, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            sak::logWarning("Failed to start netsh for firewall rule on port {}: {}",
                            port,
                            proc->errorString().toStdString());
            if (m_firewallAddProcess == proc) {
                m_firewallAddProcess = nullptr;
            }
            proc->deleteLater();
        }
    });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            proc,
            [this, port, proc](int exitCode, [[maybe_unused]] QProcess::ExitStatus status) {
                if (exitCode != 0) {
                    sak::logWarning("Failed to add firewall rule for port {}: {}",
                                    port,
                                    QString::fromUtf8(proc->readAllStandardError()).toStdString());
                }
                if (m_firewallAddProcess == proc) {
                    m_firewallAddProcess = nullptr;
                }
                proc->deleteLater();
            });
    // Scope the inbound allow tightly: private profile only (never public networks),
    // remote peers on the local subnet only, and only the bundled iperf3 binary.
    proc->start(netsh,
                {QStringLiteral("advfirewall"),
                 QStringLiteral("firewall"),
                 QStringLiteral("add"),
                 QStringLiteral("rule"),
                 QStringLiteral("name=%1").arg(ruleName),
                 QStringLiteral("dir=in"),
                 QStringLiteral("action=allow"),
                 QStringLiteral("protocol=tcp"),
                 QStringLiteral("localport=%1").arg(port),
                 QStringLiteral("profile=private"),
                 QStringLiteral("remoteip=LocalSubnet"),
                 QStringLiteral("program=%1").arg(QDir::toNativeSeparators(m_iperf3Path))});
}

void BandwidthTester::removeFirewallRule(const QString& ruleName) {
    // No rule was ever installed (server never started) -> nothing to delete, and
    // deleting name="" would nuke an unrelated unnamed rule set.
    if (ruleName.isEmpty()) {
        return;
    }
    // Serialize behind an in-flight add: deleting the rule name before the add
    // process has installed it would delete nothing, then the add would land and
    // leave a permanent inbound rule. Defer the delete until the add finishes.
    if (m_firewallAddProcess != nullptr && m_firewallAddProcess->state() != QProcess::NotRunning) {
        connect(m_firewallAddProcess,
                QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
                this,
                [this, ruleName]([[maybe_unused]] int exitCode,
                                 [[maybe_unused]] QProcess::ExitStatus status) {
                    executeFirewallDelete(ruleName);
                });
        return;
    }
    executeFirewallDelete(ruleName);
}

void BandwidthTester::executeFirewallDelete(const QString& ruleName) {
    const QString netsh = resolveNetshPath();
    if (netsh.isEmpty()) {
        sak::logWarning("Cannot resolve netsh.exe path; firewall rule '{}' not removed",
                        ruleName.toStdString());
        return;
    }
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::errorOccurred, proc, [proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            sak::logWarning("Failed to start netsh for firewall rule removal: {}",
                            proc->errorString().toStdString());
            proc->deleteLater();
        }
    });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            proc,
            [proc](int exitCode, [[maybe_unused]] QProcess::ExitStatus status) {
                if (exitCode != 0) {
                    sak::logWarning("Failed to remove firewall rule: {}",
                                    QString::fromUtf8(proc->readAllStandardError()).toStdString());
                }
                proc->deleteLater();
            });
    proc->start(netsh,
                {QStringLiteral("advfirewall"),
                 QStringLiteral("firewall"),
                 QStringLiteral("delete"),
                 QStringLiteral("rule"),
                 QStringLiteral("name=%1").arg(ruleName)});
}

}  // namespace sak
