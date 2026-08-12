// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file connectivity_tester.cpp
/// @brief Ping, Traceroute, MTR via Windows ICMP API

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <iphlpapi.h>
#include <icmpapi.h>
// clang-format on
#include "sak/connectivity_tester.h"

#include <QThread>
#include <QUrl>
#include <QVariant>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <numeric>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

namespace {

constexpr int kFillByte = 0x41;     // 'A'
constexpr int kReplyExtraSize = 8;  // Extra bytes for ICMP_ECHO_REPLY

constexpr double kFullPercent = 100.0;
constexpr WORD kWinsockMajorVersion = 2;
constexpr WORD kWinsockMinorVersion = 2;

// Dynamic-property key holding a non-zero WSAStartup error code. Stored as a per-instance dynamic
// property (the header is not modified) so the destructor can skip an unbalanced WSACleanup and
// operations can fail closed with the real Winsock error instead of a misleading resolve failure.
constexpr char kWsaInitErrorProperty[] = "sak_wsaInitError";

void updateHopStats(sak::MtrHopStats& stats,
                    const sak::PingReply& reply,
                    int ttl,
                    int& max_discovered) {
    stats.sent++;

    const bool got_response = reply.success || reply.errorMessage == QStringLiteral("TTL expired");
    if (got_response) {
        stats.received++;
        stats.ipAddress = reply.replyFrom;
        stats.lastRttMs = reply.rttMs;
        stats.bestRttMs = std::min(stats.bestRttMs, reply.rttMs);
        stats.worstRttMs = std::max(stats.worstRttMs, reply.rttMs);
        stats.avgRttMs += (reply.rttMs - stats.avgRttMs) / static_cast<double>(stats.received);
        max_discovered = std::max(max_discovered, ttl);
    }

    stats.lossPercent =
        (stats.sent > 0)
            ? (1.0 - (static_cast<double>(stats.received) / static_cast<double>(stats.sent))) *
                  kFullPercent
            : 0.0;
}

}  // namespace

