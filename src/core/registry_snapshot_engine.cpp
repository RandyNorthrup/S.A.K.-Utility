// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file registry_snapshot_engine.cpp
/// @brief Registry key snapshot capture for leftover detection

#include "sak/registry_snapshot_engine.h"

#ifdef Q_OS_WIN
#include <vector>
#endif

namespace sak {

namespace {
constexpr DWORD kRegistrySubkeyNameBufferChars = 256;
}

// Monitored registry paths for snapshot
const QStringList RegistrySnapshotEngine::kMonitoredPaths = {
    "HKLM\\SOFTWARE",
    "HKLM\\SOFTWARE\\WOW6432Node",
    "HKLM\\SYSTEM\\CurrentControlSet\\Services",
    "HKCU\\Software",
};

#ifdef Q_OS_WIN
namespace {

// Open a monitored key for read. A hard failure (e.g. ERROR_ACCESS_DENIED) means an
// entire subtree is invisible to this snapshot -- fail closed by clearing @p reliable
// so the partial result is never consumed as authoritative. A key that simply no
// longer exists (deleted between enumeration and open) is NOT a reliability failure.
HKEY openKeyForRead(HKEY hive, const QString& subkey, bool& reliable) {
    HKEY key = nullptr;
    const LONG rc =
        RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(subkey.utf16()), 0, KEY_READ, &key);
    if (rc == ERROR_SUCCESS) {
        return key;
    }
    if (rc != ERROR_FILE_NOT_FOUND && rc != ERROR_PATH_NOT_FOUND) {
        reliable = false;
    }
    return nullptr;
}

// Query the subkey count. A failure means we cannot know how many children exist, so
// any of them could be silently missed -- fail closed.
bool querySubkeyCount(HKEY key, DWORD& count, bool& reliable) {
    const LONG rc = RegQueryInfoKeyW(key,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     &count,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr,
                                     nullptr);
    if (rc != ERROR_SUCCESS) {
        reliable = false;
        return false;
    }
    return true;
}

// Read every immediate child name. An enumeration error other than "no more items"
// clears @p reliable (a child was skipped), so the snapshot is flagged partial.
QStringList readChildNames(HKEY key, bool& reliable) {
    DWORD count = 0;
    if (!querySubkeyCount(key, count, reliable)) {
        return {};
    }
    QStringList names;
    wchar_t name_buf[kRegistrySubkeyNameBufferChars];
    for (DWORD i = 0; i < count; ++i) {
        DWORD len = kRegistrySubkeyNameBufferChars;
        const LONG rc = RegEnumKeyExW(key, i, name_buf, &len, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
            if (rc != ERROR_NO_MORE_ITEMS) {
                reliable = false;
            }
            continue;
        }
        names.append(QString::fromWCharArray(name_buf, len));
    }
    return names;
}

// HKLM\SOFTWARE\Classes is enormous and irrelevant to leftover detection; skip it.
bool shouldSkipChild(const QString& childName, const QString& hiveName, const QString& subkey) {
    return childName.startsWith("Classes", Qt::CaseInsensitive) && hiveName == "HKLM" &&
           subkey == "SOFTWARE";
}

QString joinKeyPath(const QString& hiveName, const QString& subkey) {
    return subkey.isEmpty() ? hiveName : hiveName + "\\" + subkey;
}

}  // namespace
#endif  // Q_OS_WIN

QSet<QString> RegistrySnapshotEngine::captureSnapshot(bool* reliable) {
    QSet<QString> snapshot;
    bool ok = true;

#ifdef Q_OS_WIN
    SnapshotSink sink{snapshot, ok};
    for (const auto& path : kMonitoredPaths) {
        HKEY hive = nullptr;
        QString subkey;

        if (path.startsWith("HKLM\\")) {
            hive = HKEY_LOCAL_MACHINE;
            subkey = path.mid(5);  // Skip "HKLM\\"
        } else if (path.startsWith("HKCU\\")) {
            hive = HKEY_CURRENT_USER;
            subkey = path.mid(5);  // Skip "HKCU\\"
        }

        if (hive) {
            const QString hive_name = path.left(path.indexOf('\\'));
            enumerateKeys(hive, subkey, hive_name, sink, kDefaultMaxDepth);
        }
    }
#endif

    if (reliable != nullptr) {
        *reliable = ok;
    }
    return snapshot;
}

#ifdef Q_OS_WIN

void RegistrySnapshotEngine::enumerateKeys(
    HKEY hive, const QString& subkey, const QString& hiveName, SnapshotSink sink, int maxDepth) {
    if (maxDepth <= 0) {
        return;
    }
    HKEY key = openKeyForRead(hive, subkey, sink.reliable);
    if (key == nullptr) {
        return;
    }
    sink.keys.insert(joinKeyPath(hiveName, subkey));

    const QStringList children = readChildNames(key, sink.reliable);
    RegCloseKey(key);

    for (const QString& child : children) {
        if (shouldSkipChild(child, hiveName, subkey)) {
            continue;
        }
        const QString child_path = subkey.isEmpty() ? child : subkey + "\\" + child;
        enumerateKeys(hive, child_path, hiveName, sink, maxDepth - 1);
    }
}

#endif  // Q_OS_WIN

}  // namespace sak
