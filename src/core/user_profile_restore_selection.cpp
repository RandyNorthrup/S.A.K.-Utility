// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/user_profile_restore_selection.h"

namespace sak {

void applyFolderRestoreSelections(BackupManifest& manifest,
                                  const QVector<FolderRestoreChoice>& choices) {
    for (BackupUserData& user : manifest.users) {
        for (FolderSelection& folder : user.backed_up_folders) {
            for (const FolderRestoreChoice& choice : choices) {
                if (choice.username == user.username &&
                    choice.relative_path == folder.relative_path) {
                    folder.selected = choice.selected;
                    break;
                }
            }
        }
    }
}

int selectedFolderCount(const BackupManifest& manifest, const QString& username) {
    int count = 0;
    for (const BackupUserData& user : manifest.users) {
        if (user.username != username) {
            continue;
        }
        for (const FolderSelection& folder : user.backed_up_folders) {
            if (folder.selected) {
                ++count;
            }
        }
    }
    return count;
}

}  // namespace sak