namespace sak {

namespace {
void computePingStats(PingResult& result, const QVector<double>& rtts) {
    result.lost = result.sent - result.received;
    result.lossPercent =
        (result.sent > 0) ? (static_cast<double>(result.lost) / result.sent) * kFullPercent : 0.0;

    if (rtts.isEmpty()) {
        return;
    }

    result.minRtt = *std::ranges::min_element(rtts);
    result.maxRtt = *std::ranges::max_element(rtts);
    result.avgRtt = std::accumulate(rtts.begin(), rtts.end(), 0.0) /
                    static_cast<double>(rtts.size());

    // Jitter = standard deviation of RTT
    if (rtts.size() <= 1) {
        return;
    }

    double sumSqDiff = 0.0;
    for (const double rtt : rtts) {
        const double diff = rtt - result.avgRtt;
        sumSqDiff += diff * diff;
    }
    result.jitter = std::sqrt(sumSqDiff / static_cast<double>(rtts.size() - 1));
}

QString ipv4AddressToString(IPAddr address) {
    IN_ADDR replyAddr;
    replyAddr.S_un.S_addr = address;
    char ipBuf[INET_ADDRSTRLEN] = {};
    inet_ntop(AF_INET, &replyAddr, ipBuf, sizeof(ipBuf));
    return QString::fromLatin1(ipBuf);
}

bool isTtlExpiredStatus(ULONG status) {
    return status == IP_TTL_EXPIRED_TRANSIT || status == IP_TTL_EXPIRED_REASSEM;
}

void applySuccessfulEcho(PingReply& reply, const ICMP_ECHO_REPLY& echoReply) {
    reply.success = true;
    reply.rttMs = static_cast<double>(echoReply.RoundTripTime);
    reply.ttl = static_cast<int>(echoReply.Options.Ttl);
    reply.replyFrom = ipv4AddressToString(echoReply.Address);
}

void applyTtlExpiredEcho(PingReply& reply, const ICMP_ECHO_REPLY& echoReply) {
    reply.success = false;
    reply.rttMs = static_cast<double>(echoReply.RoundTripTime);
    reply.replyFrom = ipv4AddressToString(echoReply.Address);
    reply.errorMessage = QStringLiteral("TTL expired");
}

// Translate a completed IcmpSendEcho into a PingReply. A zero numReplies is ambiguous -- a real
// timeout or a local API failure -- so the already-captured sendError, not the zero itself,
// decides which is reported.
void settleEchoOutcome(
    PingReply& reply, char* replyBuffer, DWORD numReplies, DWORD sendError, double elapsedMs) {
    if (numReplies > 0) {
        auto* echoReply = reinterpret_cast<PICMP_ECHO_REPLY>(replyBuffer);

        if (echoReply->Status == IP_SUCCESS) {
            applySuccessfulEcho(reply, *echoReply);
        } else if (isTtlExpiredStatus(echoReply->Status)) {
            applyTtlExpiredEcho(reply, *echoReply);
        } else {
            reply.success = false;
            reply.errorMessage = QStringLiteral("ICMP error status %1").arg(echoReply->Status);
        }
    } else {
        reply.success = false;
        reply.rttMs = elapsedMs;
        // A genuine timeout keeps its "Request timed out" label (probeHop/updateHopStats do not
        // key on it, only on "TTL expired"); any other zero-return is a real API failure and is
        // surfaced as such instead of being falsified as a timeout.
        reply.errorMessage = (sendError == IP_REQ_TIMED_OUT || sendError == 0)
                                 ? QStringLiteral("Request timed out")
                                 : QStringLiteral("ICMP echo failed (error %1)").arg(sendError);
    }
}

[[nodiscard]] QVector<MtrHopStats> initHopStats(int maxHops) {
    QVector<MtrHopStats> hopStats(maxHops);
    for (int i = 0; i < maxHops; ++i) {
        hopStats[i].hopNumber = i + 1;
        hopStats[i].bestRttMs = std::numeric_limits<double>::max();
        hopStats[i].worstRttMs = 0.0;
    }
    return hopStats;
}

[[nodiscard]] QVector<MtrHopStats> visibleHopStats(const QVector<MtrHopStats>& hopStats,
                                                   int maxDiscoveredHop) {
    QVector<MtrHopStats> visibleHops;
    const int hopCount = static_cast<int>(hopStats.size());
    const int limit = std::min(maxDiscoveredHop, hopCount);
    for (int i = 0; i < limit; ++i) {
        visibleHops.append(hopStats[i]);
    }
    return visibleHops;
}

void finalizeHopStats(QVector<MtrHopStats>& hopStats) {
    for (auto& stats : hopStats) {
        if (stats.received > 1) {
            stats.jitterMs = stats.worstRttMs - stats.bestRttMs;
        }
        if (stats.bestRttMs == std::numeric_limits<double>::max()) {
            stats.bestRttMs = 0.0;
        }
    }
}

void populateMtrResult(MtrResult& result,
                       const QVector<MtrHopStats>& hopStats,
                       int maxDiscoveredHop,
                       int completedCycles) {
    result.hops.clear();
    const int hopCount = static_cast<int>(hopStats.size());
    const int limit = std::min(maxDiscoveredHop, hopCount);
    for (int i = 0; i < limit; ++i) {
        result.hops.append(hopStats[i]);
    }
    // Report cycles that actually completed, independent of hop responsiveness: a run in which no
    // hop ever replies still executed its cycles, so an empty hop list must not zero the count.
    result.totalCycles = completedCycles;
}
}  // namespace

ConnectivityTester::ConnectivityTester(QObject* parent) : QObject(parent) {
    WSADATA wsa_data{};
    // WSAStartup returns the error code directly (it does not set the last-error). Record a
    // failure so operations fail closed with the true cause and the destructor skips cleanup.
    const int wsaStatus = WSAStartup(MAKEWORD(kWinsockMajorVersion, kWinsockMinorVersion),
                                     &wsa_data);
    if (wsaStatus != 0) {
        setProperty(kWsaInitErrorProperty, wsaStatus);
    }
}

ConnectivityTester::~ConnectivityTester() {
    // Only balance a successful WSAStartup; calling WSACleanup after a failed init is incorrect.
    if (property(kWsaInitErrorProperty).toInt() == 0) {
        WSACleanup();
    }
}

void ConnectivityTester::cancel() {
    m_cancelled.store(true);
}

QString ConnectivityTester::resolveTargetIpOrEmitError(const QString& target,
                                                       const QString& operation) {
    if (const int wsaError = property(kWsaInitErrorProperty).toInt(); wsaError != 0) {
        Q_EMIT errorOccurred(
            QStringLiteral("%1 unavailable: Winsock initialization failed (error %2)")
                .arg(operation)
                .arg(wsaError));
        return {};
    }

    const QString trimmed = target.trimmed();
    if (trimmed.isEmpty()) {
        Q_EMIT errorOccurred(QStringLiteral("%1 target cannot be empty").arg(operation));
        return {};
    }

    QString targetIP = resolveHostname(trimmed);
    if (!targetIP.isEmpty()) {
        return targetIP;
    }

    IN_ADDR testAddr{};
    if (inet_pton(AF_INET, trimmed.toLatin1().constData(), &testAddr) == 1) {
        return trimmed;
    }

    Q_EMIT errorOccurred(QStringLiteral("Could not resolve hostname: %1").arg(trimmed));
    return {};
}

QString ConnectivityTester::resolveHostname(const QString& hostname) {
    // Sole caller resolveTargetIpOrEmitError() returns early when the trimmed target is empty.
    Q_ASSERT(!hostname.isEmpty());
    // -- Normalise user input -------------------------------------------
    // Users often paste full URLs ("https://example.com/path") or include
    // a port ("example.com:443").  Strip everything down to the bare host
    // so that getaddrinfo receives a resolvable name.
    QString host = hostname.trimmed();

    // Strip URL scheme (http://, https://, ftp://, etc.)
    if (host.contains(QStringLiteral("://"))) {
        const QUrl url(host);
        // Reject a malformed URL rather than hand-stripping the scheme: a bad URL is untrusted
        // input, and getaddrinfo below already fails closed for anything left unresolvable.
        if (!url.isValid() || url.host().isEmpty()) {
            return {};
        }
        host = url.host();
    }

    // Strip path, query, and fragment (anything after the host)
    const int slashPos = static_cast<int>(host.indexOf(QLatin1Char('/')));
    if (slashPos > 0) {
        host = host.left(slashPos);
    }

    // Strip port suffix (e.g. "example.com:443")
    const int colonPos = static_cast<int>(host.lastIndexOf(QLatin1Char(':')));
    if (colonPos > 0) {
        const auto maybPort = host.mid(colonPos + 1);
        bool isPort = false;
        maybPort.toUShort(&isPort);
        if (isPort) {
            host = host.left(colonPos);
        }
    }

    host = host.trimmed();
    if (host.isEmpty()) {
        return {};
    }

    // -- DNS resolution -------------------------------------------------
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* result = nullptr;
    const auto hostUtf8 = host.toUtf8();
    if (getaddrinfo(hostUtf8.constData(), nullptr, &hints, &result) != 0) {
        return {};
    }

    QString ip;
    if (result != nullptr) {
        auto* sa = reinterpret_cast<sockaddr_in*>(result->ai_addr);
        char ipBuf[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &sa->sin_addr, ipBuf, sizeof(ipBuf));
        ip = QString::fromLatin1(ipBuf);
    }
    freeaddrinfo(result);
    return ip;
}

QString ConnectivityTester::reverseResolve(const QString& ip) {
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    inet_pton(AF_INET, ip.toLatin1().constData(), &sa.sin_addr);

    char host[NI_MAXHOST] = {};
    if (getnameinfo(
            reinterpret_cast<sockaddr*>(&sa), sizeof(sa), host, sizeof(host), nullptr, 0, 0) == 0) {
        QString result = QString::fromUtf8(host);
        if (result != ip) {
            return result;
        }
    }
    return {};
}

PingReply ConnectivityTester::sendIcmpEcho(const QString& targetIP,
                                           int timeoutMs,
                                           int packetSize,
                                           int ttl) {
    PingReply reply;
    reply.success = false;

    HANDLE hIcmp = IcmpCreateFile();
    if (hIcmp == INVALID_HANDLE_VALUE) {
        // Fail closed with the real cause: a discarded GetLastError would leave a local API
        // failure indistinguishable from a network timeout downstream.
        reply.errorMessage =
            QStringLiteral("Failed to create ICMP handle (error %1)").arg(GetLastError());
        return reply;
    }

    // Resolve target IP
    IN_ADDR destAddr{};
    if (inet_pton(AF_INET, targetIP.toLatin1().constData(), &destAddr) != 1) {
        IcmpCloseHandle(hIcmp);
        reply.errorMessage = QStringLiteral("Invalid IP address: ") + targetIP;
        return reply;
    }

    // Options with TTL
    IP_OPTION_INFORMATION options{};
    options.Ttl = static_cast<UCHAR>(ttl);

    // Send buffer
    const auto sendSize = static_cast<size_t>(std::max(packetSize, 1));
    auto sendData = std::make_unique<char[]>(sendSize);
    std::fill_n(sendData.get(), sendSize, static_cast<char>(kFillByte));

    // Reply buffer
    const DWORD replySize = static_cast<DWORD>(sizeof(ICMP_ECHO_REPLY)) +
                            static_cast<DWORD>(sendSize) + kReplyExtraSize;
    auto replyBuffer = std::make_unique<char[]>(replySize);

    const auto start = std::chrono::high_resolution_clock::now();

    const DWORD numReplies = IcmpSendEcho(hIcmp,
                                          destAddr.S_un.S_addr,
                                          sendData.get(),
                                          static_cast<WORD>(sendSize),
                                          &options,
                                          replyBuffer.get(),
                                          replySize,
                                          static_cast<DWORD>(timeoutMs));

    // Capture the extended error IMMEDIATELY: a zero return can mean IP_REQ_TIMED_OUT OR a
    // real failure (bad parameter, buffer too small, allocation failure). Reading it here,
    // before any other call can reset the thread's last-error, lets us tell them apart.
    const DWORD sendError = (numReplies == 0) ? GetLastError() : 0;

    const auto end = std::chrono::high_resolution_clock::now();

    IcmpCloseHandle(hIcmp);

    const double elapsedMs = std::chrono::duration<double, std::milli>(end - start).count();
    settleEchoOutcome(reply, replyBuffer.get(), numReplies, sendError, elapsedMs);

    return reply;
}

ConnectivityTester::PingConfig ConnectivityTester::sanitizeConfig(PingConfig config) {
    namespace nd = netdiag;
    config.count = std::clamp(config.count, nd::kMinPingCount, nd::kMaxPingCount);
    config.intervalMs = std::clamp(config.intervalMs, nd::kMinIntervalMs, nd::kMaxIntervalMs);
    config.timeoutMs = std::clamp(config.timeoutMs, nd::kMinPingTimeoutMs, nd::kMaxPingTimeoutMs);
    config.packetSizeBytes =
        std::clamp(config.packetSizeBytes, nd::kMinPacketSizeBytes, nd::kMaxPacketSizeBytes);
    config.ttl = std::clamp(config.ttl, nd::kMinTtl, nd::kMaxTtl);
    return config;
}

ConnectivityTester::TracerouteConfig ConnectivityTester::sanitizeConfig(TracerouteConfig config) {
    namespace nd = netdiag;
    config.maxHops = std::clamp(config.maxHops, nd::kMinHops, nd::kMaxHops);
    config.timeoutMs = std::clamp(config.timeoutMs, nd::kMinPingTimeoutMs, nd::kMaxPingTimeoutMs);
    config.probesPerHop =
        std::clamp(config.probesPerHop, nd::kMinProbesPerHop, nd::kMaxProbesPerHop);
    return config;
}

ConnectivityTester::MtrConfig ConnectivityTester::sanitizeConfig(MtrConfig config) {
    namespace nd = netdiag;
    config.cycles = std::clamp(config.cycles, nd::kMinMtrCycles, nd::kMaxMtrCycles);
    config.intervalMs = std::clamp(config.intervalMs, nd::kMinIntervalMs, nd::kMaxIntervalMs);
    config.maxHops = std::clamp(config.maxHops, nd::kMinHops, nd::kMaxHops);
    config.timeoutMs = std::clamp(config.timeoutMs, nd::kMinPingTimeoutMs, nd::kMaxPingTimeoutMs);
    return config;
}

void ConnectivityTester::ping(const PingConfig& rawConfig) {
    m_cancelled.store(false);
    const PingConfig config = sanitizeConfig(rawConfig);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "Ping");
    if (targetIP.isEmpty()) {
        Q_EMIT pingComplete({});
        return;
    }

