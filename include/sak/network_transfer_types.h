#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QVector>
#include <QJsonObject>
#include <QHostAddress>
#include <QUuid>

#include "sak/user_profile_types.h"

namespace sak {

struct TransferPeerInfo {
    QString peer_id;
    QString hostname;
    QString os;
    QString app_version;
    QString ip_address;
    quint16 control_port{54322};
    quint16 data_port{54323};
    QString mode; // "source" or "destination"
    QStringList capabilities;
    QDateTime last_seen;

    QJsonObject toJson() const;
    static TransferPeerInfo fromJson(const QJsonObject& json);
};

struct TransferFileEntry {
    QString file_id;
    QString absolute_path;   // source only, not serialized to destination
    QString relative_path;   // relative to destination base
    qint64 size_bytes{0};
    QString checksum_sha256;
    QString acl_sddl;        // optional SDDL for ACL preservation

    QJsonObject toJson() const;
    static TransferFileEntry fromJson(const QJsonObject& json);
};

struct TransferManifest {
    QString protocol_version{"1.0"};
    QString transfer_id;
    QString source_hostname;
    QString source_os;
    QString sak_version;
    QDateTime created;
    QVector<BackupUserData> users;
    QVector<TransferFileEntry> files;
    qint64 total_bytes{0};
    int total_files{0};
    QString checksum_sha256; // manifest checksum

    QJsonObject toJson(bool include_files = true) const;
    static TransferManifest fromJson(const QJsonObject& json);
};

struct TransferSettings {
    bool encryption_enabled{true};
    bool compression_enabled{true};
    bool resume_enabled{true};
    bool auto_discovery_enabled{true};
    int max_bandwidth_kbps{0};
    int chunk_size{65536};
    quint16 discovery_port{54321};
    quint16 control_port{54322};
    quint16 data_port{54323};
    QString relay_server; // Phase 2
};

} // namespace sak
