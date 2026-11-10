// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_session_store.h
/// @brief Persist and restore the File Explorer tab session across app restarts.

#pragma once

#include "sak/file_explorer_types.h"

#include <QString>
#include <QVector>

class QSettings;

namespace sak {

/// A restorable snapshot of the explorer tab strip: the ordered tab states plus
/// the index of the tab that was active. Only the durable parts of each tab are
/// stored (title, per-pane target id / path / view settings, split arrangement);
/// transient back/forward history and selection are intentionally dropped.
struct FileExplorerTabSession {
    QVector<FileExplorerTabState> tabs;
    int active_index{0};

    [[nodiscard]] bool isEmpty() const { return tabs.isEmpty(); }
};

class FileExplorerSessionStore {
public:
    /// Write the session under @p group in @p settings, replacing any prior value.
    static void save(QSettings& settings,
                     const QString& group,
                     const FileExplorerTabSession& session);

    /// Read the session stored under @p group. Returns an empty session when none
    /// exists. The active index is clamped into range.
    [[nodiscard]] static FileExplorerTabSession load(QSettings& settings, const QString& group);

    /// Remove any stored session under @p group.
    static void clear(QSettings& settings, const QString& group);
};

}  // namespace sak
