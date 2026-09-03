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
#include <QFileInfo>

#include <string>

#include <windows.h>

#include <shellapi.h>

namespace sak {

namespace {
constexpr int kRegistryHivePrefixLength = 5;
constexpr int kCleanupCommandTimeoutMs = 10'000;
constexpr int kCleanupServiceStopSettleMs = kTimerProgressPollMs;

// Bound on removeFolderTreeVerified recursion: a crafted, deeply nested leftover tree must not
// exhaust the worker thread's stack. Real app-leftover trees are shallow, so past this depth the
// delete fails closed (the tree is reported not fully handled) rather than risk a stack overflow.
constexpr int kMaxCleanupTreeDepth = 256;

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

// Initial character capacity for the GetFinalPathNameByHandleW output buffer; grown on demand when
// the resolved path is longer.
constexpr int kFinalPathBufferChars = 1024;
// Length of the "\\?\UNC\" extended-length UNC prefix that finalPathOfHandle strips.
constexpr int kExtendedUncPrefixLength = 8;
// Length of the "\\?\" extended-length prefix that finalPathOfHandle strips.
constexpr int kExtendedPathPrefixLength = 4;

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
    std::wstring buffer(kFinalPathBufferChars, L'\0');
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
        return QStringLiteral("\\\\") + resolved.mid(kExtendedUncPrefixLength);
    }
    if (resolved.startsWith(QStringLiteral("\\\\?\\"))) {
        return resolved.mid(kExtendedPathPrefixLength);
    }
    return resolved;
}

bool verifiedDeleteKindMatches(DWORD attributes,
                               bool expectedDirectory,
                               bool expectedReparsePoint) {
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool is_reparse = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (expectedReparsePoint) {
        return is_reparse;
    }
    return !is_reparse && is_directory == expectedDirectory;
}

QString verifiedDeleteHandleRefusal(const QString& path,
                                    HANDLE handle,
                                    bool expectedDirectory,
                                    bool expectedReparsePoint) {
    const QString final_path = finalPathOfHandle(handle);
    const QString identity_refusal =
        sak::cleanupHandleRedirectRefusal(path, final_path, expectedReparsePoint);
    if (!identity_refusal.isEmpty()) {
        return QStringLiteral("Refusing permanent delete of %1: %2").arg(path, identity_refusal);
    }

    BY_HANDLE_FILE_INFORMATION object_info{};
    if (GetFileInformationByHandle(handle, &object_info) == 0) {
        return QStringLiteral("Could not verify deletion target type for %1 (Win32 error %2).")
            .arg(path)
            .arg(GetLastError());
    }
    if (!verifiedDeleteKindMatches(
            object_info.dwFileAttributes, expectedDirectory, expectedReparsePoint)) {
        return QStringLiteral(
                   "Target type changed before identity-bound deletion of %1. Refresh "
                   "leftovers and retry.")
            .arg(path);
    }
    return {};
}

QString deleteVerifiedHandleFailure(const QString& path, HANDLE handle) {
    // Windows 10/11 FileDispositionInfoEx removes the name bound to this exact handle. This also
    // preserves hard-link peers: only the opened link is unlinked. IGNORE_READONLY_ATTRIBUTE
    // replaces the old chmod-and-retry chain without another pathname resolution.
    FILE_DISPOSITION_INFO_EX disposition{};
    disposition.Flags = FILE_DISPOSITION_FLAG_DELETE | FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
                        FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (SetFileInformationByHandle(
            handle, FileDispositionInfoEx, &disposition, sizeof(disposition)) != 0) {
        return {};
    }
    return QStringLiteral(
               "Identity-bound deletion failed for %1 (Win32 error %2). Close "
               "applications using it and retry; no legacy, pathname, or reboot "
               "fallback was attempted.")
        .arg(path)
        .arg(GetLastError());
}

