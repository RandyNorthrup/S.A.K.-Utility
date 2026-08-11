// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_types.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>

#include <algorithm>

namespace sak {

namespace {
// Streaming read size for directory-tree hashing (B7-13).
constexpr qint64 kDirHashChunkBytes = 64 * 1024;
}  // namespace

// Helper function to safely convert JSON number to qint64
static qint64 qint64FromJson(const QJsonObject& json, const QString& key, qint64 defaultValue = 0) {
    if (!json.contains(key)) {
        return defaultValue;
    }
    const QJsonValue value = json[key];
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


QStringList SmartFilter::mandatoryDangerousFiles() {
    return {"NTUSER.DAT",
            "NTUSER.DAT.LOG1",
            "NTUSER.DAT.LOG2",
            "ntuser.ini",
            "UsrClass.dat",
            "UsrClass.dat.LOG1",
            "UsrClass.dat.LOG2"};
}

void SmartFilter::initializeDefaults() {
    // Dangerous files that MUST be excluded
    dangerous_files = mandatoryDangerousFiles();

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
    // No assert on empty: a missing/malformed "filter_rules" object must degrade to
    // the safe defaults (which the ctor already seeded), never abort or clear the
    // mandatory exclusions (B7-14).
    SmartFilter filter;
    filter.enable_file_size_limit = json.value("enable_file_size_limit").toBool(false);
    filter.enable_folder_size_limit = json.value("enable_folder_size_limit").toBool(false);
    // Preserve the seeded default when the key is absent, rather than zeroing it.
    filter.max_single_file_size_bytes =
        qint64FromJson(json, "max_single_file_size", filter.max_single_file_size_bytes);
    filter.max_folder_size_bytes =
        qint64FromJson(json, "max_folder_size", filter.max_folder_size_bytes);

    auto arrayToStringList = [](const QJsonArray& arr) {
        QStringList list;
        for (const auto& val : arr) {
            list << val.toString();
        }
        return list;
    };

    // Only override a list when the JSON actually carries an array for it; a missing
    // or non-array value keeps the default so partial/legacy manifests don't clear
    // the exclusion rules.
    auto loadListIfPresent = [&](const char* key, QStringList& target) {
        const QJsonValue value = json.value(QLatin1String(key));
        if (value.isArray()) {
            target = arrayToStringList(value.toArray());
        }
    };
    loadListIfPresent("exclude_patterns", filter.exclude_patterns);
    loadListIfPresent("exclude_folders", filter.exclude_folders);
    loadListIfPresent("dangerous_files", filter.dangerous_files);

    // dangerous_files are mandatory: re-add any built-in the supplied list omitted so
    // a hand-edited/hostile manifest can never expose a live registry hive.
    for (const QString& mandatory : mandatoryDangerousFiles()) {
        if (!filter.dangerous_files.contains(mandatory, Qt::CaseInsensitive)) {
            filter.dangerous_files.append(mandatory);
        }
    }

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
    // No assert on empty: a malformed entry degrades to defaults, never aborts (B7-14).
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
    obj["is_selected"] = is_selected;  // UI selection state was lost on round-trip (B7-34)
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
    profile.is_selected = json["is_selected"].toBool();
    profile.total_size_estimated = qint64FromJson(json, "total_size_estimated");

    const QJsonArray selections = json["folder_selections"].toArray();
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
    // No assert on empty: a malformed entry degrades to defaults, never aborts (B7-14).
    BackupUserData data;
    data.username = json["username"].toString();
    data.sid = json["sid"].toString();
    data.profile_path = json["profile_path"].toString();

    const QJsonArray folders = json["backed_up_folders"].toArray();
    for (const auto& val : folders) {
        data.backed_up_folders.append(FolderSelection::fromJson(val.toObject()));
    }

    // Parse permission mode
    const QString permMode = json["permissions_mode"].toString();
    if (permMode == "PreserveOriginal") {
        data.permissions_mode = PermissionMode::PreserveOriginal;
    } else if (permMode == "AssignToDestination") {
        data.permissions_mode = PermissionMode::AssignToDestination;
    } else {
        // Legacy manifests can carry "Hybrid". That mode stripped, so read it as StripAll
        // rather than dropping to the same default silently by accident - the mapping is
        // deliberate and matches what those backups actually did.
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
    obj["compressed"] = compressed;
    obj["encrypted"] = encrypted;

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
    // No assert on empty: loadFromFile of a truncated/corrupt manifest yields an
    // empty object; degrade to an all-default manifest instead of aborting (B7-14).
    BackupManifest manifest;

    QJsonObject metadata = json["backup_metadata"].toObject();
    manifest.version = metadata["version"].toString();
    manifest.created = QDateTime::fromString(metadata["created_date"].toString(), Qt::ISODate);
    manifest.source_machine = metadata["source_machine"].toString();
    manifest.sak_version = metadata["sak_utility_version"].toString();
    manifest.backup_type = metadata["backup_type"].toString();

    const QJsonArray usersArray = json["users"].toArray();
    for (const auto& val : usersArray) {
        manifest.users.append(BackupUserData::fromJson(val.toObject()));
    }

    manifest.filter_rules = SmartFilter::fromJson(json["filter_rules"].toObject());
    manifest.total_backup_size_bytes = qint64FromJson(json, "total_backup_size_bytes");
    manifest.manifest_checksum = json["manifest_checksum"].toString();
    // Absent in manifests written before per-file transforms existed, which were always
    // verbatim copies - so false is the correct reading of an older backup, not a guess.
    manifest.compressed = json["compressed"].toBool(false);
    manifest.encrypted = json["encrypted"].toBool(false);

    const QJsonArray wifiArray = json["wifi_profiles"].toArray();
    for (const auto& val : wifiArray) {
        manifest.wifi_profiles.append(WifiProfileInfo::fromJson(val.toObject()));
    }
    const QJsonArray ethArray = json["ethernet_configs"].toArray();
    for (const auto& val : ethArray) {
        manifest.ethernet_configs.append(EthernetConfigInfo::fromJson(val.toObject()));
    }
    const QJsonArray appDataArray = json["app_data_sources"].toArray();
    for (const auto& val : appDataArray) {
        manifest.app_data_sources.append(AppDataSourceInfo::fromJson(val.toObject()));
    }

    return manifest;
}

bool BackupManifest::saveToFile(const QString& path) const {
    // An empty path fails the open below and returns false. No assert.
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    const QJsonDocument doc(toJson());
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        return false;
    }
    return true;
}

BackupManifest BackupManifest::loadFromFile(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        // Fail closed: a missing/unreadable manifest must not masquerade as a valid
        // default manifest. The default ctor seeds version "1.0", which passes a
        // caller's version.isEmpty() validity gate; clear it so an open failure is
        // rejected exactly as a malformed-JSON parse already is (fromJson of an empty
        // object leaves version empty).
        BackupManifest invalid;
        invalid.version.clear();
        return invalid;
    }

    const QByteArray data = file.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(data);
    return fromJson(doc.object());
}

QString BackupManifest::checksumOfManifestJson(QJsonObject obj) {
    // Never let the digest cover itself: drop the checksum field before hashing so
    // save-side (field absent) and load-side (field present, then removed) agree.
    obj.remove(QStringLiteral("manifest_checksum"));
    const QByteArray canonical = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    return QString::fromLatin1(
        QCryptographicHash::hash(canonical, QCryptographicHash::Sha256).toHex());
}

QString BackupManifest::computeManifestChecksum() const {
    return checksumOfManifestJson(toJson());
}

bool BackupManifest::verifyManifestChecksum() const {
    // A legacy backup with no stored checksum is unverifiable, not corrupt.
    if (manifest_checksum.isEmpty()) {
        return true;
    }
    return computeManifestChecksum() == manifest_checksum;
}

QString BackupManifest::hashDirectoryTree(const QString& dir_path) {
    const QDir root(dir_path);
    if (!root.exists()) {
        return QString();
    }

    // Collect every regular file as a forward-slash relative path, then hash in a
    // stable sorted order so the digest is independent of filesystem enumeration
    // order (which differs between the backup source and the restored copy).
    QStringList relative_files;
    QDirIterator it(dir_path,
                    QDir::Files | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        relative_files.append(root.relativeFilePath(it.filePath()));
    }
    relative_files.sort();

    QCryptographicHash hasher(QCryptographicHash::Sha256);
    for (const QString& rel : relative_files) {
        QFile file(root.filePath(rel));
        if (!file.open(QIODevice::ReadOnly)) {
            // An unreadable file is folded in by path + a sentinel so it still
            // affects the digest (and a later readable/unreadable flip is caught).
            hasher.addData(rel.toUtf8());
            hasher.addData(QByteArrayLiteral("\0<unreadable>"));
            continue;
        }
        hasher.addData(rel.toUtf8());
        hasher.addData(QByteArrayLiteral("\0"));
        while (!file.atEnd()) {
            hasher.addData(file.read(kDirHashChunkBytes));
        }
    }
    return QString::fromLatin1(hasher.result().toHex());
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

namespace {

struct FolderTypeEntry {
    FolderType type;
    const char* name;
};

static constexpr FolderTypeEntry kFolderTypes[] = {
    {.type = FolderType::Documents, .name = "Documents"},
    {.type = FolderType::Desktop, .name = "Desktop"},
    {.type = FolderType::Pictures, .name = "Pictures"},
    {.type = FolderType::Videos, .name = "Videos"},
    {.type = FolderType::Music, .name = "Music"},
    {.type = FolderType::Downloads, .name = "Downloads"},
    {.type = FolderType::AppData_Roaming, .name = "AppData_Roaming"},
    {.type = FolderType::AppData_Local, .name = "AppData_Local"},
    {.type = FolderType::Favorites, .name = "Favorites"},
    {.type = FolderType::StartMenu, .name = "StartMenu"},
    {.type = FolderType::Custom, .name = "Custom"},
};

}  // namespace

QString folderTypeToString(FolderType type) {
    const auto* it = std::ranges::find_if(kFolderTypes,

                                          [type](const auto& entry) { return entry.type == type; });
    if (it != std::end(kFolderTypes)) {
        return QString::fromLatin1(it->name);
    }
    return QStringLiteral("Unknown");
}

FolderType stringToFolderType(const QString& str) {
    // Parses a name out of persisted JSON, so an empty or unrecognised value is
    // data and maps to Custom. The BackupManifest, FolderSelection and
    // BackupUserData fromJson asserts were removed for exactly this reason; this
    // one was missed and still aborted every Debug run.
    const auto* it = std::ranges::find_if(kFolderTypes, [&str](const auto& entry) {
        return str == QLatin1String(entry.name);
    });
    if (it != std::end(kFolderTypes)) {
        return it->type;
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
