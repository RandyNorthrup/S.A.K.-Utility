// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_archive_service.cpp
/// @brief Zip compress/extract engine for the File Management Explorer.

#include "sak/file_explorer_archive_service.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QSet>

#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

namespace sak {

namespace {

// Bounds shared with the bridge directory walks: finite so a hostile tree or
// archive cannot run away.
constexpr int kArchiveMaxEntries = 100'000;
// Extraction resource ceilings so a decompression bomb cannot exhaust the
// destination volume. A hostile zip commonly declares a small compressed size
// but a huge expanded size; extraction fails closed before writing past these.
constexpr qint64 kExtractMaxTotalBytes = 8LL * 1024 * 1024 * 1024;  // 8 GiB expanded
constexpr qint64 kExtractMaxFileBytes = 4LL * 1024 * 1024 * 1024;   // 4 GiB per file

// True when @p entry_name would escape @p destination_dir (zip-slip): an
// absolute path, or a path whose cleaned form leaves the destination root.
bool entryEscapesDestination(const QString& destination_dir, const QString& entry_name) {
    if (QDir::isAbsolutePath(entry_name)) {
        return true;
    }
    const QString candidate = QDir::cleanPath(destination_dir + QLatin1Char('/') + entry_name);
    const QString root = QDir::cleanPath(destination_dir);
    return candidate != root && !candidate.startsWith(root + QLatin1Char('/'));
}

// Materialize one file entry at @p out_path (parent dirs, corrupt-payload
// guard, atomic write). Returns false and records a blocker on failure.
bool writeExtractedFile(const QZipReader& reader,
                        const QString& out_path,
                        const QZipReader::FileInfo& info,
                        FileExplorerArchiveResult* result) {
    if (!QDir().mkpath(QFileInfo(out_path).absolutePath())) {
        result->blockers.append(
            QStringLiteral("Could not create directory for %1.").arg(info.filePath));
        return false;
    }
    const QByteArray data = reader.fileData(info.filePath);
    // A declared-nonzero entry that yields no bytes is a corrupt or lying
    // header; fail closed rather than write a truncated file.
    if (info.size > 0 && data.isEmpty()) {
        result->blockers.append(
            QStringLiteral("Extraction of entry %1 failed (corrupt or unsupported).")
                .arg(info.filePath));
        return false;
    }
    QSaveFile out(out_path);
    if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size() || !out.commit()) {
        result->blockers.append(QStringLiteral("Could not write %1.").arg(out_path));
        return false;
    }
    ++result->entries;
    return true;
}

// Extract one bounded zip entry into @p destination_dir. Returns false (and
// records a blocker) to abort the whole extraction; true continues.
// Directories and skipped symlinks return true without writing a file.
bool extractZipEntry(const QZipReader& reader,
                     const QString& destination_dir,
                     const QZipReader::FileInfo& info,
                     qint64* total_bytes,
                     FileExplorerArchiveResult* result) {
    if (!info.isValid()) {
        return true;
    }
    if (info.isSymLink) {
        result->warnings.append(
            QStringLiteral("Skipped symlink %1 (links are not extracted).").arg(info.filePath));
        return true;
    }
    if (entryEscapesDestination(destination_dir, info.filePath)) {
        result->blockers.append(
            QStringLiteral("Refused entry %1 (path escapes the destination).").arg(info.filePath));
        return false;
    }
    const QString out_path = QDir(destination_dir).filePath(info.filePath);
    if (info.isDir) {
        if (QDir().mkpath(out_path)) {
            return true;
        }
        result->blockers.append(QStringLiteral("Could not create directory %1.").arg(out_path));
        return false;
    }
    *total_bytes += info.size;
    if (info.size > kExtractMaxFileBytes || *total_bytes > kExtractMaxTotalBytes) {
        result->blockers.append(
            QStringLiteral("Entry %1 exceeds the extraction size limit.").arg(info.filePath));
        return false;
    }
    return writeExtractedFile(reader, out_path, info, result);
}

// Add one file to the writer under @p entry_name, streaming from disk.
bool addFileEntry(QZipWriter* writer,
                  const QString& entry_name,
                  const QString& host_path,
                  QStringList* blockers) {
    QFile file(host_path);
    if (!file.open(QIODevice::ReadOnly)) {
        blockers->append(
            QStringLiteral("Could not read %1: %2").arg(host_path, file.errorString()));
        return false;
    }
    writer->addFile(entry_name, &file);
    return true;
}

// Recursively add @p directory under the archive prefix @p prefix.
void addDirectoryEntries(QZipWriter* writer,
                         const QString& prefix,
                         const QString& directory,
                         FileExplorerArchiveResult* result) {
    writer->addDirectory(prefix);
    QDirIterator it(directory,
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden,
                    QDirIterator::Subdirectories);
    const QDir base(directory);
    while (it.hasNext() && result->entries < kArchiveMaxEntries) {
        const QString path = it.next();
        const QFileInfo info = it.fileInfo();
        const QString entry_name = prefix + QLatin1Char('/') + base.relativeFilePath(path);
        if (info.isSymLink()) {
            result->warnings.append(
                QStringLiteral("Skipped symlink %1 (links are not archived).").arg(path));
            continue;
        }
        if (info.isDir()) {
            writer->addDirectory(entry_name);
            continue;
        }
        if (addFileEntry(writer, entry_name, path, &result->blockers)) {
            ++result->entries;
        }
    }
    // Stopping on the entry cap while the tree still has entries would ship a
    // silently partial archive; fail closed so the caller discards it instead
    // of reporting a truncated zip as a successful compress.
    if (it.hasNext()) {
        const QString truncated =
            QStringLiteral(
                "Archive exceeds the %1-entry limit; refusing to write a partial archive.")
                .arg(kArchiveMaxEntries);
        if (!result->blockers.contains(truncated)) {
            result->blockers.append(truncated);
        }
    }
}

// First meaningful path segment of a zip entry name ("a/b/c" -> "a").
QString topLevelSegment(const QString& entry_name) {
    QString clean = entry_name;
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (clean.startsWith(QLatin1Char('/'))) {
        clean.remove(0, 1);
    }
    const int slash = clean.indexOf(QLatin1Char('/'));
    return slash < 0 ? clean : clean.left(slash);
}

// Like topLevelSegment, but empty for the "." / ".." noise entries Files skips.
QString meaningfulTopLevelSegment(const QString& entry_name) {
    const QString segment = topLevelSegment(entry_name);
    if (segment == QStringLiteral(".") || segment == QStringLiteral("..")) {
        return {};
    }
    return segment;
}

}  // namespace