// True only if the key opened WITHOUT following the link (REG_OPTION_OPEN_LINK affects the FINAL
// path component only) carries the reserved "SymbolicLinkValue" REG_LINK value that marks a link
// key. Fails CLOSED: a probe that cannot be resolved (ACCESS_DENIED, sharing, invalid handle, ...)
// is reported AS a link so the caller refuses the delete rather than letting RegDeleteTreeW re-
// resolve through an indeterminate key. ERROR_FILE_NOT_FOUND is the one benign "not a link" (a
// nonexistent key makes the subsequent delete a no-op).
bool registryLeafComponentIsLink(HKEY hive, const QString& prefix, bool& notFound) {
    notFound = false;
    HKEY key = nullptr;
    const LONG rc = RegOpenKeyExW(hive,
                                  reinterpret_cast<LPCWSTR>(prefix.utf16()),
                                  REG_OPTION_OPEN_LINK,
                                  KEY_QUERY_VALUE,
                                  &key);
    if (rc == ERROR_FILE_NOT_FOUND) {
        notFound = true;
        return false;
    }
    if (rc != ERROR_SUCCESS) {
        return true;  // fail closed: an indeterminate probe is treated as a link
    }
    DWORD type = 0;
    const LONG q = RegQueryValueExW(key, L"SymbolicLinkValue", nullptr, &type, nullptr, nullptr);
    RegCloseKey(key);
    return q == ERROR_SUCCESS && type == REG_LINK;
}

// A registry key can be a REG_LINK symbolic-link key whose deletion BY PATH would traverse to the
// link TARGET and delete ITS subtree -- escaping the denylist that screened only the literal path.
// A REG_LINK ANCESTOR is equally dangerous: RegDeleteTreeW / RegOpenKeyExW re-resolve the whole
// pathname, so a link anywhere along the path redirects the operation. Probe EVERY component from
// the root down (each as the final component of its own prefix, since REG_OPTION_OPEN_LINK only
// suppresses resolution of the last component, and every shorter prefix has already been proven
// non-link by the time we reach it). True => some component is (or cannot be proven not to be) a
// link and the key must NOT be tree-deleted by path.
bool registryKeyIsSymbolicLink(HKEY hive, const QString& subkey) {
    const QStringList components = subkey.split(QLatin1Char('\\'), Qt::SkipEmptyParts);
    QString prefix;
    for (const QString& component : components) {
        prefix = prefix.isEmpty() ? component : prefix + QLatin1Char('\\') + component;
        bool notFound = false;
        if (registryLeafComponentIsLink(hive, prefix, notFound)) {
            return true;
        }
        if (notFound) {
            // This component does not exist, so no deeper component can either; the delete/open
            // will no-op on a nonexistent key. Nothing left to resolve through -- not a link.
            return false;
        }
    }
    return false;
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
    // Single authority on "has a Recycle Bin": the shared predicate (GetVolumePathNameW
    // resolves mount points and now also consults the shell for a policy-disabled bin),
    // instead of a second, weaker copy that only sliced the drive letter (R5-G22-3).
    return pathVolumeHasRecycleBin(path) &&
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

    const int total = static_cast<int>(m_items.size());

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

        m_currentItemError.clear();
        // Defense-in-depth: re-screen each item against the cleanup denylist before dispatching, so
        // a caller that bypasses AdvancedUninstallController cannot reach the destructive syscalls.
        const bool ok = cleanItemIfAllowed(item);

        if (ok) {
            ++succeeded;
            bytes_recovered += item.sizeBytes;
        } else {
            ++failed;
            if (m_currentItemError.isEmpty()) {
                noteItemFailure(
                    QStringLiteral("Cleanup operation failed; target was left in place. Review the "
                                   "log and retry."));
            }
            Q_EMIT itemCleanupError(item.path, m_currentItemError);
        }

        Q_EMIT itemCleaned(item.path, ok);
    }

    reportProgress(total, total, "Cleanup complete");
    Q_EMIT cleanupComplete(succeeded, failed, bytes_recovered);

    if (!m_recycleFallbackPaths.isEmpty()) {
        Q_EMIT recycleFallbackItems(m_recycleFallbackPaths);
    }

    return {};
}

