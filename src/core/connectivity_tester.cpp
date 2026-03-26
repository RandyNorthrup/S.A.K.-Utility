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

void updateHopStats(sak::MtrHopStats& stats,
                    const sak::PingReply& reply,
                    int ttl,
                    int& max_discovered) {
    stats.sent++;

    bool got_response = reply.success || reply.errorMessage == QStringLiteral("TTL expired");
    if (got_response) {
        stats.received++;
        stats.ipAddress = reply.replyFrom;
        stats.lastRttMs = reply.rttMs;
        stats.bestRttMs = std::min(stats.bestRttMs, reply.rttMs);
        stats.worstRttMs = std::max(stats.worstRttMs, reply.rttMs);
        stats.avgRttMs += (reply.rttMs - stats.avgRttMs) / static_cast<double>(stats.received);
        max_discovered = std::max(max_discovered, ttl);
    }

    stats.lossPercent = (stats.sent > 0) ? (1.0 - static_cast<double>(stats.received) /
                                                      static_cast<double>(stats.sent)) *
                                               kFullPercent
                                         : 0.0;
}

}  // namespace

namespace sak {

namespace {
void computePingStats(PingResult& result, const QVector<double>& rtts) {
    result.lost = result.sent - result.received;
    result.lossPercent =
        (result.sent > 0) ? (static_cast<double>(result.lost) / result.sent) * 100.0 : 0.0;

    if (rtts.isEmpty()) {
        return;
    }

    result.minRtt = *std::min_element(rtts.begin(), rtts.end());
    result.maxRtt = *std::max_element(rtts.begin(), rtts.end());
    result.avgRtt = std::accumulate(rtts.begin(), rtts.end(), 0.0) /
                    static_cast<double>(rtts.size());

    // Jitter = standard deviation of RTT
    if (rtts.size() <= 1) {
        return;
    }

    double sumSqDiff = 0.0;
    for (double rtt : rtts) {
        const double diff = rtt - result.avgRtt;
        sumSqDiff += diff * diff;
    }
    result.jitter = std::sqrt(sumSqDiff / static_cast<double>(rtts.size() - 1));
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
                       bool cancelled) {
    result.hops.clear();
    const int hopCount = static_cast<int>(hopStats.size());
    const int limit = std::min(maxDiscoveredHop, hopCount);
    for (int i = 0; i < limit; ++i) {
        result.hops.append(hopStats[i]);
    }
    result.totalCycles =
        cancelled ? 0 : static_cast<int>(result.hops.isEmpty() ? 0 : result.hops.first().sent);
}
}  // namespace

ConnectivityTester::ConnectivityTester(QObject* parent) : QObject(parent) {
    WSADATA wsa_data{};
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
}

ConnectivityTester::~ConnectivityTester() {
    WSACleanup();
}

void ConnectivityTester::cancel() {
    m_cancelled.store(true);
}

QString ConnectivityTester::resolveTargetIpOrEmitError(const QString& target,
                                                       const QString& operation) {
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
    Q_ASSERT(!hostname.isEmpty());
    // -- Normalise user input -------------------------------------------
    // Users often paste full URLs ("https://example.com/path") or include
    // a port ("example.com:443").  Strip everything down to the bare host
    // so that getaddrinfo receives a resolvable name.
    QString host = hostname.trimmed();

    // Strip URL scheme (http://, https://, ftp://, etc.)
    if (host.contains(QStringLiteral("://"))) {
        QUrl url(host);
        if (url.isValid() && !url.host().isEmpty()) {
            host = url.host();
        } else {
            // Fallback: remove scheme manually
            host = host.mid(host.indexOf(QStringLiteral("://")) + 3);
        }
    }

    // Strip path, query, and fragment (anything after the host)
    const int slashPos = host.indexOf(QLatin1Char('/'));
    if (slashPos > 0) {
        host = host.left(slashPos);
    }

    // Strip port suffix (e.g. "example.com:443")
    const int colonPos = host.lastIndexOf(QLatin1Char(':'));
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
        reply.errorMessage = QStringLiteral("Failed to create ICMP handle");
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

    const auto end = std::chrono::high_resolution_clock::now();

    IcmpCloseHandle(hIcmp);

    if (numReplies > 0) {
        auto* echoReply = reinterpret_cast<PICMP_ECHO_REPLY>(replyBuffer.get());

        if (echoReply->Status == IP_SUCCESS) {
            reply.success = true;
            reply.rttMs = static_cast<double>(echoReply->RoundTripTime);
            reply.ttl = static_cast<int>(echoReply->Options.Ttl);

            IN_ADDR replyAddr;
            replyAddr.S_un.S_addr = echoReply->Address;
            char ipBuf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &replyAddr, ipBuf, sizeof(ipBuf));
            reply.replyFrom = QString::fromLatin1(ipBuf);
        } else if (echoReply->Status == IP_TTL_EXPIRED_TRANSIT ||
                   echoReply->Status == IP_TTL_EXPIRED_REASSEM) {
            // TTL expired -- used in traceroute
            reply.success = false;
            reply.rttMs = static_cast<double>(echoReply->RoundTripTime);

            IN_ADDR replyAddr;
            replyAddr.S_un.S_addr = echoReply->Address;
            char ipBuf[INET_ADDRSTRLEN] = {};
            inet_ntop(AF_INET, &replyAddr, ipBuf, sizeof(ipBuf));
            reply.replyFrom = QString::fromLatin1(ipBuf);
            reply.errorMessage = QStringLiteral("TTL expired");
        } else {
            reply.success = false;
            reply.errorMessage = QStringLiteral("ICMP error status %1").arg(echoReply->Status);
        }
    } else {
        reply.success = false;
        const double elapsed = std::chrono::duration<double, std::milli>(end - start).count();
        reply.rttMs = elapsed;
        reply.errorMessage = QStringLiteral("Request timed out");
    }