QString FileExplorerArchiveService::archiveBaseName(const QStringList& item_names,
                                                    const QString& parent_name) {
    // Files GenerateArchiveNameFromItems: single item -> its own name (with
    // extension), several items -> the parent folder's name.
    if (item_names.size() == 1) {
        return item_names.first();
    }
    return parent_name.isEmpty() ? QStringLiteral("Archive") : parent_name;
}

bool FileExplorerArchiveService::isZipName(const QString& name) {
    return name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive);
}

FileExplorerArchiveResult FileExplorerArchiveService::compressToZip(
    const QString& zip_path, const QStringList& source_paths) {
    FileExplorerArchiveResult result;
    result.output_path = zip_path;
    QZipWriter writer(zip_path);
    if (writer.status() != QZipWriter::NoError) {
        result.blockers.append(QStringLiteral("Could not create archive %1.").arg(zip_path));
        return result;
    }
    writer.setCompressionPolicy(QZipWriter::AutoCompress);
    for (const QString& source : source_paths) {
        const QFileInfo info(source);
        if (info.isSymLink()) {
            result.warnings.append(
                QStringLiteral("Skipped symlink %1 (links are not archived).").arg(source));
            continue;
        }
        if (info.isDir()) {
            addDirectoryEntries(&writer, info.fileName(), source, &result);
            continue;
        }
        if (!info.isFile()) {
            result.warnings.append(QStringLiteral("Skipped special entry %1.").arg(source));
            continue;
        }
        if (addFileEntry(&writer, info.fileName(), source, &result.blockers)) {
            ++result.entries;
        }
    }
    writer.close();
    if (writer.status() != QZipWriter::NoError) {
        result.blockers.append(QStringLiteral("Could not finalize archive %1.").arg(zip_path));
    }
    result.ok = result.blockers.isEmpty();
    if (!result.ok) {
        QFile::remove(zip_path);
    }
    return result;
}

FileExplorerArchiveResult FileExplorerArchiveService::extractZip(const QString& zip_path,
                                                                 const QString& destination_dir) {
    FileExplorerArchiveResult result;
    result.output_path = destination_dir;
    QZipReader reader(zip_path);
    if (!reader.isReadable()) {
        result.blockers.append(QStringLiteral("Could not open archive %1.").arg(zip_path));
        return result;
    }
    if (!QDir().mkpath(destination_dir)) {
        result.blockers.append(
            QStringLiteral("Could not create destination %1.").arg(destination_dir));
        return result;
    }
    // Bounded per-entry extraction (Qt's extractAll enforces no caps): reject a
    // zip-bomb or zip-slip before writing, and cap entry count / expanded bytes.
    const QList<QZipReader::FileInfo> entries = reader.fileInfoList();
    if (entries.size() > kArchiveMaxEntries) {
        result.blockers.append(QStringLiteral("Archive %1 exceeds the %2-entry limit.")
                                   .arg(zip_path)
                                   .arg(kArchiveMaxEntries));
        return result;
    }
    qint64 total_bytes = 0;
    for (const QZipReader::FileInfo& info : entries) {
        if (!extractZipEntry(reader, destination_dir, info, &total_bytes, &result)) {
            return result;
        }
    }
    result.ok = true;
    return result;
}

bool FileExplorerArchiveService::hasSingleTopLevelRoot(const QString& zip_path,
                                                       QString* root_name) {
    QZipReader reader(zip_path);
    if (!reader.isReadable()) {
        return false;
    }
    QSet<QString> roots;
    // The lone root counts only when it is a folder: a single top-level FILE
    // still needs the wrapper folder (Files GetFirstMeaningfulSegment walk).
    bool root_is_directory = false;
    const auto entries = reader.fileInfoList();
    for (const auto& entry : entries) {
        const QString segment = meaningfulTopLevelSegment(entry.filePath);
        if (segment.isEmpty()) {
            continue;
        }
        roots.insert(segment);
        root_is_directory = root_is_directory || entry.isDir ||
                            entry.filePath.contains(QLatin1Char('/'));
    }
    if (roots.size() != 1 || !root_is_directory) {
        return false;
    }
    if (root_name) {
        *root_name = *roots.cbegin();
    }
    return true;
}

}  // namespace sak
