// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_transfer_worker.h"

#include "sak/recycle_bin.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QRandomGenerator>
#include <QTemporaryDir>

#include <filesystem>
#include <system_error>
#include <utility>

namespace sak {

namespace {

// Matches the bridge's recursive-walk depth bound.
constexpr int kDiscoveryMaxDepth = 32;
constexpr int kDiscoveryMaxEntriesPerDirectory = 10'000;

// How much of the destination's own name a staging/backup sibling keeps. The
// prefix and the entropy suffix are added on top, so trimming the readable part
// keeps the whole component inside the 255-character file-name limit even for a
// destination whose name already sits near it.
constexpr qsizetype kSiblingTempNameChars = 64;

QString transferItemName(const QString& path) {
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? clean : clean.mid(slash + 1);
}

struct SplitTargetPath {
    QString parent;
    QString name;
};

// Split a destination path into its parent directory and last component. Both
// separators are accepted because a target path can be host-native or the
// "/"-separated form the raw file-system readers use.
SplitTargetPath splitTargetPath(const QString& path) {
    QString clean = path;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    const qsizetype slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash < 0) {
        return {QString(), clean};
    }
    return {clean.left(slash), clean.mid(slash + 1)};
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

void FileExplorerTransferEngine::noteIncompleteTransfer(const bool sanctioned) {
    m_last_transfer_incomplete = true;
    if (!sanctioned) {
        m_last_transfer_unrequested_loss = true;
    }
}

bool FileExplorerTransferEngine::transferEntry(const FileExplorerTransferItem& item,
                                               const FileManagementTransferObserver& observer) {
    m_last_transfer_incomplete = false;
    m_last_transfer_unrequested_loss = false;
    // A Replace copies into a sibling temp and swaps it in only after it lands whole,
    // so a copy failure never destroys the original (B8-06). A non-Replace writes to
    // the final path directly (the backends atomically overwrite their own temp).
    if (item.replace_destination) {
        return transferReplacing(item, observer);
    }
    return transferItemTo(item, item.destination_path, observer);
}

bool FileExplorerTransferEngine::transferItemTo(const FileExplorerTransferItem& item,
                                                const QString& destination,
                                                const FileManagementTransferObserver& observer) {
    if (m_source_target.local_file_system) {
        return transferFromHost(item, destination, observer);
    }
    if (m_destination_target.local_file_system) {
        return transferRawToLocal(item, destination, observer);
    }
    return transferRawStaged(item, destination, observer);
}

QString FileExplorerTransferEngine::siblingTempPath(const QString& destination_path,
                                                    const QString& prefix) {
    const SplitTargetPath split = splitTargetPath(destination_path);
    // 64 bits of system entropy on top of the per-engine sequence. The bare
    // sequence number alone made the name predictable, so a co-located writer
    // could plant content where the staged copy merges into it, or where the
    // cleanup then deletes it; a guessed name is now impractical.
    const QString token =
        QStringLiteral("%1-%2")
            .arg(m_replace_seq++)
            .arg(QRandomGenerator::system()->generate64(), 16, 16, QLatin1Char('0'));
    const QString temp =
        QStringLiteral("%1%2.%3").arg(prefix, split.name.left(kSiblingTempNameChars), token);
    return split.parent.isEmpty() ? temp : split.parent + QLatin1Char('/') + temp;
}

bool FileExplorerTransferEngine::destinationPathVacant(const QString& path) {
    if (m_destination_target.local_file_system) {
        // symlink_status, not QFileInfo::exists: QFileInfo also reports "missing"
        // when the stat itself failed (a permission-denied ancestor), and only a
        // proven not_found may be treated as vacant (the removeExistingEntry
        // idiom). Anything undecidable fails closed.
        std::error_code ec;
        const auto status =
            std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()), ec);
        return status.type() == std::filesystem::file_type::not_found;
    }
    // Raw targets expose no stat primitive; list the parent one entry past the
    // bound so a truncated listing is detectable and refused.
    const SplitTargetPath split = splitTargetPath(path);
    const FileManagementListResult listing = FileManagementFileSystemBridge::listDirectory(
        m_destination_target, split.parent, kDiscoveryMaxEntriesPerDirectory + 1);
    if (!listing.ok || listing.entries.size() > kDiscoveryMaxEntriesPerDirectory) {
        return false;
    }
    for (const FileManagementEntry& entry : listing.entries) {
        // Case-insensitively, as removeExistingEntry matches: a differing-case
        // occupant may well be the same entry on the destination volume.
        if (QString::compare(entry.name, split.name, Qt::CaseInsensitive) == 0) {
            return false;
        }
    }
    return true;
}

