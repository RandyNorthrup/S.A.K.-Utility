// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDir>
#include <QFileInfo>
#include <QLatin1Char>
#include <QObject>
#include <QString>
#include <QStringList>

/// @file backup_destination_guard.h
/// @brief Fail-closed screen for the user-profile backup destination folder.
///
/// The settings page hands its destination straight to UserProfileBackupWorker, which creates the
/// folder (QDir::mkpath) and then writes manifest.json plus one "<destination>/<username>" subtree
/// per selected user. Three shapes of destination silently corrupt that run:
///
///   * whitespace-only / empty text -- QLineEdit::text().isEmpty() is false for "   ", so the run
///     started and mkpath("   ") produced a junk folder (or failed) mid-backup.
///   * a RELATIVE path -- QDir resolves it against the process working directory, so the backup
///     lands somewhere the technician never chose and cannot find.
///   * a path that overlaps a selected source profile -- the copy walks the source while the
///     backup grows inside it, so the run recurses into its own output and never terminates.
///
/// Screening happens BEFORE the run starts and refuses outright; nothing is normalized to a
/// "close enough" default and no partially-valid destination is allowed through. The only
/// canonicalization applied is trimming surrounding whitespace, and the trimmed string is what the
/// caller must store, so the value that was screened is the value that is used.
///
/// Pure string/path logic (no filesystem access) so it is unit-testable without a display or a
/// disk; existence/"folder already exists" prompts stay in the page.
namespace sak {

/// Outcome of screening a destination: either an accepted, trimmed path or a refusal reason.
struct BackupDestinationCheck {
    bool accepted{false};
    QString path;     ///< Trimmed destination; only meaningful when @c accepted.
    QString refusal;  ///< Human-readable reason; only meaningful when NOT @c accepted.
};

/// Canonical comparison form for a path: cleaned, forward slashes, no trailing separator.
/// Windows paths are compared case-insensitively, so "C:\Users\Username\" and "c:/users/username"
/// are recognized as the same directory and a separator/case trick cannot evade the overlap check.
[[nodiscard]] inline QString backupPathKey(const QString& path) {
    QString key = QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed()));
    while (key.size() > 1 && key.endsWith(QLatin1Char('/'))) {
        key.chop(1);
    }
#ifdef Q_OS_WIN
    return key.toLower();
#else
    return key;
#endif
}

/// True when @p path is the same directory as @p root or lies underneath it.
[[nodiscard]] inline bool backupPathIsWithin(const QString& path, const QString& root) {
    const QString path_key = backupPathKey(path);
    const QString root_key = backupPathKey(root);
    if (path_key.isEmpty() || root_key.isEmpty()) {
        return false;
    }
    if (path_key == root_key) {
        return true;
    }
    // Append the separator so "D:/Profiles/Alice2" is not treated as living under
    // "D:/Profiles/Alice".
    const QString prefix = root_key.endsWith(QLatin1Char('/')) ? root_key
                                                               : root_key + QLatin1Char('/');
    return path_key.startsWith(prefix);
}

/// Every directory the backup run writes into: the destination root itself (manifest.json and the
/// metadata sidecars) plus the per-user subtree the worker derives as "<destination>/<username>".
/// A destination one level above a source profile is only dangerous through the subtree -- e.g.
/// destination "C:\Users" with user "Username" writes straight into "C:\Users\Username" -- so the
/// subtrees are screened alongside the root instead of banning every ancestor of a source.
[[nodiscard]] inline QStringList backupOutputRoots(const QString& destination,
                                                   const QStringList& userFolderNames) {
    QStringList roots;
    roots.append(destination);
    for (const QString& name : userFolderNames) {
        if (!name.trimmed().isEmpty()) {
            roots.append(destination + QLatin1Char('/') + name);
        }
    }
    return roots;
}

/// Refusal reason when any output root overlaps a selected source profile, else an empty string.
[[nodiscard]] inline QString backupDestinationOverlapRefusal(const QString& destination,
                                                             const QStringList& sourceRoots,
                                                             const QStringList& userFolderNames) {
    const QStringList output_roots = backupOutputRoots(destination, userFolderNames);
    for (const QString& source : sourceRoots) {
        if (source.trimmed().isEmpty()) {
            continue;
        }
        for (const QString& output : output_roots) {
            if (backupPathIsWithin(output, source)) {
                return QObject::tr(
                           "The backup destination writes into the profile being backed "
                           "up (%1). Choose a folder outside every selected profile, "
                           "ideally on another drive.")
                    .arg(QDir::toNativeSeparators(source.trimmed()));
            }
        }
    }
    return {};
}

/// Number of characters in a bare drive specifier such as "C:" (drive letter plus colon). The
/// path separator of a fully absolute path, if present, sits at the index one past this prefix.
inline constexpr qsizetype kDrivePrefixLength = 2;

/// True for a drive-relative path such as "C:Backups" (a drive letter and colon with no
/// separator after it). Qt reports these as absolute, but Windows resolves them against that
/// drive's own current directory, so the same text can name a different folder from one process
/// to the next. A backup destination must never be one.
[[nodiscard]] inline bool backupPathIsDriveRelative(const QString& path) {
    const QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (normalized.size() < kDrivePrefixLength || normalized.at(1) != QLatin1Char(':')) {
        return false;
    }
    if (!normalized.at(0).isLetter()) {
        return false;
    }
    return normalized.size() == kDrivePrefixLength ||
           normalized.at(kDrivePrefixLength) != QLatin1Char('/');
}

/// Screen a raw destination from the settings page against the selected source profiles.
///
/// @param rawDestination  Text exactly as typed/browsed by the technician.
/// @param sourceRoots     Profile paths of the users selected for backup.
/// @param userFolderNames Usernames of those same users (each becomes a destination subfolder).
/// @return An accepted, trimmed destination, or a refusal reason. Never a substitute path.
[[nodiscard]] inline BackupDestinationCheck screenBackupDestination(
    const QString& rawDestination,
    const QStringList& sourceRoots,
    const QStringList& userFolderNames) {
    BackupDestinationCheck result;
    const QString trimmed = rawDestination.trimmed();
    if (trimmed.isEmpty()) {
        result.refusal = QObject::tr("Please select a backup destination folder.");
        return result;
    }
    if (!QFileInfo(trimmed).isAbsolute() || backupPathIsDriveRelative(trimmed)) {
        result.refusal = QObject::tr(
            "The backup destination must be a full path (for example "
            "D:\\Backups\\Profiles), not a relative one.");
        return result;
    }
    const QString overlap = backupDestinationOverlapRefusal(trimmed, sourceRoots, userFolderNames);
    if (!overlap.isEmpty()) {
        result.refusal = overlap;
        return result;
    }
    result.accepted = true;
    result.path = trimmed;
    return result;
}

}  // namespace sak
