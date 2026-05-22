// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file port_scanner.cpp
/// @brief TCP connect scanning with service fingerprinting

#include "sak/port_scanner.h"

#include <QElapsedTimer>
#include <QSemaphore>
#include <QTcpSocket>
#include <QThread>
#include <QTimer>

namespace sak {

namespace {
constexpr int kBannerGrabTimeout = netdiag::kBannerGrabTimeoutMs;
constexpr int kBannerMaxRead = netdiag::kBannerMaxBytes;

struct TcpProbeResult {
    bool connected{false};
    bool timed_out{false};
    QString error_message;
    QByteArray banner;
    double response_time_ms{0.0};
};

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

    QObject::connect(&thread, &QThread::started, context, [&]() {
        auto* socket = new QTcpSocket(context);
        auto* connectTimer = new QTimer(context);
        connectTimer->setSingleShot(true);
        auto* bannerTimer = new QTimer(context);
        bannerTimer->setSingleShot(true);
        auto* probeTimer = new QTimer(context);
        probeTimer->setSingleShot(true);
        QElapsedTimer elapsed;
        bool finished = false;
        bool probeSent = false;

        auto finish = [&]() {
            if (finished) {
                return;
            }
            finished = true;
            result.response_time_ms = elapsed.elapsed();
            socket->disconnectFromHost();
            socket->deleteLater();
            context->deleteLater();
            thread.quit();
            done.release();
        };

        QObject::connect(connectTimer, &QTimer::timeout, context, [&]() {
            result.timed_out = true;
            result.error_message = QStringLiteral("Connection timed out");
            socket->abort();
            finish();
        });
        QObject::connect(bannerTimer, &QTimer::timeout, context, [&]() { finish(); });
        QObject::connect(probeTimer, &QTimer::timeout, context, [&]() {
            if (probeSent) {
                return;
            }
            probeSent = true;
            socket->write("HEAD / HTTP/1.0\r\nHost: " + target.toLatin1() + "\r\n\r\n");
            socket->flush();
            bannerTimer->start(bannerTimeoutMs);
        });
        QObject::connect(socket, &QTcpSocket::connected, context, [&]() {
            connectTimer->stop();
            result.connected = true;
            result.response_time_ms = elapsed.elapsed();
            if (!grabBanner) {
                finish();
                return;
            }
            probeTimer->start(bannerTimeoutMs);
        });
        QObject::connect(socket, &QTcpSocket::readyRead, context, [&]() {
            result.banner += socket->read(kBannerMaxRead);
            finish();
        });
        QObject::connect(socket, &QTcpSocket::disconnected, context, [&]() {
            if (result.connected) {
                finish();
            }
        });
        QObject::connect(
            socket, &QTcpSocket::errorOccurred, context, [&](QAbstractSocket::SocketError error) {
                if (finished || error == QAbstractSocket::RemoteHostClosedError) {
                    return;
                }
                result.error_message = socket->errorString();
                finish();
            });

        elapsed.start();
        connectTimer->start(connectTimeoutMs);
        socket->connectToHost(target, port);
    });

    thread.start();
    done.acquire();
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
}  // namespace

PortScanner::PortScanner(QObject* parent) : QObject(parent) {}

void PortScanner::cancel() {
    m_cancelled.store(true);
}

void PortScanner::scan(const ScanConfig& config) {
    m_cancelled.store(false);

    if (config.target.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("Target cannot be empty"));
        Q_EMIT scanComplete({});
        return;
    }

    // Build port list: explicit ports + range
    QVector<uint16_t> ports = config.ports;
    if (config.portRangeStart > 0 && config.portRangeEnd >= config.portRangeStart) {
        for (uint32_t p = config.portRangeStart; p <= config.portRangeEnd; ++p) {
            const auto port = static_cast<uint16_t>(p);
            if (!ports.contains(port)) {
                ports.append(port);
            }
        }
    }

    if (ports.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("No ports specified for scanning"));
        Q_EMIT scanComplete({});
        return;
    }

    Q_EMIT scanStarted(config.target, ports.size());

    QVector<PortScanResult> results;
    results.reserve(ports.size());

    for (int i = 0; i < ports.size(); ++i) {
        if (m_cancelled.load()) {
            break;
        }

        auto result = scanPort(config.target, ports[i], config.timeoutMs, config.grabBanners);
        results.append(result);
        Q_EMIT portScanned(result);
        Q_EMIT scanProgress(i + 1, ports.size());
    }

    Q_EMIT scanComplete(results);
}

PortScanResult PortScanner::scanPort(const QString& target,
                                     uint16_t port,
                                     int timeoutMs,
                                     bool grabBanner) {
    PortScanResult result;
    result.target = target;
    result.port = port;
    result.scanTimestamp = QDateTime::currentDateTime();

    const TcpProbeResult probe =
        runTcpProbe(target, port, timeoutMs, grabBanner, kBannerGrabTimeout);

    result.responseTimeMs = probe.response_time_ms;

    if (probe.connected) {
        result.state = PortScanResult::State::Open;
        result.serviceName = getServiceName(port);

        if (grabBanner) {
            result.banner = QString::fromUtf8(probe.banner).trimmed();
            result.banner.remove(QLatin1Char('\0'));
        }
    } else if (probe.error_message.contains(QStringLiteral("refused"), Qt::CaseInsensitive)) {
        result.state = PortScanResult::State::Closed;
    } else if (probe.timed_out) {
        result.state = PortScanResult::State::Filtered;
    } else {
        result.state = PortScanResult::State::Error;
        result.errorMessage = probe.error_message;
    }

    return result;
}

QString PortScanner::grabBannerData(const QString& target, uint16_t port, int timeoutMs) {
    Q_ASSERT(!target.isEmpty());
    Q_ASSERT(timeoutMs >= 0);
    const TcpProbeResult probe = runTcpProbe(target, port, timeoutMs, true, timeoutMs);
    if (probe.banner.isEmpty()) {
        return {};
    }

    // Clean up non-printable characters
    QString banner = QString::fromUtf8(probe.banner).trimmed();
    banner.remove(QLatin1Char('\0'));
    return banner;
}

QVector<PortPreset> PortScanner::getPresets() {
    return {
        {QStringLiteral("Common Services"),
         {20,  21,  22,  23,  25,  53,   80,   110,  115,  135, 139,
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