bool FileExplorerTransferEngine::reserveStagingPath(const QString& path, const bool directory) {
    if (!m_destination_target.local_file_system) {
        // The raw backends have no exclusive-create primitive; the bounded probe
        // plus an unguessable name is the strongest claim available there.
        return destinationPathVacant(path);
    }
    if (directory) {
        // mkdir, NOT mkpath: mkpath succeeds on an existing directory, which is
        // exactly the silent merge this reservation has to refuse.
        return QDir().mkdir(path);
    }
    QFile placeholder(path);
    // NewOnly is an exclusive create: it fails when anything already occupies the
    // name, including a symlink planted to redirect the staged write elsewhere.
    if (!placeholder.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        return false;
    }
    placeholder.close();
    return true;
}

bool FileExplorerTransferEngine::removeDestinationEntry(const QString& path) {
    // Resolve the entry's kind (file/dir/symlink) at execution time -- a staged copy
    // matches the source's kind, but a set-aside backup matches the OLD occupant's,
    // which the item does not describe.
    return FileManagementFileSystemBridge::removeExistingEntry(m_destination_target,
                                                               path,
                                                               kDiscoveryMaxEntriesPerDirectory)
        .ok;
}

bool FileExplorerTransferEngine::renameDestination(const QString& from, const QString& to) {
    return FileManagementFileSystemBridge::renameEntry(m_destination_target, from, to).ok;
}

bool FileExplorerTransferEngine::transferReplacing(const FileExplorerTransferItem& item,
                                                   const FileManagementTransferObserver& observer) {
    // 1. Stage the whole copy beside the destination, on a name this engine
    //    claims exclusively so the copy can never merge into (or the cleanup
    //    delete) content that was already sitting there. A failure here leaves
    //    the original completely untouched.
    const QString staged = siblingTempPath(item.destination_path, QStringLiteral(".sak-stage-"));
    if (!reserveStagingPath(staged, item.directory)) {
        m_blockers.append(QStringLiteral("Could not claim a staging name beside %1, so nothing "
                                         "was replaced.")
                              .arg(transferItemName(item.destination_path)));
        return false;
    }
    if (!transferItemTo(item, staged, observer)) {
        removeDestinationEntry(staged);  // best-effort partial cleanup
        return false;
    }
    // A directory copy can report ok while still being INCOMPLETE: depth or entry limits,
    // skipped reparse points and capped reads each set m_last_transfer_incomplete without
    // failing the copy. Swapping such a tree over the destination and then deleting the
    // set-aside original (step 4) would destroy complete data and replace it with a subset.
    // Replace therefore requires wholeness, not merely success, before it lands.
    if (m_last_transfer_incomplete) {
        m_blockers.append(QStringLiteral("The replacement copy of %1 is incomplete, so it was "
                                         "not swapped in; the original is unchanged.")
                              .arg(transferItemName(item.destination_path)));
        removeDestinationEntry(staged);
        return false;
    }
    // 2. Try to move the current occupant aside. Success means there WAS an occupant
    //    and we hold a rollback copy; failure means the path was vacant (occupant
    //    vanished since collision-resolve) OR the occupant is stuck -- both are handled
    //    by step 3, which only lands on a vacant final name.
    const QString backup = siblingTempPath(item.destination_path, QStringLiteral(".sak-old-"));
    if (!destinationPathVacant(backup)) {
        // The set-aside name cannot be pre-created (the rename needs it vacant), so
        // it is proven vacant instead: moving the original onto occupied ground
        // would put the rollback copy on top of someone else's data.
        m_blockers.append(QStringLiteral("Could not claim a set-aside name beside %1, so the "
                                         "original was left in place.")
                              .arg(transferItemName(item.destination_path)));
        removeDestinationEntry(staged);
        return false;
    }
    const bool occupant_set_aside = renameDestination(item.destination_path, backup);
    // 3. Move the staged copy into place. If it fails, restore the original (when one
    //    was set aside) so a stuck occupant never costs the original.
    if (!renameDestination(staged, item.destination_path)) {
        if (occupant_set_aside && !renameDestination(backup, item.destination_path)) {
            m_blockers.append(QStringLiteral("Could not move the replacement into place for %1 AND "
                                             "could not restore the original; it is kept at %2.")
                                  .arg(transferItemName(item.destination_path), backup));
            return false;
        }
        m_blockers.append(QStringLiteral("Could not move the replacement into place for %1; the "
                                         "original was left intact.")
                              .arg(transferItemName(item.destination_path)));
        removeDestinationEntry(staged);
        return false;
    }
    // 4. Success: drop the set-aside original, if any (best-effort -- the replace is done). If the
    //    removal fails, do not report success silently: the previous version's data is left behind
    //    at an internal name, so surface it as a warning rather than swallowing the error.
    if (occupant_set_aside && !removeDestinationEntry(backup)) {
        m_warnings.append(QStringLiteral("Replaced %1, but the previous version could not be "
                                         "removed and remains at %2.")
                              .arg(transferItemName(item.destination_path), backup));
    }
    return true;
}

