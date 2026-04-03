// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bandwidth_tester.cpp
/// @brief LAN bandwidth via iPerf3 and HTTP-based internet speed testing

#include "sak/bandwidth_tester.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QEventLoop>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTimer>

#include <chrono>

namespace sak {

namespace {
constexpr int kProcessTimeout = 120'000;         // 2 minutes max for iPerf3
constexpr int kServerStartTimeout = 3000;
constexpr int kSpeedTestChunkBytes = 1'048'576;  // 1 MB
constexpr int kSpeedTestDurationMs = 10'000;     // 10 seconds
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

bool waitForIperfWithProgress(QProcess& proc,
                              std::atomic<bool>& cancelled,
                              int durationSec,
                              BandwidthTester* tester) {
    auto elapsed = 0.0;
    while (proc.state() == QProcess::Running && !cancelled.load()) {
        proc.waitForReadyRead(500);
        elapsed += 0.5;
        Q_EMIT tester->testProgress(0.0, elapsed, static_cast<double>(durationSec));
    }

    if (!cancelled.load()) {
        return true;
    }

    proc.terminate();
    if (!proc.waitForFinished(2000)) {
        proc.kill();
        proc.waitForFinished(1000);
    }
    return false;
}

struct HttpSample {
    double bytes = 0.0;
    double timeMs = 0.0;
};

HttpSample downloadHttpSample(const QString& url) {
    Q_ASSERT(!url.isEmpty());
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(kSpeedTestDurationMs);

    const auto start = std::chrono::high_resolution_clock::now();
    QNetworkReply* reply = manager.get(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(kSpeedTestDurationMs, &loop, &QEventLoop::quit);
    loop.exec();
    const auto end = std::chrono::high_resolution_clock::now();

    HttpSample sample;
    if (reply->error() == QNetworkReply::NoError) {
        const auto data = reply->readAll();
        sample.bytes = static_cast<double>(data.size());
        sample.timeMs = std::chrono::duration<double, std::milli>(end - start).count();
    }

    reply->deleteLater();
    return sample;
}

HttpSample uploadHttpSample(const QString& url, int payloadBytes) {
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(payloadBytes >= 0);
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(kSpeedTestDurationMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/octet-stream"));

    const QByteArray payload(payloadBytes, '\0');

    const auto start = std::chrono::high_resolution_clock::now();
    QNetworkReply* reply = manager.post(request, payload);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(kSpeedTestDurationMs, &loop, &QEventLoop::quit);
    loop.exec();
    const auto end = std::chrono::high_resolution_clock::now();

    HttpSample sample;
    if (reply->error() == QNetworkReply::NoError) {
        sample.bytes = static_cast<double>(payloadBytes);
        sample.timeMs = std::chrono::duration<double, std::milli>(end - start).count();
    }

    reply->deleteLater();
    return sample;
}

double measureHttpHeadLatencyMs(const QString& url, int timeoutMs) {
    Q_ASSERT(!url.isEmpty());
    Q_ASSERT(timeoutMs >= 0);
    QNetworkAccessManager manager;
    QNetworkRequest request{QUrl(url)};
    request.setTransferTimeout(timeoutMs);

    const auto start = std::chrono::high_resolution_clock::now();
    QNetworkReply* reply = manager.head(request);

    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();

    const auto end = std::chrono::high_resolution_clock::now();
    reply->deleteLater();
    return std::chrono::duration<double, std::milli>(end - start).count();
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
    return (total_bytes * kBitsPerByte) / (total_time_ms / 1000.0) / kMegabit;
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
    Q_ASSERT(m_serverProcess);
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
                removeFirewallRule();
                Q_EMIT serverStopped();
            });

    m_serverProcess->start();
    if (!m_serverProcess->waitForStarted(kServerStartTimeout)) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to start iPerf3 server: %1")
                                 .arg(m_serverProcess->errorString()));
        delete m_serverProcess;
        m_serverProcess = nullptr;
        return;
    }

    Q_EMIT serverStarted(port);
}

void BandwidthTester::stopIperfServer() {
    Q_ASSERT(m_serverProcess);
    if (m_serverProcess == nullptr) {
        return;
    }

    if (m_serverProcess->state() == QProcess::Running) {
        m_serverProcess->terminate();
        if (!m_serverProcess->waitForFinished(3000)) {
            m_serverProcess->kill();
            m_serverProcess->waitForFinished(2000);
        }
    }

    removeFirewallRule();
    delete m_serverProcess;
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

    QProcess proc;
    proc.setProgram(m_iperf3Path);
    proc.setArguments(args);
    proc.start();

    if (!proc.waitForStarted(kServerStartTimeout)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Failed to start iPerf3 client: %1").arg(proc.errorString()));
        Q_EMIT testComplete({});
        return;
    }

    if (!waitForIperfWithProgress(proc, m_cancelled, config.durationSec, this)) {
        Q_EMIT testComplete({});
        return;
    }

    if (!proc.waitForFinished(kProcessTimeout)) {
        sak::logError("iPerf3 process timed out after test completion");
        proc.kill();
        proc.waitForFinished(2000);
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 process timed out"));
        Q_EMIT testComplete({});
        return;
    }

    const QByteArray output = proc.readAllStandardOutput();
    const QByteArray errOutput = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 failed (exit %1): %2")
                                 .arg(proc.exitCode())
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
        QStringLiteral("https://speed.cloudflare.com/__down?bytes=0"), 5000);

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
    QProcess proc;
    proc.start(QStringLiteral("netsh"),
               {QStringLiteral("advfirewall"),
                QStringLiteral("firewall"),
                QStringLiteral("add"),
                QStringLiteral("rule"),
                QStringLiteral("name=%1").arg(kFirewallRuleName),
                QStringLiteral("dir=in"),
                QStringLiteral("action=allow"),
                QStringLiteral("protocol=tcp"),
                QStringLiteral("localport=%1").arg(port)});
    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logWarning("Failed to start netsh for firewall rule on port {}", port);
        return;
    }
    if (!proc.waitForFinished(5000)) {
        sak::logWarning("Timed out adding firewall rule for port {}", port);
        proc.kill();
        proc.waitForFinished(2000);
    } else if (proc.exitCode() != 0) {
        sak::logWarning("Failed to add firewall rule: {}",
                        QString::fromUtf8(proc.readAllStandardError()).toStdString());
    }
}

void BandwidthTester::removeFirewallRule() {
    QProcess proc;
    proc.start(QStringLiteral("netsh"),
               {QStringLiteral("advfirewall"),
                QStringLiteral("firewall"),
                QStringLiteral("delete"),
                QStringLiteral("rule"),
                QStringLiteral("name=%1").arg(kFirewallRuleName)});
    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs)) {
        sak::logWarning("Failed to start netsh for firewall rule removal");
        return;
    }
    if (!proc.waitForFinished(5000)) {
        sak::logWarning("Timed out removing firewall rule");
        proc.kill();
        proc.waitForFinished(2000);
    } else if (proc.exitCode() != 0) {
        sak::logWarning("Failed to remove firewall rule: {}",
                        QString::fromUtf8(proc.readAllStandardError()).toStdString());
    }
}

}  // namespace sak
