// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QChar>
#include <QLatin1Char>
#include <QLatin1String>
#include <QString>
#include <QStringList>

#include <algorithm>

/// @file windows_path_policy.h
/// @brief One shared dialect for screening a Windows path that arrived as UNTRUSTED input
///        and is about to acquire EXECUTION or DELETION authority.
///
/// Both hazards come from the same place: a registry Uninstall entry, which ANY user can
/// create under HKCU. Its UninstallString names a program the uninstaller launches with an
/// ELEVATED token; its InstallLocation names a directory the leftover cleanup deletes
/// RECURSIVELY. In both cases the string must be pinned to one literal local directory
/// before it is acted on -- a bare, relative or drive-relative form is resolved by Windows
/// against the current directory/drive, a UNC form points at a remote origin nobody here
/// can vouch for, an unexpanded "%VAR%" does not name what its author meant, and a "."/".."
/// segment moves the target out of the directory the string appears to name.
///
/// These predicates are pure, allocation-light and unit-testable. They deliberately REFUSE
/// rather than normalize: silently collapsing a traversal would hide the very manipulation
/// that must be rejected. Callers fail closed on false.
namespace sak {

/// @brief Normalize an untrusted path token for screening: trim surrounding whitespace,
///        fold '\\' to '/', and drop trailing separators. Never resolves "." or ".."
///        segments -- pathSegmentsAreLiteral refuses those instead. A bare volume root
///        ("C:/") is left intact so driveQualifiedPath can refuse it.
[[nodiscard]] inline QString screeningPathForm(const QString& raw) {
    constexpr int kDriveRootLength = 3;  // "X:/"
    QString path = raw.trimmed();
    path.replace(QLatin1Char('\\'), QLatin1Char('/'));
    while (path.size() > kDriveRootLength && path.endsWith(QLatin1Char('/'))) {
        path.chop(1);
    }
    return path;
}

/// @brief True for a path whose every separator-delimited segment is a real name.
///
/// A doubled separator (UNC "\\\\server\\share", or a malformed path) and any "."/".."
/// segment are refused: the first is an untrusted remote origin, the second lets a
/// traversal move the target out of the directory the string names. A leading separator
/// yields an empty first segment ("\\dir\\x" is relative to the CURRENT drive), so it is
/// refused here too. @p slashPath must already be in screeningPathForm.
[[nodiscard]] inline bool pathSegmentsAreLiteral(const QString& slashPath) {
    if (slashPath.contains(QLatin1String("//"))) {
        return false;
    }
    const QStringList segments = slashPath.split(QLatin1Char('/'));
    return std::none_of(segments.cbegin(), segments.cend(), [](const QString& segment) {
        return segment.isEmpty() || segment == QLatin1String(".") || segment == QLatin1String("..");
    });
}

/// @brief True for "X:/dir/name" -- a drive letter, a colon and a separator, followed by at
///        least one more character, with no further colon.
///
/// Rejects the drive-relative "C:name" (resolved against that drive's CURRENT directory),
/// the bare volume root "C:/" (nothing on a whole volume may become a target), and an
/// alternate-data-stream suffix ("C:/dir/f.txt:payload.exe", which would otherwise satisfy
/// an extension check). @p slashPath must already be in screeningPathForm.
[[nodiscard]] inline bool driveQualifiedPath(const QString& slashPath) {
    constexpr int kDriveRootLength = 3;  // "X:/"
    if (slashPath.size() <= kDriveRootLength || !slashPath.at(0).isLetter() ||
        slashPath.at(1) != QLatin1Char(':') || slashPath.at(2) != QLatin1Char('/')) {
        return false;
    }
    return slashPath.indexOf(QLatin1Char(':'), kDriveRootLength) < 0;
}

/// @brief The combined screen every untrusted path token must pass before it may name
///        something this app executes or deletes: non-empty, no unexpanded "%VAR%",
///        literal segments only, and a drive-qualified local root that is not a volume
///        root. Says nothing about the filesystem -- existence, reparse points and
///        ownership are the caller's additional, boundary-specific checks.
[[nodiscard]] inline bool literalLocalPathTrusted(const QString& raw) {
    const QString trimmed = raw.trimmed();
    // CreateProcess and the file APIs do not expand "%VAR%", so a token still carrying one
    // does not name the target its author intended.
    if (trimmed.isEmpty() || trimmed.contains(QLatin1Char('%'))) {
        return false;
    }
    const QString path = screeningPathForm(trimmed);
    return pathSegmentsAreLiteral(path) && driveQualifiedPath(path);
}

}  // namespace sak
