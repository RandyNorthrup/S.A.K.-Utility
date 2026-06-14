// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_file_system_tool_manifest.cpp
/// @brief Bundled filesystem tool manifest validation for Partition Manager.

#include "sak/partition_file_system_tool_manifest.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>

namespace sak {

namespace {

constexpr int kManifestSchemaVersion = 1;
constexpr qsizetype kSha256HexLength = 64;
constexpr auto kRuntimeManifestRelativePath = "tools/filesystem/manifest.json";

QString objectString(const QJsonObject& object, const QString& key) {
    return object.value(key).toString().trimmed();
}

QStringList objectStringList(const QJsonObject& object, const QString& key) {
    QStringList values;
    const auto array = object.value(key).toArray();
    for (const auto& entry : array) {
        const QString value = entry.toString().trimmed();
        if (!value.isEmpty()) {
            values.append(value);
        }
    }
    values.removeDuplicates();
    return values;
}

bool validSha256Hex(const QString& value) {
    if (value.size() != kSha256HexLength) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](QChar ch) {
        return ch.isDigit() || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
               (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
    });
}

QString normalizedRelativePath(const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    return QDir::cleanPath(trimmed).replace(QLatin1Char('\\'), QLatin1Char('/'));
}

bool safeRelativePath(const QString& path) {
    const QString clean = normalizedRelativePath(path);
    if (clean.isEmpty() || clean.startsWith(QLatin1Char('/')) || QFileInfo(clean).isAbsolute()) {
        return false;
    }
    const QStringList parts = clean.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return std::none_of(parts.cbegin(), parts.cend(), [](const QString& part) {
        return part == QStringLiteral("..");
    });
}

QString fileSha256Hex(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) {
        return {};
    }
    return QString::fromLatin1(hash.result().toHex());
}

QVector<PartitionFileSystemToolRuntimeFile> objectRuntimeFiles(const QJsonObject& object) {
    QVector<PartitionFileSystemToolRuntimeFile> files;
    const auto array = object.value(QStringLiteral("runtime_files")).toArray();
    for (const auto& entry : array) {
        if (!entry.isObject()) {
            files.append(PartitionFileSystemToolRuntimeFile{});
            continue;
        }
        const QJsonObject fileObject = entry.toObject();
        files.append({.relative_path = normalizedRelativePath(
                          objectString(fileObject, QStringLiteral("relative_path"))),
                      .sha256 = objectString(fileObject, QStringLiteral("sha256"))});
    }
    return files;
}

PartitionFileSystemToolSpec parseTool(const QJsonObject& object) {
    PartitionFileSystemToolSpec tool;
    tool.id = objectString(object, QStringLiteral("id"));
    tool.display_name = objectString(object, QStringLiteral("display_name"));
    tool.version = objectString(object, QStringLiteral("version"));
    tool.upstream_url = objectString(object, QStringLiteral("upstream_url"));
    tool.license = objectString(object, QStringLiteral("license"));
    tool.source_archive_sha256 = objectString(object, QStringLiteral("source_archive_sha256"));
    tool.relative_path =
        normalizedRelativePath(objectString(object, QStringLiteral("relative_path")));
    tool.binary_sha256 = objectString(object, QStringLiteral("binary_sha256"));
    tool.file_systems = objectStringList(object, QStringLiteral("file_systems"));
    tool.operations = objectStringList(object, QStringLiteral("operations"));
    tool.runtime_files = objectRuntimeFiles(object);
    return tool;
}

void appendRequiredFieldError(const QString& toolId,
                              const QString& field,
                              const QString& value,
                              QStringList* errors) {
    if (value.trimmed().isEmpty()) {
        errors->append(QStringLiteral("Tool '%1' is missing required field '%2'")
                           .arg(toolId.isEmpty() ? QStringLiteral("<unknown>") : toolId, field));
    }
}

