// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_archive_worker.h
/// @brief Off-GUI-thread zip compress/extract batches with Files StatusCenter
/// cards (Files CompressArchiveModels/DecompressHelper run on background
/// threads; the zip codec reports no progress, so the card is indeterminate).

#pragma once

#include "sak/file_explorer_transfer_worker.h"
#include "sak/file_management_file_system.h"
#include "sak/worker_base.h"

#include <QList>
#include <QString>
#include <QStringList>

namespace sak {

/// Extraction destination policy (Files DecompressArchiveHere /
/// DecompressArchiveHereSmart / DecompressArchiveToChildFolder).
enum class FileExplorerArchiveWrapMode {
    None,   ///< Extract directly into the current folder.
    Smart,  ///< Wrap unless the archive has a single top-level root.
    Wrap,   ///< Always wrap in "{archive stem}".
};

/// One archive to extract. A non-empty @ref dialog_destination is the Files
/// DecompressArchiveDialog leg: extract into that host folder verbatim.
struct FileExplorerArchiveExtractItem {
    QString source_path;  ///< Archive path on the source target.
    QString name;         ///< Display name ("{stem}.zip").
    quint64 size_bytes{0};
    QString dialog_destination;
};

/// One archive batch (a compress or an extract run) with every GUI-thread
/// decision (gates, dialog destinations, the resolved zip name) made before
/// the worker starts.
struct FileExplorerArchiveRequest {
    bool compress{false};
    FileManagementTarget target;
    QString directory;  ///< Current folder on @ref target.
    /// Compress: resolved "{selection}.zip" name inside @ref directory.
    QString zip_name;
    /// Compress: the selected entries to pack (destination_path unused).
    QList<FileExplorerTransferItem> sources;
    /// Extract: the selected archives.
    QList<FileExplorerArchiveExtractItem> archives;
    FileExplorerArchiveWrapMode wrap_mode{FileExplorerArchiveWrapMode::Smart};
    /// Per-file raw read window for staging raw sources out.
    uint64_t raw_read_cap{0};
};

/// Runs one archive batch off the GUI thread. Raw targets stage through the
/// certified bridge readers/writers exactly like the synchronous path did.
/// The zip codec has no progress callback, so the worker emits no snapshots
/// and the in-progress card stays indeterminate; a stop request is honored
/// between archives, never mid-zip.
class FileExplorerArchiveWorker : public WorkerBase {
    Q_OBJECT

public:
    explicit FileExplorerArchiveWorker(FileExplorerArchiveRequest request,
                                       QObject* parent = nullptr);

    // Join the worker thread while this class's members are still alive (the base
    // ~WorkerBase runs after they are destroyed). See WorkerBase::stopAndJoin.
    ~FileExplorerArchiveWorker() override { stopAndJoin(); }

    [[nodiscard]] const FileExplorerArchiveRequest& request() const { return m_request; }
    /// Safe to read after QThread::finished (the worker thread has exited).
    [[nodiscard]] const QStringList& blockers() const { return m_blockers; }
    [[nodiscard]] const QStringList& warnings() const { return m_warnings; }
    /// Extract: archives fully delivered. Compress: 1 when the zip landed.
    [[nodiscard]] int completedCount() const { return m_completed; }
    /// Compress: files written into the zip.
    [[nodiscard]] int zipEntryCount() const { return m_zip_entries; }

protected:
    auto execute() -> std::expected<void, sak::error_code> override;

private:
    void runCompress();
    [[nodiscard]] QStringList collectCompressSources(const QString& staging_dir);
    void runExtract();
    [[nodiscard]] bool extractOne(const FileExplorerArchiveExtractItem& archive,
                                  const QString& staging_dir);
    [[nodiscard]] bool extractRawArchive(const QString& host_zip, const QString& wrap_name);
    [[nodiscard]] bool deliverTree(const QString& host_out_dir, const QString& wrap_name);
    [[nodiscard]] bool deliverFlattened(const QString& host_out_dir);
    [[nodiscard]] QString stageSource(const FileExplorerTransferItem& item,
                                      const QString& staging_dir);
    [[nodiscard]] QString availableChildName(const QString& name) const;
    [[nodiscard]] bool destinationOccupied(const QString& name) const;
    [[nodiscard]] QString childPath(const QString& name) const;

    FileExplorerArchiveRequest m_request;
    QStringList m_blockers;
    QStringList m_warnings;
    int m_completed{0};
    int m_zip_entries{0};
};

}  // namespace sak
