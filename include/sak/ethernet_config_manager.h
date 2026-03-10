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
    /// @return true if all netsh commands succeeded
    [[nodiscard]] bool restoreSettings(const EthernetConfigSnapshot& snapshot,
                                       const QString& adapterName);

    /// @brief List available Ethernet adapter names for backup
    [[nodiscard]] QStringList listEthernetAdapters();

Q_SIGNALS:
    /// @brief Emitted with status messages during backup/restore
    void logOutput(const QString& message);

    /// @brief Emitted on errors
    void errorOccurred(const QString& error);

private:
    /// @brief Run a netsh command and return stdout
    [[nodiscard]] QString runNetsh(const QStringList& args);

    /// @brief Parse the output of `netsh interface ip show config` for an adapter
    [[nodiscard]] static EthernetConfigSnapshot parseNetshConfig(const QString& output,
                                                                 const QString& adapterName);
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_default_constructible_v<EthernetConfigSnapshot>,
              "EthernetConfigSnapshot must be default-constructible.");
static_assert(std::is_copy_constructible_v<EthernetConfigSnapshot>,
              "EthernetConfigSnapshot must be copy-constructible.");
static_assert(!std::is_copy_constructible_v<EthernetConfigManager>,
              "EthernetConfigManager must not be copy-constructible.");

}  // namespace sak
