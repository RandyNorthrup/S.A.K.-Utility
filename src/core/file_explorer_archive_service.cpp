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

#ifdef _WIN32
#include <windows.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots
#endif

namespace sak {

namespace {

// Bounds shared with the bridge directory walks: finite so a hostile tree or
// archive cannot run away.
constexpr int kArchiveMaxEntries = 100'000;
// Ceiling on the central-directory byte size a LISTING will read. fileInfoList() materializes the
// whole central directory (~2-3x its size in transient memory), so this bounds peak allocation
// regardless of the file-size cap. 64 MiB comfortably holds the 100k-entry cap even with long
// names (~46 bytes + name per record) while refusing a hostile multi-GB central directory.
constexpr qint64 kMaxCentralDirBytes = 64LL * 1024 * 1024;
// Extraction resource ceilings so a decompression bomb cannot exhaust the
// destination volume. A hostile zip commonly declares a small compressed size
// but a huge expanded size; extraction fails closed before writing past these.
constexpr qint64 kExtractMaxTotalBytes = 8LL * 1024 * 1024 * 1024;  // 8 GiB expanded
constexpr qint64 kExtractMaxFileBytes = 4LL * 1024 * 1024 * 1024;   // 4 GiB per file

// Byte size of a zip's central directory (from the End-Of-Central-Directory record), or -1 when no
// EOCD is found (i.e. not a zip). QZipReader::isReadable() only confirms the DEVICE opened and
// status() stays NoError on a garbage file, so a non-zip would otherwise list as a fake "0
// entries"; and reading the whole central directory (fileInfoList) allocates ~2-3x its size, so the
// caller must bound this BEFORE listing -- a hostile zip can pack a multi-GB central directory of
// maximal-length names under a file-size cap. The EOCD is within the last 64 KiB (max comment) + 22
// bytes; scan that bounded tail (which also accepts a zip with a prepended stub). A ZIP64 archive
// stores 0xFFFFFFFF here (the real value lives in the ZIP64 EOCD); that reads as ~4 GiB and is
// correctly refused by the caller's ceiling -- fail closed on the oversize case.
qint64 zipCentralDirectorySize(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return -1;
    }
    const qint64 size = file.size();
    const qint64 tail = qMin<qint64>(size, (64LL * 1024) + 22);
    if (tail < 22) {  // too small to hold an EOCD record
        return -1;
    }
    if (!file.seek(size - tail)) {
        return -1;
    }
    const QByteArray buf = file.read(tail);
    const int eocd = buf.lastIndexOf(QByteArrayLiteral("PK\x05\x06"));
    if (eocd < 0 || eocd + 16 > buf.size()) {
        return -1;
    }
    // Central-directory size is a little-endian uint32 at EOCD offset +12.
    const auto byte_at = [&](int i) {
        return static_cast<quint32>(static_cast<quint8>(buf.at(eocd + i)));
    };
    return static_cast<qint64>(byte_at(12) | (byte_at(13) << 8) | (byte_at(14) << 16) |
                               (byte_at(15) << 24));
}

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

// True when @p path is an existing reparse point (symlink or junction). On
// Windows the native attribute covers both; elsewhere a symlink is equivalent.
bool isReparsePoint(const QString& path) {
#ifdef _WIN32
    const DWORD attrs = GetFileAttributesW(path.toStdWString().c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
#else
    return QFileInfo(path).isSymLink();
#endif
}

// True when any parent directory of @p out_path between @p destination_dir and
// the leaf is an existing reparse point. The lexical containment check trusts
// the path text, but a pre-planted junction (e.g. destination\sub pointing at an
// unrelated system folder) would redirect the write outside the destination even
// though the text stays under it, so each descended component is checked.
bool traversesReparsePoint(const QString& destination_dir, const QString& out_path) {
    const QString root = QDir::cleanPath(destination_dir);
    const QString full = QDir::cleanPath(out_path);
    if (!full.startsWith(root + QLatin1Char('/'))) {
        return false;
    }
    const QStringList parts =
        full.mid(root.length() + 1).split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString current = root;
    for (qsizetype i = 0; i + 1 < parts.size(); ++i) {
        current += QLatin1Char('/') + parts.at(i);
        if (isReparsePoint(current)) {
            return true;
        }
    }
    return false;
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
    // Exclusive create (NewOnly): if a file was raced into the (op-layer-verified
    // new/empty) destination at this entry's path, fail closed rather than clobber
    // it. Validated entries never collide with each other, so this only trips on a
    // foreign, raced-in file.
    QFile out(out_path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        result->blockers.append(
            QStringLiteral("Could not write %1 (it already exists or is not writable).")
                .arg(out_path));
        return false;
    }
    if (out.write(data) != data.size()) {
        out.close();
        out.remove();
        result->blockers.append(QStringLiteral("Could not write %1.").arg(out_path));
        return false;
    }
    out.close();
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
    if (traversesReparsePoint(destination_dir, out_path)) {
        result->blockers.append(
            QStringLiteral("Refused entry %1 (path crosses a symlink or junction).")
                .arg(info.filePath));
        return false;
    }
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

    // Exclusive create: NewOnly makes open() fail if the path already exists, closing
    // the TOCTOU window after the op-layer's exists() check -- a file raced in at
    // zip_path is NOT clobbered. It also guarantees the archive is the file we
    // created, so the remove-on-failure below can never delete a pre-existing file.
    QFile zip_file(zip_path);
    if (!zip_file.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
        result.blockers.append(
            QStringLiteral("Could not create archive %1 (it already exists or is not writable).")
                .arg(zip_path));
        return result;
    }

    QZipWriter writer(&zip_file);
    if (writer.status() != QZipWriter::NoError) {
        result.blockers.append(QStringLiteral("Could not create archive %1.").arg(zip_path));
        zip_file.close();
        zip_file.remove();
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
    zip_file.close();
    result.ok = result.blockers.isEmpty();
    if (!result.ok) {
        // Safe: zip_file was created exclusively above, so this only ever removes
        // the archive we just wrote, never a pre-existing/raced-in file.
        zip_file.remove();
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

ArchiveListing FileExplorerArchiveService::listEntries(const QString& zip_path) {
    ArchiveListing listing;
    QZipReader reader(zip_path);
    // Fail closed on a non-zip BEFORE trusting an empty fileInfoList: a garbage file opens readable
    // and lists as "0 entries" (a dishonest empty-success). A genuinely empty VALID zip has an EOCD
    // (central-dir size 0) and is honestly reported as 0 entries.
    const qint64 central_dir_size = zipCentralDirectorySize(zip_path);
    if (!reader.isReadable() || central_dir_size < 0) {
        listing.blockers.append(QStringLiteral("%1 is not a readable zip archive.").arg(zip_path));
        return listing;
    }
    // Bound peak memory BEFORE fileInfoList() materializes the whole central directory (~2-3x its
    // size): a hostile zip can pack a multi-GB central directory of maximal-length names under the
    // caller's file-size cap, so the count cap below (checked only after fileInfoList) is not
    // enough on its own.
    if (central_dir_size > kMaxCentralDirBytes) {
        listing.blockers.append(
            QStringLiteral("%1 has a central directory too large to list (%2 bytes > %3 limit).")
                .arg(zip_path)
                .arg(central_dir_size)
                .arg(kMaxCentralDirBytes));
        return listing;
    }
    // fileInfoList() reads only the central directory (bounded above), so listing is cheap. Still
    // cap the entry count so a directory with many records cannot run away.
    const QList<QZipReader::FileInfo> entries = reader.fileInfoList();
    if (entries.size() > kArchiveMaxEntries) {
        listing.blockers.append(QStringLiteral("Archive %1 exceeds the %2-entry limit.")
                                    .arg(zip_path)
                                    .arg(kArchiveMaxEntries));
        return listing;
    }
    listing.total_entries = static_cast<int>(entries.size());
    listing.entries.reserve(static_cast<int>(entries.size()));
    for (const QZipReader::FileInfo& info : entries) {
        if (!info.isValid()) {
            continue;
        }
        ArchiveEntryInfo entry;
        entry.path = info.filePath;
        entry.is_dir = info.isDir;
        entry.size = info.isDir ? 0 : info.size;
        listing.total_uncompressed_bytes += entry.size;
        listing.entries.append(entry);
    }
    listing.ok = true;
    return listing;
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
