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
#include <QDirIterator>
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

// Append the scan-time identity qualifiers that narrow a netsh firewall delete to the
// single matching rule. Each is emitted only when known so an unbound/unknown field
// does not over-constrain (and thus fail to delete) the intended rule.
void appendFirewallDeleteFilters(QStringList& args, const LeftoverItem& item) {
    if (!item.firewallDirection.isEmpty()) {
        args << QStringLiteral("dir=%1").arg(item.firewallDirection);
    }
    if (!item.firewallProfile.isEmpty()) {
        args << QStringLiteral("profile=%1").arg(item.firewallProfile);
    }
    if (!item.firewallProgram.isEmpty()) {
        args << QStringLiteral("program=%1").arg(item.firewallProgram);
    }
}

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

// A registry key can be a REG_LINK symbolic-link key whose deletion BY PATH would traverse to the
// link TARGET and delete ITS subtree -- escaping the denylist that screened only the literal path.
// Detect it by opening the EXACT key WITHOUT following the link (REG_OPTION_OPEN_LINK) and probing
// for the reserved "SymbolicLinkValue" value that marks a link key. True => the key is a symbolic
// link and must NOT be tree-deleted by path. A key that cannot be opened as a link returns false;
// the subsequent RegDeleteTreeW then resolves/errors on it normally (a nonexistent key no-ops).
bool registryKeyIsSymbolicLink(HKEY hive, const QString& subkey) {
    HKEY key = nullptr;
    const LONG rc = RegOpenKeyExW(hive,
                                  reinterpret_cast<LPCWSTR>(subkey.utf16()),
                                  REG_OPTION_OPEN_LINK,
                                  KEY_QUERY_VALUE,
                                  &key);
    if (rc != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    const LONG q = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return q == ERROR_SUCCESS && type == REG_LINK;
}
#endif  // Q_OS_WIN

// True if @p path is on a volume that actually HAS a Recycle Bin. On a removable / network /
// optical volume, the shell's FOF_ALLOWUNDO "recycle" silently PERMANENTLY deletes, so recycle-only
// (auto-clean) mode must refuse there rather than assume recoverability. A fixed drive is the fast,
// reliable proxy for "has a Recycle Bin".
bool volumeSupportsRecycleBin(const QString& path) {
#ifdef Q_OS_WIN
    const QString root = QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()).left(3);
    if (root.size() < 3 || root[1] != QLatin1Char(':')) {
        return false;
    }
    return GetDriveTypeW(reinterpret_cast<LPCWSTR>(root.utf16())) == DRIVE_FIXED;
#else
    Q_UNUSED(path)
    return true;
#endif
}

// Above this the item is deemed too large to GUARANTEE it fits the Recycle Bin (the shell silently
// PERMANENTLY deletes an item exceeding the bin's configured max). Conservative: a default bin
// holds GBs, so 4 GiB leaves the common small app leftover recyclable while a huge one is left for
// review.
constexpr qint64 kRecoverableRecycleSizeCap = 4LL * 1024 * 1024 * 1024;

// On-disk size of @p path, summed but SHORT-CIRCUITED once it exceeds @p limit (so a huge tree is
// not fully walked). Does not follow symlinks (QDirIterator default).
qint64 boundedItemSize(const QString& path, qint64 limit) {
    const QFileInfo info(path);
    if (info.isFile()) {
        return info.size();
    }
    if (!info.isDir()) {
        return 0;
    }
    qint64 total = 0;
    // NoSymLinks: never list (and never size()/stat) a nested symlink -- following one to a UNC
    // target would trigger a remote handshake and mis-measure by the target's size. QDirIterator
    // also does not recurse INTO symlinked directories (FollowSymlinks is not set).
    QDirIterator it(path,
                    QDir::Files | QDir::Hidden | QDir::System | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        total += it.fileInfo().size();
        if (total > limit) {
            return total;
        }
    }
    return total;
}

