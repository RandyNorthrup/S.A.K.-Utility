// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_archive_service.cpp
/// @brief Zip compress/extract engine for the File Management Explorer.

#include "sak/file_explorer_archive_service.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>

#include <private/qzipreader_p.h>
#include <private/qzipwriter_p.h>

namespace sak {

namespace {

// Bounds shared with the bridge directory walks: finite so a hostile tree or
// archive cannot run away.
constexpr int kArchiveMaxEntries = 100'000;

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
    if (!reader.extractAll(destination_dir)) {
        result.blockers.append(
            QStringLiteral("Extraction of %1 failed (corrupt or unsupported entry).")
                .arg(zip_path));
        return result;
    }
    result.entries = static_cast<int>(reader.fileInfoList().size());
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
