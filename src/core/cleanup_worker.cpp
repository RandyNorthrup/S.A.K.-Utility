// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cleanup_worker.cpp
/// @brief Deletes selected leftover items safely on a background thread

#include "sak/cleanup_worker.h"

#include "sak/layout_constants.h"
#include "sak/leftover_cleanup_guard.h"
#include "sak/logger.h"
#include "sak/process_runner.h"
#include "sak/recycle_bin.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_WIN
#include <string>

#include <windows.h>

#include <shellapi.h>
#endif

namespace sak {

namespace {
constexpr int kRegistryHivePrefixLength = 5;
constexpr int kCleanupCommandTimeoutMs = 10'000;
constexpr int kCleanupServiceStopSettleMs = kTimerProgressPollMs;

#ifdef Q_OS_WIN
// Open a handle to the EXACT path for delete-time verification, WITHOUT following a leaf reparse
// point (FILE_FLAG_OPEN_REPARSE_POINT). Ancestor junctions are still traversed by the OS during
// path parsing -- which is the point: GetFinalPathNameByHandleW then reveals the real resolved
// target. FILE_FLAG_BACKUP_SEMANTICS is required to obtain a directory handle. DELETE access lets
// the same handle be used to unlink the object.
HANDLE openForVerifiedDelete(const QString& path) {
    return CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()),
                       DELETE | FILE_READ_ATTRIBUTES,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr,
                       OPEN_EXISTING,
                       FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS,
                       nullptr);
}

// The REAL on-disk path of an open handle, every ancestor junction/symlink resolved, with the
// \\?\ (and \\?\UNC\) extended-length prefix stripped. Empty on failure.
QString finalPathOfHandle(HANDLE handle) {
    std::wstring buffer(1024, L'\0');
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD length =
        GetFinalPathNameByHandleW(handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
    if (length == 0) {
        return {};
    }
    if (length > buffer.size()) {
        buffer.resize(length);
        length = GetFinalPathNameByHandleW(
            handle, buffer.data(), static_cast<DWORD>(buffer.size()), flags);
        if (length == 0 || length > buffer.size()) {
            return {};
        }
    }
    QString resolved = QString::fromWCharArray(buffer.data(), static_cast<int>(length));
    if (resolved.startsWith(QStringLiteral("\\\\?\\UNC\\"))) {
        return QStringLiteral("\\\\") + resolved.mid(8);
    }
    if (resolved.startsWith(QStringLiteral("\\\\?\\"))) {
        return resolved.mid(4);
    }
    return resolved;
}

// Unlink the object an open handle refers to, WITHOUT re-resolving any path. Prefers POSIX
// semantics (name removed immediately) and falls back to legacy delete-on-close.
bool unlinkByHandle(HANDLE handle) {
    FILE_DISPOSITION_INFO_EX info_ex{};
    info_ex.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS;
    if (SetFileInformationByHandle(handle, FileDispositionInfoEx, &info_ex, sizeof(info_ex))) {
        return true;
    }
    FILE_DISPOSITION_INFO info{};
    info.DeleteFile = TRUE;
    return SetFileInformationByHandle(handle, FileDispositionInfo, &info, sizeof(info)) != 0;
}
#endif  // Q_OS_WIN
}  // namespace

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
            // Do NOT emit cleanupComplete on cancel -- that carries success-shaped
            // counts and reads as a completed run. WorkerBase emits cancelled()
            // instead, which the controller handles distinctly.
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

    if (!m_recycleFallbackPaths.isEmpty()) {
        Q_EMIT recycleFallbackItems(m_recycleFallbackPaths);
    }

    return {};
}

