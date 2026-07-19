// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_transfer_worker.h"

#include "sak/recycle_bin.h"

#include <QDir>
#include <QDirIterator>
#include <QTemporaryDir>

#include <utility>

namespace sak {

namespace {

// Matches the bridge's recursive-walk depth bound.
constexpr int kDiscoveryMaxDepth = 32;
constexpr int kDiscoveryMaxEntriesPerDirectory = 10'000;

QString transferItemName(const QString& path) {
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? clean : clean.mid(slash + 1);
}

}  // namespace

FileExplorerTransferEngine::FileExplorerTransferEngine(FileManagementTarget source_target,
                                                       FileManagementTarget destination_target,
                                                       const uint64_t raw_read_cap,
                                                       const bool allow_capped_raw_reads)
    : m_source_target(std::move(source_target))
    , m_destination_target(std::move(destination_target))
    , m_raw_read_cap(raw_read_cap)
    , m_allow_capped_raw_reads(allow_capped_raw_reads) {}

bool FileExplorerTransferEngine::transferEntry(const FileExplorerTransferItem& item,
                                               const FileManagementTransferObserver& observer) {
    if (m_source_target.local_file_system) {
        return transferFromHost(item, item.destination_path, observer);
    }
    if (m_destination_target.local_file_system) {
        return transferRawToLocal(item, item.destination_path, observer);
    }
    return transferRawStaged(item, observer);
}

bool FileExplorerTransferEngine::renameWithinTarget(const FileExplorerTransferItem& item) {
    const auto result = FileManagementFileSystemBridge::renameEntry(m_destination_target,
                                                                    item.source_path,
                                                                    item.destination_path);
    if (!result.ok) {
        m_blockers.append(result.blockers);
    }
    return result.ok;
}

bool FileExplorerTransferEngine::deleteMovedSource(const FileExplorerTransferItem& item) {
    const auto result =
        item.directory
            ? FileManagementFileSystemBridge::deleteDirectoryTree(m_source_target, item.source_path)
            : FileManagementFileSystemBridge::deleteFile(m_source_target, item.source_path);
    if (!result.ok) {
        m_blockers.append(QStringLiteral("Copied but could not remove the moved source %1: %2")
                              .arg(item.source_path, result.blockers.join(QStringLiteral("; "))));
        return false;
    }
    return true;
}

bool FileExplorerTransferEngine::transferFromHost(const FileExplorerTransferItem& item,
                                                  const QString& destination,
                                                  const FileManagementTransferObserver& observer) {
    if (item.directory) {
        const auto result = FileManagementFileSystemBridge::importDirectoryFromHost(
            m_destination_target, item.source_path, destination, observer);
        m_warnings.append(result.warnings);
        m_blockers.append(result.blockers);
        return result.ok;
    }
    const auto result = FileManagementFileSystemBridge::writeFileFromHostPath(
        m_destination_target, destination, item.source_path, observer);
    if (!result.ok) {
        m_blockers.append(result.blockers);
        return false;
    }
    return true;
}

bool FileExplorerTransferEngine::transferRawToLocal(
    const FileExplorerTransferItem& item,
    const QString& destination,
    const FileManagementTransferObserver& observer) {
    const QString name = transferItemName(item.source_path);
    if (item.directory) {
        const auto result = FileManagementFileSystemBridge::exportDirectoryToHost(
            m_source_target, item.source_path, destination, m_raw_read_cap, observer);
        m_warnings.append(result.warnings);
        m_blockers.append(result.blockers);
        if (result.ok && result.capped_files > 0 && !m_allow_capped_raw_reads) {
            // A truncated file means the tree did not land whole: report it and
            // fail the item so a move never deletes the intact source.
            m_blockers.append(QStringLiteral("%1 file(s) inside %2 exceed the raw read "
                                             "window; the pasted copy is incomplete.")
                                  .arg(result.capped_files)
                                  .arg(name));
            return false;
        }
        return result.ok;
    }
    if (item.size_bytes > m_raw_read_cap && !m_allow_capped_raw_reads) {
        m_blockers.append(QStringLiteral("%1 exceeds the raw read window; a complete paste "
                                         "is not possible (use Copy Out for an explicitly "
                                         "capped copy).")
                              .arg(name));
        return false;
    }
    const auto result = FileManagementFileSystemBridge::copyFileToHost(
        m_source_target, item.source_path, destination, m_raw_read_cap, observer);
    if (!result.ok) {
        m_blockers.append(result.blockers);
        return false;
    }
    m_last_file_sha256 = result.sha256;
    m_last_file_hash_capped = result.capped;
    return true;
}

bool FileExplorerTransferEngine::transferRawStaged(const FileExplorerTransferItem& item,
                                                   const FileManagementTransferObserver& observer) {
    QTemporaryDir staging;
    if (!staging.isValid()) {
        m_blockers.append(
            QStringLiteral("Could not create a staging folder for the raw-to-raw transfer."));
        return false;
    }
    const QString staged = QDir(staging.path()).filePath(transferItemName(item.source_path));
    if (!transferRawToLocal(item, staged, observer)) {
        return false;
    }
    FileExplorerTransferItem staged_item = item;
    staged_item.source_path = staged;
    // The staged copy is a host source now; stream it into the destination.
    FileExplorerTransferEngine host_leg(FileManagementFileSystemBridge::localTarget(QString()),
                                        m_destination_target,
                                        m_raw_read_cap);
    const bool ok = host_leg.transferFromHost(staged_item, item.destination_path, observer);
    m_warnings.append(host_leg.warnings());
    m_blockers.append(host_leg.blockers());
    return ok;
}

FileExplorerTransferWorker::FileExplorerTransferWorker(FileExplorerTransferRequest request,
                                                       QObject* parent)
    : WorkerBase(parent), m_request(std::move(request)) {
    qRegisterMetaType<FileExplorerStatusProgress>();
}

auto FileExplorerTransferWorker::execute() -> std::expected<void, sak::error_code> {
    FileExplorerStatusProgressReporter reporter(
        [this](const FileExplorerStatusProgress& snapshot) { Q_EMIT statusProgress(snapshot); });
    discover(&reporter);
    if (checkStop()) {
        return {};
    }
    reporter.setEnumerationCompleted();
    reporter.report();

    FileExplorerTransferEngine engine(m_request.source_target,
                                      m_request.destination_target,
                                      m_request.raw_read_cap,
                                      m_request.allow_capped_raw_reads);
    transferItems(&reporter, &engine);

    m_blockers.append(engine.blockers());
    m_warnings.append(engine.warnings());
    m_last_file_sha256 = engine.lastFileSha256();
    m_last_file_hash_capped = engine.lastFileHashCapped();
    if (stopRequested()) {
        // The cancel blockers are bookkeeping, not user-facing errors.
        m_blockers.removeAll(kFileManagementTransferCancelledBlocker);
        reporter.setStatus(FileExplorerReturnResult::Cancelled);
    } else if (!m_blockers.isEmpty()) {
        reporter.setStatus(FileExplorerReturnResult::Failed);
    }
    reporter.flushReport();
    return {};
}

// Files "Discovering items...": enumerate the batch before processing so the
// card can show discovered counts and a real percentage denominator.
void FileExplorerTransferWorker::discover(FileExplorerStatusProgressReporter* reporter) {
    for (const FileExplorerTransferItem& item : m_request.items) {
        if (checkStop()) {
            return;
        }
        ++m_discovered_items;
        if (m_request.rename_within_target ||
            m_request.kind != FileExplorerTransferKind::Transfer) {
            // Renames and the delete family move no bytes; the card tracks
            // the selected item count (Files "{0}/{1} items processed").
            reporter->setItemsCount(m_discovered_items);
            reporter->report();
            continue;
        }
        if (!item.directory) {
            m_discovered_bytes += static_cast<qint64>(item.size_bytes);
        } else if (m_request.source_target.local_file_system) {
            discoverHostTree(item.source_path, reporter);
        } else {
            discoverRawTree(item.source_path, 0, reporter);
        }
        reporter->setItemsCount(m_discovered_items);
        reporter->setTotalSize(m_discovered_bytes);
        reporter->report();
    }
}

void FileExplorerTransferWorker::discoverHostTree(const QString& path,
                                                  FileExplorerStatusProgressReporter* reporter) {
    QDirIterator it(path,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (checkStop()) {
            return;
        }
        it.next();
        const QFileInfo info = it.fileInfo();
        ++m_discovered_items;
        if (info.isFile() && !info.isSymLink()) {
            m_discovered_bytes += info.size();
        }
        reporter->setItemsCount(m_discovered_items);
        reporter->setTotalSize(m_discovered_bytes);
        reporter->report();
    }
}

void FileExplorerTransferWorker::discoverRawTree(const QString& path,
                                                 const int depth,
                                                 FileExplorerStatusProgressReporter* reporter) {
    if (depth > kDiscoveryMaxDepth || checkStop()) {
        return;
    }
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        m_request.source_target, path, kDiscoveryMaxEntriesPerDirectory);
    if (!listing.ok) {
        return;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        ++m_discovered_items;
        if (entry.directory) {
            discoverRawTree(entry.path, depth + 1, reporter);
        } else if (entry.regular_file) {
            m_discovered_bytes += static_cast<qint64>(entry.size_bytes);
        }
        reporter->setItemsCount(m_discovered_items);
        reporter->setTotalSize(m_discovered_bytes);
        reporter->report();
    }
}