// True if recycle-only (auto-clean) mode can SAFELY recycle @p path: the volume has a Recycle Bin
// AND the item is small enough to actually fit it. A huge item can be silently nuked past the bin's
// max even on a fixed drive, so it is left for human review instead.
bool recoverableRecycleAllowed(const QString& path) {
    return volumeSupportsRecycleBin(path) &&
           boundedItemSize(path, kRecoverableRecycleSizeCap) <= kRecoverableRecycleSizeCap;
}

}  // namespace

QString CleanupWorker::systemToolPath(const QString& systemRoot, const QString& exeName) {
    // Compose a Windows system tool (e.g. "sc.exe") into an ABSOLUTE <root>\System32 path. A bare
    // program name handed to CreateProcess resolves through the app dir / CWD / PATH first, so a
    // portable install with a same-named binary planted alongside it would run the attacker's tool
    // in our stead (the hijack AppScanner::composePowerShellPath also defends against).
    //
    // THE ROOT MUST NOT COME FROM %SystemRoot%. This function used to be called with
    // qEnvironmentVariable("SystemRoot") while its own comment claimed to defend against hijacking,
    // and that variable is the classic same-user UAC-bypass primitive: an unprivileged attacker
    // writes HKCU\Environment\SystemRoot, the value propagates through Explorer into the elevated
    // sak_utility process, and every "absolute System32 path" below then pointed into a directory
    // the attacker owns -- with the app's elevated token. Callers now pass sak::system32Path(),
    // which asks the OS via GetSystemDirectoryW. This overload survives only as the pure seam the
    // unit test drives, and it validates what it is handed rather than trusting it.
    if (systemRoot.isEmpty()) {
        return {};
    }
    // A relative root ("windows"), a UNC root (\\attacker\share) or a bare drive-relative path
    // would all compose into something that is not the local system directory. Requiring a real
    // absolute local path keeps the seam honest even though its only production caller now
    // supplies an OS-derived one.
    const QString normalized = QDir::fromNativeSeparators(systemRoot);
    if (normalized.startsWith(QStringLiteral("//"))) {
        return {};  // UNC: never the local Windows directory
    }
    if (!QDir::isAbsolutePath(normalized)) {
        return {};
    }
    return QDir::cleanPath(normalized + QStringLiteral("/System32/") + exeName);
}

QString CleanupWorker::launchableSystemTool(const QString& exeName) {
    // The single place the destructive CLI tools are resolved for launch, so "which root does an
    // elevated sc.exe come from?" has exactly one answer and a test can pin it. sak::system32Path
    // asks the OS (GetSystemDirectoryW); it is deliberately NOT %SystemRoot%, which an unprivileged
    // same-user attacker controls via HKCU\Environment. Empty means fail closed and the caller
    // refuses to launch rather than falling back to a bare name that PATH would resolve.
    return sak::system32Path(exeName);
}

CleanupWorker::CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                             bool useRecycleBin,
                             QObject* parent)
    // Choosing the Recycle Bin IS a choice for recoverability, so recoverable-only defaults ON
    // with it: a caller that never calls setRequireRecoverable can no longer end up permanently
    // destroying an item whose recycle failed. A caller that has a reviewed-by-a-human contract
    // (the GUI manual clean) opts back out explicitly via setRequireRecoverable(false).
    : WorkerBase(parent)
    , m_items(selectedItems)
    , m_useRecycleBin(useRecycleBin)
    , m_requireRecoverable(useRecycleBin) {}

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
        // Hard backstop at the deleting worker: a report-only item (one whose path the
        // scanner could not tie to the program -- e.g. an unverifiable registry
        // InstallLocation) is refused here too, so no caller can route it to a delete.
        if (!item.selected || !item.deletable) {
            continue;
        }

        reportProgress(idx, total, QString("Cleaning: %1").arg(item.path));

        // Defense-in-depth: re-screen each item against the cleanup denylist before dispatching, so
        // a caller that bypasses AdvancedUninstallController cannot reach the destructive syscalls.
        const bool ok = cleanItemIfAllowed(item);

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

