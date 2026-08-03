// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file cleanup_worker.h
/// @brief WorkerBase subclass for deleting selected leftover items safely

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/worker_base.h"

#include <QDir>
#include <QFileInfo>
#include <QVector>

#include <type_traits>

// Global-namespace unit-test class, befriended below so it can reach the pure static
// denylist seams. Forward-declared here so the qualified friend name resolves.
class TestCleanupWorker;

namespace sak {

/// @brief Deletes selected leftover items
///        (files, folders, registry, services, tasks)
///
/// Processes each item sequentially, reporting progress and continuing
/// on individual failures. Tracks succeeded, failed, and bytes recovered.
/// Supports recycle bin deletion and scheduling locked files for removal on reboot.
class CleanupWorker : public WorkerBase {
    Q_OBJECT

    // Unit-test seam: exercise the pure static denylist screens (itemCleanupRefusal /
    // startupEntryCleanupRefusal) directly without driving a full worker run. The test class
    // lives in the global namespace, so befriend ::TestCleanupWorker (not sak::TestCleanupWorker).
    friend class ::TestCleanupWorker;

public:
    /// @param selectedItems  Items to clean up
    /// @param useRecycleBin  If true, files are sent to the Recycle Bin instead
    ///                       of permanent deletion
    /// @param parent         Parent QObject
    explicit CleanupWorker(const QVector<LeftoverItem>& selectedItems,
                           bool useRecycleBin = false,
                           QObject* parent = nullptr);
    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~CleanupWorker() override { stopAndJoin(); }

    CleanupWorker(const CleanupWorker&) = delete;
    CleanupWorker& operator=(const CleanupWorker&) = delete;
    CleanupWorker(CleanupWorker&&) = delete;
    CleanupWorker& operator=(CleanupWorker&&) = delete;

    /// @brief Require RECOVERABLE deletion: when set, a file/folder that cannot be sent to the
    ///        Recycle Bin is reported FAILED and left in place rather than permanently deleted or
    ///        scheduled for reboot removal. Used by AUTOMATIC (no human review) cleanup so an
    ///        auto-deleted item is always undoable. Must be paired with useRecycleBin=true.
    void setRequireRecoverable(bool require) { m_requireRecoverable = require; }

    /// @brief Resolve a Windows system tool (e.g. "sc.exe") to its ABSOLUTE
    ///        @p systemRoot \\System32 path so a same-named binary planted in the app dir / CWD /
    ///        PATH can never run in our stead (CreateProcess resolves a bare program name through
    ///        those first). Returns empty (fail closed) when @p systemRoot is empty; callers then
    ///        refuse to launch the destructive op. Pure; unit-testable.
    [[nodiscard]] static QString systemToolPath(const QString& systemRoot, const QString& exeName);

Q_SIGNALS:
    void itemCleaned(const QString& path, bool success);
    void cleanupComplete(int succeeded, int failed, qint64 bytesRecovered);

    /// @brief Emitted when locked files are scheduled for removal on next reboot
    void rebootPendingItems(QStringList paths);

    /// @brief Emitted when recycle-bin mode was requested but recycling failed and
    ///        the items were removed permanently instead. Surfaces the silent
    ///        escalation so the user knows these items are NOT recoverable.
    void recycleFallbackItems(QStringList paths);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    QVector<LeftoverItem> m_items;
    bool m_useRecycleBin = false;
    bool m_requireRecoverable = false;   ///< Recycle-only: never permanently delete on recycle fail
    QStringList m_rebootPendingPaths;    ///< Paths scheduled for removal on reboot
    QStringList m_recycleFallbackPaths;  ///< Recycle failed -> deleted permanently instead

    /// @brief Record @p path as a recycle->permanent fallback when @p fallback.
    void noteRecycleFallback(bool fallback, const QString& path);

