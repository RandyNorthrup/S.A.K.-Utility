// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cleanup_worker.cpp
/// @brief Deletes selected leftover items safely on a background thread

#include "sak/cleanup_worker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

CleanupWorker::CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                             QObject* parent)
    : WorkerBase(parent)
    , m_items(selectedItems)
{
}

auto CleanupWorker::execute() -> std::expected<void, sak::error_code>
{
    int succeeded = 0;
    int failed = 0;
    qint64 bytes_recovered = 0;

    const int total = m_items.size();

    for (int i = 0; i < total; ++i) {
        if (checkStop()) {
            Q_EMIT cleanupComplete(succeeded, failed, bytes_recovered);
            return {};
        }

        const auto& item = m_items[i];
        if (!item.selected) {
            continue;
        }

        reportProgress(i, total,
                       QString("Cleaning: %1").arg(item.path));

        bool ok = false;

        switch (item.type) {
        case LeftoverItem::Type::File:
            ok = deleteFile(item.path);
            break;

        case LeftoverItem::Type::Folder:
            ok = deleteFolder(item.path);
            break;

        case LeftoverItem::Type::RegistryKey:
            ok = deleteRegistryKey(item.path);
            break;

        case LeftoverItem::Type::RegistryValue:
            ok = deleteRegistryValue(item.path, item.registryValueName);
            break;

        case LeftoverItem::Type::Service:
            ok = removeService(item.path);
            break;

        case LeftoverItem::Type::ScheduledTask:
            ok = removeScheduledTask(item.path);
            break;

        case LeftoverItem::Type::FirewallRule:
            ok = removeFirewallRule(item.path);
            break;

        case LeftoverItem::Type::StartupEntry:
            // Could be a file (shortcut) or registry value
            if (!item.registryValueName.isEmpty()) {
                ok = deleteRegistryValue(item.path, item.registryValueName);
            } else {
                ok = deleteFile(item.path);
            }
            break;

        case LeftoverItem::Type::ShellExtension:
            ok = deleteRegistryKey(item.path);
            break;
        }

        if (ok) {
            ++succeeded;
            bytes_recovered += item.sizeBytes;
        } else {
            ++failed;
        }

        Q_EMIT itemCleaned(item.path, ok);
    }

    reportProgress(total, total, "Cleanup complete");
    Q_EMIT cleanupComplete(succeeded, failed, bytes_recovered);

    return {};
}

bool CleanupWorker::deleteFile(const QString& path)
{
    QFileInfo info(path);
    if (!info.exists()) {
        return true;  // Already gone
    }

    if (!QFile::remove(path)) {
        // Try setting writable first
        QFile file(path);
        file.setPermissions(QFile::ReadOther | QFile::WriteOther);
        return file.remove();
    }
    return true;
}

bool CleanupWorker::deleteFolder(const QString& path)
{
    QDir dir(path);
    if (!dir.exists()) {
        return true;  // Already gone
    }

    return dir.removeRecursively();
}

bool CleanupWorker::deleteRegistryKey(const QString& fullKeyPath)
{
#ifdef Q_OS_WIN
    QString path = fullKeyPath;
    HKEY hive = nullptr;

    if (path.startsWith("HKLM\\")) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(5);
    } else if (path.startsWith("HKCU\\")) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(5);
    } else if (path.startsWith("HKCR\\")) {
        hive = HKEY_CLASSES_ROOT;
        path = path.mid(5);
    } else {
        return false;
    }

    // RegDeleteTree deletes the key and ALL subkeys
    LONG rc = RegDeleteTreeW(hive,
                             reinterpret_cast<LPCWSTR>(path.utf16()));

    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(fullKeyPath)
    return false;
#endif
}

bool CleanupWorker::deleteRegistryValue(const QString& keyPath,
                                         const QString& valueName)
{
#ifdef Q_OS_WIN
    QString path = keyPath;
    HKEY hive = nullptr;

    if (path.startsWith("HKLM\\")) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(5);
    } else if (path.startsWith("HKCU\\")) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(5);
    } else if (path.startsWith("HKCR\\")) {
        hive = HKEY_CLASSES_ROOT;
        path = path.mid(5);
    } else {
        return false;
    }

    HKEY key = nullptr;
    LONG rc = RegOpenKeyExW(hive,
                            reinterpret_cast<LPCWSTR>(path.utf16()),
                            0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    rc = RegDeleteValueW(key,
                         reinterpret_cast<LPCWSTR>(valueName.utf16()));
    RegCloseKey(key);

    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(keyPath)
    Q_UNUSED(valueName)
    return false;
#endif
}

bool CleanupWorker::removeService(const QString& serviceName)
{
    // Stop the service first
    QProcess stop_proc;
    stop_proc.setProgram("sc.exe");
    stop_proc.setArguments({"stop", serviceName});
    stop_proc.start();
    stop_proc.waitForFinished(10000);

    // Wait a moment for it to stop
    QThread::msleep(1000);

    // Delete the service
    QProcess del_proc;
    del_proc.setProgram("sc.exe");
    del_proc.setArguments({"delete", serviceName});
    del_proc.start();

    if (!del_proc.waitForFinished(10000)) {
        return false;
    }

    return del_proc.exitCode() == 0;
}

bool CleanupWorker::removeScheduledTask(const QString& taskName)
{
    QProcess proc;
    proc.setProgram("schtasks.exe");
    proc.setArguments({"/delete", "/tn", taskName, "/f"});
    proc.start();

    if (!proc.waitForFinished(10000)) {
        return false;
    }

    return proc.exitCode() == 0;
}

bool CleanupWorker::removeFirewallRule(const QString& ruleName)
{
    QProcess proc;
    proc.setProgram("netsh.exe");
    proc.setArguments({"advfirewall", "firewall", "delete", "rule",
                       QString("name=%1").arg(ruleName)});
    proc.start();

    if (!proc.waitForFinished(10000)) {
        return false;
    }

    return proc.exitCode() == 0;
}

} // namespace sak