void CleanupWorker::noteItemFailure(const QString& reason) {
    if (m_currentItemError.isEmpty()) {
        m_currentItemError = reason;
    }
    sak::logWarning("CleanupWorker: " + reason.toStdString());
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
        noteItemFailure(QStringLiteral("Refusing cleanup of %1: %2").arg(item.path, refusal));
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
    HANDLE handle = openForVerifiedDelete(path);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        noteItemFailure(
            QStringLiteral("Could not verify target identity for %1 (Win32 error %2). Close "
                           "applications using it and retry; no pathname fallback was attempted.")
                .arg(path)
                .arg(error));
        return false;
    }
    const QString final_path = finalPathOfHandle(handle);
    CloseHandle(handle);
    const QString refusal = sak::cleanupHandleRedirectRefusal(path, final_path);
    if (!refusal.isEmpty()) {
        noteItemFailure(QStringLiteral("Refusing cleanup delete of %1: %2").arg(path, refusal));
        return false;
    }
    return true;
}

bool CleanupWorker::deletePathByVerifiedHandle(const QString& path,
                                               VerifiedDeleteKind expectedKind) {
    HANDLE handle = openForVerifiedDelete(path);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
            return true;
        }
        noteItemFailure(
            QStringLiteral("Could not open %1 for identity-bound deletion (Win32 error %2). Close "
                           "applications using it and retry; no pathname or reboot fallback was "
                           "attempted.")
                .arg(path)
                .arg(error));
        return false;
    }
    const bool expected_directory = expectedKind == VerifiedDeleteKind::Directory;
    const bool expected_reparse = expectedKind == VerifiedDeleteKind::ReparsePoint;
    const QString refusal =
        verifiedDeleteHandleRefusal(path, handle, expected_directory, expected_reparse);
    if (!refusal.isEmpty()) {
        CloseHandle(handle);
        noteItemFailure(refusal);
        return false;
    }
    const QString delete_failure = deleteVerifiedHandleFailure(path, handle);
    CloseHandle(handle);
    if (!delete_failure.isEmpty()) {
        noteItemFailure(delete_failure);
        return false;
    }
    return true;
}

bool CleanupWorker::removeFilePermanent(const QString& path) {
    return deletePathByVerifiedHandle(path, VerifiedDeleteKind::RegularFile);
}

