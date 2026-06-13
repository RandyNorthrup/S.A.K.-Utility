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
};

struct FileExplorerCommand {
    FileExplorerCommandId id{FileExplorerCommandId::Open};
    QString text;
    QString accessible_name;
    QString status_text;
    QString shortcut;
    bool destructive{false};
    bool selection_required{false};
    bool write_operation{false};
};

struct FileExplorerCommandContext {
    FileManagementTarget target;
    FileExplorerPaneState pane;
    bool can_create_tabs{false};
    bool can_use_dual_pane{false};
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
};

}  // namespace sak

Q_DECLARE_METATYPE(sak::FileExplorerCommandId)
