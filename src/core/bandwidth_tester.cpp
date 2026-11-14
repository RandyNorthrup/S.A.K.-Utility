// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bandwidth_tester.cpp
/// @brief LAN bandwidth via iPerf3 and HTTP-based internet speed testing

#include "sak/bandwidth_tester.h"

#include "sak/elevation_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/network_transfer_runner.h"
#include "sak/process_runner.h"
#include "sak/windows_process_job.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <thread>

#include <windows.h>

namespace sak {

namespace {

/// A rule name led by this process's ID, so a later start can tell a crashed owner's rule.
QString ownedFirewallRuleName(uint16_t port) {
    return BandwidthTester::composeFirewallRuleName(
        port,
        QStringLiteral("%1-%2")
            .arg(QCoreApplication::applicationPid())
            .arg(QUuid::createUuid().toString(QUuid::Id128)));
}

/// Keeps the iPerf3 server's kill-on-close Job alive exactly as long as its QProcess (its parent).
class ServerJobHolder final : public QObject {
public:
    ServerJobHolder(std::unique_ptr<WindowsProcessJob> job, QObject* parent)
        : QObject(parent), m_job(std::move(job)) {
        setObjectName(QStringLiteral("iperf3ServerJob"));
    }

    [[nodiscard]] WindowsProcessJob& job() const { return *m_job; }

private:
    std::unique_ptr<WindowsProcessJob> m_job;
};

constexpr int kProcessTimeout = 120'000;      // 2 minutes max for iPerf3
constexpr int kServerStartTimeout = 3000;
constexpr int kSpeedTestDurationMs = 10'000;  // 10 seconds
constexpr int kIperfProgressPollMs = kTimerRetryBaseMs;
constexpr int kCloudflareLatencyTimeoutMs = kTimeoutProcessShortMs;
constexpr double kBitsPerByte = 8.0;
constexpr double kMegabit = 1'000'000.0;
// Upload ACK / HEAD responses carry no meaningful body; cap them small so a hostile
// server cannot make us buffer a large reply on the control legs of the speed test.
constexpr qint64 kSpeedTestControlResponseCap = 64 * 1024;  // 64 KB
// iPerf3 -J output is a compact summary (start/intervals/end); even a long test stays
// far under a megabyte. Cap the bytes before parsing so a corrupt or hostile stream
// cannot drive an unbounded JSON allocation; oversized output fails closed (unparseable).
constexpr qint64 kMaxIperfJsonBytes = 16 * 1024 * 1024;  // 16 MiB

// iPerf3 client parameters arrive from the GUI/AI layer; clamp them to sane bands before they
// reach the argv so a negative, zero, or absurd value cannot produce a nonsensical invocation.
// Actual runtime is still separately bounded by kProcessTimeout.
constexpr int kMinIperfDurationSec = 1;
constexpr int kMaxIperfDurationSec = 3600;    // 1 hour ceiling; well past any real bandwidth run
constexpr int kMinIperfStreams = 1;
constexpr int kMaxIperfStreams = 128;         // iperf3's own parallel-stream maximum
constexpr int kMinUdpBandwidthMbps = 1;
constexpr int kMaxUdpBandwidthMbps = 10'000;  // 10 Gbps

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
                                                static_cast<double>(elapsed_ms) /
                                                    kMillisecondsPerSecondF,
                                                static_cast<double>(durationSec));
                },
                Qt::QueuedConnection);
        }
    });

    const ProcessResult process =
        runProcessStreaming({.program = program,
                             .args = args,
                             .timeout_ms = kProcessTimeout,
                             .on_output = {},
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
    // Treat the brief Starting window (before started() fires) as running too, not just
    // Running: a caller that gates re-entry or teardown on this must see a server that is
    // coming up as live, or it could orphan it and leak its inbound firewall rule.
    return m_serverProcess != nullptr && m_serverProcess->state() != QProcess::NotRunning;
}

QString BandwidthTester::findIperf3Path() const {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates = {
        appDir + QStringLiteral("/tools/iperf3/") + kIperf3Exe,
        appDir + QStringLiteral("/../tools/iperf3/") + kIperf3Exe,
        appDir + QStringLiteral("/") + kIperf3Exe,
    };

    const auto match = std::ranges::find_if(candidates, [](const QString& path) {
        return QFileInfo::exists(path);
    });
    if (match != candidates.cend()) {
        return QDir::cleanPath(*match);
    }
    return {};
}

QStringList BandwidthTester::iperfServerArguments(uint16_t port, const QHostAddress& bind_address) {
    return {
        QStringLiteral("-s"),
        QStringLiteral("-p"),
        QString::number(port),
        // Only the chosen interface: iperf3 otherwise listens on every address this PC has.
        QStringLiteral("-B"),
        bind_address.toString(),
        // A peer that connects and goes quiet is dropped instead of holding the server.
        QStringLiteral("--idle-timeout"),
        QString::number(kIperfServerIdleTimeoutSec),
        QStringLiteral("--one-off"),
        QStringLiteral("-J"),
    };
}

bool BandwidthTester::isSpecificListenAddress(const QHostAddress& address) {
    return !address.isNull() && address != QHostAddress(QHostAddress::Any) &&
           address != QHostAddress(QHostAddress::AnyIPv4) &&
           address != QHostAddress(QHostAddress::AnyIPv6) && !address.isBroadcast() &&
           !address.isMulticast();
}

bool BandwidthTester::serverStartRefused(const QHostAddress& bind_address) {
    if (!isSpecificListenAddress(bind_address)) {
        Q_EMIT errorOccurred(
            QStringLiteral("Choose the network adapter address the iPerf3 server listens on"));
        return true;
    }
    if (!isIperf3Available()) {
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 not found. Place iperf3.exe in tools/iperf3/"));
        return true;
    }
    if (m_serverProcess != nullptr) {
        // Reject re-entry while a server object exists in ANY state -- waiting for its firewall
        // rule, Starting before started() fires, or Running. Guarding on the pointer (not the
        // process state) means a second start cannot overwrite m_serverProcess and
        // m_firewallRuleName, orphaning the first server and leaking its inbound rule. Both
        // teardown handlers null m_serverProcess, so a genuine restart still proceeds.
        Q_EMIT errorOccurred(QStringLiteral("iPerf3 server is already running"));
        return true;
    }
    return false;
}

QProcess* BandwidthTester::containedServerProcess(uint16_t port,
                                                  const QHostAddress& bind_address,
                                                  const QString& ruleName) {
    // The server runs inside a kill-on-close Job: if this app crashes, closing the Job ends the
    // server too, instead of leaving it listening (and its inbound rule allowing) for good.
    auto created_job = WindowsProcessJob::create();
    if (!created_job) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot contain the iPerf3 server: %1").arg(created_job.error()));
        return nullptr;
    }
    std::unique_ptr<WindowsProcessJob> job = std::move(*created_job);
    m_serverProcess = new QProcess(this);
    QProcess* const server = m_serverProcess;  // a FailedToStart handler nulls the member
    server->setProgram(m_iperf3Path);
    server->setArguments(iperfServerArguments(port, bind_address));
    connectServerProcessSignals(ruleName, port);
    if (const auto installed = job->installOn(*server); !installed) {
        Q_EMIT errorOccurred(
            QStringLiteral("Cannot contain the iPerf3 server: %1").arg(installed.error()));
        m_serverProcess = nullptr;
        server->deleteLater();
        return nullptr;
    }
    // The Job lives exactly as long as the server's QProcess: ~QProcess stops and reaps the
    // server first, then this child closes the emptied Job.
    new ServerJobHolder(std::move(job), server);
    return server;
}

