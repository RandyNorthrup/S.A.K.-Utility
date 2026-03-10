// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_diagnostic_types.h
/// @brief Shared data structures for the Network Diagnostics Panel

#pragma once

#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QVector>

#include <cstdint>
#include <type_traits>

namespace sak {

// ═══════════════════════════════════════════════════════════════════════════
// Named Constants (TigerStyle)
// ═══════════════════════════════════════════════════════════════════════════

namespace netdiag {

// ── Default configuration values ──
constexpr int kDefaultPingCount = 10;
constexpr int kDefaultPingIntervalMs = 1000;
constexpr int kDefaultPingTimeoutMs = 4000;
constexpr int kDefaultPingPacketSize = 32;
constexpr int kDefaultPingTtl = 128;
constexpr int kDefaultTracerouteMaxHops = 30;
constexpr int kDefaultTracerouteTimeout = 5000;
constexpr int kDefaultTracerouteProbes = 3;
constexpr int kDefaultMtrCycles = 100;
constexpr int kDefaultPortScanTimeoutMs = 3000;
constexpr int kDefaultMaxConcurrent = 50;
constexpr int kDefaultBandwidthDuration = 10;
constexpr uint16_t kDefaultIperfPort = 5201;
constexpr int kDefaultWifiScanIntervalMs = 5000;
constexpr int kDefaultConnRefreshMs = 2000;
constexpr int kBannerGrabTimeoutMs = 2000;
constexpr int kBannerMaxBytes = 512;

// ── WiFi frequency boundaries (kHz) ──
constexpr uint32_t kFreq2GHzStart = 2'412'000;
constexpr uint32_t kFreq2GHzEnd = 2'484'000;
constexpr uint32_t kFreq5GHzStart = 5'170'000;
constexpr uint32_t kFreq5GHzEnd = 5'835'000;
constexpr uint32_t kFreq6GHzStart = 5'955'000;
constexpr uint32_t kFreq6GHzEnd = 7'115'000;

// ── Signal quality thresholds (dBm) ──
constexpr int kSignalExcellent = -50;
constexpr int kSignalGood = -60;
constexpr int kSignalFair = -70;
constexpr int kSignalWeak = -80;

}  // namespace netdiag

// ═══════════════════════════════════════════════════════════════════════════
// Network Adapter Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Complete network adapter information
struct NetworkAdapterInfo {
    // Identity
    QString name;
    QString description;
    QString adapterType;  ///< "Ethernet" / "WiFi" / "VPN" / "Loopback" / "Other"
    QString macAddress;
    uint32_t interfaceIndex = 0;

    // Status
    bool isConnected = false;
    uint64_t linkSpeedBps = 0;
    QString mediaState;

    // IPv4 Configuration
    QVector<QString> ipv4Addresses;
    QVector<QString> ipv4SubnetMasks;
    QString ipv4Gateway;
    QVector<QString> ipv4DnsServers;
    bool dhcpEnabled = false;
    QString dhcpServer;
    QDateTime dhcpLeaseObtained;
    QDateTime dhcpLeaseExpires;

    // IPv6 Configuration
    QVector<QString> ipv6Addresses;
    QString ipv6Gateway;
    QVector<QString> ipv6DnsServers;

    // Driver Info
    QString driverName;
    QString driverVersion;
    QString driverDate;

    // Statistics
    uint64_t bytesReceived = 0;
    uint64_t bytesSent = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsSent = 0;
    uint64_t errorsReceived = 0;
    uint64_t errorsSent = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Connectivity Testing Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Single ICMP echo reply
struct PingReply {
    int sequenceNumber = 0;
    bool success = false;
    double rttMs = 0.0;
    int ttl = 0;
    QString replyFrom;
    QString errorMessage;
};

/// @brief Aggregated ping result
struct PingResult {
    QString target;
    QString resolvedIP;
    QVector<PingReply> replies;

    // Statistics
    int sent = 0;
    int received = 0;
    int lost = 0;
    double lossPercent = 0.0;
    double minRtt = 0.0;
    double maxRtt = 0.0;
    double avgRtt = 0.0;
    double jitter = 0.0;
};

/// @brief Single traceroute hop
struct TracerouteHop {
    int hopNumber = 0;
    QString ipAddress;
    QString hostname;
    double rtt1Ms = 0.0;
    double rtt2Ms = 0.0;
    double rtt3Ms = 0.0;
    double avgRttMs = 0.0;
    bool timedOut = false;
};

/// @brief Complete traceroute result
struct TracerouteResult {
    QString target;
    QString resolvedIP;
    QVector<TracerouteHop> hops;
    bool reachedTarget = false;
    int totalHops = 0;
};

/// @brief MTR hop statistics (continuous ping+traceroute)
struct MtrHopStats {
    int hopNumber = 0;
    QString ipAddress;
    QString hostname;
    int sent = 0;
    int received = 0;
    double lossPercent = 0.0;
    double lastRttMs = 0.0;
    double avgRttMs = 0.0;
    double bestRttMs = 0.0;
    double worstRttMs = 0.0;
    double jitterMs = 0.0;
};

/// @brief Complete MTR result
struct MtrResult {
    QString target;
    QVector<MtrHopStats> hops;
    QDateTime startTime;
    int totalCycles = 0;
};

// ═══════════════════════════════════════════════════════════════════════════
// DNS Diagnostic Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Single DNS query result
struct DnsQueryResult {
    QString queryName;
    QString recordType;
    QString dnsServer;

