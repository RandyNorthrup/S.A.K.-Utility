// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file user_profile_restore_selection.h
/// @brief Bridge-free helpers that map restore-wizard folder selections onto a
/// backup manifest. Pure logic (no Qt widgets) so it can be unit tested headless.

#pragma once

#include "sak/user_profile_types.h"

#include <QString>
#include <QVector>

namespace sak {

/// @brief One folder's restore selection, keyed by owning user + relative path.
struct FolderRestoreChoice {
    QString username;
    QString relative_path;
    bool selected{true};
};

/// @brief Apply per-folder restore selections onto a manifest in place.
///
/// Each folder in manifest.users[].backed_up_folders is matched to a choice by
/// (username, relative_path); a matched folder's `selected` is set from the
/// choice. Folders with no matching choice keep their existing `selected` value
/// (fail-safe: an unmatched folder is never silently dropped).
void applyFolderRestoreSelections(BackupManifest& manifest,
                                  const QVector<FolderRestoreChoice>& choices);

/// @brief Count folders that would actually be restored for `username`.
/// @return number of backed_up_folders whose `selected` is true (0 if the user
///         is absent from the manifest).
int selectedFolderCount(const BackupManifest& manifest, const QString& username);

}  // namespace sak