bool BandwidthTester::serverNeedsElevation(const QHostAddress& bind_address, bool elevated) {
    return !elevated && !bind_address.isLoopback();
}

QString BandwidthTester::needsElevationMessage() {
    return QStringLiteral(
        "Run as Admin to start the iPerf3 server: adding its firewall rule needs administrator "
        "rights.");
}

QString BandwidthTester::firewallRuleRefusalMessage(const QString& why) {
    // netsh ends its own sentence, sometimes after a bracket ("... (Run as administrator)."):
    // its last full stop goes, and this message adds the one it ends with.
    QString reason = why.simplified();
    while (!reason.isEmpty() && QStringLiteral(".!?;:,").contains(reason.back())) {
        reason.chop(1);
        reason = reason.trimmed();
    }
    const QString lead =
        QStringLiteral("iPerf3 server not started: Windows Firewall did not add its inbound rule");
    return reason.isEmpty() ? lead + QLatin1Char('.') : QStringLiteral("%1: %2.").arg(lead, reason);
}

QString BandwidthTester::netshFailureReason(int exit_code,
                                            QProcess::ExitStatus status,
                                            const QString& output) {
    QString said = output.simplified();
    if (!said.isEmpty()) {
        return said;
    }
    if (status == QProcess::CrashExit) {
        return QStringLiteral("netsh was stopped before it finished");
    }
    return QStringLiteral("netsh exited with code %1 and said nothing").arg(exit_code);
}

