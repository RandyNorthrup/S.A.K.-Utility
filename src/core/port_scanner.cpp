// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file port_scanner.cpp
/// @brief TCP connect scanning with service fingerprinting

#include "sak/port_scanner.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QSet>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <future>
#include <memory>
#include <system_error>
#include <utility>
#include <vector>

namespace sak {

namespace {
constexpr int kBannerGrabTimeout = netdiag::kBannerGrabTimeoutMs;
constexpr int kBannerMaxRead = netdiag::kBannerMaxBytes;
constexpr int kMaxScanConcurrency = 256;  // upper bound on ports probed at once
// A per-connect timeout must be a positive, bounded QTimer interval: a value <= 0 makes
// the connect timer never fire (so a filtered host hangs runTcpProbe's wait forever) and
// INT_MAX would block a probe for ~25 days. Clamp the untrusted config value fail-closed.
constexpr int kMinPortScanTimeoutMs = 1;
constexpr int kMaxPortScanTimeoutMs = 60'000;
// Grace beyond a probe's own connect+banner timeouts before its blocking wait gives up;
// only a genuine QThread start failure can consume it (matches the ai-client semaphore
// grace). Kept generous so no normal probe is ever cut short.
constexpr int kProbeThreadStartGraceMs = 30'000;
// A real hostname is <= 253 bytes and an IPv6 literal ~45; reject anything longer rather
// than hand an unbounded string to connectToHost / the HTTP Host header.
constexpr int kMaxTargetLength = 255;
constexpr auto kHttpHeadProbe = "HEAD / HTTP/1.0\r\nHost: ";
constexpr auto kHttpHeaderTerminator = "\r\n\r\n";

struct TcpProbeResult {
    bool connected{false};
    bool timed_out{false};
    QString error_message;
    /// The socket error enum, so callers classify (e.g. connection-refused) via the
    /// stable value rather than the localized errorString() text.
    QAbstractSocket::SocketError socket_error{QAbstractSocket::UnknownSocketError};
    QByteArray banner;
    double response_time_ms{0.0};
};

struct TcpProbeSettings {
    QString target;
    int banner_timeout_ms{0};
    bool grab_banner{false};
};

struct TcpProbeContext {
    TcpProbeContext(TcpProbeResult& resultRef,
                    QThread& workerThread,
                    QSemaphore& completionSignal,
                    TcpProbeSettings probeSettings)
        : result(resultRef)
        , thread(workerThread)
        , done(completionSignal)
        , target(std::move(probeSettings.target))
        , banner_timeout_ms(probeSettings.banner_timeout_ms)
        , grab_banner(probeSettings.grab_banner) {}

