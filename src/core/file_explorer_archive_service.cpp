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

#include <optional>

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
// Per-file cap. QZipReader has no streaming decode -- each entry is decompressed
// whole into one QByteArray before it is written -- so this also bounds the peak
// RAM one entry can allocate. Held to 512 MiB (was 4 GiB) so a single crafted
// entry, or several concurrent extractions, cannot exhaust memory; the entry's
// DECLARED uncompressed size is checked against it before any decode (B8-10).
constexpr qint64 kExtractMaxFileBytes = 512LL * 1024 * 1024;  // 512 MiB per file

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

// Read a zip's central-directory entry list only AFTER bounding its byte size, so
// a hostile multi-GB central directory of maximal-length names (packed under a
// modest file-size cap) cannot force a ~2-3x materialization before the entry
// count is capped. Returns nullopt and sets *error on a non-zip / unreadable
// archive, an oversize central directory, or an over-cap entry count. Every path
// that materializes fileInfoList() must go through this so none skips the bound
// (B8-09).
std::optional<QList<QZipReader::FileInfo>> boundedZipEntries(const QString& zip_path,
                                                             const QZipReader& reader,
                                                             QString* error) {
    const qint64 central_dir_size = zipCentralDirectorySize(zip_path);
    if (!reader.isReadable() || central_dir_size < 0) {
        *error = QStringLiteral("%1 is not a readable zip archive.").arg(zip_path);
        return std::nullopt;
    }
    if (central_dir_size > kMaxCentralDirBytes) {
        *error =
            QStringLiteral("%1 has a central directory too large to read (%2 bytes > %3 limit).")
                .arg(zip_path)
                .arg(central_dir_size)
                .arg(kMaxCentralDirBytes);
        return std::nullopt;
    }
    QList<QZipReader::FileInfo> entries = reader.fileInfoList();
    if (entries.size() > kArchiveMaxEntries) {
        *error = QStringLiteral("Archive %1 exceeds the %2-entry limit.")
                     .arg(zip_path)
                     .arg(kArchiveMaxEntries);
        return std::nullopt;
    }
    return entries;
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

// Records exactly what an extraction created so a mid-extraction failure can be
// rolled back to the pre-extraction state (B8-08). Only entries WE created are
// tracked -- writeExtractedFile opens files NewOnly, so every recorded file is one
// we wrote (never a pre-existing file), and only newly-created directories are
// recorded -- so rollback never removes pre-existing content.
struct ExtractionRollback {
    QStringList files;  // files this extraction created
    QStringList dirs;   // directories this extraction created
};

// Per-extraction state threaded through the entry writers: the destination root,
// the running expanded-byte total (bomb cap), the result being built, and the
// rollback ledger. Bundled so the entry writers stay within the parameter budget.
struct ExtractionContext {
    QString destination_dir;
    qint64 total_bytes{0};
    FileExplorerArchiveResult* result{nullptr};
    ExtractionRollback rollback;
};

// Undo a partial extraction: delete every file we wrote, then remove the
// directories we created deepest-first. rmdir is empty-only, so a pre-existing
// directory that still holds content (or a dir we mkpath'd into a populated tree)
// is never deleted.
void performExtractionRollback(const ExtractionRollback& rollback) {
    for (const QString& file : rollback.files) {
        QFile::remove(file);
    }
    QStringList dirs = rollback.dirs;
    std::sort(dirs.begin(), dirs.end(), [](const QString& a, const QString& b) {
        return a.size() > b.size();
    });
    for (const QString& dir : dirs) {
        QDir().rmdir(dir);
    }
}

// mkpath @p dir_path, appending it to @p rollback only when it did not already
// exist, so rollback removes exactly the directories this extraction created.
bool makeDirRecording(const QString& dir_path, ExtractionRollback* rollback) {
    const bool existed = QFileInfo::exists(dir_path);
    if (!QDir().mkpath(dir_path)) {
        return false;
    }
    if (!existed && rollback != nullptr) {
        rollback->dirs.append(dir_path);
    }
    return true;
}

// Materialize one file entry at @p out_path (parent dirs, corrupt-payload
// guard, atomic write). Returns false and records a blocker on failure.
bool writeExtractedFile(const QZipReader& reader,
                        const QString& out_path,
                        const QZipReader::FileInfo& info,
                        ExtractionContext* ctx) {
    FileExplorerArchiveResult* result = ctx->result;
    if (!makeDirRecording(QFileInfo(out_path).absolutePath(), &ctx->rollback)) {
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
    // The reader's own verdict on the decode. Without it a partial or failed inflate is written
    // out and counted in result->entries as a complete file: the caller is told the archive
    // extracted, and the short file is indistinguishable from the real one afterwards.
    if (reader.status() != QZipReader::NoError) {
        result->blockers.append(
            QStringLiteral("Extraction of entry %1 failed (archive read error).")
                .arg(info.filePath));
        return false;
    }
    // And the decode has to have produced what the entry declared. A short inflate that still
    // returns some bytes passes the isEmpty guard above; comparing against the declared size is
    // what makes "extracted" mean the whole entry. info.size is validated non-negative before
    // this point, so the comparison is between two known-good lengths.
    if (data.size() != info.size) {
        result->blockers.append(QStringLiteral("Extraction of entry %1 produced %2 bytes, not the "
                                               "declared %3 (truncated or corrupt).")
                                    .arg(info.filePath)
                                    .arg(data.size())
                                    .arg(info.size));
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
    ctx->rollback.files.append(out_path);
    ++result->entries;
    return true;
}

// Extract one bounded zip entry into @p ctx->destination_dir. Returns false (and
// records a blocker) to abort the whole extraction; true continues.
// Directories and skipped symlinks return true without writing a file.
bool extractZipEntry(const QZipReader& reader,
                     const QZipReader::FileInfo& info,
                     ExtractionContext* ctx) {
    const QString& destination_dir = ctx->destination_dir;
    FileExplorerArchiveResult* result = ctx->result;
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
        if (makeDirRecording(out_path, &ctx->rollback)) {
            return true;
        }
        result->blockers.append(QStringLiteral("Could not create directory %1.").arg(out_path));
        return false;
    }
    // A NEGATIVE declared size is not a small file, it is a lie the size checks cannot see: a
    // ZIP64 length past 2^63 lands negative in info.size, passes `info.size > kExtractMaxFileBytes`
    // (it is less than the cap, not more), drags ctx->total_bytes DOWN so later entries get a
    // larger budget than the archive is allowed, and skips the corrupt-payload guard because that
    // is gated on info.size > 0 -- so the entry is written and counted as an extracted file.
    // Refuse it before it can influence any of those.
    if (info.size < 0) {
        result->blockers.append(
            QStringLiteral("Refused entry %1 (declared size %2 is not a valid length).")
                .arg(info.filePath)
                .arg(info.size));
        return false;
    }
    ctx->total_bytes += info.size;
    if (info.size > kExtractMaxFileBytes || ctx->total_bytes > kExtractMaxTotalBytes) {
        result->blockers.append(
            QStringLiteral("Entry %1 exceeds the extraction size limit.").arg(info.filePath));
        return false;
    }
    return writeExtractedFile(reader, out_path, info, ctx);
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
    ++result->entries;
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
            // Count directories toward the cap too. A tree of millions of empty
            // directories would otherwise never increment the counter, so the
            // loop guard never trips and the entry cap is bypassed entirely.
            writer->addDirectory(entry_name);
            ++result->entries;
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

// Add one compress source to the writer, classifying it: directory -> recurse,
// file -> stream, symlink/special -> warn. A source the caller asked to archive
// that no longer exists is a hard blocker -- silently downgrading it to a warning
// would ship an archive missing a requested item as a success (B8-21).
void addCompressSource(QZipWriter* writer,
                       const QString& source,
                       FileExplorerArchiveResult* result) {
    const QFileInfo info(source);
    if (info.isSymLink()) {
        result->warnings.append(
            QStringLiteral("Skipped symlink %1 (links are not archived).").arg(source));
        return;
    }
    if (info.isDir()) {
        addDirectoryEntries(writer, info.fileName(), source, result);
        return;
    }
    if (info.isFile()) {
        if (addFileEntry(writer, info.fileName(), source, &result->blockers)) {
            ++result->entries;
        }
        return;
    }
    if (!info.exists()) {
        result->blockers.append(
            QStringLiteral("Source %1 no longer exists; the archive was not written.").arg(source));
    } else {
        result->warnings.append(QStringLiteral("Skipped special entry %1.").arg(source));
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
        addCompressSource(&writer, source, &result);
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
    // Bound the central directory BEFORE materializing it (a hostile multi-GB
    // central directory would otherwise force a ~2-3x allocation before the entry
    // cap), then cap the entry count (B8-09).
    QString entries_error;
    const auto entries = boundedZipEntries(zip_path, reader, &entries_error);
    if (!entries) {
        result.blockers.append(entries_error);
        return result;
    }
    if (!QDir().mkpath(destination_dir)) {
        result.blockers.append(
            QStringLiteral("Could not create destination %1.").arg(destination_dir));
        return result;
    }
    // Bounded per-entry extraction (Qt's extractAll enforces no caps): reject a
    // zip-bomb or zip-slip before writing, and cap entry count / expanded bytes.
    ExtractionContext ctx{.destination_dir = destination_dir, .result = &result};
    for (const QZipReader::FileInfo& info : *entries) {
        if (!extractZipEntry(reader, info, &ctx)) {
            // Roll the partial extraction back so a mid-extraction failure never
            // leaves a half-written tree behind (B8-08).
            performExtractionRollback(ctx.rollback);
            return result;
        }
    }
    result.ok = true;
    return result;
}

ArchiveListing FileExplorerArchiveService::listEntries(const QString& zip_path) {
    ArchiveListing listing;
    QZipReader reader(zip_path);
    // Fail closed on a non-zip BEFORE trusting an empty fileInfoList (a garbage file opens readable
    // and lists as a dishonest "0 entries"), and bound the central directory before materializing
    // it (a hostile multi-GB central directory of maximal-length names). A genuinely empty VALID
    // zip has an EOCD (central-dir size 0) and is honestly reported as 0 entries.
    QString entries_error;
    const auto entries = boundedZipEntries(zip_path, reader, &entries_error);
    if (!entries) {
        listing.blockers.append(entries_error);
        return listing;
    }
    listing.total_entries = static_cast<int>(entries->size());
    listing.entries.reserve(static_cast<int>(entries->size()));
    for (const QZipReader::FileInfo& info : *entries) {
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
    // Bound the central directory before materializing it -- this runs per archive
    // (once per extraction), so an unbounded fileInfoList() here would repeat the
    // hostile-central-directory allocation (B8-09).
    QString entries_error;
    const auto entries = boundedZipEntries(zip_path, reader, &entries_error);
    if (!entries) {
        return false;
    }
    QSet<QString> roots;
    // The lone root counts only when it is a folder: a single top-level FILE
    // still needs the wrapper folder (Files GetFirstMeaningfulSegment walk).
    bool root_is_directory = false;
    for (const auto& entry : *entries) {
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
