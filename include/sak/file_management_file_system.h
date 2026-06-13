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

    [[nodiscard]] static FileManagementListResult listDirectory(
        const FileManagementTarget& target,
        const QString& path = {},
        int max_entries = 1000);
    [[nodiscard]] static FileManagementReadResult readFile(const FileManagementTarget& target,
                                                           const QString& path,
                                                           uint64_t max_bytes);
    [[nodiscard]] static FileManagementMutationResult createDirectory(
        const FileManagementTarget& target,
        const QString& path);
    [[nodiscard]] static FileManagementMutationResult deleteDirectory(
        const FileManagementTarget& target,
        const QString& path);
    [[nodiscard]] static FileManagementMutationResult writeFile(const FileManagementTarget& target,
                                                                const QString& path,
                                                                const QByteArray& data);
    [[nodiscard]] static FileManagementMutationResult deleteFile(const FileManagementTarget& target,
                                                                 const QString& path);
    [[nodiscard]] static FileManagementMutationResult renameEntry(
        const FileManagementTarget& target,
        const QString& source_path,
        const QString& destination_path);
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileManagementTarget)