QString CleanupWorker::startupEntryCleanupRefusal(const LeftoverItem& item) {
    // A startup leftover is either a Run/RunOnce registry value or an on-disk shortcut/exe; screen
    // whichever this one is, mirroring cleanStartupEntry's own dispatch.
    if (!item.registryValueName.isEmpty()) {
        return registryValueDeletionRefusal(item.path, item.registryValueName);
    }
    return filePathDeletionRefusal(item.path);
}

QString CleanupWorker::itemCleanupRefusal(const LeftoverItem& item) {
    switch (item.type) {
    case LeftoverItem::Type::File:
    case LeftoverItem::Type::Folder:
        return filePathDeletionRefusal(item.path);
    case LeftoverItem::Type::RegistryKey:
    case LeftoverItem::Type::ShellExtension:
        return registryKeyDeletionRefusal(item.path);
    case LeftoverItem::Type::RegistryValue:
        return registryValueDeletionRefusal(item.path, item.registryValueName);
    case LeftoverItem::Type::Service:
        return serviceDeletionRefusal(item.path);
    case LeftoverItem::Type::ScheduledTask:
        return scheduledTaskDeletionRefusal(item.path);
    case LeftoverItem::Type::FirewallRule:
        return firewallRuleDeletionRefusal(item.path);
    case LeftoverItem::Type::StartupEntry:
        return startupEntryCleanupRefusal(item);
    }
    return QStringLiteral("unknown leftover item type");
}

bool CleanupWorker::cleanItemIfAllowed(const LeftoverItem& item) {
    const QString refusal = itemCleanupRefusal(item);
    if (!refusal.isEmpty()) {
        sak::logWarning("Refusing cleanup of " + item.path.toStdString() + ": " +
                        refusal.toStdString());
        return false;
    }
    return cleanSingleItem(item);
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
        return removeFirewallRule(item);
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
    if (unlinkByHandle(handle)) {
        CloseHandle(handle);
        return HandleDeleteOutcome::Deleted;
    }
    // The object was opened and verified NOT redirected, but the unlink itself failed (typically a
    // sharing/lock violation). Surface the Win32 error so the caller's string-unlink/reboot
    // fallback is not silent; the earlier redirect check means the fallback cannot hit a swapped-in
    // target.
    const DWORD err = GetLastError();
    CloseHandle(handle);
    sak::logWarning("By-handle delete of " + path.toStdString() +
                    " failed (GetLastError=" + std::to_string(err) + "); using fallback removal");
    return HandleDeleteOutcome::NotOpenable;
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
    // NotOpenable (locked/denied, or the object was opened+verified but the unlink itself failed).
    // Do NOT bare-unlink: re-run the handle-verified redirect screen IMMEDIATELY before the string
    // delete so an ancestor swapped into a junction since the earlier check is refused fail-closed
    // (deleteTimeRedirectSafe re-opens+verifies, or lexically re-screens ancestors when the object
    // still cannot be opened). Only a target that re-verifies clean falls to the last-resort
    // unlink.
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }
    return QFile::remove(path);
#else
    return QFile::remove(path);
#endif
}