    /// Outcome of the shared recycle attempt.
    enum class RecycleOutcome {
        Recycled,            ///< Sent to the Recycle Bin (done)
        RefusedRecoverable,  ///< Recoverable-only + could not recycle -> leave in place (failed)
        RefusedRedirected,   ///< Delete-time by-handle re-verify caught a redirect -> refuse
                             ///< (failed)
        FallThrough          ///< Proceed to permanent deletion
    };
    /// @brief Try the Recycle Bin (when enabled), honoring recoverable-only mode. Sets
    ///        @p recycleFallback when a recycle failure falls through to permanent deletion.
    [[nodiscard]] RecycleOutcome attemptRecycle(const QString& path, bool& recycleFallback);

    /// @brief Delete a file, falling back to recycle bin or reboot scheduling
    [[nodiscard]] bool deleteFile(const QString& path);

    /// @brief Recycle-or-permanent tail of deleteFile (after the reparse/redirect screens): try the
    ///        Recycle Bin honoring recoverable-only mode, else permanently remove with the
    ///        writable-retry/reboot tail. Split out to keep deleteFile within the complexity
    ///        budget.
    [[nodiscard]] bool recycleOrPermanentFile(const QString& path);

    /// @brief Delete-time re-verification that @p path still resolves to exactly the validated
    ///        target. Between validate and delete, a local attacker can swap an ANCESTOR directory
    ///        into a junction so the same string resolves elsewhere; this opens a handle, reads the
    ///        object's real path (GetFinalPathNameByHandleW), and refuses (false) on a redirect or
    ///        a resolved-to-protected target. Non-Windows: always true. See
    ///        cleanupHandleRedirectRefusal.
    [[nodiscard]] bool deleteTimeRedirectSafe(const QString& path);

    /// @brief Permanently unlink a file. On Windows deletes BY HANDLE (open, re-verify, unlink via
    ///        the handle -- no path re-resolution after the check), falling back to a string unlink
    ///        only when the object could not be opened (locked/denied) and was NOT redirected.
    [[nodiscard]] bool removeFilePermanent(const QString& path);

    /// @brief Permanent-file removal tail of deleteFile: try by-handle delete, a writable-bit
    /// retry,
    ///        then reboot scheduling for a locked file. Split out to keep deleteFile within budget.
    [[nodiscard]] bool removeFilePermanentWithRetry(const QString& path, bool recycleFallback);

#ifdef Q_OS_WIN
    /// Outcome of a by-handle permanent delete attempt.
    enum class HandleDeleteOutcome {
        Deleted,
        Redirected,
        NotOpenable
    };
    /// @brief Open @p path (no leaf-reparse follow), verify it is still the validated target, and
    ///        delete it via the handle. Redirected => refused, never fall back to a string delete.
    [[nodiscard]] HandleDeleteOutcome deleteFileByVerifiedHandle(const QString& path);
#endif

    /// @brief Dispatch cleanup for a single leftover item by type. Re-screens the item against the
    ///        leftover_cleanup_guard denylist FIRST (defense-in-depth) and refuses fail-closed, so
    ///        a caller that bypasses AdvancedUninstallController cannot drive a protected target
    ///        into the destructive syscalls.
    [[nodiscard]] bool cleanSingleItem(const LeftoverItem& item);
    [[nodiscard]] bool cleanStartupEntry(const LeftoverItem& item);

    /// @brief Run the defense-in-depth denylist screen (itemCleanupRefusal) for @p item, then
    ///        dispatch to cleanSingleItem only when allowed; a refused item is logged and reported
    ///        failed (fail-closed) without reaching any destructive syscall.
    [[nodiscard]] bool cleanItemIfAllowed(const LeftoverItem& item);

    /// @brief Per-type denylist refusal for @p item (empty => allowed). Runs the same
    ///        leftover_cleanup_guard screen the controller applies, matched to the item's type, as
    ///        an independent second layer inside the worker.
    [[nodiscard]] static QString itemCleanupRefusal(const LeftoverItem& item);
    /// @brief Denylist refusal for a StartupEntry (registry value if named, else its on-disk file).
    [[nodiscard]] static QString startupEntryCleanupRefusal(const LeftoverItem& item);

