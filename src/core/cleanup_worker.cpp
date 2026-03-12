// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cleanup_worker.cpp
/// @brief Deletes selected leftover items safely on a background thread

#include "sak/cleanup_worker.h"

#include "sak/layout_constants.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>

#include <shellapi.h>
#endif

namespace sak {

CleanupWorker::CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                             bool useRecycleBin,
                             QObject* parent)
    : WorkerBase(parent), m_items(selectedItems), m_useRecycleBin(useRecycleBin) {}

auto CleanupWorker::execute() -> std::expected<void, sak::error_code> {
    int succeeded = 0;
    int failed = 0;
    qint64 bytes_recovered = 0;

    const int total = m_items.size();

    for (int idx = 0; idx < total; ++idx) {
        if (checkStop()) {
            Q_EMIT cleanupComplete(succeeded, failed, bytes_recovered);
            return {};
        }

        const auto& item = m_items[idx];
        if (!item.selected) {
            continue;
        }

        reportProgress(idx, total, QString("Cleaning: %1").arg(item.path));

        const bool ok = cleanSingleItem(item);

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

    if (!m_rebootPendingPaths.isEmpty()) {
        Q_EMIT rebootPendingItems(m_rebootPendingPaths);
    }

    return {};
}

bool CleanupWorker::cleanSingleItem(const LeftoverItem& item) {
    switch (item.type) {
    case LeftoverItem::Type::File:
        return deleteFile(item.path);
    case LeftoverItem::Type::Folder:
        return deleteFolder(item.path);
    case LeftoverItem::Type::RegistryKey:
        return deleteRegistryKey(item.path);
    case LeftoverItem::Type::RegistryValue:
        return deleteRegistryValue(item.path, item.registryValueName);
    case LeftoverItem::Type::Service:
        return removeService(item.path);
    case LeftoverItem::Type::ScheduledTask:
        return removeScheduledTask(item.path);
    case LeftoverItem::Type::FirewallRule:
        return removeFirewallRule(item.path);
    case LeftoverItem::Type::StartupEntry:
        return cleanStartupEntry(item);
    case LeftoverItem::Type::ShellExtension:
        return deleteRegistryKey(item.path);
    }
    return false;
}

bool CleanupWorker::cleanStartupEntry(const LeftoverItem& item) {
    if (!item.registryValueName.isEmpty()) {
        return deleteRegistryValue(item.path, item.registryValueName);
    }
    return deleteFile(item.path);
}

bool CleanupWorker::deleteFile(const QString& path) {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!m_rebootPendingPaths.isEmpty());
    QFileInfo info(path);
    if (!info.exists()) {
        return true;  // Already gone
    }

    // If recycle bin mode is enabled, try that first
    if (m_useRecycleBin) {
        if (sendToRecycleBin(path)) {
            return true;
        }
        // Fall through to direct deletion if recycle bin fails
    }

    if (QFile::remove(path)) {
        return true;
    }

    // Try setting writable and retry
    QFile file(path);
    file.setPermissions(QFile::ReadOther | QFile::WriteOther);
    if (file.remove()) {
        return true;
    }

    // File is locked -- schedule removal on next reboot
    if (scheduleRebootRemoval(path)) {
        m_rebootPendingPaths.append(path);
        return true;  // Counted as success; actual removal happens on reboot
    }

    return false;
}

bool CleanupWorker::deleteFolder(const QString& path) {
    Q_ASSERT(!path.isEmpty());
    Q_ASSERT(!m_rebootPendingPaths.isEmpty());
    QDir dir(path);
    if (!dir.exists()) {
        return true;
    }

    if (m_useRecycleBin && sendToRecycleBin(path)) {
        return true;
    }

    if (dir.removeRecursively()) {
        return true;
    }

    bool all_handled = removeFolderContentsForced(dir);

    if (!dir.rmdir(path)) {
        all_handled = tryScheduleReboot(path) && all_handled;
    }

    return all_handled;
}

