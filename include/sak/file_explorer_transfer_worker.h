// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_transfer_worker.h
/// @brief Off-GUI-thread explorer transfers with Files StatusCenter progress
/// (Files FilesystemHelpers copy/move wiring: enumerate first, then process
/// with byte progress through IProgress, cancelable via the card token).

#pragma once

#include "sak/file_explorer_status_center.h"
#include "sak/file_management_file_system.h"
#include "sak/worker_base.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace sak {

/// One resolved transfer leg. Collisions are resolved on the GUI thread
/// before the worker starts (Files shows its conflict dialog up front), so
/// @ref destination_path is final.
struct FileExplorerTransferItem {
    QString source_path;
    QString destination_path;
    quint64 size_bytes{0};
    bool directory{false};
    /// Replace-resolved collision: the engine removes the destination's current
    /// occupant right before this item's own copy (not batch-up-front), so a
    /// cancel or failure earlier in the batch leaves it untouched.
    bool replace_destination{false};
};

/// Shared synchronous transfer legs between two targets: host source streams
/// in through the bridge writers, raw source exports through the certified
/// readers, raw-to-raw stages through a scratch host folder, and a
/// same-target move reparents via renameEntry with no data copy. One
/// implementation serves both the worker and the panel's synchronous
/// undo/redo history path.
class FileExplorerTransferEngine {
public:
    /// @param allow_capped_raw_reads Copy Out semantics: a raw file larger
    /// than the window copies capped (and marked) instead of failing; a paste
    /// keeps the fail-closed guard so moves never delete an intact source.
    FileExplorerTransferEngine(FileManagementTarget source_target,
                               FileManagementTarget destination_target,
                               uint64_t raw_read_cap,
                               bool allow_capped_raw_reads = false);

    /// Copies @p item to its resolved destination. Returns false (with
    /// blockers) when the copy is incomplete, so a move never deletes a
    /// source that did not land whole.
    [[nodiscard]] bool transferEntry(const FileExplorerTransferItem& item,
                                     const FileManagementTransferObserver& observer = {});
    /// Same-target move: renameEntry reparents through the certified writers
    /// on raw volumes and QFile::rename locally, no data copy.
    [[nodiscard]] bool renameWithinTarget(const FileExplorerTransferItem& item);
    /// Removes the source of a completed move.
    [[nodiscard]] bool deleteMovedSource(const FileExplorerTransferItem& item);

    [[nodiscard]] const QStringList& blockers() const { return m_blockers; }
    [[nodiscard]] const QStringList& warnings() const { return m_warnings; }
    /// SHA-256 of the last raw-to-local single-file export (empty otherwise).
    [[nodiscard]] const QString& lastFileSha256() const { return m_last_file_sha256; }
    [[nodiscard]] bool lastFileHashCapped() const { return m_last_file_hash_capped; }
    /// False when the last transferEntry copied but dropped entries (skipped
    /// symlinks, capped raw files, or depth/entry-cap overflow); a move must
    /// never delete the source when the copy did not land whole.
    [[nodiscard]] bool lastTransferComplete() const { return !m_last_transfer_incomplete; }
    /// False when the last transferEntry lost content the caller never asked to
    /// lose. Narrower than @ref lastTransferComplete: an explicitly permitted
    /// capped raw read (Copy Out, which marks the copy as capped) is a shortfall
    /// the caller requested and stays true here. This is the predicate for "may
    /// be reported as a completed item".
    [[nodiscard]] bool lastTransferLandedAsRequested() const {
        return !m_last_transfer_unrequested_loss;
    }

private:
    /// Record a shortfall in the last copy leg. @p sanctioned marks a shortfall
    /// the caller explicitly asked for (an allowed capped raw read), which still
    /// blocks a move's source delete but not the completed-item report.
    void noteIncompleteTransfer(bool sanctioned);
    [[nodiscard]] bool removeReplacedDestination(const FileExplorerTransferItem& item);
    /// Dispatch @p item to the correct copy leg, writing to @p destination.
    [[nodiscard]] bool transferItemTo(const FileExplorerTransferItem& item,
                                      const QString& destination,
                                      const FileManagementTransferObserver& observer);
    /// Replace an existing destination without ever risking the original: stage the
    /// copy to a sibling temp, move the original aside to a backup, move the staged
    /// copy into place, then drop the backup -- rolling back to the original if any
    /// step fails, so a copy or move failure never destroys the original (B8-06).
    [[nodiscard]] bool transferReplacing(const FileExplorerTransferItem& item,
                                         const FileManagementTransferObserver& observer);
    /// A staging/backup sibling of @p destination_path in its own parent (so the
    /// final move is a same-directory rename), named with @p prefix, a per-engine
    /// sequence for uniqueness within the run, and 64 bits of system entropy so the
    /// name cannot be guessed and pre-planted by a co-located writer.
    [[nodiscard]] QString siblingTempPath(const QString& destination_path, const QString& prefix);
    /// True only when NOTHING occupies @p path on the destination target. Anything
    /// short of proof of absence -- an occupant, an undecidable stat, a failed or
    /// truncated listing -- reports false so the caller fails closed instead of
    /// planting a staging/backup sibling on top of existing data.
    [[nodiscard]] bool destinationPathVacant(const QString& path);
    /// Claim @p path as this engine's staging sibling: a local destination gets a
    /// genuine exclusive create (a directory when @p directory, otherwise an empty
    /// file), so a pre-existing entry or a planted symlink at the name fails rather
    /// than being merged into, overwritten, or deleted by the cleanup. Raw
    /// destinations have no exclusive-create primitive and fall back to proving the
    /// name vacant through @ref destinationPathVacant.
    [[nodiscard]] bool reserveStagingPath(const QString& path, bool directory);
    // Best-effort cleanup of a staged/backup sibling, resolving its kind at
    // execution time; the caller reports the real outcome, so the removal result is
    // advisory (not [[nodiscard]]).
    bool removeDestinationEntry(const QString& path);
    [[nodiscard]] bool renameDestination(const QString& from, const QString& to);
    /// Source and destination name the SAME entry: parent and last component match
    /// case-insensitively (an exact spelling, or a case-only change on a
    /// case-insensitive volume). Such a rename has no true occupant to replace -- it
    /// must route straight to renameEntry rather than delete a "replaced" source.
    [[nodiscard]] static bool isSameTargetEntry(const FileExplorerTransferItem& item);
    /// Settle a failed renameEntry: restore a set-aside occupant, and when even that
    /// restore fails, surface both failures (the occupant is stranded at @p backup).
    /// Always reports failure (returns false), so the caller returns it directly.
    [[nodiscard]] bool reportFailedRename(const FileManagementMutationResult& result,
                                          const FileExplorerTransferItem& item,
                                          bool occupant_set_aside,
                                          const QString& backup);
    /// Best-effort removal of the set-aside occupant after a successful rename; a
    /// removal failure leaves the previous occupant at an internal name, so warn
    /// rather than swallow it.
    void dropSetAsideRenameOccupant(const FileExplorerTransferItem& item,
                                    bool occupant_set_aside,
                                    const QString& backup);
    [[nodiscard]] bool transferFromHost(const FileExplorerTransferItem& item,
                                        const QString& destination,
                                        const FileManagementTransferObserver& observer);
    [[nodiscard]] bool transferRawToLocal(const FileExplorerTransferItem& item,
                                          const QString& destination,
                                          const FileManagementTransferObserver& observer);
    [[nodiscard]] bool transferRawDirectoryToLocal(const FileExplorerTransferItem& item,
                                                   const QString& destination,
                                                   const FileManagementTransferObserver& observer);
    [[nodiscard]] bool transferRawStaged(const FileExplorerTransferItem& item,
                                         const QString& destination,
                                         const FileManagementTransferObserver& observer);