    /// @brief Delete a folder, falling back to reboot scheduling for locked contents
    [[nodiscard]] bool deleteFolder(const QString& path);

    /// @brief Recycle-or-permanent tail of deleteFolder (after the reparse/redirect screens): try
    ///        the Recycle Bin, else permanently remove. Split out to keep deleteFolder within the
    ///        complexity budget.
    [[nodiscard]] bool finishFolderDelete(QDir& dir, const QString& path);

    /// @brief Permanently remove a (non-reparse, redirect-verified) folder. On Windows this is a
    ///        HANDLE-VERIFIED recursive delete (each file deleted by verified handle, each subdir
    ///        re-verified, the emptied dir removed by handle) so an ancestor-junction swap
    ///        mid-delete cannot redirect the recursion into a real target. Elsewhere:
    ///        removeRecursively + a forced per-entry sweep + rmdir, scheduling locked leftovers for
    ///        reboot removal.
    [[nodiscard]] bool removeFolderPermanent(QDir& dir, const QString& path);

#ifdef Q_OS_WIN
    /// @brief Handle-verified recursive delete of @p path's tree (Windows). Every destructive
    ///        syscall is preceded by an immediate open+GetFinalPathNameByHandleW verification of
    ///        the exact target, so a swapped ancestor is caught at every node (fail-closed).
    [[nodiscard]] bool removeFolderTreeVerified(const QString& path);
    /// @brief Remove one child during removeFolderTreeVerified: reparse -> unlink the link only;
    ///        subdir -> recurse; file -> delete by verified handle.
    [[nodiscard]] bool removeVerifiedFolderEntry(const QFileInfo& entry);
    /// @brief Remove an (expected-empty) directory by a redirect-verified handle (POSIX unlink),
    ///        falling back to RemoveDirectoryW / reboot scheduling only when it cannot be opened.
    [[nodiscard]] bool removeEmptyDirByVerifiedHandle(const QString& path);
#endif

    /// @brief Unlink a reparse point (junction / symlink) WITHOUT touching its target
    [[nodiscard]] bool unlinkReparsePoint(const QString& path);

    /// @brief Attempt to remove each file in dir, scheduling locked ones
    [[nodiscard]] bool removeFolderContentsForced(const QDir& dir);

    /// @brief Remove one directory entry during a forced folder cleanup (never follows reparse pts)
    [[nodiscard]] bool removeForcedEntry(const QFileInfo& entry);

    /// @brief Record a reboot-removal schedule for @p path, then return NOT-handled (false). A
    ///        reboot-scheduled delete has not happened yet and cannot be pinned to the verified
    ///        target, so it is surfaced via rebootPendingItems but never counted as an immediate
    ///        success (fail closed).
    [[nodiscard]] bool tryScheduleReboot(const QString& path);

    /// @brief Schedule a locked file for removal on next Windows reboot. Re-verifies the target by
    ///        handle first and REFUSES to schedule a redirected / reparse-ancestor path; the
    ///        reboot-time re-resolution itself is an inherent, unpinnable residual (documented).
    [[nodiscard]] bool scheduleRebootRemoval(const QString& path);

    [[nodiscard]] bool deleteRegistryKey(const QString& fullKeyPath);
    [[nodiscard]] bool deleteRegistryValue(const QString& keyPath, const QString& valueName);
    [[nodiscard]] bool removeService(const QString& serviceName);
    [[nodiscard]] bool removeScheduledTask(const QString& taskName);
    [[nodiscard]] bool removeFirewallRule(const LeftoverItem& item);
};

// -- Compile-Time Invariants -------------------------------------------------

static_assert(std::is_base_of_v<WorkerBase, CleanupWorker>,
              "CleanupWorker must inherit WorkerBase.");
static_assert(!std::is_copy_constructible_v<CleanupWorker>,
              "CleanupWorker must not be copy-constructible.");

}  // namespace sak