bool FileExplorerTransferEngine::isSameTargetEntry(const FileExplorerTransferItem& item) {
    const SplitTargetPath src = splitTargetPath(item.source_path);
    const SplitTargetPath dst = splitTargetPath(item.destination_path);
    return QString::compare(src.parent, dst.parent, Qt::CaseInsensitive) == 0 &&
           QString::compare(src.name, dst.name, Qt::CaseInsensitive) == 0;
}

bool FileExplorerTransferEngine::reportFailedRename(const FileManagementMutationResult& result,
                                                    const FileExplorerTransferItem& item,
                                                    const bool occupant_set_aside,
                                                    const QString& backup) {
    if (occupant_set_aside && !renameDestination(backup, item.destination_path)) {
        m_blockers.append(QStringLiteral("Could not rename onto %1 AND could not restore the "
                                         "original occupant; it is kept at %2.")
                              .arg(transferItemName(item.destination_path), backup));
        return false;
    }
    m_blockers.append(result.blockers);
    return false;
}

void FileExplorerTransferEngine::dropSetAsideRenameOccupant(const FileExplorerTransferItem& item,
                                                            const bool occupant_set_aside,
                                                            const QString& backup) {
    // Success: drop the set-aside occupant (best-effort -- the rename is done). Surface a
    // failure to remove it rather than swallow it, so leftover data is never silent.
    if (occupant_set_aside && !removeDestinationEntry(backup)) {
        m_warnings.append(QStringLiteral("Renamed onto %1, but the previous occupant could not be "
                                         "removed and remains at %2.")
                              .arg(transferItemName(item.destination_path), backup));
    }
}

bool FileExplorerTransferEngine::renameWithinTarget(const FileExplorerTransferItem& item) {
    // Same-entry rename (an exact spelling, or a case-only difference on a
    // case-insensitive volume where the destination resolves to the SAME file as the
    // source): deleting a "replaced" occupant here would destroy the source itself, so
    // never replace -- route straight to renameEntry, which performs the on-disk case
    // change directly. If renameEntry cannot (a stuck case-only rename), it fails
    // closed with the source intact rather than losing it.
    const bool same_entry = isSameTargetEntry(item);
    if (same_entry && item.source_path == item.destination_path) {
        return true;  // exact no-op
    }
    // Replace: set the current occupant ASIDE to an unguessable backup name instead of
    // deleting it up front, so a failed rename rolls it back rather than losing it
    // (mirrors transferReplacing). The user authorised "replace D with S", never
    // "destroy D and keep S". A same-entry rename has no true occupant to set aside.
    QString backup;
    bool occupant_set_aside = false;
    if (item.replace_destination && !same_entry) {
        backup = siblingTempPath(item.destination_path, QStringLiteral(".sak-old-"));
        if (!destinationPathVacant(backup)) {
            m_blockers.append(QStringLiteral("Could not claim a set-aside name beside %1, so it "
                                             "was left in place.")
                                  .arg(transferItemName(item.destination_path)));
            return false;
        }
        // A failed set-aside means the path was vacant (occupant vanished) or the
        // occupant is stuck; the rename below only lands on a vacant final name, so
        // either way nothing has been destroyed.
        occupant_set_aside = renameDestination(item.destination_path, backup);
    }
    const auto result = FileManagementFileSystemBridge::renameEntry(m_destination_target,
                                                                    item.source_path,
                                                                    item.destination_path);
    if (!result.ok) {
        return reportFailedRename(result, item, occupant_set_aside, backup);
    }
    dropSetAsideRenameOccupant(item, occupant_set_aside, backup);
    return true;
}

