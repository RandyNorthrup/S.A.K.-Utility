// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file registry_snapshot_engine.cpp
/// @brief Registry key snapshot capture and diff for leftover detection

#include "sak/registry_snapshot_engine.h"

#ifdef Q_OS_WIN
#include <vector>
#endif

namespace sak {

// Monitored registry paths for snapshot
const QStringList RegistrySnapshotEngine::kMonitoredPaths = {
    "HKLM\\SOFTWARE",
    "HKLM\\SOFTWARE\\WOW6432Node",
    "HKLM\\SYSTEM\\CurrentControlSet\\Services",
    "HKCU\\Software",
};

QSet<QString> RegistrySnapshotEngine::captureSnapshot() {
    QSet<QString> snapshot;

#ifdef Q_OS_WIN
    for (const auto& path : kMonitoredPaths) {
        // Parse hive and subkey
        HKEY hive = nullptr;
        QString subkey;

        if (path.startsWith("HKLM\\")) {
            hive = HKEY_LOCAL_MACHINE;
            subkey = path.mid(5);  // Skip "HKLM\\"
        } else if (path.startsWith("HKCU\\")) {
            hive = HKEY_CURRENT_USER;
            subkey = path.mid(5);  // Skip "HKCU\\"
        } else if (path.startsWith("HKCR")) {
            hive = HKEY_CLASSES_ROOT;
            subkey = path.mid(5);  // Skip "HKCR\\"
            if (path == "HKCR") {
                subkey = "";
            }
        }

        if (hive) {
            QString hive_name = path.left(path.indexOf('\\'));
            if (path == "HKCR") {
                hive_name = "HKCR";
            }
            enumerateKeys(hive, subkey, hive_name, snapshot, kDefaultMaxDepth);
        }
    }
#endif

    return snapshot;
}

QVector<LeftoverItem> RegistrySnapshotEngine::diffSnapshots(
    const QSet<QString>& before,
    const QSet<QString>& after,
    const QStringList& programNamePatterns) {
    QVector<LeftoverItem> leftovers;

    // Keys present in 'after' but not in 'before' = added during uninstall (rare)
    // Keys present in both = survived uninstall = potential leftovers
    // We care about keys that survived uninstall AND match program patterns

    for (const auto& key : after) {
        if (before.contains(key)) {
            // Key existed before AND after uninstall — survived
            // Check if it matches any program pattern
            bool matches = false;
            const QString key_lower = key.toLower();

            for (const auto& pattern : programNamePatterns) {
                if (key_lower.contains(pattern.toLower())) {
                    matches = true;
                    break;
                }
            }

            if (matches) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::RegistryKey;
                item.path = key;
                item.description = "Registry key survived uninstallation";
                item.risk = LeftoverItem::RiskLevel::Review;
                item.selected = false;
                leftovers.append(item);
            }
        }
    }

    // Keys added during uninstall (after but not before) — suspicious
    for (const auto& key : after) {
        if (!before.contains(key)) {
            const QString key_lower = key.toLower();
            bool matches = false;

            for (const auto& pattern : programNamePatterns) {
                if (key_lower.contains(pattern.toLower())) {
                    matches = true;
                    break;
                }
            }

            if (matches) {
                LeftoverItem item;
                item.type = LeftoverItem::Type::RegistryKey;
                item.path = key;
                item.description = "Registry key added during uninstallation";
                item.risk = LeftoverItem::RiskLevel::Review;
                item.selected = false;
                leftovers.append(item);
            }
        }
    }

    return leftovers;
}

#ifdef Q_OS_WIN

void RegistrySnapshotEngine::enumerateKeys(HKEY hive,
                                           const QString& subkey,
                                           const QString& hiveName,
                                           QSet<QString>& output,
                                           int maxDepth) {
    if (maxDepth <= 0) {
        return;
    }

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key);
    if (rc != ERROR_SUCCESS) {
        return;
    }

    // Add this key to snapshot
    QString full_path;
    if (subkey.isEmpty()) {
        full_path = hiveName;
    } else {
        full_path = hiveName + "\\" + subkey;
    }
    output.insert(full_path);

    // Enumerate subkeys
    DWORD subkey_count = 0;
    RegQueryInfoKeyW(key,
                     nullptr,
                     nullptr,
                     nullptr,
                     &subkey_count,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr,
                     nullptr);

    wchar_t subkey_name[256];
    for (DWORD i = 0; i < subkey_count; ++i) {
        DWORD name_len = 256;
        rc = RegEnumKeyExW(key, i, subkey_name, &name_len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            continue;
        }

        QString child_name = QString::fromWCharArray(subkey_name, name_len);

        // Skip known-large hives that would slow down snapshot
        if (child_name.startsWith("Classes", Qt::CaseInsensitive) && hiveName == "HKLM" &&
            subkey == "SOFTWARE") {
            continue;  // HKLM\SOFTWARE\Classes is huge
        }

        QString child_path = subkey.isEmpty() ? child_name : subkey + "\\" + child_name;

        enumerateKeys(hive, child_path, hiveName, output, maxDepth - 1);
    }

    RegCloseKey(key);
}

#endif  // Q_OS_WIN

}  // namespace sak
