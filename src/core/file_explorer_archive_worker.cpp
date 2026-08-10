// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_archive_worker.h"

#include "sak/file_explorer_archive_service.h"

#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QTemporaryDir>

#include <algorithm>
#include <utility>

namespace sak {

namespace {

// Matches the panel's listing bound for the collision probe.
constexpr int kArchiveListMaxEntries = 10'000;

// A name that must land as a SINGLE child of the destination folder. QDir::filePath() returns an
// ABSOLUTE name unchanged and a ".." component walks out of the folder, so an unvalidated name --
// a request field, or an entry name read back out of an archive through a foreign filesystem --
// would place the zip, the wrap folder or an extracted entry outside the destination the user
// chose. A colon is a drive letter or an NTFS alternate data stream.
bool isSafeChildName(const QString& name) {
    if (name.isEmpty() || name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    if (QDir::isAbsolutePath(name)) {
        return false;
    }
    for (const QChar ch : name) {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

}  // namespace

FileExplorerArchiveWorker::FileExplorerArchiveWorker(FileExplorerArchiveRequest request,
                                                     QObject* parent)
    : WorkerBase(parent), m_request(std::move(request)) {}

// Every field screened here previously had a working default: a blank directory makes
// childPath()/QDir resolve against the PROCESS working directory (clobbering a same-named file
// next to the executable), an unsafe zip name escapes the destination folder, and an empty
// payload list ran to completion as a zero-work success with no blocker to say so.
bool FileExplorerArchiveWorker::requestIsWellFormed() {
    if (m_request.directory.isEmpty()) {
        m_blockers.append(
            QStringLiteral("No destination folder was given for this archive batch."));
        return false;
    }
    if (m_request.compress) {
        if (!isSafeChildName(m_request.zip_name)) {
            m_blockers.append(QStringLiteral("Refusing to write the archive: %1 is not a valid "
                                             "name inside the destination folder.")
                                  .arg(m_request.zip_name));
            return false;
        }
        if (m_request.sources.isEmpty()) {
            m_blockers.append(QStringLiteral("Nothing was selected to compress."));
            return false;
        }
        return true;
    }
    if (m_request.archives.isEmpty()) {
        m_blockers.append(QStringLiteral("No archive was selected to extract."));
        return false;
    }
    return true;
}

auto FileExplorerArchiveWorker::execute() -> std::expected<void, sak::error_code> {
    // Blockers are this class's failure channel (the panel builds the terminal card from
    // blockers().isEmpty()), so a refused request reports through them and does no work.
    if (!requestIsWellFormed()) {
        return {};
    }
    if (m_request.compress) {
        runCompress();
    } else {
        runExtract();
    }
    return {};
}

// Files CompressArchiveModels: pack on a background thread, then (raw
// targets) import the finished zip through the certified writer.
void FileExplorerArchiveWorker::runCompress() {
    const bool local = m_request.target.local_file_system;
    QTemporaryDir staging;
    if (!local && !staging.isValid()) {
        m_blockers.append(QStringLiteral("Could not create a staging folder for the archive."));
        return;
    }
    const QStringList host_sources = collectValidatedSources(staging.path());
    if (host_sources.isEmpty() || checkStop()) {
        return;
    }
    const QString zip_host = local ? childPath(m_request.zip_name)
                                   : QDir(staging.path()).filePath(m_request.zip_name);
    const auto result = FileExplorerArchiveService::compressToZip(zip_host, host_sources);
    m_blockers.append(result.blockers);
    m_warnings.append(result.warnings);
    m_zip_entries = result.entries;
    if (!result.ok) {
        return;
    }
    // A cancel that arrived during the (uncancellable) codec run must not still trigger the raw
    // mutation that follows it: poll the stop state before writing the finished zip to the target.
    if (checkStop()) {
        return;
    }
    if (!local && !FileManagementFileSystemBridge::writeFileFromHostPath(
                       m_request.target, childPath(m_request.zip_name), zip_host)
                       .ok) {
        m_blockers.append(
            QStringLiteral("Could not write %1 to the target.").arg(m_request.zip_name));
        return;
    }
    m_completed = 1;
}

// Stage all sources; fail closed if any could not be prepared. A raw selection
// that fails to stage out is dropped by collectCompressSources, leaving fewer
// host sources than were requested -- compressing that subset would ship an
// archive silently missing a requested item as a success (B8-21).
QStringList FileExplorerArchiveWorker::collectValidatedSources(const QString& staging_dir) {
    const QStringList host_sources = collectCompressSources(staging_dir);
    if (checkStop() || host_sources.size() == m_request.sources.size()) {
        return host_sources;
    }
    if (m_blockers.isEmpty()) {
        m_blockers.append(
            QStringLiteral("Could not prepare every selected item; the archive was not written."));
    }
    return {};
}

// Host-side source list: local selections pass through, raw selections stage
// out through the certified readers first.
QStringList FileExplorerArchiveWorker::collectCompressSources(const QString& staging_dir) {
    QStringList host_sources;
    // Raw sources stage into ONE shared host folder by basename, and the zip codec names each
    // entry by that basename. Two selections whose basenames collide -- case-insensitively,
    // because the host staging folder is case-insensitive, so case-distinct siblings from a
    // case-sensitive APFS/HFSX/ext source collapse together -- would clobber each other in staging
    // and pack a duplicate-named entry, silently dropping one requested item while the cardinality
    // check still passes. Refuse the whole batch instead (fail closed) rather than lose content.
    QSet<QString> staged_basenames;
    for (const FileExplorerTransferItem& item : m_request.sources) {
        if (checkStop()) {
            return host_sources;
        }
        if (m_request.target.local_file_system) {
            host_sources.append(item.source_path);
            continue;
        }
        const QString basename = QFileInfo(item.source_path).fileName();
        if (staged_basenames.contains(basename.toLower())) {
            m_blockers.append(
                QStringLiteral("Refusing to compress: more than one selected item resolves to the "
                               "name %1 inside the archive.")
                    .arg(basename));
            return {};
        }
        staged_basenames.insert(basename.toLower());
        const QString staged = stageSource(item, staging_dir);
        if (!staged.isEmpty()) {
            host_sources.append(staged);
        }
    }
    return host_sources;
}

void FileExplorerArchiveWorker::runExtract() {
    QTemporaryDir staging;
    // A raw target stages the archive/output through this temp dir; an invalid
    // temp dir would make QDir(staging.path()) resolve relative to the process
    // working directory and clobber a same-named host file (mirrors runCompress).
    if (!m_request.target.local_file_system && !staging.isValid()) {
        m_blockers.append(QStringLiteral("Could not create a staging folder for extraction."));
        return;
    }
    for (const FileExplorerArchiveExtractItem& archive : m_request.archives) {
        if (checkStop()) {
            return;
        }
        if (extractOne(archive, staging.path())) {
            ++m_completed;
        }
    }
}

bool FileExplorerArchiveWorker::extractOne(const FileExplorerArchiveExtractItem& archive,
                                           const QString& staging_dir) {
    const bool local = m_request.target.local_file_system;
    const QString host_zip =
        local
            ? archive.source_path
            : stageSource({archive.source_path, QString(), archive.size_bytes, false}, staging_dir);
    if (host_zip.isEmpty()) {
        return false;
    }
    if (!archive.dialog_destination.isEmpty()) {
        return extractToDialogDestination(archive, host_zip);
    }
    const QString stem = QFileInfo(archive.name).completeBaseName();
    const bool wrap = wrapsInStemFolder(host_zip);
    // The wrap folder is named from the archive's own display name, which on a raw target comes
    // off a foreign filesystem. A name that does not yield a plain child ("..", a separator, an
    // extension-only name whose stem is empty) would wrap outside the destination folder.
    if (wrap && !isSafeChildName(stem)) {
        m_blockers.append(QStringLiteral("Refusing to extract %1: its name does not yield a valid "
                                         "folder name inside the destination.")
                              .arg(archive.name));
        return false;
    }
    const QString wrap_name = wrap ? stem : QString();
    return local ? extractLocalArchive(host_zip, wrap_name)
                 : extractRawArchive(host_zip, wrap_name);
}

// Files DecompressArchiveDialog leg: the chosen host folder is final, so it has to actually be
// one. A relative destination would resolve against the PROCESS working directory instead of the
// folder the chooser returned, so refuse it rather than extracting somewhere else.
bool FileExplorerArchiveWorker::extractToDialogDestination(
    const FileExplorerArchiveExtractItem& archive, const QString& host_zip) {
    if (!QDir::isAbsolutePath(archive.dialog_destination) ||
        !QFileInfo(archive.dialog_destination).isDir()) {
        m_blockers.append(QStringLiteral("Refusing to extract %1: %2 is not an existing "
                                         "destination folder.")
                              .arg(archive.name, archive.dialog_destination));
        return false;
    }
    const auto result = FileExplorerArchiveService::extractZip(host_zip,
                                                               archive.dialog_destination);
    m_blockers.append(result.blockers);
    m_warnings.append(result.warnings);
    return result.ok;
}

// Files smart rule: a single top-level folder extracts in place (no
// redundant wrapper); anything else wraps in "{archive stem}".
bool FileExplorerArchiveWorker::wrapsInStemFolder(const QString& host_zip) const {
    return m_request.wrap_mode == FileExplorerArchiveWrapMode::Wrap ||
           (m_request.wrap_mode == FileExplorerArchiveWrapMode::Smart &&
            !FileExplorerArchiveService::hasSingleTopLevelRoot(host_zip, nullptr));
}

// Local destination: the extractor writes straight into the chosen folder. A wrap folder has to
// have an unused name resolved first -- childPath("") is the destination itself, so extracting
// there would unpack over the current folder instead of into a child of it.
bool FileExplorerArchiveWorker::extractLocalArchive(const QString& host_zip,
                                                    const QString& wrap_name) {
    QString destination = m_request.directory;
    if (!wrap_name.isEmpty()) {
        const QString child = availableChildName(wrap_name);
        if (child.isEmpty()) {
            m_blockers.append(QStringLiteral("Could not find an unused name for %1; all numbered "
                                             "variants are in use.")
                                  .arg(wrap_name));
            return false;
        }
        destination = childPath(child);
    }
    const auto result = FileExplorerArchiveService::extractZip(host_zip, destination);
    m_blockers.append(result.blockers);
    m_warnings.append(result.warnings);
    return result.ok;
}

// Raw destination: extract to a scratch folder, then import through the
// certified writers.
bool FileExplorerArchiveWorker::extractRawArchive(const QString& host_zip,
                                                  const QString& wrap_name) {
    QTemporaryDir out;
    if (!out.isValid()) {
        m_blockers.append(QStringLiteral("Could not create a staging folder for the extraction."));
        return false;
    }
    const auto result = FileExplorerArchiveService::extractZip(host_zip, out.path());
    m_blockers.append(result.blockers);
    m_warnings.append(result.warnings);
    // A cancel during the (uncancellable) extract must not still drive the raw import that
    // follows: poll the stop state before delivering the tree onto the target.
    if (!result.ok || checkStop()) {
        return false;
    }
    return deliverTree(out.path(), wrap_name);
}

bool FileExplorerArchiveWorker::deliverTree(const QString& host_out_dir, const QString& wrap_name) {
    if (!wrap_name.isEmpty()) {
        const QString child = availableChildName(wrap_name);
        if (child.isEmpty()) {
            m_blockers.append(
                QStringLiteral("Could not find an unused name for %1; all numbered variants "
                               "are in use.")
                    .arg(wrap_name));
            return false;
        }
        const QString destination = childPath(child);
        const auto result = FileManagementFileSystemBridge::importDirectoryFromHost(
            m_request.target, host_out_dir, destination);
        m_blockers.append(result.blockers);
        m_warnings.append(result.warnings);
        return result.ok;
    }
    return deliverFlattened(host_out_dir);
}

// Flatten: import each extracted top-level entry into the current folder.
bool FileExplorerArchiveWorker::deliverFlattened(const QString& host_out_dir) {
    bool all_ok = true;
    const QFileInfoList infos =
        QDir(host_out_dir).entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& info : infos) {
        // A cancel must stop further raw mutation: each entry is a separate certified write.
        if (checkStop()) {
            return false;
        }
        // The entry names come out of an attacker-supplied archive. Anything that is not a plain
        // child name would be delivered outside the current folder by childPath().
        if (!isSafeChildName(info.fileName())) {
            m_blockers.append(
                QStringLiteral("Refusing to deliver %1: it is not a valid name inside the "
                               "destination folder.")
                    .arg(info.fileName()));
            all_ok = false;
            continue;
        }
        const QString name = availableChildName(info.fileName());
        if (name.isEmpty()) {
            // Every numbered variant is taken (or the destination listing failed):
            // childPath("") would resolve to the directory itself, so the transfer
            // would target the folder instead of a child. Skip and fail closed.
            m_blockers.append(
                QStringLiteral("Could not find an unused name for %1; it was not delivered.")
                    .arg(info.fileName()));
            all_ok = false;
            continue;
        }
        FileExplorerTransferEngine engine(FileManagementFileSystemBridge::localTarget(QString()),
                                          m_request.target,
                                          m_request.raw_read_cap);
        const bool delivered =
            engine.transferEntry({info.absoluteFilePath(),
                                  childPath(name),
                                  static_cast<quint64>(std::max<qint64>(info.size(), 0)),
                                  info.isDir()});
        // A copy that landed but dropped entries (skipped links, depth/entry-cap overflow) is not
        // a delivered entry: fail closed rather than counting a partial extraction as delivered.
        if (!delivered || !engine.lastTransferComplete()) {
            all_ok = false;
        }
        m_blockers.append(engine.blockers());
        m_warnings.append(engine.warnings());
    }
    return all_ok;
}

// Stages one raw-target entry out to the host scratch folder through the
// certified readers (mirrors the panel's synchronous stageEntryToHost).
QString FileExplorerArchiveWorker::stageSource(const FileExplorerTransferItem& item,
                                               const QString& staging_dir) {
    const QString staged = QDir(staging_dir).filePath(QFileInfo(item.source_path).fileName());
    FileExplorerTransferEngine engine(m_request.target,
                                      FileManagementFileSystemBridge::localTarget(QString()),
                                      m_request.raw_read_cap);
    const bool ok =
        engine.transferEntry({item.source_path, staged, item.size_bytes, item.directory});
    m_blockers.append(engine.blockers());
    m_warnings.append(engine.warnings());
    // A copy that landed but dropped entries (depth/entry caps, skipped links) is not complete
    // staging: compressing or extracting that subset would treat missing content as a success,
    // so treat an incomplete transfer as a staging failure (fail closed).
    return (ok && engine.lastTransferComplete()) ? staged : QString();
}

// The name itself when free; otherwise the Files incremental "{name} (n)"
// (FileOperationsHelpers.GetIncrementalName, starting at 2).
QString FileExplorerArchiveWorker::availableChildName(const QString& name) const {
    if (!destinationOccupied(name)) {
        return name;
    }
    const qsizetype last_dot = name.lastIndexOf(QLatin1Char('.'));
    const QString base = last_dot > 0 ? name.left(last_dot) : name;
    const QString extension = last_dot > 0 ? name.mid(last_dot) : QString();
    for (int index = 2; index < 10'000; ++index) {
        const QString candidate = QStringLiteral("%1 (%2)%3").arg(base).arg(index).arg(extension);
        if (!destinationOccupied(candidate)) {
            return candidate;
        }
    }
    // Every "{name} (2..9999)" variant is occupied: signal exhaustion instead of
    // returning an occupied name, which the caller would extract into/over.
    return QString();
}

bool FileExplorerArchiveWorker::destinationOccupied(const QString& name) const {
    if (m_request.target.local_file_system) {
        return QFileInfo::exists(QDir(m_request.directory).filePath(name));
    }
    // Ask for one past the cap so a directory larger than the cap is DETECTABLE: listDirectory
    // reports truncation only by returning more than the requested count, not through ok.
    const auto listing = FileManagementFileSystemBridge::listDirectory(m_request.target,
                                                                       m_request.directory,
                                                                       kArchiveListMaxEntries + 1);
    if (!listing.ok || listing.entries.size() > kArchiveListMaxEntries) {
        // The listing failed OR was truncated, so we cannot prove the name is free (an occupant
        // could sit beyond the cap). Treat it as occupied: availableChildName then tries other
        // names and ultimately fails closed rather than delivering into an unverified destination.
        return true;
    }
    return std::any_of(listing.entries.cbegin(),
                       listing.entries.cend(),
                       [&name](const FileManagementEntry& entry) { return entry.name == name; });
}

QString FileExplorerArchiveWorker::childPath(const QString& name) const {
    if (m_request.target.local_file_system) {
        return QDir(m_request.directory).filePath(name);
    }
    return m_request.directory.endsWith(QLatin1Char('/'))
               ? m_request.directory + name
               : m_request.directory + QLatin1Char('/') + name;
}

}  // namespace sak