    FileManagementTarget m_source_target;
    FileManagementTarget m_destination_target;
    uint64_t m_raw_read_cap{0};
    bool m_allow_capped_raw_reads{false};
    QStringList m_blockers;
    QStringList m_warnings;
    QString m_last_file_sha256;
    bool m_last_file_hash_capped{false};
    bool m_last_transfer_incomplete{false};
    bool m_last_transfer_unrequested_loss{false};
    int m_replace_seq{0};
};

/// What one worker run does with its items (Files FileOperationType routing:
/// Delete and Recycle post the Delete-family cards and walk no bytes).
enum class FileExplorerTransferKind {
    Transfer,
    Delete,
    Recycle,
};

/// One transfer batch for the worker.
struct FileExplorerTransferRequest {
    FileManagementTarget source_target;
    FileManagementTarget destination_target;
    QList<FileExplorerTransferItem> items;
    FileExplorerTransferKind kind{FileExplorerTransferKind::Transfer};
    bool move{false};
    /// Same-target move: rename legs instead of copy+delete.
    bool rename_within_target{false};
    /// Copy Out semantics: oversized raw files copy capped instead of failing.
    bool allow_capped_raw_reads{false};
    /// Per-file raw read window for raw-to-local legs.
    uint64_t raw_read_cap{0};
};

/// Runs a resolved transfer batch off the GUI thread: discovery first
/// (items/sizes reported progressively, Files "Discovering items..."), then
/// per-item transfers with byte-level progress through the bridge observer.
/// Cancellation comes from the status-center card via requestStop().
class FileExplorerTransferWorker : public WorkerBase {
    Q_OBJECT

public:
    explicit FileExplorerTransferWorker(FileExplorerTransferRequest request,
                                        QObject* parent = nullptr);

    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~FileExplorerTransferWorker() override { stopAndJoin(); }

    /// Safe to read after QThread::finished (the worker thread has exited).
    [[nodiscard]] const QStringList& blockers() const { return m_blockers; }
    [[nodiscard]] const QStringList& warnings() const { return m_warnings; }
    /// Items that landed whole (moves: copied and source removed).
    [[nodiscard]] const QList<FileExplorerTransferItem>& completedItems() const {
        return m_completed;
    }
    [[nodiscard]] const QString& lastFileSha256() const { return m_last_file_sha256; }
    [[nodiscard]] bool lastFileHashCapped() const { return m_last_file_hash_capped; }

Q_SIGNALS:
    /// Throttled progress snapshots for the status-center card (queued to the
    /// GUI thread).
    void statusProgress(const sak::FileExplorerStatusProgress& snapshot);

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    void discover(FileExplorerStatusProgressReporter* reporter);
    void discoverHostTree(const QString& path, FileExplorerStatusProgressReporter* reporter);
    void discoverRawTree(const QString& path,
                         int depth,
                         FileExplorerStatusProgressReporter* reporter);
    void transferItems(FileExplorerStatusProgressReporter* reporter,
                       FileExplorerTransferEngine* engine);
    [[nodiscard]] bool transferOne(const FileExplorerTransferItem& item,
                                   FileExplorerTransferEngine* engine,
                                   FileExplorerStatusProgressReporter* reporter);
    [[nodiscard]] bool deleteOne(const FileExplorerTransferItem& item);

    FileExplorerTransferRequest m_request;
    QStringList m_blockers;
    QStringList m_warnings;
    QList<FileExplorerTransferItem> m_completed;
    QString m_last_file_sha256;
    bool m_last_file_hash_capped{false};
    qint64 m_discovered_items{0};
    qint64 m_discovered_bytes{0};
    qint64 m_processed_bytes{0};
};

}  // namespace sak
