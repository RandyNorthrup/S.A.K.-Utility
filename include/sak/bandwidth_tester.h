// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bandwidth_tester.h
/// @brief LAN bandwidth via iPerf3 and HTTP-based internet speed testing

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QObject>
#include <QProcess>

#include <atomic>
#include <functional>
#include <optional>
#include <type_traits>

namespace sak {

inline constexpr int kDefaultUdpBandwidthMbps = 100;

/// @brief Bandwidth testing via bundled iPerf3 and HTTP download
///
/// For LAN: wraps iperf3.exe (bundled in tools/iperf3/) using QProcess
/// with JSON output parsing. For WAN: HTTP download speed measurement.
class BandwidthTester : public QObject {
    Q_OBJECT

public:
    struct IperfConfig {
        QString serverAddress;
        uint16_t port = netdiag::kDefaultIperfPort;
        int durationSec = netdiag::kDefaultBandwidthDuration;
        int parallelStreams = 1;
        bool bidirectional = true;
        bool udpMode = false;
        int udpBandwidthMbps = kDefaultUdpBandwidthMbps;
    };

    explicit BandwidthTester(QObject* parent = nullptr);
    ~BandwidthTester() override;

    BandwidthTester(const BandwidthTester&) = delete;
    BandwidthTester& operator=(const BandwidthTester&) = delete;
    BandwidthTester(BandwidthTester&&) = delete;
    BandwidthTester& operator=(BandwidthTester&&) = delete;

    /// @brief Start iPerf3 server (other SAK instances test against this)
    void startIperfServer(uint16_t port = netdiag::kDefaultIperfPort);

    /// @brief Stop iPerf3 server
    void stopIperfServer();

    /// @brief Check if server is running
    [[nodiscard]] bool isServerRunning() const;

    /// @brief Run iPerf3 client test (blocking)
    void runIperfTest(const IperfConfig& config);

    /// @brief Run HTTP-based internet speed test (blocking)
    void runHttpSpeedTest();

    /// @brief Check if iPerf3 is available
    [[nodiscard]] bool isIperf3Available() const;

    /// @brief Compose the inbound firewall-rule name for an iPerf3 server instance.
    ///
    /// The name embeds a per-instance @p uniqueToken so two concurrent servers (or
    /// two SAK instances) never share a rule -- otherwise one server stopping would
    /// delete the still-running other's rule. Pure and deterministic; testable.
    [[nodiscard]] static QString composeFirewallRuleName(uint16_t port, const QString& uniqueToken);

    void cancel();

Q_SIGNALS:
    void serverStarted(uint16_t port);
    void serverStopped();
    void serverClientConnected(QString clientIP);
    void testStarted(QString target);
    void testProgress(double currentMbps, double elapsedSec, double totalSec);
    void testComplete(sak::BandwidthTestResult result);
    void httpSpeedTestProgress(double downloadMbps, double uploadMbps);
    void httpSpeedTestComplete(double downloadMbps, double uploadMbps, double latencyMs);
    void errorOccurred(QString error);

private:
    QString m_iperf3Path;
    QProcess* m_serverProcess = nullptr;  ///< Owned; destroyed in destructor
    QString m_firewallRuleName;           ///< Unique inbound rule for the running server
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] QString findIperf3Path() const;
    [[nodiscard]] BandwidthTestResult parseIperfJson(const QByteArray& json);
    [[nodiscard]] std::optional<double> measureTransferMbps(
        int sample_count, const std::function<std::pair<double, double>()>& sampler);
    void connectServerProcessSignals(const QString& ruleName, uint16_t port);
    void createFirewallRule(const QString& ruleName, uint16_t port);
    void removeFirewallRule(const QString& ruleName);
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::BandwidthTester>,
              "BandwidthTester must not be copyable.");