    bool success = false;
    double responseTimeMs = 0.0;

    QVector<QString> answers;
    QString errorMessage;

    int ttlSeconds = 0;
    QString authoritySection;

    QDateTime queryTimestamp;
};

/// @brief Multi-server DNS comparison
struct DnsServerComparison {
    QString queryName;
    QString recordType;
    QVector<DnsQueryResult> results;

    bool allAgree = false;
    QString fastestServer;
    double fastestTimeMs = 0.0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Port Scanner Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Single port scan result
struct PortScanResult {
    QString target;
    uint16_t port = 0;

    enum class State {
        Open,
        Closed,
        Filtered,
        Error
    };
    State state = State::Error;

    double responseTimeMs = 0.0;
    QString serviceName;
    QString banner;
    QString errorMessage;

    QDateTime scanTimestamp;
};

/// @brief Named port preset for quick scanning
struct PortPreset {
    QString name;
    QVector<uint16_t> ports;
};

// ═══════════════════════════════════════════════════════════════════════════
// Bandwidth Test Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Bandwidth test result (iPerf3 or HTTP)
struct BandwidthTestResult {
    enum class TestMode {
        LanIperf3,
        WanHttp
    };
    TestMode mode = TestMode::LanIperf3;

    QString target;

    double downloadMbps = 0.0;
    double uploadMbps = 0.0;

    // iPerf3 specific
    double tcpWindowSize = 0.0;
    double retransmissions = 0.0;
    double jitterMs = 0.0;
    double packetLossPercent = 0.0;

    // Per-interval data for graphing
    struct IntervalData {
        double startSec = 0.0;
        double endSec = 0.0;
        double bitsPerSecond = 0.0;
        int retransmits = 0;
    };
    QVector<IntervalData> intervals;

    int durationSec = 0;
    int parallelStreams = 0;
    bool reverseMode = false;

    QDateTime timestamp;
};

// ═══════════════════════════════════════════════════════════════════════════
// WiFi Analyzer Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief WiFi network information from scan
struct WiFiNetworkInfo {
    QString ssid;
    QString bssid;

    int signalQuality = 0;
    int rssiDbm = 0;

    uint32_t channelFrequencyKHz = 0;
    int channelNumber = 0;
    QString band;
    int channelWidthMHz = 20;

    QString authentication;
    QString encryption;
    bool isSecure = false;

    QString bssType;
    bool isConnected = false;

    QString apVendor;
};

/// @brief Channel utilization analysis
struct WiFiChannelUtilization {
    int channelNumber = 0;
    QString band;
    int networkCount = 0;
    QVector<QString> ssids;
    double averageSignalDbm = 0.0;
    double interferenceScore = 0.0;
};

// ═══════════════════════════════════════════════════════════════════════════
// Active Connection Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Active network connection info
struct ConnectionInfo {
    enum class Protocol {
        TCP,
        UDP
    };
    Protocol protocol = Protocol::TCP;

    QString localAddress;
    uint16_t localPort = 0;
    QString remoteAddress;
    uint16_t remotePort = 0;

    QString state;

    uint32_t processId = 0;
    QString processName;
    QString processPath;

    QString remoteHostname;
    QString serviceName;
};

// ═══════════════════════════════════════════════════════════════════════════
// Firewall Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Windows Firewall rule
struct FirewallRule {
    QString name;
    QString description;
    bool enabled = false;

    enum class Direction {
        Inbound,
        Outbound
    };
    Direction direction = Direction::Inbound;

    enum class Action {
        Allow,
        Block
    };
    Action action = Action::Allow;

    enum class Protocol {
        TCP,
        UDP,
        ICMPv4,
        ICMPv6,
        Any
    };
    Protocol protocol = Protocol::Any;

    QString localPorts;
    QString remotePorts;
    QString localAddresses;
    QString remoteAddresses;

    QString applicationPath;
    QString serviceName;

    enum class Profile : int {
        Domain = 1,
        Private = 2,
        Public = 4
    };
    int profiles = 0;

    QString grouping;
};

/// @brief Firewall rule conflict
struct FirewallConflict {
    FirewallRule ruleA;
    FirewallRule ruleB;
    QString conflictDescription;

    enum class Severity {
        Info,
        Warning,
        Critical
    };
    Severity severity = Severity::Info;
};

/// @brief Firewall coverage gap
struct FirewallGap {
    QString description;
    QString recommendation;

