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
    // Resolve the elevated-tool manifest (root of trust) from the trusted application
    // directory only; never from a current-working-directory path an attacker could plant.
    // Returns an empty string when there is no trusted anchor, which makes the tool runner
    // fail closed. See the definition for the SAK_DEV_SOURCE_TREE developer opt-in.
    [[nodiscard]] static QString resolveRuntimeManifestPath();
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateManifestFile(
        const QString& manifest_path, const QString& tools_root);
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateManifestJson(
        const QByteArray& json_data, const QString& tools_root);
    [[nodiscard]] static PartitionFileSystemToolManifestResult validateRequiredTool(
        const QString& manifest_path, const QString& tools_root, const QString& tool_id);
};

}  // namespace sak