    PingResult result;
    result.target = config.target;
    result.resolvedIP = targetIP;

    QVector<double> rtts;

    for (int i = 0; i < config.count; ++i) {
        if (m_cancelled.load()) {
            break;
        }

        PingReply reply =
            sendIcmpEcho(targetIP, config.timeoutMs, config.packetSizeBytes, config.ttl);
        reply.sequenceNumber = i + 1;

        if (reply.success) {
            rtts.append(reply.rttMs);
            result.received++;
        }

        result.replies.append(reply);
        Q_EMIT pingReply(reply);

        // Wait between pings (but not after the last one)
        if (i < config.count - 1 && !m_cancelled.load()) {
            QThread::msleep(static_cast<unsigned long>(config.intervalMs));
        }
    }

    // Report the number actually sent (== attempts made), not the configured count:
    // a cancelled run stops early, and loss stats must reflect real attempts.
    result.sent = static_cast<int>(result.replies.size());

    computePingStats(result, rtts);

    Q_EMIT pingComplete(result);
}

void ConnectivityTester::finalizeHop(TracerouteHop& hop,
                                     const QVector<double>& rtts,
                                     const QString& hopIP,
                                     bool resolveHostnames) {
    if (hop.timedOut) {
        return;
    }
    hop.ipAddress = hopIP;
    hop.avgRttMs = rtts.isEmpty() ? 0.0
                                  : std::accumulate(rtts.begin(), rtts.end(), 0.0) /
                                        static_cast<double>(rtts.size());

    if (resolveHostnames && !hopIP.isEmpty()) {
        hop.hostname = reverseResolve(hopIP);
    }
}

