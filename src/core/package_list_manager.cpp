// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file package_list_manager.cpp
/// @brief Curated package list management implementation

#include "sak/package_list_manager.h"

#include "sak/logger.h"
#include "sak/offline_deployment_constants.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace sak {

namespace {
/// @brief Hard cap on a package-list JSON file read from disk. A list holds at most
/// kMaxPackageListEntries small entries, so a few hundred KiB is realistic; 8 MiB
/// bounds a hostile/corrupt file before it is read wholesale into memory.
constexpr qint64 kMaxPackageListFileBytes = 8LL * 1024 * 1024;
}  // namespace

// ============================================================================
// Preset Lists
// ============================================================================

QStringList PackageListManager::presetNames() {
    return {"Office PC",
            "Developer Workstation",
            "Kiosk / POS",
            "Security / IT Admin",
            "Education Lab"};
}

PackageList PackageListManager::preset(const QString& name) {
    if (name == "Office PC") {
        return buildOfficePreset();
    }
    if (name == "Developer Workstation") {
        return buildDeveloperPreset();
    }
    if (name == "Kiosk / POS") {
        return buildKioskPreset();
    }
    if (name == "Security / IT Admin") {
        return buildSecurityPreset();
    }
    if (name == "Education Lab") {
        return buildEducationPreset();
    }

    sak::logWarning("[PackageListManager] Unknown preset: {}", name.toStdString());
    return createList(name, "Unknown preset");
}

// ============================================================================
// List Operations
// ============================================================================

PackageList PackageListManager::createList(const QString& name, const QString& description) {
    PackageList list;
    list.name = name;
    list.description = description;
    list.created_date = QDateTime::currentDateTime().toString(Qt::ISODate);
    list.modified_date = list.created_date;
    return list;
}

bool PackageListManager::addPackage(PackageList& list,
                                    const QString& package_id,
                                    const QString& version,
                                    const QString& notes,
                                    bool pinned) {
    if (package_id.trimmed().isEmpty()) {
        sak::logWarning("[PackageListManager] Rejected package with empty id");
        return false;
    }
    if (list.entries.size() >= offline::kMaxPackageListEntries) {
        sak::logWarning("[PackageListManager] List full: {} entries",
                        static_cast<int>(list.entries.size()));
        return false;
    }

    // Check for duplicates
    const bool exists =
        std::ranges::any_of(list.entries, [&package_id](const PackageListEntry& entry) {
            return entry.package_id.compare(package_id, Qt::CaseInsensitive) == 0;
        });

    if (exists) {
        return false;
    }

    PackageListEntry entry;
    entry.package_id = package_id;
    entry.version = version;
    entry.notes = notes;
    entry.pinned = pinned;
    list.entries.append(entry);

    list.modified_date = QDateTime::currentDateTime().toString(Qt::ISODate);
    return true;
}

bool PackageListManager::removePackage(PackageList& list, const QString& package_id) {
    auto iter = std::ranges::find_if(list.entries, [&package_id](const PackageListEntry& entry) {
        return entry.package_id.compare(package_id, Qt::CaseInsensitive) == 0;
    });

    if (iter == list.entries.end()) {
        return false;
    }

    list.entries.erase(iter);
    list.modified_date = QDateTime::currentDateTime().toString(Qt::ISODate);
    return true;
}

int PackageListManager::mergeLists(PackageList& target, const PackageList& source) {
    int added = 0;
    for (const auto& entry : source.entries) {
        // Preserve the pinned flag: dropping it here would let a pinned source entry merge in
        // as unpinned, silently losing the caller's intent to hold that package at its version.
        if (addPackage(target, entry.package_id, entry.version, entry.notes, entry.pinned)) {
            added++;
        }
    }
    return added;
}

// ============================================================================
// JSON Serialization
// ============================================================================

bool PackageListManager::saveToFile(const PackageList& list, const QString& file_path) {
    QJsonObject root;
    root["name"] = list.name;
    root["description"] = list.description;
    root["created"] = list.created_date;
    root["modified"] = list.modified_date;

    QJsonArray packages;
    for (const auto& entry : list.entries) {
        QJsonObject pkg;
        pkg["package_id"] = entry.package_id;
        if (!entry.version.isEmpty()) {
            pkg["version"] = entry.version;
        }
        if (!entry.notes.isEmpty()) {
            pkg["notes"] = entry.notes;
        }
        if (entry.pinned) {
            pkg["pinned"] = true;
        }
        packages.append(pkg);
    }
    root["packages"] = packages;

    // QSaveFile: a plain WriteOnly open truncates the existing list to 0 before writing, so a
    // failed/short write would destroy the prior good list. QSaveFile writes to a temp file and
    // only atomically renames on commit(), leaving the previous file intact on any failure.
    QSaveFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logError("[PackageListManager] Cannot write: {}", file_path.toStdString());
        return false;
    }

    const QJsonDocument doc(root);
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        sak::logError("[PackageListManager] Incomplete write: {}", file_path.toStdString());
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        sak::logError("[PackageListManager] Commit failed: {}", file_path.toStdString());
        return false;
    }

    sak::logInfo("[PackageListManager] Saved list '{}' ({} packages) to: {}",
                 list.name.toStdString(),
                 static_cast<int>(list.entries.size()),
                 file_path.toStdString());
    return true;
}

