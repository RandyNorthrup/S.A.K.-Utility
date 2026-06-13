// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_types.h
/// @brief File Management explorer state and view model types.

#pragma once

#include "sak/file_management_file_system.h"
#include "sak/layout_constants.h"

#include <QString>
#include <QStringList>
#include <QVector>
#include <Qt>

#include <cstdint>

namespace sak {

enum class FileExplorerViewMode {
    Details,
    List,
    Grid,
    Cards,
    Columns,
    Adaptive,
};

enum class FileExplorerPaneSplit {
    None,
    Vertical,
    Horizontal,
};

struct FileExplorerTargetId {
    QString value;

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] static FileExplorerTargetId fromTarget(const FileManagementTarget& target);

    [[nodiscard]] friend bool operator==(const FileExplorerTargetId& lhs,
                                         const FileExplorerTargetId& rhs) {
        return lhs.value == rhs.value;
    }

    [[nodiscard]] friend bool operator!=(const FileExplorerTargetId& lhs,
                                         const FileExplorerTargetId& rhs) {
        return !(lhs == rhs);
    }
};

struct FileExplorerLocation {
    FileExplorerTargetId target_id;
    QString path;

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] FileExplorerLocation normalized(bool local_file_system) const;
    [[nodiscard]] bool atRoot(bool local_file_system) const;

    [[nodiscard]] static QString normalizePath(const QString& path, bool local_file_system);
    [[nodiscard]] static QString parentPath(const QString& path, bool local_file_system);

    [[nodiscard]] friend bool operator==(const FileExplorerLocation& lhs,
                                         const FileExplorerLocation& rhs) {
        return lhs.target_id == rhs.target_id && lhs.path == rhs.path;
    }

    [[nodiscard]] friend bool operator!=(const FileExplorerLocation& lhs,
                                         const FileExplorerLocation& rhs) {
        return !(lhs == rhs);
    }
};

struct FileExplorerSelection {
    QVector<FileManagementEntry> entries;

    void clear();

    [[nodiscard]] bool isEmpty() const;
    [[nodiscard]] bool hasSingleEntry() const;
    [[nodiscard]] int count() const;
    [[nodiscard]] bool containsDirectory() const;
    [[nodiscard]] bool containsRegularFile() const;
    [[nodiscard]] uint64_t totalRegularFileBytes() const;
    [[nodiscard]] QStringList paths() const;
};

struct FileExplorerItemCapabilities {
    bool can_open{false};
    bool can_preview{false};
    bool can_copy_path{false};
    bool can_rename{false};
    bool can_delete{false};
    QStringList blockers;

    [[nodiscard]] static FileExplorerItemCapabilities fromTargetAndEntry(
        const FileManagementTarget& target,
        const FileManagementEntry& entry);
};

struct FileExplorerViewSettings {
    FileExplorerViewMode mode{FileExplorerViewMode::Details};
    bool show_hidden{false};
    bool show_extensions{true};
    int item_size_px{kFileExplorerItemSizeDefault};
    QString sort_key{QStringLiteral("name")};
    Qt::SortOrder sort_order{Qt::AscendingOrder};
};

struct FileExplorerPaneState {
    FileExplorerLocation location;
    QVector<FileExplorerLocation> back_stack;
    QVector<FileExplorerLocation> forward_stack;
    FileExplorerSelection selection;
    FileExplorerViewSettings view;

    [[nodiscard]] bool canGoBack() const;
    [[nodiscard]] bool canGoForward() const;

    void navigateTo(const FileExplorerLocation& destination, bool local_file_system);
    [[nodiscard]] bool goBack();
    [[nodiscard]] bool goForward();
    [[nodiscard]] bool goUp(bool local_file_system);
};

struct FileExplorerTabState {
    QString title;
    FileExplorerPaneState primary;
    FileExplorerPaneState secondary;
    FileExplorerPaneSplit split{FileExplorerPaneSplit::None};
    bool secondary_pane_enabled{false};
    int active_pane_index{0};

    [[nodiscard]] bool hasSecondaryPane() const;
    void setSecondaryPaneEnabled(bool enabled, FileExplorerPaneSplit requested_split);
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileExplorerViewMode)
Q_DECLARE_METATYPE(sak::FileExplorerPaneSplit)
Q_DECLARE_METATYPE(sak::FileExplorerTargetId)
Q_DECLARE_METATYPE(sak::FileExplorerLocation)
