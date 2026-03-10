// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_types.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

namespace sak {

// Helper function to safely convert JSON number to qint64
static qint64 qint64FromJson(const QJsonObject& json, const QString& key, qint64 defaultValue = 0) {
    if (!json.contains(key)) {
        return defaultValue;
    }
    QJsonValue value = json[key];
    if (value.isDouble()) {
        return static_cast<qint64>(value.toDouble());
    }
    return defaultValue;
}


QJsonObject InstalledAppInfo::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["version"] = version;
    obj["publisher"] = publisher;
    obj["choco_package"] = choco_package;
    obj["category"] = category;
    obj["selected"] = selected;
    return obj;
}

InstalledAppInfo InstalledAppInfo::fromJson(const QJsonObject& json) {
    InstalledAppInfo info;
    info.name = json["name"].toString();
    info.version = json["version"].toString();
    info.publisher = json["publisher"].toString();
    info.choco_package = json["choco_package"].toString();
    info.category = json["category"].toString();
    info.selected = json["selected"].toBool(true);
    return info;
}


void SmartFilter::initializeDefaults() {
    // Dangerous files that MUST be excluded
    dangerous_files = {"NTUSER.DAT",
                       "NTUSER.DAT.LOG1",
                       "NTUSER.DAT.LOG2",
                       "ntuser.ini",
                       "UsrClass.dat",
                       "UsrClass.dat.LOG1",
                       "UsrClass.dat.LOG2"};

    // Pattern exclusions (case-insensitive)
    exclude_patterns = {".*\\.tmp$",
                        ".*\\.temp$",
                        ".*\\.cache$",
                        ".*\\.lock$",
                        ".*\\.lck$",
                        ".*~$",
                        ".*\\.crdownload$",
                        ".*\\.part$",
                        "desktop\\.ini$",
                        "thumbs\\.db$",
                        "\\.DS_Store$"};

    // Folder exclusions
    exclude_folders = {
        "Temp",
        "temp",
        "$RECYCLE.BIN",
        "Cache",
        "GPUCache",
        "Code Cache",
        "Service Worker",
        "Session Storage",
        "WebCache",
        "node_modules",
        ".git",
        ".svn",
        "__pycache__",
        "Packages"  // UWP apps
    };
}

QJsonObject SmartFilter::toJson() const {
    QJsonObject obj;
    obj["enable_file_size_limit"] = enable_file_size_limit;
    obj["enable_folder_size_limit"] = enable_folder_size_limit;
    obj["max_single_file_size"] = static_cast<double>(max_single_file_size_bytes);
    obj["max_folder_size"] = static_cast<double>(max_folder_size_bytes);
    obj["exclude_patterns"] = QJsonArray::fromStringList(exclude_patterns);
    obj["exclude_folders"] = QJsonArray::fromStringList(exclude_folders);
    obj["dangerous_files"] = QJsonArray::fromStringList(dangerous_files);
    return obj;
}

SmartFilter SmartFilter::fromJson(const QJsonObject& json) {
    Q_ASSERT(!json.isEmpty());
    SmartFilter filter;
    filter.enable_file_size_limit = json.value("enable_file_size_limit").toBool(false);
    filter.enable_folder_size_limit = json.value("enable_folder_size_limit").toBool(false);
    filter.max_single_file_size_bytes = qint64FromJson(json, "max_single_file_size");
    filter.max_folder_size_bytes = qint64FromJson(json, "max_folder_size");

    auto arrayToStringList = [](const QJsonArray& arr) {
        QStringList list;
        for (const auto& val : arr) {
            list << val.toString();
        }
        return list;
    };

    filter.exclude_patterns = arrayToStringList(json["exclude_patterns"].toArray());
    filter.exclude_folders = arrayToStringList(json["exclude_folders"].toArray());
    filter.dangerous_files = arrayToStringList(json["dangerous_files"].toArray());

    return filter;
}

QJsonObject FolderSelection::toJson() const {
    QJsonObject obj;
    obj["type"] = folderTypeToString(type);
    obj["display_name"] = display_name;
    obj["relative_path"] = relative_path;
    obj["selected"] = selected;
    obj["include_patterns"] = QJsonArray::fromStringList(include_patterns);
    obj["exclude_patterns"] = QJsonArray::fromStringList(exclude_patterns);
    obj["size_bytes"] = static_cast<double>(size_bytes);
    obj["file_count"] = file_count;
    return obj;
}

