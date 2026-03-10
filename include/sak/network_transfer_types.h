// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/network_constants.h"
#include "sak/user_profile_types.h"

#include <QDateTime>
#include <QHostAddress>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QUuid>
#include <QVector>

namespace sak {

/// @brief Discovery metadata for a network transfer peer
struct TransferPeerInfo {
    QString peer_id;
    QString hostname;
    QString os;
    QString app_version;
    QString ip_address;
    quint16 control_port{sak::kPortControl};
    quint16 data_port{sak::kPortData};
    QString mode;  // "source" or "destination"
    QStringList capabilities;
    QDateTime last_seen;

    QJsonObject toJson() const;
    static TransferPeerInfo fromJson(const QJsonObject& json);
};

/// @brief Metadata for a single file in a transfer manifest
struct TransferFileEntry {
    QString file_id;
    QString absolute_path;  // source only, not serialized to destination
    QString relative_path;  // relative to destination base
    qint64 size_bytes{0};
    QString checksum_sha256;
    QString acl_sddl;  // optional SDDL for ACL preservation

    QJsonObject toJson() const;
    static TransferFileEntry fromJson(const QJsonObject& json);
};

/// @brief Complete manifest describing files and users to transfer
struct TransferManifest {
    QString protocol_version{"1.0"};
    QString transfer_id;
    QString source_hostname;
    QString source_os;
    QString sak_version;
    QDateTime created;
    QVector<BackupUserData> users;
    QVector<TransferFileEntry> files;
    QVector<InstalledAppInfo> installed_apps;
    QVector<WifiProfileInfo> wifi_profiles;
    QVector<EthernetConfigInfo> ethernet_configs;
    QVector<AppDataSourceInfo> app_data_sources;
    qint64 total_bytes{0};
    int total_files{0};
    QString checksum_sha256;  // manifest checksum

    QJsonObject toJson(bool include_files = true) const;
    static TransferManifest fromJson(const QJsonObject& json);
};

/// @brief Configuration options for network transfer sessions
struct TransferSettings {
    bool encryption_enabled{true};
    bool compression_enabled{true};
    bool resume_enabled{true};
    bool auto_discovery_enabled{true};
    int max_bandwidth_kbps{0};
    int chunk_size{static_cast<int>(sak::kBufferChunkDefault)};
    quint16 discovery_port{sak::kPortDiscovery};
    quint16 control_port{sak::kPortControl};
    quint16 data_port{sak::kPortData};
    QString relay_server;  // Phase 2
};

}  // namespace sak
