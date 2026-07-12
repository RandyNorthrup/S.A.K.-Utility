// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_management_file_system.h
/// @brief Shared file-system target bridge for File Management tools.

#pragma once

#include "sak/partition_manager_types.h"

#include <QByteArray>
#include <QDateTime>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <cstdint>

namespace sak {

/// Default upper bound on entries returned by a single directory listing.
inline constexpr int kDefaultFileManagementListEntries = 1000;

enum class FileManagementTargetKind {
    LocalPath,
    Partition,
    ImageFile,
};

struct FileManagementTarget {
    QString id;
    QString label;
    QString root_path;
    QString file_system;
    QString source;
    QStringList details;
    uint64_t size_bytes{0};
    FileManagementTargetKind kind{FileManagementTargetKind::LocalPath};
    bool local_file_system{true};
    bool read_only{false};
    bool can_browse{true};
    bool can_read_files{true};
    bool can_write_files{true};
    bool can_organize{true};
    bool can_duplicate_scan{true};
    bool can_advanced_search{true};
    QStringList blockers;
};

struct FileManagementEntry {
    QString name;
    QString path;
    QString type;
    uint64_t size_bytes{0};
    QDateTime modified_time;
    QDateTime created_time;
    QString identifier;
    QString link_target;
    bool directory{false};
    bool regular_file{false};
    bool symlink{false};
    // File-system-specific storage detail, surfaced in the Properties pane where the
    // reader provides it. resource_fork_bytes: HFS+ resource fork size (0 = none).
    // compressed/sparse: APFS transparent compression / sparse (holes) flags.
    uint64_t resource_fork_bytes{0};
    bool compressed{false};
    bool sparse{false};
};

struct FileManagementListResult {
    bool ok{false};
    QString file_system;
    QString volume_name;
    QStringList blockers;
    QStringList warnings;
    QVector<FileManagementEntry> entries;
};

struct FileManagementReadResult {
    bool ok{false};
    QString file_system;
    QStringList blockers;
    QStringList warnings;
    QByteArray data;
};

struct FileManagementMutationResult {
    bool ok{false};
    QString file_system;
    QString path;
    uint64_t bytes_written{0};
    QString before_sha256;
    QString after_sha256;
    QStringList blockers;
    QStringList warnings;
};

/// @brief A rendered, display-ready preview of a file's leading bytes.
/// Text content is shown verbatim; binary content is shown as a hex+ASCII dump.
struct FileManagementPreview {
    QString text;             ///< Display string: decoded text, or a hex+ASCII dump for binary.
    bool is_binary{false};    ///< True when the bytes were rendered as a hex dump.
    bool truncated{false};    ///< True when more bytes exist past the previewed window.
    uint64_t shown_bytes{0};  ///< Number of source bytes represented in @ref text.
};

/// @brief Result of an on-demand SHA-256 hash of a file.
struct FileManagementHashResult {
    bool ok{false};
    QString file_system;
    QString sha256;            ///< Lowercase hex SHA-256 digest.
    bool capped{false};        ///< True when only the first @ref hashed_bytes were hashed.
    uint64_t hashed_bytes{0};  ///< Number of source bytes fed into the digest.
    QStringList blockers;
};

/// Result of copying a source file out to a local host destination.
struct FileManagementExportResult {
    bool ok{false};
    QString destination;        ///< Host path written.
    QString sha256;             ///< Lowercase hex SHA-256 of the bytes written.
    uint64_t bytes_written{0};  ///< Number of bytes copied to the destination.
    bool capped{false};         ///< True when a raw source was truncated at the read cap.
    QStringList blockers;
};

/// Result of recursively exporting a directory tree to a local host destination.
struct FileManagementDirectoryExportResult {
    bool ok{false};       ///< True when every reachable file exported completely.
    QString destination;  ///< Host directory the tree was written under.
    int files_exported{0};
    int directories_created{0};
    int symlinks_skipped{0};
    int capped_files{0};  ///< Raw files truncated at the per-file read window.
    uint64_t bytes_written{0};
    QStringList blockers;
    QStringList warnings;
};

class FileManagementFileSystemBridge {
public:
    [[nodiscard]] static QVector<FileManagementTarget> mountedTargets();
    [[nodiscard]] static QVector<FileManagementTarget> targetsFromInventory(
        const PartitionInventory& inventory);
    [[nodiscard]] static FileManagementTarget manualTarget(const QString& root_path,
                                                           const QString& file_system,
                                                           uint64_t size_bytes = 0);
    [[nodiscard]] static FileManagementTarget localTarget(const QString& root_path);