void BandwidthTester::startIperfServer(uint16_t port, const QHostAddress& bind_address) {
    if (serverStartRefused(bind_address)) {
        return;
    }
    // A server other PCs can reach needs its inbound rule, and adding one needs administrator
    // rights: without them nothing runs at all (no rule sweep, no server process, no netsh).
    const bool elevated = m_elevatedForTesting.value_or(ElevationManager::isElevated());
    if (serverNeedsElevation(bind_address, elevated)) {
        sak::logWarning("iPerf3 server not started on {}: S.A.K. is not running as administrator",
                        bind_address.toString().toStdString());
        Q_EMIT errorOccurred(needsElevationMessage());
        return;
    }
    // A crash before this change (or of an older build) left its rule behind; sweep those first.
    removeStaleFirewallRules();

    // Unique per-instance rule name: two concurrent servers must not share one, or one stopping
    // would tear down the other's still-needed inbound rule. The token leads with this process's
    // ID so a later start can tell a crashed owner's rule from a live one's. Windows Firewall does
    // not filter loopback, so a server only this PC can reach has no rule to add.
    const bool needs_rule = !bind_address.isLoopback();
    m_firewallRuleName = needs_rule ? ownedFirewallRuleName(port) : QString();
    const QString ruleName = m_firewallRuleName;
    QProcess* const server = containedServerProcess(port, bind_address, ruleName);
    if (server == nullptr) {
        m_firewallRuleName.clear();
        return;
    }
    if (!needs_rule) {
        launchServer(server);
        return;
    }
    // Not started until its rule is in place. Without the rule other PCs cannot reach the server
    // anyway, and Windows would ask the user whether to let iperf3.exe through -- on public
    // networks as well -- instead of the narrow rule this adds.
    createFirewallRule(ruleName,
                       port,
                       bind_address,
                       [this, pending = QPointer<QProcess>(server)](bool added,
                                                                    const QString& why) {
                           if (pending != nullptr && m_serverProcess == pending) {
                               onServerFirewallRule(pending, added, why);
                           }
                       });
}

void BandwidthTester::onServerFirewallRule(QProcess* server, bool added, const QString& why) {
    if (added) {
        launchServer(server);
        return;
    }
    sak::logWarning("iPerf3 server not started: inbound firewall rule not added: {}",
                    why.toStdString());
    // Elevated here (startIperfServer refuses earlier without it), so the cause is netsh's own.
    Q_EMIT errorOccurred(firewallRuleRefusalMessage(why));
    m_serverProcess = nullptr;
    m_firewallRuleName.clear();
    server->deleteLater();
}