CleanupWorker::RecycleOutcome CleanupWorker::attemptRecycle(const QString& path,
                                                            bool& recycleFallback) {
    if (m_useRecycleBin) {
        // Recycle-only mode GUARANTEES recoverability: refuse when the volume has no Recycle Bin OR
        // the item is too large to fit it (the shell silently permanent-deletes in both cases),
        // leaving it in place for review instead.
        if (m_requireRecoverable && !recoverableRecycleAllowed(path)) {
            sak::logWarning("Auto-clean leaves " + path.toStdString() +
                            " in place: no Recycle Bin on volume, or too large to guarantee "
                            "recoverable");
            return RecycleOutcome::RefusedRecoverable;
        }
        // TOCTOU close for the shell recycle: SHFileOperationW takes a PATH STRING and cannot be
        // handle-pinned, and recoverableRecycleAllowed above may have walked the tree (re-resolving
        // paths). Re-verify by handle IMMEDIATELY before the shell call so an ancestor swapped into
        // a junction since deleteFile's gate is caught. A redirect here refuses OUTRIGHT -- it must
        // never fall through to a permanent delete of the swapped-in target.
        if (!deleteTimeRedirectSafe(path)) {
            return RecycleOutcome::RefusedRedirected;
        }
        if (sendPathToRecycleBin(path)) {
            return RecycleOutcome::Recycled;
        }
        // Recycle-only (automatic) mode: never fall through to permanent deletion -- report failed
        // and leave the item in place so an auto-deleted item is always recoverable.
        if (m_requireRecoverable) {
            sak::logWarning("Recycle bin failed for " + path.toStdString() +
                            "; auto-clean leaves it in place (no permanent delete)");
            return RecycleOutcome::RefusedRecoverable;
        }
        // Recycle failed: the item is about to be deleted PERMANENTLY despite the recycle choice.
        // Record it so the silent escalation is surfaced.
        recycleFallback = true;
        sak::logWarning("Recycle bin failed for " + path.toStdString() +
                        "; falling back to permanent deletion");
        return RecycleOutcome::FallThrough;
    }
    // Recoverable required but recycle disabled: refuse to permanently delete (defensive -- auto-
    // clean always pairs requireRecoverable with useRecycleBin, but never destroy from this mode).
    if (m_requireRecoverable) {
        sak::logWarning("Recoverable-only cleanup with recycle disabled; leaving " +
                        path.toStdString() + " in place");
        return RecycleOutcome::RefusedRecoverable;
    }
    return RecycleOutcome::FallThrough;
}

bool CleanupWorker::deleteFile(const QString& path) {
    // cleanItemIfAllowed screens every item through filePathDeletionRefusal, which rejects an empty
    // path, before cleanSingleItem/cleanStartupEntry can dispatch here.
    Q_ASSERT(!path.isEmpty());
    // Belt and braces on a permanent delete: an empty path reaches QFileInfo("").exists()
    // == false and is currently reported as "already gone", i.e. a SUCCESSFUL deletion of
    // something that was never named. Refuse it explicitly instead.
    if (path.isEmpty()) {
        sak::logError("CleanupWorker: refusing to delete a file with an empty path");
        return false;
    }
    // Ancestor reparse guard FIRST, before any stat that FOLLOWS the path: an ancestor swapped to a
    // (UNC) symlink would otherwise make exists() trigger a remote handshake. pathHasReparsePoint
    // Ancestor reads each ancestor's own attributes WITHOUT following, so it is safe to call first.
    if (sak::pathHasReparsePointAncestor(path)) {
        sak::logWarning("Refusing file delete of " + path.toStdString() +
                        ": an ancestor is a symlink/junction");
        return false;
    }
    const QFileInfo info(path);
    // Leaf reparse check uses isSymLink/isJunction (which read the LINK's own attributes, no
    // follow) BEFORE exists() (which FOLLOWS the link and would trigger an SMB/NTLM handshake if
    // the leaf was swapped to a UNC symlink). A file symlink leftover is unlinked as the LINK only.
    if (info.isSymLink() || info.isJunction()) {
        if (m_requireRecoverable) {
            sak::logWarning("Auto-clean leaves reparse-point leftover for review: " +
                            path.toStdString());
            return false;
        }
        return unlinkReparsePoint(path);
    }
    if (!info.exists()) {
        return true;  // Already gone
    }

    // Ancestor-junction-swap close: verify the string still resolves to the validated object before
    // ANY deletion (recycle or permanent). A redirected/protected target is refused outright.
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }

    // Recycle first (when enabled), honoring recoverable-only mode; else permanently remove.
    return recycleOrPermanentFile(path);
}