void validateRequiredFields(const PartitionFileSystemToolSpec& tool, QStringList* errors) {
    appendRequiredFieldError(tool.id, QStringLiteral("id"), tool.id, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("display_name"), tool.display_name, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("version"), tool.version, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("upstream_url"), tool.upstream_url, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("license"), tool.license, errors);
    appendRequiredFieldError(
        tool.id, QStringLiteral("source_archive_sha256"), tool.source_archive_sha256, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("relative_path"), tool.relative_path, errors);
    appendRequiredFieldError(tool.id, QStringLiteral("binary_sha256"), tool.binary_sha256, errors);
}

void validateCapabilities(const PartitionFileSystemToolSpec& tool, QStringList* errors) {
    if (tool.file_systems.isEmpty()) {
        errors->append(QStringLiteral("Tool '%1' must list supported file systems").arg(tool.id));
    }
    if (tool.operations.isEmpty()) {
        errors->append(QStringLiteral("Tool '%1' must list supported operations").arg(tool.id));
    }
}

void validateDigestFields(const PartitionFileSystemToolSpec& tool, QStringList* errors) {
    if (!tool.source_archive_sha256.isEmpty() && !validSha256Hex(tool.source_archive_sha256)) {
        errors->append(QStringLiteral("Tool '%1' source_archive_sha256 is not a SHA-256 hex digest")
                           .arg(tool.id));
    }
    if (!tool.binary_sha256.isEmpty() && !validSha256Hex(tool.binary_sha256)) {
        errors->append(
            QStringLiteral("Tool '%1' binary_sha256 is not a SHA-256 hex digest").arg(tool.id));
    }
    for (const auto& runtimeFile : tool.runtime_files) {
        if (!validSha256Hex(runtimeFile.sha256)) {
            errors->append(
                QStringLiteral("Tool '%1' runtime file sha256 is not a SHA-256 hex digest: %2")
                    .arg(tool.id,
                         runtimeFile.relative_path.isEmpty() ? QStringLiteral("<missing path>")
                                                             : runtimeFile.relative_path));
        }
    }
}

struct PathHashCheck {
    QString tool_id;
    QString relative_path;
    QString expected_hash;
    QString tools_root;
    QString label;
};

void validatePathAndHash(const PathHashCheck& check, QStringList* errors) {
    if (!safeRelativePath(check.relative_path)) {
        errors->append(QStringLiteral("Tool '%1' %2 relative_path must stay under tools root")
                           .arg(check.tool_id, check.label));
        return;
    }

    const QString filePath = QDir(check.tools_root).filePath(check.relative_path);
    if (!QFileInfo::exists(filePath)) {
        errors->append(QStringLiteral("Tool '%1' %2 is missing: %3")
                           .arg(check.tool_id, check.label, check.relative_path));
        return;
    }

    const QString actualHash = fileSha256Hex(filePath);
    if (actualHash.isEmpty() || actualHash.compare(check.expected_hash, Qt::CaseInsensitive) != 0) {
        errors->append(
            QStringLiteral("Tool '%1' %2 hash mismatch").arg(check.tool_id, check.label));
    }
}

void validateBinaryPathAndHash(const PartitionFileSystemToolSpec& tool,
                               const QString& toolsRoot,
                               QStringList* errors) {
    validatePathAndHash({.tool_id = tool.id,
                         .relative_path = tool.relative_path,
                         .expected_hash = tool.binary_sha256,
                         .tools_root = toolsRoot,
                         .label = QStringLiteral("binary")},
                        errors);
}

