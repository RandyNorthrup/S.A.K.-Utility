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

const auto kIperf3Exe = QStringLiteral("iperf3.exe");
const auto kFirewallRuleName = QStringLiteral("SAK_Utility_iPerf3");

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

HttpSample downloadHttpSample(const QString& url) {
    Q_ASSERT(!url.isEmpty());
    HttpSample sample;
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.timeout_ms = kSpeedTestDurationMs;
    const auto transfer = runNetworkTransfer(request);
    if (transfer.success) {
        sample.bytes = static_cast<double>(transfer.body.size());
        sample.timeMs = static_cast<double>(transfer.elapsed_ms);
    }
    return sample;
}

HttpSample uploadHttpSample(const QString& url, int payloadBytes) {
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
    const auto transfer = runNetworkTransfer(request);
    HttpSample sample;
    if (transfer.success) {
        sample.bytes = static_cast<double>(payloadBytes);
        sample.timeMs = static_cast<double>(transfer.elapsed_ms);
    }
    return sample;
}

double measureHttpHeadLatencyMs(const QString& url, int timeoutMs) {
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(timeoutMs >= 0);
    NetworkTransferRequest request;
    request.url = QUrl(url);
    request.method = NetworkTransferMethod::Head;
    request.timeout_ms = timeoutMs;
    const auto transfer = runNetworkTransfer(request);
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

    createFirewallRule(port);

    m_serverProcess = new QProcess(this);
    m_serverProcess->setProgram(m_iperf3Path);
    m_serverProcess->setArguments({
        QStringLiteral("-s"),
        QStringLiteral("-p"),
        QString::number(port),
        QStringLiteral("--one-off"),
        QStringLiteral("-J"),
    });

    connect(m_serverProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        const auto data = m_serverProcess->readAllStandardOutput();
        for (const auto& line : extractAcceptedClientLines(QString::fromUtf8(data))) {
            Q_EMIT serverClientConnected(line);
        }
    });

    connect(m_serverProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this]([[maybe_unused]] int exitCode, [[maybe_unused]] QProcess::ExitStatus status) {
                auto* process = qobject_cast<QProcess*>(sender());
                if (process != nullptr && m_serverProcess == process) {
                    m_serverProcess = nullptr;
                    process->deleteLater();
                }
                removeFirewallRule();
                Q_EMIT serverStopped();
            });

    connect(m_serverProcess, &QProcess::started, this, [this, port]() {
        Q_EMIT serverStarted(port);
    });

    connect(m_serverProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart || m_serverProcess == nullptr) {
            return;
        }
        Q_EMIT errorOccurred(QStringLiteral("Failed to start iPerf3 server: %1")
                                 .arg(m_serverProcess->errorString()));
        m_serverProcess->deleteLater();
        m_serverProcess = nullptr;
    });

    m_serverProcess->start();
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

    removeFirewallRule();
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

    const QByteArray output = process.std_out;
    const QByteArray errOutput = process.std_err;

    if (process.exit_code != 0) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 failed (exit %1): %2")
                                 .arg(process.exit_code)
                                 .arg(QString::fromUtf8(errOutput)));
        Q_EMIT testComplete({});
        return;
    }

    auto result = parseIperfJson(output);
    result.target = config.serverAddress;
    result.mode = BandwidthTestResult::TestMode::LanIperf3;
    result.durationSec = config.durationSec;
    result.parallelStreams = config.parallelStreams;
    result.reverseMode = config.bidirectional;
    result.timestamp = QDateTime::currentDateTime();

    Q_EMIT testComplete(result);
}

BandwidthTestResult BandwidthTester::parseIperfJson(const QByteArray& json) {
    Q_ASSERT(!json.isEmpty());
    BandwidthTestResult result;

    QJsonParseError parseErr;
    const auto doc = QJsonDocument::fromJson(json, &parseErr);
    if (doc.isNull()) {
        return result;
    }

    const auto root = doc.object();
    const auto end = root.value(QStringLiteral("end")).toObject();

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

    const QString downloadUrl =
        QStringLiteral("https://speed.cloudflare.com/__down?bytes=%1").arg(kDownloadBytes);
    const QString uploadUrl = QStringLiteral("https://speed.cloudflare.com/__up");

    double latencyMs = measureHttpHeadLatencyMs(
        QStringLiteral("https://speed.cloudflare.com/__down?bytes=0"), kCloudflareLatencyTimeoutMs);

    if (m_cancelled.load()) {
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
        return;
    }

    constexpr int kSampleCount = 2;
    auto downloadMbps = measureTransferMbps(kSampleCount, [&]() -> std::pair<double, double> {
        auto sample = downloadHttpSample(downloadUrl);
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

void BandwidthTester::createFirewallRule(uint16_t port) {
    auto* proc = new QProcess(this);
    connect(proc, &QProcess::errorOccurred, proc, [port, proc](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            sak::logWarning("Failed to start netsh for firewall rule on port {}: {}",
                            port,
                            proc->errorString().toStdString());
            proc->deleteLater();
        }
    });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            proc,
            [port, proc](int exitCode, [[maybe_unused]] QProcess::ExitStatus status) {
                if (exitCode != 0) {
                    sak::logWarning("Failed to add firewall rule for port {}: {}",
                                    port,
                                    QString::fromUtf8(proc->readAllStandardError()).toStdString());
                }
                proc->deleteLater();
            });
    proc->start(QStringLiteral("netsh"),
                {QStringLiteral("advfirewall"),
                 QStringLiteral("firewall"),
                 QStringLiteral("add"),
                 QStringLiteral("rule"),
                 QStringLiteral("name=%1").arg(kFirewallRuleName),
                 QStringLiteral("dir=in"),
                 QStringLiteral("action=allow"),
                 QStringLiteral("protocol=tcp"),
                 QStringLiteral("localport=%1").arg(port)});
}

void BandwidthTester::removeFirewallRule() {
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
    proc->start(QStringLiteral("netsh"),
                {QStringLiteral("advfirewall"),
                 QStringLiteral("firewall"),
                 QStringLiteral("delete"),
                 QStringLiteral("rule"),
                 QStringLiteral("name=%1").arg(kFirewallRuleName)});
}

}  // namespace sak