bool CleanupWorker::recycleOrPermanentFile(const QString& path) {
    bool recycleFallback = false;
    switch (attemptRecycle(path, recycleFallback)) {
    case RecycleOutcome::Recycled:
        return true;
    case RecycleOutcome::RefusedRecoverable:
    case RecycleOutcome::RefusedRedirected:
        return false;
    case RecycleOutcome::FallThrough:
        break;
    }
    return removeFilePermanentWithRetry(path, recycleFallback);
}

bool CleanupWorker::removeFilePermanentWithRetry(const QString& path, bool recycleFallback) {
    if (removeFilePermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }

    // Try setting writable and retry.
    QFile file(path);
    file.setPermissions(QFile::ReadOther | QFile::WriteOther);
    if (removeFilePermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }

    // File is locked -- schedule removal on next reboot. A reboot-scheduled delete is NOT counted
    // as success: the object is still present now and its reboot-time removal cannot be pinned to
    // the verified target (MoveFileExW re-resolves the pathname at reboot -- see
    // scheduleRebootRemoval). Record it so rebootPendingItems surfaces it to the operator, but
    // report the item as not-yet- deleted (fail closed) rather than an unconditional success.
    // recycleFallback is not noted here: nothing has been permanently deleted, so the
    // recycle-escalation list would over-state it.
    if (scheduleRebootRemoval(path)) {
        m_rebootPendingPaths.append(path);
    }
    return false;
}

bool CleanupWorker::deleteFolder(const QString& path) {
    // Same guarantor as deleteFile: filePathDeletionRefusal rejects an empty path in
    // cleanItemIfAllowed. The recursive caller (removeForcedEntry) passes an entryInfoList
    // absoluteFilePath, which is never empty either.
    Q_ASSERT(!path.isEmpty());
    // The assert is not enough on its own for a function that recursively removes a tree.
    // QDir("") resolves to the process working directory and reports exists() == true, so
    // if the upstream screen were ever bypassed the forced-removal path below would target
    // that tree. Two lines to make the guarantor irrelevant.
    if (path.isEmpty()) {
        sak::logError("CleanupWorker: refusing to delete a folder with an empty path");
        return false;
    }
    // Ancestor reparse guard FIRST, before dir.exists() (which FOLLOWS the path -- a UNC-symlink
    // ancestor would trigger a remote handshake). Also catches a leaf junction with a swapped
    // ancestor before the leaf-reparse branch below.
    if (sak::pathHasReparsePointAncestor(path)) {
        sak::logWarning("Refusing folder delete of " + path.toStdString() +
                        ": an ancestor is a symlink/junction");
        return false;
    }

    // Leaf reparse check via isSymLink/isJunction (no follow) BEFORE QDir::exists() (which FOLLOWS
    // the link -- a leaf swapped to a UNC junction would trigger a remote handshake). A reparse
    // point is UNLINKED (link only), never recursed into.
    const QFileInfo info(path);
    if (info.isSymLink() || info.isJunction()) {
        // Recycle-only (auto-clean): unlinking a reparse point is a permanent (non-recycled)
        // operation, so leave it for human review rather than auto-remove it.
        if (m_requireRecoverable) {
            sak::logWarning("Auto-clean leaves reparse-point leftover for review: " +
                            path.toStdString());
            return false;
        }
        return unlinkReparsePoint(path);
    }

    QDir dir(path);
    if (!dir.exists()) {
        return true;
    }

    // Ancestor-junction-swap close: verify the folder's real resolved path is still the validated,
    // non-protected target before recursing/deleting. This pins the tree ROOT (an ancestor swapped
    // into a junction to System32 is caught here); per-child reparse points are screened during the
    // forced removal below.
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }
    return finishFolderDelete(dir, path);
}