void FileExplorerTransferWorker::transferItems(FileExplorerStatusProgressReporter* reporter,
                                               FileExplorerTransferEngine* engine) {
    for (const FileExplorerTransferItem& item : m_request.items) {
        if (checkStop()) {
            return;
        }
        reporter->setFileName(transferItemName(item.source_path));
        reporter->report();
        if (transferOne(item, engine, reporter)) {
            m_completed.append(item);
        }
        reporter->addProcessedItems(1);
        reporter->report();
    }
}

bool FileExplorerTransferWorker::transferOne(const FileExplorerTransferItem& item,
                                             FileExplorerTransferEngine* engine,
                                             FileExplorerStatusProgressReporter* reporter) {
    if (m_request.kind != FileExplorerTransferKind::Transfer) {
        return deleteOne(item);
    }
    if (m_request.rename_within_target) {
        return engine->renameWithinTarget(item);
    }
    const FileManagementTransferObserver observer{
        .on_bytes =
            [this, reporter](const qint64 delta) {
                m_processed_bytes += delta;
                reporter->setProcessedSize(m_processed_bytes);
                reporter->report();
            },
        .cancelled = [this]() { return stopRequested(); },
    };
    if (!engine->transferEntry(item, observer)) {
        return false;
    }
    if (m_request.move) {
        return engine->deleteMovedSource(item);
    }
    return true;
}

// Files delete family: Recycle sends local paths to the bin, Delete removes
// through the certified writers on raw targets (trees depth-first).
bool FileExplorerTransferWorker::deleteOne(const FileExplorerTransferItem& item) {
    if (m_request.kind == FileExplorerTransferKind::Recycle) {
        if (sendPathToRecycleBin(item.source_path)) {
            return true;
        }
        m_blockers.append(
            QStringLiteral("Could not move %1 to the Recycle Bin.").arg(item.source_path));
        return false;
    }
    const auto result =
        item.directory
            ? FileManagementFileSystemBridge::deleteDirectoryTree(m_request.source_target,
                                                                  item.source_path)
            : FileManagementFileSystemBridge::deleteFile(m_request.source_target, item.source_path);
    if (!result.ok) {
        m_blockers.append(result.blockers);
        return false;
    }
    m_warnings.append(result.warnings);
    return true;
}

}  // namespace sak