void validateRuntimeFiles(const PartitionFileSystemToolSpec& tool,
                          const QString& toolsRoot,
                          QStringList* errors) {
    QStringList seenPaths;
    for (const auto& runtimeFile : tool.runtime_files) {
        if (runtimeFile.relative_path.trimmed().isEmpty()) {
            errors->append(
                QStringLiteral("Tool '%1' runtime file is missing relative_path").arg(tool.id));
        }
        if (runtimeFile.sha256.trimmed().isEmpty()) {
            errors->append(QStringLiteral("Tool '%1' runtime file is missing sha256: %2")
                               .arg(tool.id,
                                    runtimeFile.relative_path.isEmpty()
                                        ? QStringLiteral("<missing path>")
                                        : runtimeFile.relative_path));
        }
        if (seenPaths.contains(runtimeFile.relative_path, Qt::CaseInsensitive)) {
            errors->append(QStringLiteral("Tool '%1' duplicates runtime file: %2")
                               .arg(tool.id, runtimeFile.relative_path));
        }
        seenPaths.append(runtimeFile.relative_path);
        validatePathAndHash({.tool_id = tool.id,
                             .relative_path = runtimeFile.relative_path,
                             .expected_hash = runtimeFile.sha256,
                             .tools_root = toolsRoot,
                             .label = QStringLiteral("runtime file")},
                            errors);
    }
}

void validateTool(const PartitionFileSystemToolSpec& tool,
                  const QString& toolsRoot,
                  QStringList* errors) {
    validateRequiredFields(tool, errors);
    validateCapabilities(tool, errors);
    validateDigestFields(tool, errors);
    validateBinaryPathAndHash(tool, toolsRoot, errors);
    validateRuntimeFiles(tool, toolsRoot, errors);
}

}  // namespace

QString PartitionFileSystemToolManifest::defaultRuntimeRelativePath() {
    return QString::fromLatin1(kRuntimeManifestRelativePath);
}

QString PartitionFileSystemToolManifest::defaultRuntimeManifestPath(const QString& app_dir) {
    return QDir(app_dir).filePath(defaultRuntimeRelativePath());
}

PartitionFileSystemToolManifestResult PartitionFileSystemToolManifest::validateManifestFile(
    const QString& manifest_path, const QString& tools_root) {
    QFile file(manifest_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {.ok = false,
                .errors = {
                    QStringLiteral("Filesystem tool manifest is missing: %1").arg(manifest_path)}};
    }
    return validateManifestJson(file.readAll(), tools_root);
}

PartitionFileSystemToolManifestResult PartitionFileSystemToolManifest::validateManifestJson(
    const QByteArray& json_data, const QString& tools_root) {
    PartitionFileSystemToolManifestResult result;
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(json_data, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.errors.append(QStringLiteral("Filesystem tool manifest JSON is invalid: %1")
                                 .arg(parseError.errorString()));
        return result;
    }

    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema_version")).toInt() != kManifestSchemaVersion) {
        result.errors.append(QStringLiteral("Filesystem tool manifest schema_version must be %1")
                                 .arg(kManifestSchemaVersion));
    }
    const QJsonArray tools = root.value(QStringLiteral("tools")).toArray();
    for (const auto& entry : tools) {
        if (!entry.isObject()) {
            result.errors.append(
                QStringLiteral("Filesystem tool manifest contains a non-object tool"));
            continue;
        }
        const PartitionFileSystemToolSpec tool = parseTool(entry.toObject());
        result.tools.append(tool);
        validateTool(tool, tools_root, &result.errors);
    }

    result.ok = result.errors.isEmpty();
    return result;
}

PartitionFileSystemToolManifestResult PartitionFileSystemToolManifest::validateRequiredTool(
    const QString& manifest_path, const QString& tools_root, const QString& tool_id) {
    auto result = validateManifestFile(manifest_path, tools_root);
    const auto found = std::any_of(result.tools.cbegin(),
                                   result.tools.cend(),
                                   [&](const auto& tool) { return tool.id == tool_id; });
    if (!found) {
        result.errors.append(
            QStringLiteral("Required filesystem tool is not manifest-approved: %1").arg(tool_id));
    }
    result.ok = result.errors.isEmpty();
    return result;
}

}  // namespace sak
