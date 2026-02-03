#include "sak/network_transfer_types.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QCryptographicHash>

namespace sak {

QJsonObject TransferPeerInfo::toJson() const {
    QJsonObject json;
    json["peer_id"] = peer_id;
    json["hostname"] = hostname;
    json["os"] = os;
    json["app_version"] = app_version;
    json["ip_address"] = ip_address;
    json["control_port"] = static_cast<int>(control_port);
    json["data_port"] = static_cast<int>(data_port);
    json["mode"] = mode;
    QJsonArray caps;
    for (const auto& cap : capabilities) {
        caps.append(cap);
    }
    json["capabilities"] = caps;
    json["timestamp"] = last_seen.toSecsSinceEpoch();
    return json;
}

TransferPeerInfo TransferPeerInfo::fromJson(const QJsonObject& json) {
    TransferPeerInfo info;
    info.peer_id = json.value("peer_id").toString();
    info.hostname = json.value("hostname").toString();
    info.os = json.value("os").toString();
    info.app_version = json.value("app_version").toString();
    info.ip_address = json.value("ip_address").toString();
    info.control_port = static_cast<quint16>(json.value("control_port").toInt(54322));
    info.data_port = static_cast<quint16>(json.value("data_port").toInt(54323));
    info.mode = json.value("mode").toString();
    auto caps = json.value("capabilities").toArray();
    for (const auto& cap : caps) {
        info.capabilities.append(cap.toString());
    }
    const auto timestamp = json.value("timestamp").toVariant().toLongLong();
    if (timestamp > 0) {
        info.last_seen = QDateTime::fromSecsSinceEpoch(timestamp);
    } else {
        info.last_seen = QDateTime::currentDateTime();
    }
    return info;
}

QJsonObject TransferFileEntry::toJson() const {
    QJsonObject json;
    json["file_id"] = file_id;
    json["relative_path"] = relative_path;
    json["size_bytes"] = static_cast<qint64>(size_bytes);
    json["checksum_sha256"] = checksum_sha256;
    if (!acl_sddl.isEmpty()) {
        json["acl_sddl"] = acl_sddl;
    }
    return json;
}

TransferFileEntry TransferFileEntry::fromJson(const QJsonObject& json) {
    TransferFileEntry entry;
    entry.file_id = json.value("file_id").toString();
    entry.relative_path = json.value("relative_path").toString();
    entry.size_bytes = json.value("size_bytes").toVariant().toLongLong();
    entry.checksum_sha256 = json.value("checksum_sha256").toString();
    entry.acl_sddl = json.value("acl_sddl").toString();
    return entry;
}

QJsonObject TransferManifest::toJson(bool include_files) const {
    QJsonObject json;
    json["protocol_version"] = protocol_version;
    json["transfer_id"] = transfer_id;
    json["source_hostname"] = source_hostname;
    json["source_os"] = source_os;
    json["sak_version"] = sak_version;
    json["created"] = created.toSecsSinceEpoch();
    json["total_bytes"] = static_cast<qint64>(total_bytes);
    json["total_files"] = total_files;
    json["checksum_sha256"] = checksum_sha256;

    QJsonArray users_array;
    for (const auto& user : users) {
        users_array.append(user.toJson());
    }
    json["users"] = users_array;

    if (include_files) {
        QJsonArray files_array;
        for (const auto& file : files) {
            files_array.append(file.toJson());
        }
        json["files"] = files_array;
    }

    return json;
}

TransferManifest TransferManifest::fromJson(const QJsonObject& json) {
    TransferManifest manifest;
    manifest.protocol_version = json.value("protocol_version").toString("1.0");
    manifest.transfer_id = json.value("transfer_id").toString();
    manifest.source_hostname = json.value("source_hostname").toString();
    manifest.source_os = json.value("source_os").toString();
    manifest.sak_version = json.value("sak_version").toString();
    manifest.created = QDateTime::fromSecsSinceEpoch(json.value("created").toVariant().toLongLong());
    manifest.total_bytes = json.value("total_bytes").toVariant().toLongLong();
    manifest.total_files = json.value("total_files").toInt();
    manifest.checksum_sha256 = json.value("checksum_sha256").toString();

    auto users_array = json.value("users").toArray();
    for (const auto& user : users_array) {
        manifest.users.append(BackupUserData::fromJson(user.toObject()));
    }

    auto files_array = json.value("files").toArray();
    for (const auto& file : files_array) {
        manifest.files.append(TransferFileEntry::fromJson(file.toObject()));
    }

    return manifest;
}

} // namespace sak
