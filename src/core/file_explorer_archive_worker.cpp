// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_archive_worker.h"

#include "sak/file_explorer_archive_service.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <utility>

namespace sak {

namespace {

// Matches the panel's listing bound for the collision probe.
constexpr int kArchiveListMaxEntries = 10'000;

}  // namespace

FileExplorerArchiveWorker::FileExplorerArchiveWorker(FileExplorerArchiveRequest request,
                                                     QObject* parent)
    : WorkerBase(parent), m_request(std::move(request)) {}

auto FileExplorerArchiveWorker::execute() -> std::expected<void, sak::error_code> {
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
    const QStringList host_sources = collectCompressSources(staging.path());
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
    if (!local && !FileManagementFileSystemBridge::writeFileFromHostPath(
                       m_request.target, childPath(m_request.zip_name), zip_host)
                       .ok) {
        m_blockers.append(
            QStringLiteral("Could not write %1 to the target.").arg(m_request.zip_name));
        return;
    }
    m_completed = 1;
}

// Host-side source list: local selections pass through, raw selections stage
// out through the certified readers first.
QStringList FileExplorerArchiveWorker::collectCompressSources(const QString& staging_dir) {
    QStringList host_sources;
    for (const FileExplorerTransferItem& item : m_request.sources) {
        if (checkStop()) {
            return host_sources;
        }
        if (m_request.target.local_file_system) {
            host_sources.append(item.source_path);
            continue;
        }
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
    // Files DecompressArchiveDialog leg: the chosen host folder is final.
    if (!archive.dialog_destination.isEmpty()) {
        const auto result = FileExplorerArchiveService::extractZip(host_zip,
                                                                   archive.dialog_destination);
        m_blockers.append(result.blockers);
        m_warnings.append(result.warnings);
        return result.ok;
    }
    // Files smart rule: a single top-level folder extracts in place (no
    // redundant wrapper); anything else wraps in "{archive stem}".
    const QString stem = QFileInfo(archive.name).completeBaseName();
    const bool wrap = m_request.wrap_mode == FileExplorerArchiveWrapMode::Wrap ||
                      (m_request.wrap_mode == FileExplorerArchiveWrapMode::Smart &&
                       !FileExplorerArchiveService::hasSingleTopLevelRoot(host_zip, nullptr));
    if (local) {
        QString destination = m_request.directory;
        if (wrap) {
            const QString child = availableChildName(stem);
            if (child.isEmpty()) {
                m_blockers.append(
                    QStringLiteral("Could not find an unused name for %1; all numbered "
                                   "variants are in use.")
                        .arg(stem));
                return false;
            }
            destination = childPath(child);
        }
        const auto result = FileExplorerArchiveService::extractZip(host_zip, destination);
        m_blockers.append(result.blockers);
        m_warnings.append(result.warnings);
        return result.ok;
    }
    return extractRawArchive(host_zip, wrap ? stem : QString());
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
    return result.ok && deliverTree(out.path(), wrap_name);
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
        const QString name = availableChildName(info.fileName());
        FileExplorerTransferEngine engine(FileManagementFileSystemBridge::localTarget(QString()),
                                          m_request.target,
                                          m_request.raw_read_cap);
        if (!engine.transferEntry({info.absoluteFilePath(),
                                   childPath(name),
                                   static_cast<quint64>(std::max<qint64>(info.size(), 0)),
                                   info.isDir()})) {
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
    return ok ? staged : QString();
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
    const QVector<FileManagementEntry> entries =
        FileManagementFileSystemBridge::listDirectory(m_request.target,
                                                      m_request.directory,
                                                      kArchiveListMaxEntries)
            .entries;
    return std::any_of(entries.cbegin(), entries.cend(), [&name](const FileManagementEntry& entry) {
        return entry.name == name;
    });
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