bool CleanupWorker::finishFolderDelete(QDir& dir, const QString& path) {
    bool recycleFallback = false;
    switch (attemptRecycle(path, recycleFallback)) {
    case RecycleOutcome::Recycled:
        return true;
    case RecycleOutcome::RefusedRecoverable:
    case RecycleOutcome::RefusedRedirected:
        return false;
    case RecycleOutcome::FallThrough:
        break;
    }

    if (removeFolderPermanent(dir, path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }
    return false;
}

bool CleanupWorker::removeFolderPermanent(QDir& dir, const QString& path) {
#ifdef Q_OS_WIN
    // Handle-verified recursive delete: QDir::removeRecursively re-resolves the path STRING for the
    // whole subtree, so a swapped ancestor junction would redirect the recursion into a real
    // target. The verified walk instead opens+verifies the exact object before every destructive
    // syscall.
    Q_UNUSED(dir)
    return removeFolderTreeVerified(path);
#else
    if (dir.removeRecursively()) {
        return true;
    }
    bool all_handled = removeFolderContentsForced(dir);
    if (!dir.rmdir(path)) {
        all_handled = tryScheduleReboot(path) && all_handled;
    }
    return all_handled;
#endif
}

#ifdef Q_OS_WIN
bool CleanupWorker::removeVerifiedFolderEntry(const QFileInfo& entry) {
    const QString child = entry.absoluteFilePath();
    // Guard the string-based paths below against an ANCESTOR swapped into a junction after the
    // parent verify: if any ancestor is now a reparse point the child string would resolve
    // elsewhere, so refuse fail-closed (the leaf being a reparse itself is fine to unlink).
    if (sak::pathHasReparsePointAncestor(child)) {
        sak::logWarning("Refusing folder-entry delete of " + child.toStdString() +
                        ": an ancestor is a symlink/junction");
        return false;
    }
    // A reparse point (junction / dir-symlink): unlink the LINK only, never recurse through it.
    if (entry.isSymLink() || entry.isJunction()) {
        return unlinkReparsePoint(child);
    }
    if (entry.isDir()) {
        return removeFolderTreeVerified(child);  // recurse; re-verifies the subdir by handle
    }
    // Plain file: delete by verified handle (redirect -> refuse). A locked (NotOpenable) file falls
    // back to a string unlink / reboot schedule -- safe now that the ancestor chain is verified.
    const HandleDeleteOutcome outcome = deleteFileByVerifiedHandle(child);
    if (outcome == HandleDeleteOutcome::Deleted) {
        return true;
    }
    if (outcome == HandleDeleteOutcome::Redirected) {
        return false;
    }
    return QFile::remove(child) || tryScheduleReboot(child);
}

bool CleanupWorker::removeEmptyDirByVerifiedHandle(const QString& path) {
    HANDLE handle = openForVerifiedDelete(path);
    if (handle == INVALID_HANDLE_VALUE) {
        // Cannot open to verify: refuse if an ancestor is now a reparse point; else best-effort
        // rmdir (only succeeds on an empty dir) or schedule for reboot.
        if (sak::pathReparseUnsafe(path)) {
            return false;
        }
        return RemoveDirectoryW(reinterpret_cast<LPCWSTR>(path.utf16())) != 0 ||
               tryScheduleReboot(path);
    }
    const QString final_path = finalPathOfHandle(handle);
    const QString refusal = sak::cleanupHandleRedirectRefusal(path, final_path);
    if (!refusal.isEmpty()) {
        CloseHandle(handle);
        sak::logWarning("Refusing folder removal of " + path.toStdString() + ": " +
                        refusal.toStdString());
        return false;
    }
    const bool ok = unlinkByHandle(handle);  // POSIX-deletes an EMPTY directory
    CloseHandle(handle);
    if (ok) {
        return true;
    }
    // Verified handle but unlink failed (dir not empty / locked). Only schedule the string-based
    // reboot removal if no ancestor is a reparse point (else the reboot path could resolve
    // elsewhere).
    if (sak::pathHasReparsePointAncestor(path)) {
        return false;
    }
    return tryScheduleReboot(path);
}

bool CleanupWorker::removeFolderTreeVerified(const QString& path) {
    // Verify THIS directory still resolves to the validated, non-protected target before touching
    // its contents (catches an ancestor swap at this level).
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }
    bool all_handled = true;
    const QDir dir(path);
    const auto entries = dir.entryInfoList(
        QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDir::DirsLast);
    for (const QFileInfo& entry : entries) {
        if (!removeVerifiedFolderEntry(entry)) {
            all_handled = false;
        }
    }
    // Remove the now-empty directory by a verified handle. If some children survived (redirected /
    // locked), the dir is not empty and this fails -> the failure is surfaced via all_handled.
    if (!removeEmptyDirByVerifiedHandle(path)) {
        all_handled = false;
    }
    return all_handled;
}
#endif  // Q_OS_WIN

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
    // Record the reboot schedule so rebootPendingItems surfaces it, but return NOT-handled: a
    // reboot-scheduled delete has not happened yet and its reboot-time resolution cannot be pinned
    // to the verified object, so it must not read as an immediate success in the folder-tree
    // accounting (fail closed). A partly-deleted tree with a locked child is thus honestly reported
    // incomplete.
    if (scheduleRebootRemoval(path)) {
        m_rebootPendingPaths.append(path);
    }
    return false;
}