void BandwidthTester::launchServer(QProcess* server) {
    // The one child by that name is the ServerJobHolder containedServerProcess attached (no
    // Q_OBJECT, so it is found by name rather than by type).
    const auto* holder = static_cast<const ServerJobHolder*>(
        server->findChild<QObject*>(QStringLiteral("iperf3ServerJob"), Qt::FindDirectChildrenOnly));
    Q_ASSERT(holder != nullptr);
    server->start();
    const bool started = server->waitForStarted(sak::kTimeoutProcessStartMs);
    if (const auto completed = holder->job().completeLaunch(*server, started); !completed) {
        // Fail closed: a server the Job did not capture must not keep running.
        Q_EMIT errorOccurred(
            QStringLiteral("iPerf3 server was not contained: %1").arg(completed.error()));
        server->kill();
    }
}

void BandwidthTester::connectServerProcessSignals(const QString& ruleName, uint16_t port) {
    connect(m_serverProcess, &QProcess::readyReadStandardOutput, this, [this]() {
        // Read from the process that actually emitted the signal, not the mutable
        // m_serverProcess member: after a stop (member nulled, deleteLater queued) a
        // still-queued readyRead would otherwise dereference a null or superseded pointer.
        auto* process = qobject_cast<QProcess*>(sender());
        if (process == nullptr) {
            return;
        }
        const auto data = process->readAllStandardOutput();
        for (const auto& line : extractAcceptedClientLines(QString::fromUtf8(data))) {
            Q_EMIT serverClientConnected(line);
        }
    });

    connect(m_serverProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, ruleName](int, QProcess::ExitStatus) {
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

// Return a copy of @p config with duration/streams/UDP-bandwidth clamped to sane bands. The
// caller-supplied values reach both the iperf3 argv and the reported result metadata, so a
// negative, zero, or absurd value must be corrected here rather than passed through.
static BandwidthTester::IperfConfig clampIperfConfig(const BandwidthTester::IperfConfig& config) {
    BandwidthTester::IperfConfig clamped = config;
    clamped.durationSec =
        std::clamp(config.durationSec, kMinIperfDurationSec, kMaxIperfDurationSec);
    clamped.parallelStreams =
        std::clamp(config.parallelStreams, kMinIperfStreams, kMaxIperfStreams);
    clamped.udpBandwidthMbps =
        std::clamp(config.udpBandwidthMbps, kMinUdpBandwidthMbps, kMaxUdpBandwidthMbps);
    return clamped;
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

void BandwidthTester::runIperfTest(const IperfConfig& requested) {
    m_cancelled.store(false);
    const IperfConfig config = clampIperfConfig(requested);

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

    const QStringList args = buildIperfClientArgs(config);

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
    if (json.size() > kMaxIperfJsonBytes) {
        return std::nullopt;  // oversized output is not a usable iPerf3 result
    }
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
        // A UDP test carries throughput only in end.sum (TCP uses sum_sent/sum_received,
        // read above), so pull it here or the run would report a bogus 0 Mbps success.
        const double udpBitsPerSecond = udpSum.value(QStringLiteral("bits_per_second")).toDouble();
        result.uploadMbps = udpBitsPerSecond / kMegabit;
        result.downloadMbps = udpBitsPerSecond / kMegabit;
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

    const QString host = QString::fromLatin1(kHttpSpeedTestHost);
    const QString downloadUrl =
        QStringLiteral("https://%1/__down?bytes=%2").arg(host).arg(kDownloadBytes);
    const QString uploadUrl = QStringLiteral("https://%1/__up").arg(host);

    // measureHttpHeadLatencyMs returns a negative value when it could not measure at all.
    // That sentinel is passed through unchanged: it used to be coerced to 0.0, which the
    // controller then logged as "latency 0.0 ms" -- presenting a measurement that failed
    // as a perfect one. Consumers must test for a negative value and say "unknown".
    const double latencyMs = measureHttpHeadLatencyMs(
        QStringLiteral("https://%1/__down?bytes=0").arg(host), kCloudflareLatencyTimeoutMs);

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

    // Fail closed: require BOTH legs to have measured throughput. A leg whose transfers all
    // failed sums to 0 bytes / 0 time -> 0.0 Mbps here, indistinguishable from a real zero,
    // so reporting the run "complete" with one leg at 0.0 would present a failed measurement
    // as a result. Only a two-legged success is surfaced as a completed test.
    if (*downloadMbps > 0.0 && *uploadMbps > 0.0) {
        Q_EMIT httpSpeedTestComplete(*downloadMbps, *uploadMbps, latencyMs);
    } else {
        Q_EMIT errorOccurred(
            QStringLiteral("HTTP speed test failed: download or upload reported no throughput"));
        Q_EMIT httpSpeedTestComplete(0.0, 0.0, 0.0);
    }
}

QString BandwidthTester::composeFirewallRuleName(uint16_t port, const QString& uniqueToken) {
    return QStringLiteral("SAK_Utility_iPerf3_%1_%2").arg(port).arg(uniqueToken);
}

QStringList BandwidthTester::staleFirewallRules(const QStringList& rule_names,
                                                qint64 own_pid,
                                                const std::function<bool(qint64)>& sak_alive) {
    static const QRegularExpression rule_pattern(
        QStringLiteral("^SAK_Utility_iPerf3_\\d+_(?:(\\d+)-)?[0-9A-Za-z]+$"));
    QStringList stale;
    for (const QString& name : rule_names) {
        const auto match = rule_pattern.match(name.trimmed());
        if (!match.hasMatch()) {
            continue;  // not one of ours: never touched
        }
        if (match.capturedLength(1) == 0) {
            stale.append(name.trimmed());  // predates the owner ID; no live owner can claim it
            continue;
        }
        const qint64 owner = match.captured(1).toLongLong();
        if (owner == own_pid || (sak_alive && sak_alive(owner))) {
            continue;
        }
        stale.append(name.trimmed());
    }
    return stale;
}

namespace {
/// True when @p pid is a running process with the same image name as this application.
bool isLiveSakProcess(qint64 pid) {
    HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        return false;
    }
    std::array<wchar_t, MAX_PATH> image{};
    DWORD length = static_cast<DWORD>(image.size());
    DWORD exit_code = 0;
    const bool running = ::GetExitCodeProcess(process, &exit_code) != 0 &&
                         exit_code == STILL_ACTIVE &&
                         ::QueryFullProcessImageNameW(process, 0, image.data(), &length) != 0;
    ::CloseHandle(process);
    if (!running) {
        return false;
    }
    const QString name = QFileInfo(QString::fromWCharArray(image.data(), length)).fileName();
    return name.compare(QFileInfo(QCoreApplication::applicationFilePath()).fileName(),
                        Qt::CaseInsensitive) == 0;
}
}  // namespace

void BandwidthTester::removeStaleFirewallRules() {
    const QString powershell = sak::systemPowerShellPath();
    if (powershell.isEmpty()) {
        return;
    }
    auto* lister = new QProcess(this);
    connect(lister,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, lister](int exit_code, QProcess::ExitStatus) {
                const QStringList names = QString::fromUtf8(lister->readAllStandardOutput())
                                              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
                lister->deleteLater();
                if (exit_code != 0) {
                    return;  // could not list; leave every rule alone
                }
                const QStringList stale =
                    staleFirewallRules(names, QCoreApplication::applicationPid(), isLiveSakProcess);
                for (const QString& rule : stale) {
                    sak::logInfo("Removing iPerf3 firewall rule left by an earlier run: {}",
                                 rule.toStdString());
                    removeFirewallRule(rule);
                }
            });
    connect(lister, &QProcess::errorOccurred, lister, [lister](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) {
            lister->deleteLater();
        }
    });
    lister->start(powershell,
                  {QStringLiteral("-NoProfile"),
                   QStringLiteral("-NonInteractive"),
                   QStringLiteral("-Command"),
                   // A wildcard that matches nothing is not an error: an empty list, exit 0.
                   QStringLiteral("Get-NetFirewallRule -DisplayName 'SAK_Utility_iPerf3_*' | "
                                  "ForEach-Object DisplayName")});
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
    // Resolve netsh from the AUTHORITATIVE system directory (GetSystemDirectoryW, via
    // system32Path), never the attacker-influenceable %SystemRoot% environment variable:
    // that env var can name a relative or planted directory, and this binary runs with our
    // (often elevated) rights. Fails closed (empty) if the system directory is unresolvable.
    return system32Path(QStringLiteral("netsh.exe"));
}

