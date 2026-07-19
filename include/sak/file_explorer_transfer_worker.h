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

private:
    [[nodiscard]] bool transferFromHost(const FileExplorerTransferItem& item,
                                        const QString& destination,
                                        const FileManagementTransferObserver& observer);
    [[nodiscard]] bool transferRawToLocal(const FileExplorerTransferItem& item,
                                          const QString& destination,
                                          const FileManagementTransferObserver& observer);
    [[nodiscard]] bool transferRawStaged(const FileExplorerTransferItem& item,
                                         const FileManagementTransferObserver& observer);

    FileManagementTarget m_source_target;
    FileManagementTarget m_destination_target;
    uint64_t m_raw_read_cap{0};
    bool m_allow_capped_raw_reads{false};
    QStringList m_blockers;
    QStringList m_warnings;
    QString m_last_file_sha256;
    bool m_last_file_hash_capped{false};
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