FolderSelection FolderSelection::fromJson(const QJsonObject& json) {
    Q_ASSERT(!json.isEmpty());
    FolderSelection sel;
    sel.type = stringToFolderType(json["type"].toString());
    sel.display_name = json["display_name"].toString();
    sel.relative_path = json["relative_path"].toString();
    sel.selected = json["selected"].toBool();
    sel.size_bytes = qint64FromJson(json, "size_bytes");
    sel.file_count = json["file_count"].toInt();

    auto arrayToStringList = [](const QJsonArray& arr) {
        QStringList list;
        for (const auto& val : arr) {
            list << val.toString();
        }
        return list;
    };

    sel.include_patterns = arrayToStringList(json["include_patterns"].toArray());
    sel.exclude_patterns = arrayToStringList(json["exclude_patterns"].toArray());

    return sel;
}

QJsonObject UserProfile::toJson() const {
    QJsonObject obj;
    obj["username"] = username;
    obj["sid"] = sid;
    obj["profile_path"] = profile_path;
    obj["is_current_user"] = is_current_user;
    obj["total_size_estimated"] = static_cast<double>(total_size_estimated);

    QJsonArray selections;
    for (const auto& sel : folder_selections) {
        selections.append(sel.toJson());
    }
    obj["folder_selections"] = selections;

    return obj;
}

UserProfile UserProfile::fromJson(const QJsonObject& json) {
    UserProfile profile;
    profile.username = json["username"].toString();
    profile.sid = json["sid"].toString();
    profile.profile_path = json["profile_path"].toString();
    profile.is_current_user = json["is_current_user"].toBool();
    profile.total_size_estimated = qint64FromJson(json, "total_size_estimated");

    QJsonArray selections = json["folder_selections"].toArray();
    for (const auto& val : selections) {
        profile.folder_selections.append(FolderSelection::fromJson(val.toObject()));
    }

    return profile;
}

QJsonObject BackupUserData::toJson() const {
    QJsonObject obj;
    obj["username"] = username;
    obj["sid"] = sid;
    obj["profile_path"] = profile_path;

    QJsonArray folders;
    for (const auto& folder : backed_up_folders) {
        folders.append(folder.toJson());
    }
    obj["backed_up_folders"] = folders;

    obj["permissions_mode"] = permissionModeToString(permissions_mode);
    obj["encrypted"] = encrypted;
    obj["compression"] = compression;
    obj["checksum_sha256"] = checksum_sha256;

    return obj;
}

BackupUserData BackupUserData::fromJson(const QJsonObject& json) {
    Q_ASSERT(!json.isEmpty());
    BackupUserData data;
    data.username = json["username"].toString();
    data.sid = json["sid"].toString();
    data.profile_path = json["profile_path"].toString();

    QJsonArray folders = json["backed_up_folders"].toArray();
    for (const auto& val : folders) {
        data.backed_up_folders.append(FolderSelection::fromJson(val.toObject()));
    }

    // Parse permission mode
    QString permMode = json["permissions_mode"].toString();
    if (permMode == "PreserveOriginal") {
        data.permissions_mode = PermissionMode::PreserveOriginal;
    } else if (permMode == "AssignToDestination") {
        data.permissions_mode = PermissionMode::AssignToDestination;
    } else if (permMode == "Hybrid") {
        data.permissions_mode = PermissionMode::Hybrid;
    } else {
        data.permissions_mode = PermissionMode::StripAll;
    }

    data.encrypted = json["encrypted"].toBool();
    data.compression = json["compression"].toString();
    data.checksum_sha256 = json["checksum_sha256"].toString();

    return data;
}