    enum class Severity {
        Info,
        Warning
    };
    Severity severity = Severity::Info;
};

// ═══════════════════════════════════════════════════════════════════════════
// Network Share Types
// ═══════════════════════════════════════════════════════════════════════════

/// @brief Network share information
struct NetworkShareInfo {
    QString hostName;
    QString shareName;
    QString uncPath;

    enum class ShareType {
        Disk,
        Printer,
        Device,
        IPC,
        Special
    };
    ShareType type = ShareType::Disk;

    QString remark;

    bool canRead = false;
    bool canWrite = false;
    bool requiresAuth = false;
    QString accessError;

    QDateTime discovered;
};

}  // namespace sak

// ═══════════════════════════════════════════════════════════════════════════
// LAN File Transfer Speed Test
// ═══════════════════════════════════════════════════════════════════════════

namespace sak {

/// @brief Result of a LAN file transfer speed test between two local devices
struct LanTransferResult {
    QString remoteAddress;       ///< IP address of the peer
    uint16_t port{0};            ///< Port used for transfer
    qint64 bytesTransferred{0};  ///< Total bytes transferred
    double durationSec{0.0};     ///< Total transfer time in seconds
    double avgSpeedMbps{0.0};    ///< Average speed in Mbps
    double peakSpeedMbps{0.0};   ///< Peak observed speed in Mbps
    bool isUpload{true};         ///< True = sent data, false = received
    QDateTime timestamp;

    /// @brief Per-second speed samples for graphing
    QVector<double> speedSamplesMbps;
};

}  // namespace sak

// ═══════════════════════════════════════════════════════════════════════════
// Qt Metatype Registration
// ═══════════════════════════════════════════════════════════════════════════

Q_DECLARE_METATYPE(sak::NetworkAdapterInfo)
Q_DECLARE_METATYPE(QVector<sak::NetworkAdapterInfo>)
Q_DECLARE_METATYPE(sak::PingReply)
Q_DECLARE_METATYPE(sak::PingResult)
Q_DECLARE_METATYPE(sak::TracerouteHop)
Q_DECLARE_METATYPE(sak::TracerouteResult)
Q_DECLARE_METATYPE(sak::MtrHopStats)
Q_DECLARE_METATYPE(QVector<sak::MtrHopStats>)
Q_DECLARE_METATYPE(sak::MtrResult)
Q_DECLARE_METATYPE(sak::DnsQueryResult)
Q_DECLARE_METATYPE(sak::DnsServerComparison)
Q_DECLARE_METATYPE(sak::PortScanResult)
Q_DECLARE_METATYPE(QVector<sak::PortScanResult>)
Q_DECLARE_METATYPE(sak::BandwidthTestResult)
Q_DECLARE_METATYPE(sak::WiFiNetworkInfo)
Q_DECLARE_METATYPE(QVector<sak::WiFiNetworkInfo>)
Q_DECLARE_METATYPE(sak::WiFiChannelUtilization)
Q_DECLARE_METATYPE(QVector<sak::WiFiChannelUtilization>)
Q_DECLARE_METATYPE(sak::ConnectionInfo)
Q_DECLARE_METATYPE(QVector<sak::ConnectionInfo>)
Q_DECLARE_METATYPE(sak::FirewallRule)
Q_DECLARE_METATYPE(QVector<sak::FirewallRule>)
Q_DECLARE_METATYPE(sak::FirewallConflict)
Q_DECLARE_METATYPE(QVector<sak::FirewallConflict>)
Q_DECLARE_METATYPE(sak::FirewallGap)
Q_DECLARE_METATYPE(QVector<sak::FirewallGap>)
Q_DECLARE_METATYPE(sak::NetworkShareInfo)
Q_DECLARE_METATYPE(QVector<sak::NetworkShareInfo>)
Q_DECLARE_METATYPE(sak::LanTransferResult)

// ═══════════════════════════════════════════════════════════════════════════
// Compile-Time Invariants (TigerStyle)
// ═══════════════════════════════════════════════════════════════════════════

static_assert(std::is_default_constructible_v<sak::NetworkAdapterInfo>,
              "NetworkAdapterInfo must be default-constructible for signal transport.");
static_assert(std::is_copy_constructible_v<sak::PingResult>,
              "PingResult must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::TracerouteResult>,
              "TracerouteResult must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::DnsQueryResult>,
              "DnsQueryResult must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::PortScanResult>,
              "PortScanResult must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::BandwidthTestResult>,
              "BandwidthTestResult must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::WiFiNetworkInfo>,
              "WiFiNetworkInfo must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::ConnectionInfo>,
              "ConnectionInfo must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::FirewallRule>,
              "FirewallRule must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::NetworkShareInfo>,
              "NetworkShareInfo must be copyable for signal transport.");
static_assert(std::is_copy_constructible_v<sak::LanTransferResult>,
              "LanTransferResult must be copyable for signal transport.");
