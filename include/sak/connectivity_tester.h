// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file connectivity_tester.h
/// @brief Ping, Traceroute, and MTR via Windows ICMP API

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>

#include <atomic>
#include <type_traits>

namespace sak {

/// @brief ICMP-based connectivity testing (ping, traceroute, MTR)
///
/// Uses IcmpSendEcho via iphlpapi for ICMP echo requests.
/// All blocking methods should be invoked from a worker thread.
class ConnectivityTester : public QObject {
    Q_OBJECT

public:
    struct PingConfig {
        QString target;
        int count = netdiag::kDefaultPingCount;
        int intervalMs = netdiag::kDefaultPingIntervalMs;
        int timeoutMs = netdiag::kDefaultPingTimeoutMs;
        int packetSizeBytes = netdiag::kDefaultPingPacketSize;
        int ttl = netdiag::kDefaultPingTtl;
        bool resolveHostnames = true;
    };

    struct TracerouteConfig {
        QString target;
        int maxHops = netdiag::kDefaultTracerouteMaxHops;
        int timeoutMs = netdiag::kDefaultTracerouteTimeout;
        int probesPerHop = netdiag::kDefaultTracerouteProbes;
        bool resolveHostnames = true;
    };

    struct MtrConfig {
        QString target;
        int cycles = netdiag::kDefaultMtrCycles;
        int intervalMs = netdiag::kDefaultPingIntervalMs;
        int maxHops = netdiag::kDefaultTracerouteMaxHops;
        int timeoutMs = netdiag::kDefaultTracerouteTimeout;
    };

    explicit ConnectivityTester(QObject* parent = nullptr);
    ~ConnectivityTester() override = default;

    ConnectivityTester(const ConnectivityTester&) = delete;
    ConnectivityTester& operator=(const ConnectivityTester&) = delete;
    ConnectivityTester(ConnectivityTester&&) = delete;
    ConnectivityTester& operator=(ConnectivityTester&&) = delete;

    /// @brief Run ping (blocking)
    void ping(const PingConfig& config);

    /// @brief Run traceroute (blocking)
    void traceroute(const TracerouteConfig& config);

    /// @brief Run MTR — continuous ping+traceroute (blocking)
    void mtr(const MtrConfig& config);

    /// @brief Cancel current operation
    void cancel();

Q_SIGNALS:
    void pingReply(sak::PingReply reply);
    void pingComplete(sak::PingResult result);
    void tracerouteHop(sak::TracerouteHop hop);
    void tracerouteComplete(sak::TracerouteResult result);
    void mtrUpdate(QVector<sak::MtrHopStats> hops, int cycle);
    void mtrComplete(sak::MtrResult result);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    /// @brief Resolve target to IPv4, emitting an error message on failure
    [[nodiscard]] QString resolveTargetIpOrEmitError(const QString& target,
                                                     const QString& operation);

    /// @brief Send single ICMP echo
    [[nodiscard]] PingReply sendIcmpEcho(const QString& targetIP,
                                         int timeoutMs,
                                         int packetSize,
                                         int ttl);

    /// @brief Probe a single hop for traceroute
    [[nodiscard]] TracerouteHop probeHop(
        const QString& targetIP, int ttl, int timeoutMs, int probes, bool resolveHostnames);

    /// @brief Resolve hostname to IPv4 address
    [[nodiscard]] static QString resolveHostname(const QString& hostname);

    /// @brief Reverse-resolve IP to hostname
    [[nodiscard]] static QString reverseResolve(const QString& ip);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::ConnectivityTester>,
              "ConnectivityTester must not be copyable.");
