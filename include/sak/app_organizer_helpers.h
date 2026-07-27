// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1Char>
#include <QMap>
#include <QString>
#include <QStringList>

/// @file app_organizer_helpers.h
/// @brief Organizer category-mapping helpers shared by the AI-assistant app-action modules.
///
/// These live in one header (not duplicated per module) because the mutating
/// organizer.organize_directory op and the read-only organizer.preview_organize op MUST
/// interpret and containment-check a model-supplied category_mapping IDENTICALLY -- the
/// preview would be a lie if it categorized files or judged safety by different rules than
/// the apply that follows it.
namespace sak {

/// Convert the model-facing {category: [ext, ...]} object into the worker's
/// QMap<category, extensions>. Non-string / empty extensions are dropped; a category
/// with no usable extensions is dropped. Returns empty if nothing usable was given.
[[nodiscard]] inline QMap<QString, QStringList> categoryMappingFromArgs(
    const QJsonObject& mapping) {
    QMap<QString, QStringList> result;
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (it.key().isEmpty() || !it.value().isArray()) {
            continue;
        }
        QStringList extensions;
        for (const QJsonValue& ext : it.value().toArray()) {
            QString value = ext.toString().trimmed();
            if (value.startsWith(QLatin1Char('.'))) {
                value = value.mid(1);
            }
            if (!value.isEmpty()) {
                extensions.append(value);
            }
        }
        if (!extensions.isEmpty()) {
            result.insert(it.key(), extensions);
        }
    }
    return result;
}

/// A category name becomes a SINGLE subdirectory component under the target
/// (planMove: target_dir / category). Reject anything that is not a plain component
/// so it cannot escape the target: a separator would make it multi-component or
/// (with std::filesystem operator/) an absolute path that REPLACES the target; a
/// colon is a Windows drive / alternate-data-stream; "." / ".." are traversal. This
/// is the containment guard for a prompt-injected category_mapping (both the mutating
/// move and the read-only preview's existence probe).
[[nodiscard]] inline bool isSafeCategoryName(const QString& name) {
    if (name == QLatin1String(".") || name == QLatin1String("..")) {
        return false;
    }
    for (const QChar ch : name) {
        if (ch == QLatin1Char('/') || ch == QLatin1Char('\\') || ch == QLatin1Char(':')) {
            return false;
        }
    }
    return true;
}

/// Returns the first category key that is not a safe subdirectory name, or empty if
/// all are safe.
[[nodiscard]] inline QString firstUnsafeCategory(const QMap<QString, QStringList>& mapping) {
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (!isSafeCategoryName(it.key())) {
            return it.key();
        }
    }
    return QString();
}

}  // namespace sak