void CleanupWorker::noteRecycleFallback(bool fallback, const QString& path) {
    if (fallback) {
        m_recycleFallbackPaths.append(path);
    }
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

bool CleanupWorker::deleteTimeRedirectSafe(const QString& path) {
#ifdef Q_OS_WIN
    HANDLE handle = openForVerifiedDelete(path);
    if (handle == INVALID_HANDLE_VALUE) {
        // Cannot open to verify (locked/denied, or raced-away). Re-screen ancestors LEXICALLY now
        // so a fresh junction/symlink swap is still refused; a truly-gone path has nothing to
        // delete.
        if (sak::pathReparseUnsafe(path)) {
            sak::logWarning("Refusing cleanup delete of " + path.toStdString() +
                            ": path is (or is nested under) a symlink/junction");
            return false;
        }
        return true;
    }
    const QString final_path = finalPathOfHandle(handle);
    CloseHandle(handle);
    const QString refusal = sak::cleanupHandleRedirectRefusal(path, final_path);
    if (!refusal.isEmpty()) {
        sak::logWarning("Refusing cleanup delete of " + path.toStdString() + ": " +
                        refusal.toStdString());
        return false;
    }
    return true;
#else
    Q_UNUSED(path)
    return true;
#endif
}

#ifdef Q_OS_WIN
CleanupWorker::HandleDeleteOutcome CleanupWorker::deleteFileByVerifiedHandle(const QString& path) {
    HANDLE handle = openForVerifiedDelete(path);
    if (handle == INVALID_HANDLE_VALUE) {
        return HandleDeleteOutcome::NotOpenable;
    }
    const QString final_path = finalPathOfHandle(handle);
    const QString refusal = sak::cleanupHandleRedirectRefusal(path, final_path);
    if (!refusal.isEmpty()) {
        CloseHandle(handle);
        sak::logWarning("Refusing permanent delete of " + path.toStdString() + ": " +
                        refusal.toStdString());
        return HandleDeleteOutcome::Redirected;
    }
    const bool ok = unlinkByHandle(handle);
    CloseHandle(handle);
    return ok ? HandleDeleteOutcome::Deleted : HandleDeleteOutcome::NotOpenable;
}
#endif

bool CleanupWorker::removeFilePermanent(const QString& path) {
#ifdef Q_OS_WIN
    // Delete BY HANDLE: open the exact object, re-verify it is still the validated target (no
    // ancestor junction/symlink swap), then unlink through the handle -- no path is re-resolved
    // after the check, closing the TOCTOU window a string-based QFile::remove would leave open.
    const HandleDeleteOutcome outcome = deleteFileByVerifiedHandle(path);
    if (outcome == HandleDeleteOutcome::Deleted) {
        return true;
    }
    if (outcome == HandleDeleteOutcome::Redirected) {
        return false;  // NEVER fall back to a string delete on a redirected/protected target
    }
    // NotOpenable (locked/denied but not redirected): the caller's top-level deleteTimeRedirectSafe
    // gate already re-screened ancestors, so the string unlink cannot hit a swapped-in junction.
    return QFile::remove(path);
#else
    return QFile::remove(path);
#endif
}

bool CleanupWorker::deleteFile(const QString& path) {
    Q_ASSERT(!path.isEmpty());
    QFileInfo info(path);
    if (!info.exists()) {
        return true;  // Already gone
    }

    // Ancestor-junction-swap close: verify the string still resolves to the validated object before
    // ANY deletion (recycle or permanent). A redirected/protected target is refused outright.
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }

    // If recycle bin mode is enabled, try that first.
    bool recycleFallback = false;
    if (m_useRecycleBin) {
        if (sendPathToRecycleBin(path)) {
            return true;
        }
        // Recycle failed: every path below deletes PERMANENTLY despite the user's
        // recycle-bin choice. Record it so the silent escalation is surfaced.
        recycleFallback = true;
        sak::logWarning("Recycle bin failed for " + path.toStdString() +
                        "; falling back to permanent deletion");
    }

    if (removeFilePermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }

    // Try setting writable and retry
    QFile file(path);
    file.setPermissions(QFile::ReadOther | QFile::WriteOther);
    if (removeFilePermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }

    // File is locked -- schedule removal on next reboot
    if (scheduleRebootRemoval(path)) {
        m_rebootPendingPaths.append(path);
        noteRecycleFallback(recycleFallback, path);
        return true;  // Counted as success; actual removal happens on reboot
    }

    return false;
}

bool CleanupWorker::deleteFolder(const QString& path) {
    Q_ASSERT(!path.isEmpty());
    QDir dir(path);
    if (!dir.exists()) {
        return true;
    }

    // A reparse point (junction / directory symlink) must be UNLINKED, never recursed into -- else
    // removeRecursively()/entryInfoList would enumerate THROUGH it and permanently delete the
    // target's contents (e.g. a nested junction to System32). This function re-enters itself during
    // the forced fallback, so screen it here too.
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) {
        return unlinkReparsePoint(path);
    }

    // Ancestor-junction-swap close: verify the folder's real resolved path is still the validated,
    // non-protected target before recursing/deleting. This pins the tree ROOT (an ancestor swapped
    // into a junction to System32 is caught here); per-child reparse points are screened during the
    // forced removal below.
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }

    bool recycleFallback = false;
    if (m_useRecycleBin) {
        if (sendPathToRecycleBin(path)) {
            return true;
        }
        // Recycle failed: the folder is about to be deleted PERMANENTLY.
        recycleFallback = true;
        sak::logWarning("Recycle bin failed for folder " + path.toStdString() +
                        "; falling back to permanent deletion");
    }

    if (removeFolderPermanent(dir, path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }
    return false;
}