namespace {

/// Why the netsh call @p proc made did not succeed, from all it wrote (BandwidthTester::
/// netshFailureReason): netsh reports a refusal (elevation, policy) on standard output.
QString netshProcessFailure(QProcess* proc, int exit_code, QProcess::ExitStatus status) {
    return BandwidthTester::netshFailureReason(
        exit_code,
        status,
        QString::fromLocal8Bit(proc->readAllStandardOutput() + proc->readAllStandardError()));
}

}  // namespace

void BandwidthTester::createFirewallRule(const QString& ruleName,
                                         uint16_t port,
                                         const QHostAddress& bind_address,
                                         FirewallRuleDone done) {
    const QString netsh = resolveNetshPath();
    if (netsh.isEmpty()) {
        sak::logWarning("Cannot resolve netsh.exe path; no inbound rule for port {}", port);
        done(false, QStringLiteral("netsh.exe was not found in the Windows system folder"));
        return;
    }
    auto* proc = new QProcess(this);
    // Track the in-flight add so a remove requested before it finishes serializes
    // behind it (see removeFirewallRule) instead of racing and leaking a rule.
    m_firewallAddProcess = proc;
    connect(proc,
            &QProcess::errorOccurred,
            proc,
            [this, port, proc, done](QProcess::ProcessError error) {
                if (error != QProcess::FailedToStart) {
                    return;
                }
                sak::logWarning("Failed to start netsh for firewall rule on port {}: {}",
                                port,
                                proc->errorString().toStdString());
                if (m_firewallAddProcess == proc) {
                    m_firewallAddProcess = nullptr;
                }
                proc->deleteLater();
                done(false, proc->errorString());
            });
    connect(proc,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            proc,
            [this, port, proc, done](int exitCode, QProcess::ExitStatus status) {
                const bool added = exitCode == 0 && status == QProcess::NormalExit;
                const QString why = added ? QString() : netshProcessFailure(proc, exitCode, status);
                if (!added) {
                    sak::logWarning("Failed to add firewall rule for port {}: {}",
                                    port,
                                    why.toStdString());
                }
                if (m_firewallAddProcess == proc) {
                    m_firewallAddProcess = nullptr;
                }
                proc->deleteLater();
                done(added, why);
            });
    // Scope the inbound allow tightly: private profile only (never public networks),
    // remote peers on the local subnet only, the address the server is bound to, and only
    // the bundled iperf3 binary.
    proc->start(netsh,
                {QStringLiteral("advfirewall"),
                 QStringLiteral("firewall"),
                 QStringLiteral("add"),
                 QStringLiteral("rule"),
                 QStringLiteral("name=%1").arg(ruleName),
                 QStringLiteral("dir=in"),
                 QStringLiteral("action=allow"),
                 QStringLiteral("protocol=tcp"),
                 QStringLiteral("localip=%1").arg(bind_address.toString()),
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
                [this, ruleName](int, QProcess::ExitStatus) { executeFirewallDelete(ruleName); });
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
            [proc](int exitCode, QProcess::ExitStatus status) {
                if (exitCode != 0 || status != QProcess::NormalExit) {
                    sak::logWarning(
                        "Failed to remove firewall rule: {}",
                        netshFailureReason(exitCode,
                                           status,
                                           QString::fromLocal8Bit(proc->readAllStandardOutput() +
                                                                  proc->readAllStandardError()))
                            .toStdString());
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