bool CleanupWorker::removeFolderContentsForced(const QDir& dir) {
    bool all_handled = true;
    const auto entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::DirsLast);

    for (const auto& entry : entries) {
        const QString entry_path = entry.absoluteFilePath();
        if (entry.isDir()) {
            if (!deleteFolder(entry_path)) {
                all_handled = false;
            }
            continue;
        }

        if (QFile::remove(entry_path)) {
            continue;
        }

        if (!tryScheduleReboot(entry_path)) {
            all_handled = false;
        }
    }
    return all_handled;
}

bool CleanupWorker::tryScheduleReboot(const QString& path) {
    if (scheduleRebootRemoval(path)) {
        m_rebootPendingPaths.append(path);
        return true;
    }
    return false;
}

bool CleanupWorker::sendToRecycleBin(const QString& path) {
#ifdef Q_OS_WIN
    // SHFileOperationW requires double-null terminated string
    std::wstring widePath = path.toStdWString();
    widePath.push_back(L'\0');  // Extra null terminator

    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = widePath.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;

    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool CleanupWorker::scheduleRebootRemoval(const QString& path) {
#ifdef Q_OS_WIN
    // MoveFileExW with MOVEFILE_DELAY_UNTIL_REBOOT schedules the file
    // for deletion when Windows restarts (independent of this application)
    return MoveFileExW(reinterpret_cast<LPCWSTR>(path.utf16()),
                       nullptr,
                       MOVEFILE_DELAY_UNTIL_REBOOT) != 0;
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool CleanupWorker::deleteRegistryKey(const QString& fullKeyPath) {
    Q_ASSERT(!fullKeyPath.isEmpty());
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
    LONG rc = RegDeleteTreeW(hive, reinterpret_cast<LPCWSTR>(path.utf16()));

    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(fullKeyPath)
    return false;
#endif
}

bool CleanupWorker::deleteRegistryValue(const QString& keyPath, const QString& valueName) {
    Q_ASSERT(!keyPath.isEmpty());
    Q_ASSERT(!valueName.isEmpty());
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
    LONG rc = RegOpenKeyExW(hive, reinterpret_cast<LPCWSTR>(path.utf16()), 0, KEY_SET_VALUE, &key);
    if (rc != ERROR_SUCCESS) {
        return false;
    }

    rc = RegDeleteValueW(key, reinterpret_cast<LPCWSTR>(valueName.utf16()));
    RegCloseKey(key);

    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
#else
    Q_UNUSED(keyPath)
    Q_UNUSED(valueName)
    return false;
#endif
}

bool CleanupWorker::removeService(const QString& serviceName) {
    Q_ASSERT(!serviceName.isEmpty());
    // Stop the service first
    QProcess stop_proc;
    stop_proc.setProgram("sc.exe");
    stop_proc.setArguments({"stop", serviceName});
    stop_proc.start();
    if (stop_proc.waitForStarted(sak::kTimeoutProcessStartMs)) {
        stop_proc.waitForFinished(10'000);
    }

    // Wait a moment for it to stop
    QThread::msleep(1000);

    // Delete the service
    QProcess del_proc;
    del_proc.setProgram("sc.exe");
    del_proc.setArguments({"delete", serviceName});
    del_proc.start();

    if (!del_proc.waitForStarted(sak::kTimeoutProcessStartMs) ||
        !del_proc.waitForFinished(10'000)) {
        return false;
    }

    return del_proc.exitCode() == 0;
}

bool CleanupWorker::removeScheduledTask(const QString& taskName) {
    QProcess proc;
    proc.setProgram("schtasks.exe");
    proc.setArguments({"/delete", "/tn", taskName, "/f"});
    proc.start();

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs) || !proc.waitForFinished(10'000)) {
        return false;
    }

    return proc.exitCode() == 0;
}

bool CleanupWorker::removeFirewallRule(const QString& ruleName) {
    QProcess proc;
    proc.setProgram("netsh.exe");
    proc.setArguments(
        {"advfirewall", "firewall", "delete", "rule", QString("name=%1").arg(ruleName)});
    proc.start();

    if (!proc.waitForStarted(sak::kTimeoutProcessStartMs) || !proc.waitForFinished(10'000)) {
        return false;
    }

    return proc.exitCode() == 0;
}

}  // namespace sak
