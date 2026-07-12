// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_command_registry.h
/// @brief Shared File Explorer command metadata and enablement rules.

#pragma once

#include "sak/file_explorer_types.h"

#include <QString>
#include <QVector>

namespace sak {

enum class FileExplorerCommandId {
    Open,
    OpenInNewTab,
    OpenInSecondPane,
    Back,
    Forward,
    Up,
    Home,
    Refresh,
    CopyPath,
    CopyItemPath,
    Preview,
    Properties,
    SelectAll,
    ClearSelection,
    InvertSelection,
    NewFolder,
    WriteFile,
    Rename,
    Delete,
    ToggleHiddenItems,
    ToggleFileExtensions,
    ViewDetails,
    ViewList,
    ViewGrid,
    ViewCards,
    ViewColumns,
    ViewAdaptive,
    TogglePreviewPane,
    ToggleDualPane,
    DuplicateTab,
    ReopenClosedTab,
    Hash,
    CopyOut,
    CopyItems,
    Paste,
};

/// Command-palette section a command belongs to. Palette rows render grouped
/// under these headers, in declaration order.
enum class FileExplorerCommandGroup {
    Navigation,
    File,
    View,
    Pane,
    Target,
    Safety,
};

struct FileExplorerCommand {
    FileExplorerCommandId id{FileExplorerCommandId::Open};
    QString text;
    QString accessible_name;
    QString status_text;
    QString shortcut;
    FileExplorerCommandGroup group{FileExplorerCommandGroup::Navigation};
    bool destructive{false};
    bool selection_required{false};
    bool write_operation{false};
};

struct FileExplorerCommandContext {
    FileManagementTarget target;
    FileExplorerPaneState pane;
    bool can_create_tabs{false};
    bool can_use_dual_pane{false};
    bool has_closed_tab{false};       ///< True when a recently closed tab can be reopened.
    bool clipboard_has_files{false};  ///< True when the clipboard holds pasteable file items.
};

struct FileExplorerCommandState {
    FileExplorerCommand command;
    bool visible{true};
    bool enabled{false};
    QString blocker;
};

class FileExplorerCommandRegistry {
public:
    [[nodiscard]] static QVector<FileExplorerCommand> commands();
    [[nodiscard]] static FileExplorerCommand command(FileExplorerCommandId id);
    [[nodiscard]] static FileExplorerCommandState state(FileExplorerCommandId id,
                                                        const FileExplorerCommandContext& context);
    [[nodiscard]] static QString commandIdName(FileExplorerCommandId id);
    [[nodiscard]] static FileExplorerCommandGroup group(FileExplorerCommandId id);
    [[nodiscard]] static QString groupName(FileExplorerCommandGroup group);
    [[nodiscard]] static QVector<FileExplorerCommandGroup> groupOrder();
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileExplorerCommandId)