    TcpProbeResult& result;
    QThread& thread;
    QSemaphore& done;
    QString target;
    int banner_timeout_ms{0};
    bool grab_banner{false};
    QObject* owner{nullptr};
    QTcpSocket* socket{nullptr};
    QTimer* connect_timer{nullptr};
    QTimer* banner_timer{nullptr};
    QTimer* probe_timer{nullptr};
    QElapsedTimer elapsed;
    bool finished{false};
    bool probe_sent{false};
};

using TcpProbeContextPtr = std::shared_ptr<TcpProbeContext>;

void finishTcpProbe(const TcpProbeContextPtr& probe) {
    if (probe->finished) {
        return;
    }
    probe->finished = true;
    probe->result.response_time_ms = probe->elapsed.elapsed();
    probe->socket->disconnectFromHost();
    probe->socket->deleteLater();
    probe->owner->deleteLater();
    probe->thread.quit();
    probe->done.release();
}

void sendHttpBannerProbe(const TcpProbeContextPtr& probe) {
    if (probe->probe_sent) {
        return;
    }
    probe->probe_sent = true;
    QByteArray request{kHttpHeadProbe};
    request += probe->target.toLatin1();
    request += kHttpHeaderTerminator;
    probe->socket->write(request);
    probe->socket->flush();
    probe->banner_timer->start(probe->banner_timeout_ms);
}

void connectTcpProbeTimers(const TcpProbeContextPtr& probe) {
    QObject::connect(probe->connect_timer, &QTimer::timeout, probe->owner, [probe]() {
        probe->result.timed_out = true;
        probe->result.error_message = QStringLiteral("Connection timed out");
        probe->socket->abort();
        finishTcpProbe(probe);
    });
    QObject::connect(probe->banner_timer, &QTimer::timeout, probe->owner, [probe]() {
        finishTcpProbe(probe);
    });
    QObject::connect(probe->probe_timer, &QTimer::timeout, probe->owner, [probe]() {
        sendHttpBannerProbe(probe);
    });
}

void connectTcpProbeSocket(const TcpProbeContextPtr& probe) {
    QObject::connect(probe->socket, &QTcpSocket::connected, probe->owner, [probe]() {
        probe->connect_timer->stop();
        probe->result.connected = true;
        probe->result.response_time_ms = probe->elapsed.elapsed();
        if (!probe->grab_banner) {
            finishTcpProbe(probe);
            return;
        }
        probe->probe_timer->start(probe->banner_timeout_ms);
    });
    QObject::connect(probe->socket, &QTcpSocket::readyRead, probe->owner, [probe]() {
        probe->result.banner += probe->socket->read(kBannerMaxRead);
        finishTcpProbe(probe);
    });
    QObject::connect(probe->socket, &QTcpSocket::disconnected, probe->owner, [probe]() {
        if (probe->result.connected) {
            finishTcpProbe(probe);
        }
    });
    QObject::connect(probe->socket,
                     &QTcpSocket::errorOccurred,
                     probe->owner,
                     [probe](QAbstractSocket::SocketError error) {
                         if (probe->finished || error == QAbstractSocket::RemoteHostClosedError) {
                             return;
                         }
                         probe->result.socket_error = error;
                         probe->result.error_message = probe->socket->errorString();
                         finishTcpProbe(probe);
                     });
}

void startTcpProbe(const TcpProbeContextPtr& probe, uint16_t port, int connectTimeoutMs) {
    probe->socket = new QTcpSocket(probe->owner);
    probe->connect_timer = new QTimer(probe->owner);
    probe->connect_timer->setSingleShot(true);
    probe->banner_timer = new QTimer(probe->owner);
    probe->banner_timer->setSingleShot(true);
    probe->probe_timer = new QTimer(probe->owner);
    probe->probe_timer->setSingleShot(true);
    connectTcpProbeTimers(probe);
    connectTcpProbeSocket(probe);

    probe->elapsed.start();
    probe->connect_timer->start(connectTimeoutMs);
    probe->socket->connectToHost(probe->target, port);
}

TcpProbeResult runTcpProbe(const QString& target,
                           uint16_t port,
                           int connectTimeoutMs,
                           bool grabBanner,
                           int bannerTimeoutMs) {
    TcpProbeResult result;
    QThread thread;
    QSemaphore done;
    auto* context = new QObject();
    context->moveToThread(&thread);
    TcpProbeSettings settings{target, bannerTimeoutMs, grabBanner};
    auto probe = std::make_shared<TcpProbeContext>(result, thread, done, std::move(settings));
    probe->owner = context;

    QObject::connect(&thread, &QThread::started, context, [probe, port, connectTimeoutMs]() {
        startTcpProbe(probe, port, connectTimeoutMs);
    });

    thread.start();
    // Bound the wait. On every normal path the probe releases `done` from its own
    // connect/banner timers (the connect timer's interval is clamped positive by the
    // caller, so it always fires within connectTimeoutMs). Only a genuine QThread start
    // failure -- started() never runs, so nothing ever releases `done` -- can leave it
    // unsignalled, and an unconditional acquire would then hang this scan thread forever.
    // Fail closed: give up after a budget that already exceeds the probe's own timeouts.
    const int waitBudgetMs = connectTimeoutMs + 2 * bannerTimeoutMs + kProbeThreadStartGraceMs;
    if (!done.tryAcquire(1, waitBudgetMs)) {
        result.timed_out = true;
        result.error_message = QStringLiteral("Probe thread did not start");
    }
    // SAK-ALLOW-BLOCKING: `thread` is a stack local destroyed on return, and ~QThread on
    // a live thread aborts the process, so this join is not optional. A started probe
    // always quits via finishTcpProbe; a thread that never started returns immediately.
    thread.wait();
    return result;
}

const QHash<uint16_t, QString> kServiceDatabase = {
    {7, QStringLiteral("Echo")},
    {9, QStringLiteral("Discard")},
    {13, QStringLiteral("Daytime")},
    {20, QStringLiteral("FTP Data")},
    {21, QStringLiteral("FTP Control")},
    {22, QStringLiteral("SSH")},
    {23, QStringLiteral("Telnet")},
    {25, QStringLiteral("SMTP")},
    {37, QStringLiteral("Time")},
    {53, QStringLiteral("DNS")},
    {67, QStringLiteral("DHCP Server")},
    {68, QStringLiteral("DHCP Client")},
    {69, QStringLiteral("TFTP")},
    {79, QStringLiteral("Finger")},
    {80, QStringLiteral("HTTP")},
    {88, QStringLiteral("Kerberos")},
    {110, QStringLiteral("POP3")},
    {111, QStringLiteral("RPCbind")},
    {113, QStringLiteral("Ident")},
    {119, QStringLiteral("NNTP")},
    {123, QStringLiteral("NTP")},
    {135, QStringLiteral("MS-RPC")},
    {137, QStringLiteral("NetBIOS Name")},
    {138, QStringLiteral("NetBIOS Datagram")},
    {139, QStringLiteral("NetBIOS Session")},
    {143, QStringLiteral("IMAP")},
    {161, QStringLiteral("SNMP")},
    {162, QStringLiteral("SNMP Trap")},
    {179, QStringLiteral("BGP")},
    {389, QStringLiteral("LDAP")},
    {443, QStringLiteral("HTTPS")},
    {445, QStringLiteral("SMB")},
    {465, QStringLiteral("SMTPS")},
    {514, QStringLiteral("Syslog")},
    {515, QStringLiteral("LPD")},
    {543, QStringLiteral("Kerberos Login")},
    {544, QStringLiteral("Kerberos Shell")},
    {548, QStringLiteral("AFP")},
    {554, QStringLiteral("RTSP")},
    {587, QStringLiteral("SMTP Submission")},
    {631, QStringLiteral("IPP/CUPS")},
    {636, QStringLiteral("LDAPS")},
    {873, QStringLiteral("Rsync")},
    {993, QStringLiteral("IMAPS")},
    {995, QStringLiteral("POP3S")},
    {1080, QStringLiteral("SOCKS")},
    {1433, QStringLiteral("MS-SQL")},
    {1434, QStringLiteral("MS-SQL Browser")},
    {1521, QStringLiteral("Oracle DB")},
    {1723, QStringLiteral("PPTP")},
    {1883, QStringLiteral("MQTT")},
    {1900, QStringLiteral("SSDP/UPnP")},
    {2049, QStringLiteral("NFS")},
    {2082, QStringLiteral("cPanel")},
    {2083, QStringLiteral("cPanel SSL")},
    {2181, QStringLiteral("ZooKeeper")},
    {2375, QStringLiteral("Docker")},
    {2376, QStringLiteral("Docker TLS")},
    {3306, QStringLiteral("MySQL")},
    {3389, QStringLiteral("RDP")},
    {3690, QStringLiteral("SVN")},
    {4443, QStringLiteral("Pharos")},
    {5000, QStringLiteral("UPnP")},
    {5060, QStringLiteral("SIP")},
    {5061, QStringLiteral("SIP TLS")},
    {5201, QStringLiteral("iPerf3")},
    {5222, QStringLiteral("XMPP Client")},
    {5269, QStringLiteral("XMPP Server")},
    {5353, QStringLiteral("mDNS")},
    {5432, QStringLiteral("PostgreSQL")},
    {5631, QStringLiteral("pcAnywhere")},
    {5672, QStringLiteral("AMQP")},
    {5900, QStringLiteral("VNC")},
    {5901, QStringLiteral("VNC :1")},
    {5938, QStringLiteral("TeamViewer")},
    {6379, QStringLiteral("Redis")},
    {6443, QStringLiteral("Kubernetes API")},
    {6667, QStringLiteral("IRC")},
    {6697, QStringLiteral("IRC SSL")},
    {7070, QStringLiteral("RealServer")},
    {8000, QStringLiteral("HTTP Alt")},
    {8008, QStringLiteral("HTTP Alt")},
    {8080, QStringLiteral("HTTP Proxy")},
    {8081, QStringLiteral("HTTP Proxy")},
    {8291, QStringLiteral("WinBox")},
    {8443, QStringLiteral("HTTPS Alt")},
    {8883, QStringLiteral("MQTT SSL")},
    {8888, QStringLiteral("HTTP Alt")},
    {9090, QStringLiteral("Web Console")},
    {9100, QStringLiteral("JetDirect")},
    {9200, QStringLiteral("Elasticsearch")},
    {9418, QStringLiteral("Git")},
    {9999, QStringLiteral("ABYSS")},
    {10'000, QStringLiteral("Webmin")},
    {11'211, QStringLiteral("Memcached")},
    {27'017, QStringLiteral("MongoDB")},
    {27'018, QStringLiteral("MongoDB Shard")},
    {28'017, QStringLiteral("MongoDB Web")},
};

const QVector<PortPreset> kPortPresets = {
    {QStringLiteral("Common Services"), {20,  21,  22,  23,  25,  53,   80,   110,  115,  135, 139,
                                         143, 443, 445, 993, 995, 1723, 3306, 3389, 5900, 8080}},
    {QStringLiteral("Web Servers"), {80, 443, 8080, 8443, 8000, 8888, 9000, 9090}},
    {QStringLiteral("Database"), {1433, 1521, 3306, 5432, 6379, 27'017, 9200}},
    {QStringLiteral("File Sharing"), {20, 21, 22, 69, 111, 137, 138, 139, 445, 873, 2049}},
    {QStringLiteral("Email"), {25, 110, 143, 465, 587, 993, 995}},
    {QStringLiteral("Remote Access"), {22, 23, 3389, 5900, 5901, 5938, 8291}},
    {QStringLiteral("Top 100"),
     {7,    9,    13,     21,     22,     23,     25,    26,   37,   53,   79,   80,   81,
      88,   106,  110,    111,    113,    119,    135,   139,  143,  144,  179,  199,  389,
      427,  443,  444,    445,    465,    513,    514,   515,  543,  544,  548,  554,  587,
      631,  646,  873,    990,    993,    995,    1025,  1026, 1027, 1028, 1029, 1110, 1433,
      1720, 1723, 1755,   1900,   2000,   2001,   2049,  2121, 2717, 3000, 3128, 3306, 3389,
      3986, 4899, 5000,   5009,   5051,   5060,   5101,  5190, 5357, 5432, 5631, 5666, 5800,
      5900, 5901, 6000,   6001,   6646,   7070,   8000,  8008, 8009, 8080, 8081, 8443, 8888,
      9100, 9999, 10'000, 32'768, 49'152, 49'153, 49'154}},
};
}  // namespace

PortScanner::PortScanner(QObject* parent) : QObject(parent) {}

void PortScanner::cancel() {
    m_cancelled.store(true);
}

namespace {
// Validate the untrusted scan target, returning the human-readable rejection reason and an
// empty string when the target is safe to hand to connectToHost / the HTTP Host header.
QString validateScanTarget(const QString& target) {
    if (target.isEmpty()) {
        return QStringLiteral("Target cannot be empty");
    }
    if (target.size() > kMaxTargetLength) {
        return QStringLiteral("Target is too long");
    }
    // The target is echoed verbatim into the HTTP banner-probe Host header and handed to
    // connectToHost. A control character (CR/LF/NUL/...) in an attacker- or AI-supplied
    // target would inject additional request headers; a real hostname or IP contains none,
    // so reject the whole request rather than sanitize-and-send.
    for (const QChar ch : target) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7F) {
            return QStringLiteral("Target contains invalid control characters");
        }
    }
    return {};
}

// Build the port set: explicit ports plus any range, de-duplicated with the invalid
// port 0 dropped. A hostile ScanConfig can carry a huge duplicate-heavy ports vector;
// the QSet bounds the real work to at most the 65535 valid TCP ports (fail closed
// against amplification) while keeping membership checks O(1) rather than O(n^2).
QVector<uint16_t> collectScanPorts(const PortScanner::ScanConfig& config) {
    QVector<uint16_t> ports;
    QSet<uint16_t> seen;
    // Reserve for the unique-port ceiling, never the (possibly huge) input count, so a
    // duplicate-heavy vector cannot drive a large hash pre-allocation.
    constexpr qsizetype kMaxUniquePorts = 65'536;
    seen.reserve(std::min<qsizetype>(config.ports.size(), kMaxUniquePorts));
    for (const uint16_t port : config.ports) {
        if (port != 0 && !seen.contains(port)) {
            seen.insert(port);
            ports.append(port);
        }
    }
    if (config.portRangeStart > 0 && config.portRangeEnd >= config.portRangeStart) {
        for (uint32_t p = config.portRangeStart; p <= config.portRangeEnd; ++p) {
            const auto port = static_cast<uint16_t>(p);
            if (!seen.contains(port)) {
                seen.insert(port);
                ports.append(port);
            }
        }
    }
    return ports;
}
}  // namespace

void PortScanner::scan(const ScanConfig& config) {
    m_cancelled.store(false);

    const QString targetError = validateScanTarget(config.target);
    if (!targetError.isEmpty()) {
        Q_EMIT errorOccurred(targetError);
        Q_EMIT scanComplete({});
        return;
    }

    const QVector<uint16_t> ports = collectScanPorts(config);
    if (ports.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No ports specified for scanning"));
        Q_EMIT scanComplete({});
        return;
    }

    Q_EMIT scanStarted(config.target, ports.size());

    const QVector<PortScanResult> results = scanPortsConcurrently(config, ports);

    Q_EMIT scanComplete(results);
}

QVector<PortScanResult> PortScanner::scanPortsConcurrently(const ScanConfig& config,
                                                           const QVector<uint16_t>& ports) {
    // Honor maxConcurrent: probe up to `concurrency` ports at once (each scanPort runs
    // its own short-lived probe thread) instead of the previous strictly-serial loop.
    // Ports are processed in batches and every batch is fully joined before the next, so
    // the signals below are emitted from THIS thread in a stable, bounded order.
    const int concurrency = std::clamp(config.maxConcurrent, 1, kMaxScanConcurrency);
    QVector<PortScanResult> results;
    results.reserve(ports.size());

    int scanned = 0;
    for (int start = 0; start < ports.size() && !m_cancelled.load(); start += concurrency) {
        const int end = std::min(start + concurrency, static_cast<int>(ports.size()));

        std::vector<std::future<PortScanResult>> batch;
        batch.reserve(static_cast<size_t>(end - start));
        for (int i = start; i < end; ++i) {
            const uint16_t port = ports[i];
            try {
                batch.push_back(std::async(std::launch::async, [this, &config, port]() {
                    return scanPort(config.target, port, config.timeoutMs, config.grabBanners);
                }));
            } catch (const std::system_error&) {
                // Thread creation failed under load. std::async is the only throw source
                // here (scanPort itself fails closed via runTcpProbe's bounded wait), so
                // degrade to a deferred task that runs synchronously at get() time rather
                // than let the exception escape and terminate the scan thread.
                batch.push_back(std::async(std::launch::deferred, [this, &config, port]() {
                    return scanPort(config.target, port, config.timeoutMs, config.grabBanners);
                }));
            }
        }

        for (auto& fut : batch) {
            PortScanResult result = fut.get();
            results.append(result);
            ++scanned;
            Q_EMIT portScanned(result);
            Q_EMIT scanProgress(scanned, ports.size());
        }
    }

    return results;
}

PortScanResult PortScanner::scanPort(const QString& target,
                                     uint16_t port,
                                     int timeoutMs,
                                     bool grabBanner) {
    PortScanResult result;
    result.target = target;
    result.port = port;
    result.scanTimestamp = QDateTime::currentDateTime();

    // Clamp the untrusted per-connect timeout to a positive, bounded QTimer interval so the
    // connect timer always fires (no infinite wait) and cannot block for weeks.
    const int connectTimeoutMs =
        std::clamp(timeoutMs, kMinPortScanTimeoutMs, kMaxPortScanTimeoutMs);
    const TcpProbeResult probe =
        runTcpProbe(target, port, connectTimeoutMs, grabBanner, kBannerGrabTimeout);

    result.responseTimeMs = probe.response_time_ms;

    if (probe.connected) {
        result.state = PortScanResult::State::Open;
        result.serviceName = getServiceName(port);

        if (grabBanner) {
            result.banner = QString::fromUtf8(probe.banner).trimmed();
            result.banner.remove(QLatin1Char('\0'));
        }
    } else if (probe.socket_error == QAbstractSocket::ConnectionRefusedError) {
        // Classify via the stable socket-error enum, not the localized "refused" text,
        // so closed ports are detected on non-English Windows too.
        result.state = PortScanResult::State::Closed;
    } else if (probe.timed_out) {
        result.state = PortScanResult::State::Filtered;
    } else {
        result.state = PortScanResult::State::Error;
        result.errorMessage = probe.error_message;
    }

    return result;
}

QVector<PortPreset> PortScanner::getPresets() {
    return kPortPresets;
}

QString PortScanner::getServiceName(uint16_t port) {
    const auto& db = serviceDatabase();
    auto it = db.find(port);
    if (it != db.end()) {
        return it.value();
    }
    return {};
}

const QHash<uint16_t, QString>& PortScanner::serviceDatabase() {
    return kServiceDatabase;
}

}  // namespace sak
