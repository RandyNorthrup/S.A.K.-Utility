// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file bandwidth_tester.h
/// @brief LAN bandwidth via iPerf3 and HTTP-based internet speed testing

#pragma once

#include "sak/network_diagnostic_types.h"

#include <QHostAddress>
#include <QObject>
#include <QProcess>

#include <atomic>
#include <functional>
#include <optional>
#include <type_traits>

namespace sak {

inline constexpr int kDefaultUdpBandwidthMbps = 100;

/// A peer that connects to the iPerf3 server and then goes quiet is dropped after this long, and
/// the server goes back to listening (iperf3 --idle-timeout).
inline constexpr int kIperfServerIdleTimeoutSec = 30;

/// @brief Bandwidth testing via bundled iPerf3 and HTTP download
///
/// For LAN: wraps iperf3.exe (bundled in tools/iperf3/) using QProcess
/// with JSON output parsing. For WAN: HTTP download speed measurement.
class BandwidthTester : public QObject {
    Q_OBJECT

public:
    /// The public server the HTTP speed test measures against.
    static constexpr auto kHttpSpeedTestHost = "speed.cloudflare.com";

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

    /// @brief Start the iPerf3 server (other SAK instances test against this), listening only on
    ///        @p bind_address: one of this PC's adapter addresses, or loopback.
    ///
    /// A server other PCs can reach starts only once its inbound Windows Firewall rule (private
    /// profile, local subnet, this iperf3.exe, this address and port) is in place. Adding the
    /// rule needs administrator rights; without the rule the server is not started and
    /// errorOccurred says why. A loopback server needs no rule: Windows Firewall does not filter
    /// loopback. A peer that goes quiet is dropped after kIperfServerIdleTimeoutSec.
    void startIperfServer(uint16_t port, const QHostAddress& bind_address);

    /// @brief The iperf3 arguments for a server on @p port that listens only on
    ///        @p bind_address and drops an idle peer. Pure; testable.
    [[nodiscard]] static QStringList iperfServerArguments(uint16_t port,
                                                          const QHostAddress& bind_address);

    /// @brief True when @p address names one interface a server can listen on: not null, not
    ///        a wildcard (any address), not broadcast or multicast. Pure; testable.
    [[nodiscard]] static bool isSpecificListenAddress(const QHostAddress& address);

    /// @brief Stop iPerf3 server
    void stopIperfServer();

    /// @brief Check if server is running
    [[nodiscard]] bool isServerRunning() const;

    /// @brief Run iPerf3 client test (blocking)
    void runIperfTest(const IperfConfig& requested);

    /// @brief Run HTTP-based internet speed test (blocking)
    void runHttpSpeedTest();

    /// @brief Check if iPerf3 is available
    [[nodiscard]] bool isIperf3Available() const;

    /// @brief Parse iPerf3 JSON (`-J`) output into a result.
    ///
    /// Returns nullopt when the bytes are not valid iPerf3 result JSON (parse error,
    /// or missing the `end` throughput summary) so a zero-exit run with unusable
    /// output is reported as an error, not a bogus all-zero "successful" test. Pure
    /// and static so it can be unit-tested with crafted JSON.
    [[nodiscard]] static std::optional<BandwidthTestResult> parseIperfJson(const QByteArray& json);

    /// @brief Compose the inbound firewall-rule name for an iPerf3 server instance.
    ///
    /// The name embeds a per-instance @p uniqueToken so two concurrent servers (or
    /// two SAK instances) never share a rule -- otherwise one server stopping would
    /// delete the still-running other's rule. Pure and deterministic; testable.
    [[nodiscard]] static QString composeFirewallRuleName(uint16_t port, const QString& uniqueToken);

    /// @brief Resolve the absolute netsh.exe path under a given Windows @p systemRoot.
    ///
    /// Returns "<systemRoot>/System32/netsh.exe" (cleaned). Fails closed with an empty
    /// string when @p systemRoot is empty, so callers never fall back to invoking a
    /// bare "netsh" that a PATH/CWD-planted binary could hijack. Pure and testable.
    [[nodiscard]] static QString composeNetshPath(const QString& systemRoot);

    /// @brief The SAK iPerf3 inbound rules in @p rule_names that no running SAK owns.
    ///
    /// A rule name carries its owner's process ID (composeFirewallRuleName with a "<pid>-<uuid>"
    /// token). A crash used to leave the server and its inbound allow rule behind for good; the
    /// next server start now deletes rules whose owner is gone (or that predate the owner ID).
    /// Never names a rule of @p own_pid or of a live SAK (@p sak_alive). Pure; testable.
    [[nodiscard]] static QStringList staleFirewallRules(
        const QStringList& rule_names,
        qint64 own_pid,
        const std::function<bool(qint64)>& sak_alive);

    void cancel();

Q_SIGNALS:
    void serverStarted(uint16_t port);
    void serverStopped();
    void serverClientConnected(QString clientIP);
    void testStarted(QString target);
    void testProgress(double currentMbps, double elapsedSec, double totalSec);
    void testComplete(sak::BandwidthTestResult result);
    void httpSpeedTestProgress(double downloadMbps, double uploadMbps);
    /// @brief HTTP speed test finished.
    /// @param latencyMs Round-trip latency, or a NEGATIVE value when it could not be
    ///        measured. Receivers must test for that and report it as unknown; it was
    ///        previously coerced to 0.0, which displayed a failed measurement as a
    ///        perfect one.
    void httpSpeedTestComplete(double downloadMbps, double uploadMbps, double latencyMs);
    void errorOccurred(QString error);

private:
    QString m_iperf3Path;
    QProcess* m_serverProcess = nullptr;       ///< Owned; destroyed in destructor
    QProcess* m_firewallAddProcess = nullptr;  ///< In-flight add; removes serialize behind it
    QString m_firewallRuleName;                ///< Unique inbound rule for the running server
    std::atomic<bool> m_cancelled{false};

    [[nodiscard]] QString findIperf3Path() const;
    [[nodiscard]] std::optional<double> measureTransferMbps(
        int sample_count, const std::function<std::pair<double, double>()>& sampler);
    void connectServerProcessSignals(const QString& ruleName, uint16_t port);
    /// True (errorOccurred emitted) when a server cannot start: no specific address, no
    /// iperf3.exe, or a server already exists.
    [[nodiscard]] bool serverStartRefused(const QHostAddress& bind_address);
    /// The server's QProcess, prepared and installed in its own Job (m_serverProcess), or null
    /// (errorOccurred emitted).
    [[nodiscard]] QProcess* containedServerProcess(uint16_t port,
                                                   const QHostAddress& bind_address,
                                                   const QString& ruleName);
    /// Start the prepared server process; its Job (held by a child of the process) contains it.
    void launchServer(QProcess* server);
    /// Called once the inbound rule for @p server is in place (@p added) or could not be added.
    void onServerFirewallRule(QProcess* server, bool added, const QString& why);
    using FirewallRuleDone = std::function<void(bool added, const QString& why)>;
    void createFirewallRule(const QString& ruleName,
                            uint16_t port,
                            const QHostAddress& bind_address,
                            FirewallRuleDone done);
    void removeFirewallRule(const QString& ruleName);
    void executeFirewallDelete(const QString& ruleName);
    /// Starts an asynchronous sweep that deletes rules a crashed earlier server left behind.
    void removeStaleFirewallRules();
    [[nodiscard]] static QString resolveNetshPath();
};

}  // namespace sak

static_assert(!std::is_copy_constructible_v<sak::BandwidthTester>,
              "BandwidthTester must not be copyable.");
