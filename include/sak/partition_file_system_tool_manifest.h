// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_manifest.h
/// @brief Bundled filesystem tool manifest validation for Partition Manager.

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace sak {

struct PartitionFileSystemToolRuntimeFile {
    QString relative_path;
    QString sha256;
};

struct PartitionFileSystemToolSpec {
    QString id;
    QString display_name;
    QString version;
    QString upstream_url;
    QString license;
    QString source_archive_sha256;
    QString relative_path;
    QString binary_sha256;
    QStringList file_systems;
    QStringList operations;
    QVector<PartitionFileSystemToolRuntimeFile> runtime_files;
};

struct PartitionFileSystemToolManifestResult {
    bool ok{false};
    QVector<PartitionFileSystemToolSpec> tools;
    QStringList errors;
};

class PartitionFileSystemToolManifest {
public:
    [[nodiscard]] static QString defaultRuntimeRelativePath();
    [[nodiscard]] static QString defaultRuntimeManifestPath(const QString& app_dir);
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateManifestFile(
        const QString& manifest_path, const QString& tools_root);
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateManifestJson(
        const QByteArray& json_data, const QString& tools_root);
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateRequiredTool(
        const QString& manifest_path, const QString& tools_root, const QString& tool_id);
};

}  // namespace sak
