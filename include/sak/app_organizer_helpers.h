// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QLatin1Char>
#include <QMap>
#include <QSet>
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

namespace organizer_detail {

/// Characters a category name may never contain: the separators and ':' that would turn a
/// single component into a path, drive or alternate-data-stream reference, the characters
/// Win32 forbids in a name, and every control character (which only ever produces a move
/// that fails after earlier files have already been relocated).
[[nodiscard]] inline bool isForbiddenCategoryChar(QChar ch) {
    static const QString kForbidden = QStringLiteral("/\\:<>\"|?*");
    return ch.unicode() < 0x20 || kForbidden.contains(ch);
}

/// True for a reserved DOS device name. These resolve to a device inside EVERY directory,
/// so "moving" a file to <target>\NUL DISCARDS it instead of relocating it. Win32 matches
/// the part before the first '.', case-insensitively.
[[nodiscard]] inline bool isReservedDeviceName(const QString& name) {
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),    QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("CONIN$"), QStringLiteral("CONOUT$"),
        QStringLiteral("COM1"), QStringLiteral("COM2"),   QStringLiteral("COM3"),
        QStringLiteral("COM4"), QStringLiteral("COM5"),   QStringLiteral("COM6"),
        QStringLiteral("COM7"), QStringLiteral("COM8"),   QStringLiteral("COM9"),
        QStringLiteral("LPT1"), QStringLiteral("LPT2"),   QStringLiteral("LPT3"),
        QStringLiteral("LPT4"), QStringLiteral("LPT5"),   QStringLiteral("LPT6"),
        QStringLiteral("LPT7"), QStringLiteral("LPT8"),   QStringLiteral("LPT9")};
    return kReserved.contains(name.section(QLatin1Char('.'), 0, 0), Qt::CaseInsensitive);
}

}  // namespace organizer_detail

/// Convert the model-facing {category: [ext, ...]} object into the worker's
/// QMap<category, extensions>.
///
/// STRICT / FAIL CLOSED: the mapping is accepted whole or not at all. A wrong-typed
/// category value, a non-string or blank extension, an empty category name, or the same
/// extension claimed twice (case-insensitively, within or across categories) rejects the
/// WHOLE mapping -- it returns an empty map, which both callers already refuse with their
/// "requires a non-empty 'category_mapping'" error. Silently keeping the well-formed half
/// would run a DESTRUCTIVE move over a subset of what was asked for and report it as the
/// whole job, and a duplicated extension would leave the destination of those files to
/// whichever category happens to sort first.
[[nodiscard]] inline QMap<QString, QStringList> categoryMappingFromArgs(
    const QJsonObject& mapping) {
    QMap<QString, QStringList> result;
    QSet<QString> claimed;  // lower-cased extensions already owned by a category
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (it.key().isEmpty() || !it.value().isArray()) {
            return {};
        }
        QStringList extensions;
        for (const QJsonValue& ext : it.value().toArray()) {
            if (!ext.isString()) {
                return {};
            }
            QString value = ext.toString().trimmed();
            if (value.startsWith(QLatin1Char('.'))) {
                value = value.mid(1);
            }
            if (value.isEmpty() || claimed.contains(value.toLower())) {
                return {};
            }
            claimed.insert(value.toLower());
            extensions.append(value);
        }
        if (extensions.isEmpty()) {
            return {};
        }
        result.insert(it.key(), extensions);
    }
    return result;
}

/// A category name becomes a SINGLE subdirectory component under the target
/// (planMove: target_dir / category). Reject anything that is not a plain, literal Win32
/// path component so it cannot escape the target, alias another directory, or address a
/// device:
///   - a separator would make it multi-component or (with std::filesystem operator/) an
///     absolute path that REPLACES the target; a colon is a drive / ADS reference;
///   - "." / ".." are traversal, and ANY name ending in '.' or in whitespace is silently
///     rewritten by Win32 (trailing dots and spaces are stripped), so it aliases the
///     target directory itself or another category instead of naming its own folder;
///   - a blank name is not a component at all;
///   - reserved device names discard the file they are "moved" to;
///   - forbidden/control characters only fail the run after earlier files have moved.
/// This is the containment guard for a prompt-injected category_mapping (both the mutating
/// move and the read-only preview's existence probe).
[[nodiscard]] inline bool isSafeCategoryName(const QString& name) {
    if (name.isEmpty() || name != name.trimmed()) {
        return false;
    }
    if (name.endsWith(QLatin1Char('.'))) {
        // Covers "." and ".." (and "...", which Win32 also strips to the target itself).
        return false;
    }
    for (const QChar ch : name) {
        if (organizer_detail::isForbiddenCategoryChar(ch)) {
            return false;
        }
    }
    return !organizer_detail::isReservedDeviceName(name);
}

/// Returns the first category key that is not a safe subdirectory name, or empty if
/// all are safe.
[[nodiscard]] inline QString firstUnsafeCategory(const QMap<QString, QStringList>& mapping) {
    for (auto it = mapping.begin(); it != mapping.end(); ++it) {
        if (!isSafeCategoryName(it.key())) {
            // An empty key is unsafe too, but returning it verbatim would be read as the
            // "all safe" sentinel below and let a nameless component through, so it is
            // reported under a printable marker instead.
            return it.key().isEmpty() ? QStringLiteral("(empty)") : it.key();
        }
    }
    return QString();
}

}  // namespace sak