bool CleanupWorker::scheduleRebootRemoval(const QString& path) {
#ifdef Q_OS_WIN
    // A reboot-delete is INHERENTLY unpinnable: MoveFileExW(MOVEFILE_DELAY_UNTIL_REBOOT) stores the
    // pathname and the OS re-resolves it at reboot, so no Win32 API can bind it to today's verified
    // object -- a documented residual. The actionable mitigation is to require the target to be the
    // validated, non-redirected object at SCHEDULE time: re-verify by handle here and REFUSE to
    // schedule a redirected / reparse-ancestor path (deleteTimeRedirectSafe opens+verifies, or
    // lexically re-screens ancestors when the object cannot be opened).
    if (!deleteTimeRedirectSafe(path)) {
        return false;
    }
    return MoveFileExW(reinterpret_cast<LPCWSTR>(path.utf16()),
                       nullptr,
                       MOVEFILE_DELAY_UNTIL_REBOOT) != 0;
#else
    Q_UNUSED(path)
    return false;
#endif
}

bool CleanupWorker::deleteRegistryKey(const QString& fullKeyPath) {
    // An empty path matches no hive prefix and is rejected by the else branch below.
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

    // Refuse a bare-hive path ("HKLM\\", "HKCU\\\\", ...): the post-hive subkey collapses to empty,
    // and RegDeleteTreeW(hive, L"") would delete EVERY subkey/value of the whole hive. Fail closed
    // (defense-in-depth; the clean_leftovers guard also refuses this before construction).
    if (cleanupRegistrySubkeyIsHiveRoot(path)) {
        sak::logWarning("Refusing registry tree delete of " + fullKeyPath.toStdString() +
                        ": empty post-hive subkey targets the entire hive root");
        return false;
    }

    // Refuse a REG_LINK symbolic-link key: RegDeleteTreeW would follow it and delete the LINK
    // TARGET's subtree, escaping the path denylist that was screened at validate time.
    if (registryKeyIsSymbolicLink(hive, path)) {
        sak::logWarning("Refusing registry tree delete of " + fullKeyPath.toStdString() +
                        ": target is a symbolic-link (REG_LINK) key");
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
    // An empty keyPath matches no hive prefix and is rejected by the else branch below. An empty
    // valueName (which would delete the key's DEFAULT value) is rejected earlier, by
    // registryValueDeletionRefusal in cleanItemIfAllowed/startupEntryCleanupRefusal.
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

    // Refuse a bare-hive path: an empty post-hive subkey makes RegOpenKeyExW(hive, L"") open the
    // hive ROOT, so the value would be deleted directly under HKLM/HKCU/HKCR. Fail closed.
    if (cleanupRegistrySubkeyIsHiveRoot(path)) {
        sak::logWarning("Refusing registry value delete under " + keyPath.toStdString() +
                        ": empty post-hive subkey targets the hive root");
        return false;
    }

    // Refuse a REG_LINK symbolic-link key: opening it KEY_SET_VALUE follows the link, so the value
    // would be deleted on the link TARGET rather than the screened key.
    if (registryKeyIsSymbolicLink(hive, path)) {
        sak::logWarning("Refusing registry value delete under " + keyPath.toStdString() +
                        ": key is a symbolic-link (REG_LINK) key");
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
    // serviceDeletionRefusal, run by cleanItemIfAllowed before this dispatch, rejects an empty
    // service name.
    Q_ASSERT(!serviceName.isEmpty());
    // Launch sc.exe by its absolute System32 path so a same-named binary in the app dir/CWD/PATH
    // cannot run in our stead. Fail closed when SystemRoot is unknown.
    const QString sc = launchableSystemTool(QStringLiteral("sc.exe"));
    if (sc.isEmpty()) {
        sak::logWarning("Refusing service removal: cannot resolve System32 sc.exe");
        return false;
    }
    // Stop the service first
    const auto stop_result =
        sak::runProcess(sc, {QStringLiteral("stop"), serviceName}, kCleanupCommandTimeoutMs);
    if (stop_result.timed_out) {
        sak::logWarning("Service stop timed out for: {}", serviceName.toStdString());
    }

    // Wait a moment for it to stop
    QThread::msleep(kCleanupServiceStopSettleMs);

    // Delete the service
    const auto del_result =
        sak::runProcess(sc, {QStringLiteral("delete"), serviceName}, kCleanupCommandTimeoutMs);
    return del_result.succeeded();
}

bool CleanupWorker::removeScheduledTask(const QString& taskName) {
    // Launch schtasks.exe by its absolute System32 path (anti-hijack); fail closed when unknown.
    const QString schtasks = launchableSystemTool(QStringLiteral("schtasks.exe"));
    if (schtasks.isEmpty()) {
        sak::logWarning("Refusing scheduled-task removal: cannot resolve System32 schtasks.exe");
        return false;
    }
    const auto result = sak::runProcess(
        schtasks,
        {QStringLiteral("/delete"), QStringLiteral("/tn"), taskName, QStringLiteral("/f")},
        kCleanupCommandTimeoutMs);
    return result.succeeded();
}

bool CleanupWorker::removeFirewallRule(const LeftoverItem& item) {
    // netsh "delete rule name=X" with NO other qualifier removes EVERY rule sharing
    // that display name (inbound + outbound, all profiles, all programs). Narrow it
    // with the dir=/profile=/program= identity captured at scan time so only the one
    // matching rule is deleted.
    // Launch netsh.exe by its absolute System32 path (anti-hijack); fail closed when unknown.
    const QString netsh = launchableSystemTool(QStringLiteral("netsh.exe"));
    if (netsh.isEmpty()) {
        sak::logWarning("Refusing firewall-rule removal: cannot resolve System32 netsh.exe");
        return false;
    }
    QStringList args{QStringLiteral("advfirewall"),
                     QStringLiteral("firewall"),
                     QStringLiteral("delete"),
                     QStringLiteral("rule"),
                     QStringLiteral("name=%1").arg(item.path)};
    appendFirewallDeleteFilters(args, item);
    const auto result = sak::runProcess(netsh, args, kCleanupCommandTimeoutMs);
    return result.succeeded();
}

}  // namespace sak