bool CleanupWorker::removeFolderPermanent(QDir& dir, const QString& path) {
    if (dir.removeRecursively()) {
        return true;
    }
    bool all_handled = removeFolderContentsForced(dir);
    if (!dir.rmdir(path)) {
        all_handled = tryScheduleReboot(path) && all_handled;
    }
    return all_handled;
}

bool CleanupWorker::unlinkReparsePoint(const QString& path) {
    // rmdir (RemoveDirectoryW) unlinks a directory reparse point; QFile::remove unlinks a file
    // symlink -- both remove the LINK only, never the target's contents.
    if (QDir().rmdir(path) || QFile::remove(path)) {
        return true;
    }
    return tryScheduleReboot(path);
}

bool CleanupWorker::removeForcedEntry(const QFileInfo& entry) {
    const QString entry_path = entry.absoluteFilePath();
    // Never recurse THROUGH a reparse point (junction / directory symlink): entry.isDir() FOLLOWS
    // it, so recursing would delete the target's contents, not the leftover (a nested junction to
    // System32 must not wipe System32). Remove the link itself instead.
    const bool is_reparse = entry.isSymLink() || entry.isJunction();
    if (entry.isDir() && !is_reparse) {
        return deleteFolder(entry_path);
    }
    if (is_reparse) {
        return unlinkReparsePoint(entry_path);
    }
    // Plain file: QFile::remove unlinks it; a locked file is scheduled for reboot removal.
    return QFile::remove(entry_path) || tryScheduleReboot(entry_path);
}

bool CleanupWorker::removeFolderContentsForced(const QDir& dir) {
    bool all_handled = true;
    const auto entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::DirsLast);

    for (const auto& entry : entries) {
        if (!removeForcedEntry(entry)) {
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

    // Case-insensitive hive match: the Windows registry is case-insensitive, and the AI-assistant
    // clean_leftovers guard accepts a mixed-case hive -- so the worker must too, or a
    // guard-approved "hklm\..." item would silently no-op (guard/worker parity).
    if (path.startsWith("HKLM\\", Qt::CaseInsensitive)) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(kRegistryHivePrefixLength);
    } else if (path.startsWith("HKCU\\", Qt::CaseInsensitive)) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(kRegistryHivePrefixLength);
    } else if (path.startsWith("HKCR\\", Qt::CaseInsensitive)) {
        hive = HKEY_CLASSES_ROOT;
        path = path.mid(kRegistryHivePrefixLength);
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

    // Case-insensitive hive match (see deleteRegistryKey): parity with the clean_leftovers guard.
    if (path.startsWith("HKLM\\", Qt::CaseInsensitive)) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(kRegistryHivePrefixLength);
    } else if (path.startsWith("HKCU\\", Qt::CaseInsensitive)) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(kRegistryHivePrefixLength);
    } else if (path.startsWith("HKCR\\", Qt::CaseInsensitive)) {
        hive = HKEY_CLASSES_ROOT;
        path = path.mid(kRegistryHivePrefixLength);
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
    const auto stop_result = sak::runProcess(QStringLiteral("sc.exe"),
                                             {QStringLiteral("stop"), serviceName},
                                             kCleanupCommandTimeoutMs);
    if (stop_result.timed_out) {
        sak::logWarning("Service stop timed out for: {}", serviceName.toStdString());
    }

    // Wait a moment for it to stop
    QThread::msleep(kCleanupServiceStopSettleMs);

    // Delete the service
    const auto del_result = sak::runProcess(QStringLiteral("sc.exe"),
                                            {QStringLiteral("delete"), serviceName},
                                            kCleanupCommandTimeoutMs);
    return del_result.succeeded();
}

bool CleanupWorker::removeScheduledTask(const QString& taskName) {
    const auto result = sak::runProcess(
        QStringLiteral("schtasks.exe"),
        {QStringLiteral("/delete"), QStringLiteral("/tn"), taskName, QStringLiteral("/f")},
        kCleanupCommandTimeoutMs);
    return result.succeeded();
}

bool CleanupWorker::removeFirewallRule(const QString& ruleName) {
    const auto result = sak::runProcess(QStringLiteral("netsh.exe"),
                                        {QStringLiteral("advfirewall"),
                                         QStringLiteral("firewall"),
                                         QStringLiteral("delete"),
                                         QStringLiteral("rule"),
                                         QStringLiteral("name=%1").arg(ruleName)},
                                        kCleanupCommandTimeoutMs);
    return result.succeeded();
}

}  // namespace sak