    return reply;
}

void ConnectivityTester::ping(const PingConfig& config) {
    m_cancelled.store(false);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "Ping");
    if (targetIP.isEmpty()) {
        return;
    }

    PingResult result;
    result.target = config.target;
    result.resolvedIP = targetIP;
    result.sent = config.count;

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
        PingReply reply = sendIcmpEcho(targetIP, timeoutMs, netdiag::kDefaultPingPacketSize, ttl);

        bool got_response = reply.success || reply.errorMessage == QStringLiteral("TTL expired");
        if (got_response) {
            hopIP = reply.replyFrom;
            hop.timedOut = false;
            rtts.append(reply.rttMs);
        }

        if (probe_idx < kMaxRttSlots) {
            bool has_ip = reply.success || !reply.replyFrom.isEmpty();
            *rtt_slots[probe_idx] = has_ip ? reply.rttMs : -1.0;
        }
    }

    finalizeHop(hop, rtts, hopIP, resolveHostnames);
    return hop;
}

void ConnectivityTester::traceroute(const TracerouteConfig& config) {
    m_cancelled.store(false);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "Traceroute");
    if (targetIP.isEmpty()) {
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

        TracerouteHop hop =
            probeHop(targetIP, ttl, config.timeoutMs, config.probesPerHop, config.resolveHostnames);
        result.hops.append(hop);
        Q_EMIT tracerouteHop(hop);

        if (!hop.timedOut && hop.ipAddress == targetIP) {
            result.reachedTarget = true;
            break;
        }
    }

    result.totalHops = result.hops.size();
    Q_EMIT tracerouteComplete(result);
}

void ConnectivityTester::mtr(const MtrConfig& config) {
    m_cancelled.store(false);

    const QString targetIP = resolveTargetIpOrEmitError(config.target, "MTR");
    if (targetIP.isEmpty()) {
        return;
    }

    MtrResult result;
    result.target = config.target;
    result.startTime = QDateTime::currentDateTime();

    QVector<MtrHopStats> hopStats = initHopStats(config.maxHops);

    int maxDiscoveredHop = 0;

    for (int cycle = 0; cycle < config.cycles; ++cycle) {
        if (m_cancelled.load()) {
            break;
        }

        for (int ttl = 1; ttl <= config.maxHops; ++ttl) {
            if (m_cancelled.load()) {
                break;
            }

            PingReply reply =
                sendIcmpEcho(targetIP, config.timeoutMs, netdiag::kDefaultPingPacketSize, ttl);

            updateHopStats(hopStats[ttl - 1], reply, ttl, maxDiscoveredHop);

            if (reply.success && reply.replyFrom == targetIP) {
                break;
            }
        }

        Q_EMIT mtrUpdate(visibleHopStats(hopStats, maxDiscoveredHop), cycle + 1);

        if (!m_cancelled.load() && cycle < config.cycles - 1) {
            QThread::msleep(static_cast<unsigned long>(config.intervalMs));
        }
    }

    finalizeHopStats(hopStats);

    populateMtrResult(result, hopStats, maxDiscoveredHop, m_cancelled.load());

    Q_EMIT mtrComplete(result);
}

}  // namespace sak