QJsonObject BackupManifest::toJson() const {
    QJsonObject obj;

    QJsonObject metadata;
    metadata["version"] = version;
    metadata["created_date"] = created.toString(Qt::ISODate);
    metadata["source_machine"] = source_machine;
    metadata["sak_utility_version"] = sak_version;
    metadata["backup_type"] = backup_type;
    obj["backup_metadata"] = metadata;

    QJsonArray usersArray;
    for (const auto& user : users) {
        usersArray.append(user.toJson());
    }
    obj["users"] = usersArray;

    obj["filter_rules"] = filter_rules.toJson();
    obj["total_backup_size_bytes"] = static_cast<double>(total_backup_size_bytes);
    obj["manifest_checksum"] = manifest_checksum;

    QJsonArray wifiArray;
    for (const auto& w : wifi_profiles) {
        wifiArray.append(w.toJson());
    }
    if (!wifiArray.isEmpty()) {
        obj["wifi_profiles"] = wifiArray;
    }

    QJsonArray ethArray;
    for (const auto& e : ethernet_configs) {
        ethArray.append(e.toJson());
    }
    if (!ethArray.isEmpty()) {
        obj["ethernet_configs"] = ethArray;
    }

    QJsonArray appDataArray;
    for (const auto& a : app_data_sources) {
        appDataArray.append(a.toJson());
    }
    if (!appDataArray.isEmpty()) {
        obj["app_data_sources"] = appDataArray;
    }

    return obj;
}

BackupManifest BackupManifest::fromJson(const QJsonObject& json) {
    Q_ASSERT(!json.isEmpty());
    BackupManifest manifest;

    QJsonObject metadata = json["backup_metadata"].toObject();
    manifest.version = metadata["version"].toString();
    manifest.created = QDateTime::fromString(metadata["created_date"].toString(), Qt::ISODate);
    manifest.source_machine = metadata["source_machine"].toString();
    manifest.sak_version = metadata["sak_utility_version"].toString();
    manifest.backup_type = metadata["backup_type"].toString();

    QJsonArray usersArray = json["users"].toArray();
    for (const auto& val : usersArray) {
        manifest.users.append(BackupUserData::fromJson(val.toObject()));
    }

    manifest.filter_rules = SmartFilter::fromJson(json["filter_rules"].toObject());
    manifest.total_backup_size_bytes = qint64FromJson(json, "total_backup_size_bytes");
    manifest.manifest_checksum = json["manifest_checksum"].toString();

    QJsonArray wifiArray = json["wifi_profiles"].toArray();
    for (const auto& val : wifiArray) {
        manifest.wifi_profiles.append(WifiProfileInfo::fromJson(val.toObject()));
    }
    QJsonArray ethArray = json["ethernet_configs"].toArray();
    for (const auto& val : ethArray) {
        manifest.ethernet_configs.append(EthernetConfigInfo::fromJson(val.toObject()));
    }
    QJsonArray appDataArray = json["app_data_sources"].toArray();
    for (const auto& val : appDataArray) {
        manifest.app_data_sources.append(AppDataSourceInfo::fromJson(val.toObject()));
    }

    return manifest;
}

bool BackupManifest::saveToFile(const QString& path) const {
    Q_ASSERT(!path.isEmpty());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QJsonDocument doc(toJson());
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        return false;
    }
    return true;
}

BackupManifest BackupManifest::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return BackupManifest();
    }

    QByteArray data = file.readAll();
    QJsonDocument doc = QJsonDocument::fromJson(data);
    return fromJson(doc.object());
}

QString OperationResult::getSummary() const {
    return QString("Files: %1 processed, %2 skipped, %3 failed | Bytes: %4 | Duration: %5 sec")
        .arg(files_processed)
        .arg(files_skipped)
        .arg(files_failed)
        .arg(bytes_processed)
        .arg(started.secsTo(completed));
}

// Helper functions
QString folderTypeToString(FolderType type) {
    switch (type) {
    case FolderType::Documents:
        return "Documents";
    case FolderType::Desktop:
        return "Desktop";
    case FolderType::Pictures:
        return "Pictures";
    case FolderType::Videos:
        return "Videos";
    case FolderType::Music:
        return "Music";
    case FolderType::Downloads:
        return "Downloads";
    case FolderType::AppData_Roaming:
        return "AppData_Roaming";
    case FolderType::AppData_Local:
        return "AppData_Local";
    case FolderType::Favorites:
        return "Favorites";
    case FolderType::StartMenu:
        return "StartMenu";
    case FolderType::Custom:
        return "Custom";
    }
    return "Unknown";
}