bool FileExplorerTransferEngine::removeReplacedDestination(const FileExplorerTransferItem& item) {
    if (!item.replace_destination) {
        return true;
    }
    // The Replace delete is deferred from collision resolution to right before
    // this item's own copy: a cancel or failure earlier in the batch must not
    // cost destinations that were never rewritten.
    const auto removed = FileManagementFileSystemBridge::removeExistingEntry(
        m_destination_target, item.destination_path, kDiscoveryMaxEntriesPerDirectory);
    if (!removed.ok) {
        m_blockers.append(QStringLiteral("Could not replace %1: %2")
                              .arg(transferItemName(item.destination_path),
                                   removed.blockers.join(QStringLiteral("; "))));
    }
    return removed.ok;
}

bool FileExplorerTransferEngine::deleteMovedSource(const FileExplorerTransferItem& item) {
    // Defense in depth: never remove a moved source unless the last copy landed whole. Both
    // callers already gate on lastTransferComplete() before reaching here, but a source delete is
    // irreversible, so refuse fail-closed rather than trust the precondition was checked.
    if (m_last_transfer_incomplete) {
        m_blockers.append(QStringLiteral("Kept %1: the moved copy did not land whole, so the "
                                         "source was not removed.")
                              .arg(item.source_path));
        return false;
    }
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
        if (!result.complete) {
            // A host source cannot be capped, so an incomplete import is always
            // content the caller never agreed to lose.
            noteIncompleteTransfer(false);
        }
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

bool FileExplorerTransferEngine::transferRawDirectoryToLocal(
    const FileExplorerTransferItem& item,
    const QString& destination,
    const FileManagementTransferObserver& observer) {
    const auto result = FileManagementFileSystemBridge::exportDirectoryToHost(
        m_source_target, item.source_path, destination, m_raw_read_cap, observer);
    m_warnings.append(result.warnings);
    m_blockers.append(result.blockers);
    if (!result.complete) {
        // The only shortfall a caller can sanction up front is a capped read (Copy
        // Out asks for one and the copy is marked capped); dropped symlinks and
        // depth/entry-cap overflow are losses nobody asked for.
        const bool capped_only = result.symlinks_skipped == 0 && result.entries_skipped == 0;
        noteIncompleteTransfer(capped_only && m_allow_capped_raw_reads);
    }
    if (result.ok && result.capped_files > 0 && !m_allow_capped_raw_reads) {
        // A truncated file means the tree did not land whole: report it and
        // fail the item so a move never deletes the intact source.
        m_blockers.append(QStringLiteral("%1 file(s) inside %2 exceed the raw read "
                                         "window; the pasted copy is incomplete.")
                              .arg(result.capped_files)
                              .arg(transferItemName(item.source_path)));
        return false;
    }
    return result.ok;
}

bool FileExplorerTransferEngine::transferRawToLocal(
    const FileExplorerTransferItem& item,
    const QString& destination,
    const FileManagementTransferObserver& observer) {
    if (item.directory) {
        return transferRawDirectoryToLocal(item, destination, observer);
    }
    const QString name = transferItemName(item.source_path);
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
    // A capped single-file read means the copy is truncated; a move must not
    // then delete the whole source. Copy Out asked for the cap and marks it, so
    // only a cap outside those semantics counts as unrequested loss.
    if (result.capped) {
        noteIncompleteTransfer(m_allow_capped_raw_reads);
        if (!m_allow_capped_raw_reads) {
            // Fail the item outright on an UNREQUESTED cap so no caller -- including
            // the synchronous undo/redo path that does not re-check completeness --
            // can treat the truncated copy as whole and delete the source. Mirrors
            // transferRawDirectoryToLocal's capped_files guard for the single-file leg.
            m_blockers.append(QStringLiteral("%1 exceeds the raw read window; the pasted copy is "
                                             "truncated, so it is reported as incomplete.")
                                  .arg(name));
            return false;
        }
    }
    return true;
}

bool FileExplorerTransferEngine::transferRawStaged(const FileExplorerTransferItem& item,
                                                   const QString& destination,
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
    const bool ok = host_leg.transferFromHost(staged_item, destination, observer);
    m_warnings.append(host_leg.warnings());
    m_blockers.append(host_leg.blockers());
    // Either leg dropping entries leaves the raw-to-raw copy incomplete. The host
    // leg reads an uncapped host staging tree, so anything it dropped is loss the
    // caller never asked for.
    if (!host_leg.lastTransferComplete()) {
        noteIncompleteTransfer(false);
    }
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
        // Terminal even though nothing was transferred: without an explicit status
        // the card never leaves InProgress.
        reporter.setStatus(FileExplorerReturnResult::Cancelled);
        reporter.flushReport();
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
    } else {
        // A clean run is terminal and must say so explicitly. The reporter's
        // auto-success needs a non-zero size denominator, which the delete,
        // recycle and rename families (and a batch of zero-byte files) never
        // have -- without this the card spins as InProgress forever and the
        // completed-item cleanup never reaps it.
        reporter.setStatus(FileExplorerReturnResult::Success);
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
    // Sum the tree's BYTES for the size denominator, but do NOT add its entries
    // to the item count: transferItems advances the item counter once per
    // top-level selection, so counting nested entries here would inflate the
    // denominator past what the numerator can ever reach (B8-24).
    QDirIterator it(path,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        if (checkStop()) {
            return;
        }
        it.next();
        const QFileInfo info = it.fileInfo();
        if (info.isFile() && !info.isSymLink()) {
            m_discovered_bytes += info.size();
        }
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
        // Bytes-only, as in discoverHostTree: nested entries feed the size
        // denominator, not the item count that reaches its total per top-level
        // selection (B8-24).
        if (entry.directory) {
            discoverRawTree(entry.path, depth + 1, reporter);
        } else if (entry.regular_file) {
            m_discovered_bytes += static_cast<qint64>(entry.size_bytes);
        }
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
    // Fail closed on a malformed request item before any destructive leg runs. A batch item can
    // carry AI-planned or panel-supplied paths; an empty source (or, for a copy/move/rename, an
    // empty destination) must never be coerced into a delete/copy against a namespace root.
    if (item.source_path.isEmpty()) {
        m_blockers.append(QStringLiteral("Refusing a transfer item with no source path."));
        return false;
    }
    if (m_request.kind != FileExplorerTransferKind::Transfer) {
        return deleteOne(item);
    }
    if (item.destination_path.isEmpty()) {
        m_blockers.append(QStringLiteral("Refusing to transfer %1: it has no destination path.")
                              .arg(item.source_path));
        return false;
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
    // completedItems() promises the item LANDED WHOLE, so a copy that silently
    // dropped content (skipped symlinks, depth/entry-cap overflow, or a
    // truncation outside the explicitly capped Copy Out semantics) is reported
    // as failed instead of being listed as complete.
    if (!engine->lastTransferLandedAsRequested()) {
        m_blockers.append(QStringLiteral("%1 did not copy whole; the partial copy at %2 is not "
                                         "reported as a completed item.")
                              .arg(item.source_path, item.destination_path));
        return false;
    }
    if (m_request.move) {
        // Never delete the source of a move whose copy did not land whole -- even
        // a cap the caller sanctioned still means the destination holds less than
        // the source: the item stays put and is reported as failed.
        if (!engine->lastTransferComplete()) {
            m_blockers.append(
                QStringLiteral("Kept %1: the moved copy is incomplete, so the source was not "
                               "removed.")
                    .arg(item.source_path));
            return false;
        }
        return engine->deleteMovedSource(item);
    }
    return true;
}

// Files delete family: Recycle sends local paths to the bin, Delete removes
// through the certified writers on raw targets (trees depth-first).
bool FileExplorerTransferWorker::deleteOne(const FileExplorerTransferItem& item) {
    if (m_request.kind == FileExplorerTransferKind::Recycle) {
        // The shell "recycles" by PERMANENTLY deleting on a volume with no bin
        // (network shares, removable and optical media). Refuse there rather than
        // report an unrecoverable delete as a recoverable one: the user asked for
        // the bin, so a permanent delete has to be requested explicitly.
        if (!pathVolumeHasRecycleBin(item.source_path)) {
            m_blockers.append(QStringLiteral("%1 is not on a volume with a Recycle Bin, so it was "
                                             "left in place; use Delete to remove it permanently.")
                                  .arg(item.source_path));
            return false;
        }
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