CleanupWorker::RecycleOutcome CleanupWorker::decideRecycle(const QString& path,
                                                           bool useRecycleBin,
                                                           bool requireRecoverable,
                                                           const RecycleProbes& probes,
                                                           bool& recycleFallback) {
    if (useRecycleBin) {
        // Recycle-only mode GUARANTEES recoverability: refuse when the volume has no Recycle Bin OR
        // the item is too large to fit it (the shell silently permanent-deletes in both cases),
        // leaving it in place for review instead.
        if (requireRecoverable && !probes.recoverableAllowed()) {
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
        if (!probes.redirectSafe()) {
            return RecycleOutcome::RefusedRedirected;
        }
        if (probes.doRecycle()) {
            return RecycleOutcome::Recycled;
        }
        // Recycle-only (automatic) mode: never fall through to permanent deletion -- report failed
        // and leave the item in place so an auto-deleted item is always recoverable.
        if (requireRecoverable) {
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
    if (requireRecoverable) {
        sak::logWarning("Recoverable-only cleanup with recycle disabled; leaving " +
                        path.toStdString() + " in place");
        return RecycleOutcome::RefusedRecoverable;
    }
    return RecycleOutcome::FallThrough;
}

CleanupWorker::RecycleOutcome CleanupWorker::attemptRecycle(const QString& path,
                                                            bool& recycleFallback) {
    // Bind the real filesystem probes lazily; decideRecycle invokes each only when its branch is
    // reached, so the side-effect ordering is identical to the previous inline implementation.
    return decideRecycle(path,
                         m_useRecycleBin,
                         m_requireRecoverable,
                         RecycleProbes{[&] { return recoverableRecycleAllowed(path); },
                                       [&] { return deleteTimeRedirectSafe(path); },
                                       [&] { return sendPathToRecycleBin(path); }},
                         recycleFallback);
}

bool CleanupWorker::deleteFile(const QString& path) {
    // cleanItemIfAllowed screens every item through filePathDeletionRefusal, which rejects an empty
    // path, before cleanSingleItem/cleanStartupEntry can dispatch here.
    Q_ASSERT(!path.isEmpty());
    // Belt and braces on a permanent delete: an empty path reaches QFileInfo("").exists()
    // == false and is currently reported as "already gone", i.e. a SUCCESSFUL deletion of
    // something that was never named. Refuse it explicitly instead.
    if (path.isEmpty()) {
        noteItemFailure(QStringLiteral("Refusing to delete a file with an empty path."));
        return false;
    }
    // Ancestor reparse guard FIRST, before any stat that FOLLOWS the path: an ancestor swapped to a
    // (UNC) symlink would otherwise make exists() trigger a remote handshake. pathHasReparsePoint
    // Ancestor reads each ancestor's own attributes WITHOUT following, so it is safe to call first.
    if (sak::pathHasReparsePointAncestor(path)) {
        noteItemFailure(QStringLiteral("Refusing file delete of %1: an ancestor is a "
                                       "symlink/junction.")
                            .arg(path));
        return false;
    }
    const QFileInfo info(path);
    // Leaf reparse check uses isSymLink/isJunction (which read the LINK's own attributes, no
    // follow) BEFORE exists() (which FOLLOWS the link and would trigger an SMB/NTLM handshake if
    // the leaf was swapped to a UNC symlink). A file symlink leftover is unlinked as the LINK only.
    if (info.isSymLink() || info.isJunction()) {
        if (m_requireRecoverable) {
            noteItemFailure(QStringLiteral("Auto-clean left reparse-point item %1 in place for "
                                           "manual review.")
                                .arg(path));
            return false;
        }
        return deletePathByVerifiedHandle(path, VerifiedDeleteKind::ReparsePoint);
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
        noteItemFailure(
            QStringLiteral("Item could not be removed recoverably and was left in "
                           "place for manual review."));
        return false;
    case RecycleOutcome::RefusedRedirected:
        noteItemFailure(
            QStringLiteral("Target identity changed before recycle; item was left in "
                           "place. Refresh leftovers and retry."));
        return false;
    case RecycleOutcome::FallThrough:
        break;
    }
    if (removeFilePermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }
    return false;
}

bool CleanupWorker::deleteFolder(const QString& path) {
    // Same guarantor as deleteFile: filePathDeletionRefusal rejects an empty path in
    // cleanItemIfAllowed.
    Q_ASSERT(!path.isEmpty());
    // The assert is not enough on its own for a function that recursively removes a tree.
    // QDir("") resolves to the process working directory and reports exists() == true, so
    // if the upstream screen were ever bypassed the forced-removal path below would target
    // that tree. Two lines to make the guarantor irrelevant.
    if (path.isEmpty()) {
        noteItemFailure(QStringLiteral("Refusing to delete a folder with an empty path."));
        return false;
    }
    // Ancestor reparse guard FIRST, before dir.exists() (which FOLLOWS the path -- a UNC-symlink
    // ancestor would trigger a remote handshake). Also catches a leaf junction with a swapped
    // ancestor before the leaf-reparse branch below.
    if (sak::pathHasReparsePointAncestor(path)) {
        noteItemFailure(QStringLiteral("Refusing folder delete of %1: an ancestor is a "
                                       "symlink/junction.")
                            .arg(path));
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
            noteItemFailure(QStringLiteral("Auto-clean left reparse-point item %1 in place for "
                                           "manual review.")
                                .arg(path));
            return false;
        }
        return deletePathByVerifiedHandle(path, VerifiedDeleteKind::ReparsePoint);
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
    return finishFolderDelete(path);
}

bool CleanupWorker::finishFolderDelete(const QString& path) {
    bool recycleFallback = false;
    switch (attemptRecycle(path, recycleFallback)) {
    case RecycleOutcome::Recycled:
        return true;
    case RecycleOutcome::RefusedRecoverable:
        noteItemFailure(
            QStringLiteral("Folder could not be removed recoverably and was left in "
                           "place for manual review."));
        return false;
    case RecycleOutcome::RefusedRedirected:
        noteItemFailure(
            QStringLiteral("Folder identity changed before recycle; it was left in "
                           "place. Refresh leftovers and retry."));
        return false;
    case RecycleOutcome::FallThrough:
        break;
    }

    if (removeFolderPermanent(path)) {
        noteRecycleFallback(recycleFallback, path);
        return true;
    }
    return false;
}

bool CleanupWorker::removeFolderPermanent(const QString& path) {
    // Handle-verified recursive delete: QDir::removeRecursively re-resolves the path STRING for the
    // whole subtree, so a swapped ancestor junction would redirect the recursion into a real
    // target. The verified walk instead opens+verifies the exact object before every destructive
    // syscall.
    return removeFolderTreeVerified(path);
}

bool CleanupWorker::removeVerifiedFolderEntry(const QFileInfo& entry, int depth) {
    const QString child = entry.absoluteFilePath();
    // Guard the string-based paths below against an ANCESTOR swapped into a junction after the
    // parent verify: if any ancestor is now a reparse point the child string would resolve
    // elsewhere, so refuse fail-closed (the leaf being a reparse itself is fine to unlink).
    if (sak::pathHasReparsePointAncestor(child)) {
        noteItemFailure(QStringLiteral("Refusing folder-entry delete of %1: an ancestor is a "
                                       "symlink/junction.")
                            .arg(child));
        return false;
    }
    // A reparse point (junction / dir-symlink): unlink the LINK only, never recurse through it.
    if (entry.isSymLink() || entry.isJunction()) {
        return deletePathByVerifiedHandle(child, VerifiedDeleteKind::ReparsePoint);
    }
    if (entry.isDir()) {
        return removeFolderTreeVerified(child, depth);  // recurse; re-verifies the subdir by handle
    }
    return deletePathByVerifiedHandle(child, VerifiedDeleteKind::RegularFile);
}

bool CleanupWorker::removeFolderTreeVerified(const QString& path, int depth) {
    // Bound the recursion depth: a crafted, deeply nested leftover tree must not exhaust the
    // worker thread's stack. Past the budget, fail closed (report the tree not fully handled) so
    // the emptied-dir removal below then fails and the incompleteness propagates upward.
    if (depth > kMaxCleanupTreeDepth) {
        noteItemFailure(QStringLiteral("Refusing folder delete of %1: directory nesting exceeds "
                                       "the safe recursion depth.")
                            .arg(path));
        return false;
    }
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
        if (!removeVerifiedFolderEntry(entry, depth + 1)) {
            all_handled = false;
        }
    }
    // Remove the now-empty directory by a verified handle. If some children survived (redirected /
    // locked), the dir is not empty and this fails -> the failure is surfaced via all_handled.
    if (!deletePathByVerifiedHandle(path, VerifiedDeleteKind::Directory)) {
        all_handled = false;
    }
    return all_handled;
}

bool CleanupWorker::deleteFolderTreeVerified(const QString& path) {
    // Reuse the exact per-node handle-verified walk WITHOUT forking it. The tree walk reads no item
    // list and no recycle config, so a throwaway, empty-item worker drives it correctly. Locked or
    // unverifiable entries fail and remain in place; this standalone seam never schedules delayed
    // pathname work that its caller cannot observe.
    const QVector<LeftoverItem> noItems;
    CleanupWorker worker(noItems);
    return worker.removeFolderTreeVerified(path);
}

bool CleanupWorker::deleteRegistryKey(const QString& fullKeyPath) {
    // An empty path matches no hive prefix and is rejected by the else branch below.
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
    LONG const rc = RegDeleteTreeW(hive, reinterpret_cast<LPCWSTR>(path.utf16()));

    return rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
}

bool CleanupWorker::deleteRegistryValue(const QString& keyPath, const QString& valueName) {
    // An empty keyPath matches no hive prefix and is rejected by the else branch below. An empty
    // valueName (which would delete the key's DEFAULT value) is rejected earlier, by
    // registryValueDeletionRefusal in cleanItemIfAllowed/startupEntryCleanupRefusal.
    Q_ASSERT(!valueName.isEmpty());
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
