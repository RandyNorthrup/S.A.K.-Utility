// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ethernet_config_manager.h
/// @brief Backup and restore of Ethernet adapter IP/DNS/gateway settings

#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>

#include <type_traits>

namespace sak {

/// @brief Stores a snapshot of an Ethernet adapter's IP configuration
struct EthernetConfigSnapshot {
    QString adapterName;  ///< Windows adapter name (e.g. "Ethernet")
    QString description;  ///< Adapter description (e.g. "Intel I219-V")
    QString macAddress;   ///< Physical address for identification

    // IPv4 settings
    bool dhcpEnabled = false;
    QString ipv4Address;
    QString ipv4SubnetMask;
    QString ipv4Gateway;
    QStringList ipv4DnsServers;

    // Metadata
    QString backupTimestamp;  ///< ISO 8601 timestamp of backup
    QString computerName;     ///< Name of the machine this was backed up from

    /// @brief Serialize to JSON
    [[nodiscard]] QJsonObject toJson() const;

    /// @brief Deserialize from JSON
    [[nodiscard]] static EthernetConfigSnapshot fromJson(const QJsonObject& obj);

    /// @brief Validate that all required fields are present
    [[nodiscard]] bool isValid() const;
};

/// @brief Manages backup and restore of Ethernet adapter settings
///
/// Uses `netsh interface ip show config` to capture settings and
/// `netsh interface ip set` commands to restore them. Backups are
/// stored as JSON files for portability across machines.
class EthernetConfigManager : public QObject {
    Q_OBJECT

public:
    explicit EthernetConfigManager(QObject* parent = nullptr);
    ~EthernetConfigManager() override = default;

    EthernetConfigManager(const EthernetConfigManager&) = delete;
    EthernetConfigManager& operator=(const EthernetConfigManager&) = delete;
    EthernetConfigManager(EthernetConfigManager&&) = delete;
    EthernetConfigManager& operator=(EthernetConfigManager&&) = delete;

    /// @brief Capture current settings of a named adapter
    /// @param adapterName  The Windows adapter name (e.g. "Ethernet")
    /// @return Snapshot of the adapter's IP configuration, or invalid snapshot on failure
    [[nodiscard]] EthernetConfigSnapshot captureSettings(const QString& adapterName);

    /// @brief Resolve an adapter's MAC address ("AA:BB:CC:DD:EE:FF") via QNetworkInterface,
    ///        matched by human-readable or system name. Empty if the adapter is not found.
    ///        Static and side-effect-free so it can be unit-tested.
    [[nodiscard]] static QString lookupAdapterMac(const QString& adapterName);

    /// @brief Save a snapshot to a JSON file
    /// @param snapshot  The configuration to save
    /// @param filePath  Destination file path
    /// @return true on success
    [[nodiscard]] bool saveToFile(const EthernetConfigSnapshot& snapshot, const QString& filePath);

    /// @brief Load a snapshot from a JSON file
    /// @param filePath  Source file path
    /// @return Loaded snapshot, or invalid snapshot on failure
    [[nodiscard]] EthernetConfigSnapshot loadFromFile(const QString& filePath);

    /// @brief Restore adapter settings from a snapshot
    /// @param snapshot     The configuration to apply
    /// @param adapterName  Target adapter name (may differ from snapshot's original)
    /// @param dnsApplied   Optional out: on the DHCP path, set to whether DNS was
    ///        also switched to automatic (previously discarded). Lets a caller
    ///        report the ACTUAL DNS outcome instead of implying it always worked.
    /// @return On the STATIC path, true only when every netsh command (IP and DNS) succeeded.
    ///        On the DHCP path, switching IPv4 to DHCP is the authoritative success; the
    ///        DNS-to-automatic step is reported through @p dnsApplied and does NOT by itself
    ///        fail the restore, so a caller that needs the DNS outcome must pass @p dnsApplied.
    [[nodiscard]] bool restoreSettings(const EthernetConfigSnapshot& snapshot,
                                       const QString& adapterName,
                                       bool* dnsApplied = nullptr);

    /// @brief Set an adapter's IPv4 DNS servers to a static list, leaving the
    /// adapter's IP configuration (DHCP or static) untouched.
    /// @param adapterName    Target adapter name
    /// @param dnsServers     Ordered IPv4 DNS server addresses (first = primary)
    /// @param primaryApplied Out: set true once the primary DNS is live, so a
    ///        caller can tell a nothing-applied failure (primary set failed) from
    ///        a partial one (primary live, a secondary add failed)
    /// @return true if every netsh dnsservers command succeeded
    [[nodiscard]] bool setDnsServers(const QString& adapterName,
                                     const QStringList& dnsServers,
                                     bool& primaryApplied);

    /// @brief List available Ethernet adapter names for backup
    [[nodiscard]] QStringList listEthernetAdapters();

    /// @brief Select @p adapterName out of a Get-NetIPConfiguration JSON scan and map it into a
    ///        snapshot. Language-neutral, unlike the netsh label scrape it replaced.
    [[nodiscard]] static EthernetConfigSnapshot snapshotFromNetIpConfig(const QString& json,
                                                                        const QString& adapterName);

Q_SIGNALS:
    /// @brief Emitted with status messages during backup/restore
    void logOutput(const QString& message);

    /// @brief Emitted on errors
    void errorOccurred(const QString& error);


private:
    /// @param dnsApplied Optional out: whether the DNS-to-automatic (source=dhcp)
    ///        step succeeded. IPv4 DHCP is the authoritative success; the DNS
    ///        result is surfaced here instead of being discarded.
    [[nodiscard]] bool restoreDhcpMode(const QString& adapterName, bool* dnsApplied = nullptr);
    [[nodiscard]] bool restoreStaticIp(const EthernetConfigSnapshot& snapshot,
                                       const QString& adapterName);
    /// @param primaryApplied Optional out: set true once the primary DNS command
    ///        succeeds (so a partial failure -- primary live, a secondary add
    ///        failed -- is distinguishable from a nothing-applied failure).
    [[nodiscard]] bool restoreDnsServers(const EthernetConfigSnapshot& snapshot,
                                         const QString& adapterName,
                                         bool* primaryApplied = nullptr);

    /// @brief Run a netsh command and return stdout. Used only by the adapter ENUMERATION below;
    ///        every netsh command that MUTATES an adapter lives in network_adapter_admin.h.
    [[nodiscard]] QString runNetsh(const QStringList& args, bool* ok = nullptr);

    /// @brief Run a PowerShell scan from System32, failing closed if it cannot be resolved.
    [[nodiscard]] QString runPowerShellCapture(const QString& script);
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(std::is_default_constructible_v<EthernetConfigSnapshot>,
              "EthernetConfigSnapshot must be default-constructible.");
static_assert(std::is_copy_constructible_v<EthernetConfigSnapshot>,
              "EthernetConfigSnapshot must be copy-constructible.");
static_assert(!std::is_copy_constructible_v<EthernetConfigManager>,
              "EthernetConfigManager must not be copy-constructible.");

}  // namespace sak