    [[nodiscard]] static bool isNativeFileSystem(const QString& file_system);
    [[nodiscard]] static bool isReadableNonNativeFileSystem(const QString& file_system);
    [[nodiscard]] static QString normalizedFileSystem(const QString& file_system);
    [[nodiscard]] static QString capabilitySummary(const FileManagementTarget& target);
    /// File-system-specific label for an entry's on-disk identifier (APFS Object
    /// ID, HFS Catalog ID, ext Inode, else generic Identifier).
    [[nodiscard]] static QString identifierLabel(const QString& file_system);
    /// File-system-specific plain-language notes on why writes/reads are (un)available.
    [[nodiscard]] static QStringList safetyNotes(const FileManagementTarget& target);

    [[nodiscard]] static FileManagementListResult listDirectory(
        const FileManagementTarget& target,
        const QString& path = {},
        int max_entries = kDefaultFileManagementListEntries);
    [[nodiscard]] static FileManagementReadResult readFile(const FileManagementTarget& target,
                                                           const QString& path,
                                                           uint64_t max_bytes);
    /// Compute the SHA-256 of @p path. Local targets are hashed in full via a chunked reader;
    /// raw/non-native targets are hashed from up to @p max_bytes read through the file-system
    /// reader (the result is marked @ref FileManagementHashResult::capped when that limit is hit).
    [[nodiscard]] static FileManagementHashResult hashFile(const FileManagementTarget& target,
                                                           const QString& path,
                                                           uint64_t max_bytes);
    /// Render @p data (already capped to a preview window by the caller) into a display-ready
    /// preview: decoded UTF-8/Latin-1 text when the bytes look textual, otherwise a hex+ASCII
    /// dump. @p truncated marks that the source file has more bytes past this window.
    [[nodiscard]] static FileManagementPreview renderPreview(const QByteArray& data,
                                                             bool truncated);
    [[nodiscard]] static FileManagementMutationResult createDirectory(
        const FileManagementTarget& target, const QString& path);
    [[nodiscard]] static FileManagementMutationResult deleteDirectory(
        const FileManagementTarget& target, const QString& path);
    [[nodiscard]] static FileManagementMutationResult writeFile(const FileManagementTarget& target,
                                                                const QString& path,
                                                                const QByteArray& data);
    /// Stream a host file into @p path on @p target without holding the payload in RAM.
    /// APFS and the local filesystem stream block-by-block (peak RAM one window); other
    /// backends read the file whole until they gain a streaming writer. Prefer this over
    /// writeFile for copies whose size is not known-small.
    [[nodiscard]] static FileManagementMutationResult writeFileFromHostPath(
        const FileManagementTarget& target, const QString& path, const QString& host_file_path);
    /// Copy @p source_path from @p target out to a local host file @p destination_path.
    /// Local sources are copied in full (streamed, no cap) and hashed; raw/non-native
    /// sources are read through the file-system reader up to @p max_bytes and marked
    /// @ref FileManagementExportResult::capped when that limit is hit.
    [[nodiscard]] static FileManagementExportResult copyFileToHost(
        const FileManagementTarget& target,
        const QString& source_path,
        const QString& destination_path,
        uint64_t max_bytes);
    /// Recursively export the directory tree at @p source_path into the local host
    /// directory @p destination_dir (created if missing). Regular files copy through
    /// @ref copyFileToHost (raw sources bounded per file by @p max_file_bytes and
    /// counted in @ref FileManagementDirectoryExportResult::capped_files); symlinks are
    /// skipped with a warning. Depth and per-directory entry counts are bounded.
    [[nodiscard]] static FileManagementDirectoryExportResult exportDirectoryToHost(
        const FileManagementTarget& target,
        const QString& source_path,
        const QString& destination_dir,
        uint64_t max_file_bytes);
    [[nodiscard]] static FileManagementMutationResult deleteFile(const FileManagementTarget& target,
                                                                 const QString& path);
    [[nodiscard]] static FileManagementMutationResult renameEntry(
        const FileManagementTarget& target,
        const QString& source_path,
        const QString& destination_path);
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileManagementTarget)