FolderType stringToFolderType(const QString& str) {
    Q_ASSERT(!str.isEmpty());
    if (str == "Documents") {
        return FolderType::Documents;
    }
    if (str == "Desktop") {
        return FolderType::Desktop;
    }
    if (str == "Pictures") {
        return FolderType::Pictures;
    }
    if (str == "Videos") {
        return FolderType::Videos;
    }
    if (str == "Music") {
        return FolderType::Music;
    }
    if (str == "Downloads") {
        return FolderType::Downloads;
    }
    if (str == "AppData_Roaming") {
        return FolderType::AppData_Roaming;
    }
    if (str == "AppData_Local") {
        return FolderType::AppData_Local;
    }
    if (str == "Favorites") {
        return FolderType::Favorites;
    }
    if (str == "StartMenu") {
        return FolderType::StartMenu;
    }
    return FolderType::Custom;
}

QString permissionModeToString(PermissionMode mode) {
    switch (mode) {
    case PermissionMode::StripAll:
        return "StripAll";
    case PermissionMode::PreserveOriginal:
        return "PreserveOriginal";
    case PermissionMode::AssignToDestination:
        return "AssignToDestination";
    case PermissionMode::Hybrid:
        return "Hybrid";
    }
    return "StripAll";
}

QString mergeModeToString(MergeMode mode) {
    switch (mode) {
    case MergeMode::ReplaceDestination:
        return "Replace";
    case MergeMode::MergeIntoDestination:
        return "Merge";
    case MergeMode::CreateNewUser:
        return "Create New";
    }
    return "Replace";
}

QString conflictResolutionToString(ConflictResolution mode) {
    switch (mode) {
    case ConflictResolution::SkipDuplicate:
        return "Skip";
    case ConflictResolution::RenameWithSuffix:
        return "Rename";
    case ConflictResolution::KeepNewer:
        return "Keep Newer";
    case ConflictResolution::KeepLarger:
        return "Keep Larger";
    case ConflictResolution::PromptUser:
        return "Ask Me";
    }
    return "Rename";
}

QJsonObject WifiProfileInfo::toJson() const {
    QJsonObject obj;
    obj["profile_name"] = profile_name;
    obj["security_type"] = security_type;
    obj["xml_data"] = xml_data;
    obj["selected"] = selected;
    return obj;
}

WifiProfileInfo WifiProfileInfo::fromJson(const QJsonObject& json) {
    WifiProfileInfo info;
    info.profile_name = json["profile_name"].toString();
    info.security_type = json["security_type"].toString();
    info.xml_data = json["xml_data"].toString();
    info.selected = json["selected"].toBool(true);
    return info;
}

QJsonObject EthernetConfigInfo::toJson() const {
    QJsonObject obj;
    obj["adapter_name"] = adapter_name;
    obj["description"] = description;
    obj["dhcp_enabled"] = dhcp_enabled;
    obj["ip_address"] = ip_address;
    obj["subnet_mask"] = subnet_mask;
    obj["default_gateway"] = default_gateway;
    obj["dns_primary"] = dns_primary;
    obj["dns_secondary"] = dns_secondary;
    obj["selected"] = selected;
    return obj;
}

EthernetConfigInfo EthernetConfigInfo::fromJson(const QJsonObject& json) {
    EthernetConfigInfo info;
    info.adapter_name = json["adapter_name"].toString();
    info.description = json["description"].toString();
    info.dhcp_enabled = json["dhcp_enabled"].toBool(true);
    info.ip_address = json["ip_address"].toString();
    info.subnet_mask = json["subnet_mask"].toString();
    info.default_gateway = json["default_gateway"].toString();
    info.dns_primary = json["dns_primary"].toString();
    info.dns_secondary = json["dns_secondary"].toString();
    info.selected = json["selected"].toBool(true);
    return info;
}

QJsonObject AppDataSourceInfo::toJson() const {
    QJsonObject obj;
    obj["name"] = name;
    obj["category"] = category;
    obj["relative_path"] = relative_path;
    obj["size_bytes"] = static_cast<double>(size_bytes);
    obj["exists"] = exists;
    obj["selected"] = selected;
    return obj;
}

AppDataSourceInfo AppDataSourceInfo::fromJson(const QJsonObject& json) {
    AppDataSourceInfo info;
    info.name = json["name"].toString();
    info.category = json["category"].toString();
    info.relative_path = json["relative_path"].toString();
    info.size_bytes = qint64FromJson(json, "size_bytes");
    info.exists = json["exists"].toBool(false);
    info.selected = json["selected"].toBool(true);
    return info;
}

}  // namespace sak