TracerouteHop ConnectivityTester::probeHop(
    const QString& targetIP, int ttl, int timeoutMs, int probes, bool resolveHostnames) {
    TracerouteHop hop;
    hop.hopNumber = ttl;
    hop.timedOut = true;

    QVector<double> rtts;
    QString hopIP;

    double* rtt_slots[] = {&hop.rtt1Ms, &hop.rtt2Ms, &hop.rtt3Ms};
    constexpr int kMaxRttSlots = 3;

    for (int probe_idx = 0; probe_idx < probes; ++probe_idx) {
        const PingReply reply =
            sendIcmpEcho(targetIP, timeoutMs, netdiag::kDefaultPingPacketSize, ttl);

        const bool got_response = reply.success ||
                                  reply.errorMessage == QStringLiteral("TTL expired");
        if (got_response) {
            hopIP = reply.replyFrom;
            hop.timedOut = false;
            rtts.append(reply.rttMs);
        }

        if (probe_idx < kMaxRttSlots) {
            const bool has_ip = reply.success || !reply.replyFrom.isEmpty();
            *rtt_slots[probe_idx] = has_ip ? reply.rttMs : -1.0;
        }
    }

    finalizeHop(hop, rtts, hopIP, resolveHostnames);
    return hop;
}

void ConnectivityTester::traceroute(const TracerouteConfig& rawConfig) {
    m_cancelled.store(false);
    const TracerouteConfig config = sanitizeConfig(rawConfig);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "Traceroute");
    if (targetIP.isEmpty()) {
        Q_EMIT tracerouteComplete({});
        return;
    }

    TracerouteResult result;
    result.target = config.target;
    result.resolvedIP = targetIP;
    result.reachedTarget = false;

    for (int ttl = 1; ttl <= config.maxHops; ++ttl) {
        if (m_cancelled.load()) {
            break;
        }

        const TracerouteHop hop =
            probeHop(targetIP, ttl, config.timeoutMs, config.probesPerHop, config.resolveHostnames);
        result.hops.append(hop);
        Q_EMIT tracerouteHop(hop);

        if (!hop.timedOut && hop.ipAddress == targetIP) {
            result.reachedTarget = true;
            break;
        }
    }

    result.totalHops = static_cast<int>(result.hops.size());
    Q_EMIT tracerouteComplete(result);
}