PackageList PackageListManager::loadFromFile(const QString& file_path) {
    PackageList list;

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logError("[PackageListManager] Cannot open: {}", file_path.toStdString());
        return list;
    }

    const qint64 size = file.size();
    if (size < 0 || size > kMaxPackageListFileBytes) {
        sak::logError("[PackageListManager] File too large or unreadable: {}",
                      file_path.toStdString());
        return list;
    }

    const QByteArray raw = file.readAll();
    if (file.error() != QFileDevice::NoError) {
        sak::logError("[PackageListManager] Read error: {}", file_path.toStdString());
        return list;
    }
    file.close();

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &parse_error);

    if (parse_error.error != QJsonParseError::NoError) {
        sak::logError("[PackageListManager] JSON parse error: {}",
                      parse_error.errorString().toStdString());
        return list;
    }

    if (!doc.isObject()) {
        sak::logError("[PackageListManager] Root is not a JSON object: {}",
                      file_path.toStdString());
        return list;
    }

    QJsonObject root = doc.object();
    list.name = root["name"].toString();
    list.description = root["description"].toString();
    list.created_date = root["created"].toString();
    list.modified_date = root["modified"].toString();

    const QJsonArray packages = root["packages"].toArray();
    for (const auto& val : packages) {
        if (list.entries.size() >= offline::kMaxPackageListEntries) {
            sak::logWarning("[PackageListManager] Entry cap reached ({}); extra packages ignored",
                            offline::kMaxPackageListEntries);
            break;
        }
        QJsonObject pkg = val.toObject();
        const QString package_id = pkg["package_id"].toString();
        if (package_id.trimmed().isEmpty()) {
            continue;  // a package with no id is unusable downstream; skip it (fail closed)
        }
        PackageListEntry entry;
        entry.package_id = package_id;
        entry.version = pkg["version"].toString();
        entry.notes = pkg["notes"].toString();
        entry.pinned = pkg["pinned"].toBool();
        list.entries.append(entry);
    }

    sak::logInfo("[PackageListManager] Loaded list '{}' ({} packages) from: {}",
                 list.name.toStdString(),
                 static_cast<int>(list.entries.size()),
                 file_path.toStdString());
    return list;
}

QVector<QPair<QString, QString>> PackageListManager::toPackagePairs(const PackageList& list) {
    QVector<QPair<QString, QString>> pairs;
    pairs.reserve(list.entries.size());
    for (const auto& entry : list.entries) {
        pairs.append({entry.package_id, entry.version});
    }
    return pairs;
}

// ============================================================================
// Preset Builders
// ============================================================================

PackageList PackageListManager::buildOfficePreset() {
    auto list = createList("Office PC",
                           "Standard office workstation with productivity "
                           "tools, browsers, and document viewers");

    addPackage(list, "googlechrome", "", "Web browser");
    addPackage(list, "firefox", "", "Web browser");
    addPackage(list, "7zip", "", "Archive utility");
    addPackage(list, "vlc", "", "Media player");
    addPackage(list, "adobereader", "", "PDF viewer");
    addPackage(list, "libreoffice-fresh", "", "Office suite");
    addPackage(list, "notepadplusplus", "", "Text editor");
    addPackage(list, "greenshot", "", "Screenshot tool");
    addPackage(list, "everything", "", "File search");
    addPackage(list, "treesizefree", "", "Disk space analyzer");

    return list;
}

PackageList PackageListManager::buildDeveloperPreset() {
    auto list = createList("Developer Workstation",
                           "Software development tools, IDEs, runtimes, "
                           "and version control utilities");

    addPackage(list, "git", "", "Version control");
    addPackage(list, "vscode", "", "Code editor");
    addPackage(list, "nodejs-lts", "", "Node.js runtime");
    addPackage(list, "python3", "", "Python runtime");
    addPackage(list, "dotnet-sdk", "", ".NET SDK");
    addPackage(list, "powershell-core", "", "PowerShell 7+");
    addPackage(list, "windows-terminal", "", "Terminal");
    addPackage(list, "postman", "", "API testing");
    addPackage(list, "winscp", "", "File transfer");
    addPackage(list, "putty", "", "SSH client");
    addPackage(list, "7zip", "", "Archive utility");
    addPackage(list, "notepadplusplus", "", "Text editor");

    return list;
}

PackageList PackageListManager::buildKioskPreset() {
    auto list = createList("Kiosk / POS",
                           "Minimal setup for kiosk or point-of-sale "
                           "terminals with only essential software");

    addPackage(list, "googlechrome", "", "Web browser (kiosk mode)");
    addPackage(list, "adobereader", "", "PDF viewer");
    addPackage(list, "7zip", "", "Archive utility");
    addPackage(list, "vlc", "", "Media player");

    return list;
}

PackageList PackageListManager::buildSecurityPreset() {
    auto list = createList("Security / IT Admin",
                           "Security tools, network utilities, and system "
                           "administration software for IT professionals");

    addPackage(list, "wireshark", "", "Network analyzer");
    addPackage(list, "nmap", "", "Port scanner");
    addPackage(list, "putty", "", "SSH client");
    addPackage(list, "winscp", "", "Secure file transfer");
    addPackage(list, "sysinternals", "", "System utilities");
    addPackage(list, "7zip", "", "Archive utility");
    addPackage(list, "notepadplusplus", "", "Text editor");
    addPackage(list, "everything", "", "File search");
    addPackage(list, "powershell-core", "", "PowerShell 7+");
    addPackage(list, "keepassxc", "", "Password manager");

    return list;
}

PackageList PackageListManager::buildEducationPreset() {
    auto list = createList("Education Lab",
                           "Software for educational computer labs with "
                           "browsers, office suite, and educational tools");

    addPackage(list, "googlechrome", "", "Web browser");
    addPackage(list, "firefox", "", "Web browser");
    addPackage(list, "libreoffice-fresh", "", "Office suite");
    addPackage(list, "vlc", "", "Media player");
    addPackage(list, "gimp", "", "Image editor");
    addPackage(list, "audacity", "", "Audio editor");
    addPackage(list, "7zip", "", "Archive utility");
    addPackage(list, "notepadplusplus", "", "Text editor");

    return list;
}

}  // namespace sak