void ConnectivityTester::runMtrCycle(const QString& targetIP,
                                     const MtrConfig& config,
                                     QVector<MtrHopStats>& hopStats,
                                     int& maxDiscoveredHop) {
    for (int ttl = 1; ttl <= config.maxHops; ++ttl) {
        if (m_cancelled.load()) {
            break;
        }

        const PingReply reply =
            sendIcmpEcho(targetIP, config.timeoutMs, netdiag::kDefaultPingPacketSize, ttl);

        updateHopStats(hopStats[ttl - 1], reply, ttl, maxDiscoveredHop);

        if (reply.success && reply.replyFrom == targetIP) {
            break;
        }
    }
}

void ConnectivityTester::mtr(const MtrConfig& rawConfig) {
    m_cancelled.store(false);
    const MtrConfig config = sanitizeConfig(rawConfig);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "MTR");
    if (targetIP.isEmpty()) {
        Q_EMIT mtrComplete({});
        return;
    }

    MtrResult result;
    result.target = config.target;
    result.startTime = QDateTime::currentDateTime();

    QVector<MtrHopStats> hopStats = initHopStats(config.maxHops);

    int maxDiscoveredHop = 0;
    int completedCycles = 0;

    for (int cycle = 0; cycle < config.cycles; ++cycle) {
        if (m_cancelled.load()) {
            break;
        }

        runMtrCycle(targetIP, config, hopStats, maxDiscoveredHop);

        // Do not count or publish a cycle that cancellation cut short mid-way: it would report a
        // completed cycle when its hop sweep was interrupted.
        if (!m_cancelled.load()) {
            ++completedCycles;
            Q_EMIT mtrUpdate(visibleHopStats(hopStats, maxDiscoveredHop), completedCycles);
        }

        if (!m_cancelled.load() && cycle < config.cycles - 1) {
            QThread::msleep(static_cast<unsigned long>(config.intervalMs));
        }
    }

    finalizeHopStats(hopStats);

    populateMtrResult(result, hopStats, maxDiscoveredHop, completedCycles);

    Q_EMIT mtrComplete(result);
}

}  // namespace sak
