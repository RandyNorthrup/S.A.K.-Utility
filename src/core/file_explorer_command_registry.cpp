// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_command_registry.h"

namespace sak {
namespace {

FileExplorerCommand makeCommand(const FileExplorerCommandId id,
                                const QString& text,
                                const QString& status_text,
                                const QString& shortcut = {},
                                const bool destructive = false,
                                const bool selection_required = false,
                                const bool write_operation = false) {
    return FileExplorerCommand{id, text, text, status_text, shortcut, destructive,
                               selection_required, write_operation};
}

bool hasSelectedTarget(const FileExplorerCommandContext& context) {
    return !FileExplorerTargetId::fromTarget(context.target).isEmpty() &&
           !context.target.root_path.trimmed().isEmpty();
}

QString joinedTargetBlockers(const FileManagementTarget& target) {
    return target.blockers.join(QStringLiteral("; "));
}

QString browseBlocker(const FileManagementTarget& target) {
    const QString target_blocker = joinedTargetBlockers(target);
    if (!target_blocker.isEmpty()) {
        return target_blocker;
    }
    return QStringLiteral("Selected target cannot browse files.");
}

QString readBlocker(const FileManagementTarget& target) {
    const QString target_blocker = joinedTargetBlockers(target);
    if (!target_blocker.isEmpty()) {
        return target_blocker;
    }
    return QStringLiteral("Selected target cannot read files.");
}

QString writeBlocker(const FileManagementTarget& target) {
    const QString target_blocker = joinedTargetBlockers(target);
    if (!target_blocker.isEmpty()) {
        return target_blocker;
    }
    if (target.read_only) {
        return QStringLiteral("Selected target is read-only.");
    }
    return QStringLiteral("Selected target does not allow file writes.");
}

FileExplorerCommandState disabledState(const FileExplorerCommand& command, const QString& blocker) {
    FileExplorerCommandState state;
    state.command = command;
    state.enabled = false;
    state.blocker = blocker;
    return state;
}

FileExplorerCommandState enabledState(const FileExplorerCommand& command) {
    FileExplorerCommandState state;
    state.command = command;
    state.enabled = true;
    return state;
}

}  // namespace

QVector<FileExplorerCommand> FileExplorerCommandRegistry::commands() {
    return {
        makeCommand(FileExplorerCommandId::Open, QStringLiteral("Open"),
                    QStringLiteral("Open selected item."), QStringLiteral("Enter"), false, true),
        makeCommand(FileExplorerCommandId::OpenInNewTab, QStringLiteral("Open in New Tab"),
                    QStringLiteral("Open selected item in a new explorer tab."), {}, false, true),
        makeCommand(FileExplorerCommandId::OpenInSecondPane, QStringLiteral("Open in Second Pane"),
                    QStringLiteral("Open selected item in the inactive pane."), {}, false, true),
        makeCommand(FileExplorerCommandId::Back, QStringLiteral("Back"),
                    QStringLiteral("Go to previous location."), QStringLiteral("Alt+Left")),
        makeCommand(FileExplorerCommandId::Forward, QStringLiteral("Forward"),
                    QStringLiteral("Go to next location."), QStringLiteral("Alt+Right")),
        makeCommand(FileExplorerCommandId::Up, QStringLiteral("Up"),
                    QStringLiteral("Go to parent folder."), QStringLiteral("Alt+Up")),
        makeCommand(FileExplorerCommandId::Home, QStringLiteral("Home"),
                    QStringLiteral("Go to target root."), QStringLiteral("Alt+Home")),
        makeCommand(FileExplorerCommandId::Refresh, QStringLiteral("Refresh"),
                    QStringLiteral("Refresh current folder."), QStringLiteral("F5")),
        makeCommand(FileExplorerCommandId::CopyPath, QStringLiteral("Copy Path"),
                    QStringLiteral("Copy current folder path.")),
        makeCommand(FileExplorerCommandId::CopyItemPath, QStringLiteral("Copy Item Path"),
                    QStringLiteral("Copy selected item path."), QStringLiteral("Ctrl+Shift+C"), false, true),
        makeCommand(FileExplorerCommandId::Preview, QStringLiteral("Preview"),
                    QStringLiteral("Preview selected item."), QStringLiteral("Space"), false, true),
        makeCommand(FileExplorerCommandId::Properties, QStringLiteral("Properties"),
                    QStringLiteral("Show selected item properties."), QStringLiteral("Alt+Enter"), false, true),
        makeCommand(FileExplorerCommandId::SelectAll, QStringLiteral("Select All"),
                    QStringLiteral("Select every item in the current folder."), QStringLiteral("Ctrl+A")),
        makeCommand(FileExplorerCommandId::ClearSelection, QStringLiteral("Clear Selection"),
                    QStringLiteral("Clear current selection."), QStringLiteral("Esc")),
        makeCommand(FileExplorerCommandId::InvertSelection, QStringLiteral("Invert Selection"),
                    QStringLiteral("Invert current folder selection.")),
        makeCommand(FileExplorerCommandId::NewFolder, QStringLiteral("New Folder"),
                    QStringLiteral("Create a folder in the current location."),
                    QStringLiteral("Ctrl+Shift+N"), false, false, true),
        makeCommand(FileExplorerCommandId::WriteFile, QStringLiteral("Write File"),
                    QStringLiteral("Import or write a file into the current location."),
                    QStringLiteral("Ctrl+V"), false, false, true),
        makeCommand(FileExplorerCommandId::Rename, QStringLiteral("Rename"),
                    QStringLiteral("Rename selected item."), QStringLiteral("F2"), false, true, true),
        makeCommand(FileExplorerCommandId::Delete, QStringLiteral("Delete"),
                    QStringLiteral("Delete selected item."), QStringLiteral("Delete"), true, true, true),
        makeCommand(FileExplorerCommandId::ToggleHiddenItems, QStringLiteral("Hidden Items"),
                    QStringLiteral("Toggle hidden item display."), QStringLiteral("Ctrl+H")),
        makeCommand(FileExplorerCommandId::ToggleFileExtensions, QStringLiteral("File Extensions"),
                    QStringLiteral("Toggle file extension display.")),
        makeCommand(FileExplorerCommandId::ViewDetails, QStringLiteral("Details"),
                    QStringLiteral("Switch to details view."), QStringLiteral("Ctrl+Shift+1")),
        makeCommand(FileExplorerCommandId::ViewList, QStringLiteral("List"),
                    QStringLiteral("Switch to list view."), QStringLiteral("Ctrl+Shift+2")),
        makeCommand(FileExplorerCommandId::ViewGrid, QStringLiteral("Grid"),
                    QStringLiteral("Switch to grid view."), QStringLiteral("Ctrl+Shift+3")),
        makeCommand(FileExplorerCommandId::ViewCards, QStringLiteral("Cards"),
                    QStringLiteral("Switch to cards view."), QStringLiteral("Ctrl+Shift+4")),
        makeCommand(FileExplorerCommandId::ViewColumns, QStringLiteral("Columns"),
                    QStringLiteral("Switch to columns view."), QStringLiteral("Ctrl+Shift+5")),
        makeCommand(FileExplorerCommandId::ViewAdaptive, QStringLiteral("Adaptive"),
                    QStringLiteral("Switch to adaptive icon view."), QStringLiteral("Ctrl+Shift+6")),
        makeCommand(FileExplorerCommandId::TogglePreviewPane, QStringLiteral("Preview Pane"),
                    QStringLiteral("Toggle preview and details pane."), QStringLiteral("Ctrl+Alt+I")),
        makeCommand(FileExplorerCommandId::ToggleDualPane, QStringLiteral("Dual Pane"),
                    QStringLiteral("Toggle dual-pane explorer layout."), QStringLiteral("Ctrl+Alt+D")),
    };
}

FileExplorerCommand FileExplorerCommandRegistry::command(const FileExplorerCommandId id) {
    const QVector<FileExplorerCommand> registry = commands();
    for (const FileExplorerCommand& entry : registry) {
        if (entry.id == id) {
            return entry;
        }
    }
    return makeCommand(id, commandIdName(id), QStringLiteral("Unknown File Explorer command."));
}

FileExplorerCommandState FileExplorerCommandRegistry::state(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context) {
    const FileExplorerCommand entry = command(id);
    const bool selected_target = hasSelectedTarget(context);
    const bool has_selection = !context.pane.selection.isEmpty();
    const bool has_single_selection = context.pane.selection.hasSingleEntry();

    if (id == FileExplorerCommandId::OpenInNewTab && !context.can_create_tabs) {
        return disabledState(entry, QStringLiteral("Explorer tabs are unavailable in this build."));
    }
    if ((id == FileExplorerCommandId::OpenInSecondPane ||
         id == FileExplorerCommandId::ToggleDualPane) &&
        !context.can_use_dual_pane) {
        return disabledState(entry, QStringLiteral("Dual pane is unavailable in this build."));
    }
    switch (id) {
    case FileExplorerCommandId::Back:
        return context.pane.canGoBack()
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("No previous location."));
    case FileExplorerCommandId::Forward:
        return context.pane.canGoForward()
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("No next location."));
    case FileExplorerCommandId::Up:
        if (!selected_target) {
            return disabledState(entry, QStringLiteral("No File Explorer target selected."));
        }
        return !context.pane.location.atRoot(context.target.local_file_system)
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("Already at target root."));
    case FileExplorerCommandId::ClearSelection:
        return has_selection ? enabledState(entry)
                             : disabledState(entry, QStringLiteral("No selection to clear."));
    default:
        break;
    }

    if (!selected_target) {
        return disabledState(entry, QStringLiteral("No File Explorer target selected."));
    }

    if (entry.selection_required && !has_selection) {
        return disabledState(entry, QStringLiteral("Select an item first."));
    }

    switch (id) {
    case FileExplorerCommandId::Open:
    case FileExplorerCommandId::OpenInNewTab:
    case FileExplorerCommandId::OpenInSecondPane:
    case FileExplorerCommandId::Preview:
    case FileExplorerCommandId::Properties:
    case FileExplorerCommandId::Rename:
        if (entry.selection_required && !has_single_selection) {
            return disabledState(entry, QStringLiteral("Select one item."));
        }
        break;
    default:
        break;
    }

    if (entry.write_operation && !context.target.can_write_files) {
        return disabledState(entry, writeBlocker(context.target));
    }

    switch (id) {
    case FileExplorerCommandId::Open:
    case FileExplorerCommandId::OpenInNewTab:
    case FileExplorerCommandId::OpenInSecondPane:
        if (!context.target.can_browse) {
            return disabledState(entry, browseBlocker(context.target));
        }
        break;
    case FileExplorerCommandId::Preview:
        if (!context.target.can_read_files) {
            return disabledState(entry, readBlocker(context.target));
        }
        break;
    case FileExplorerCommandId::Home:
    case FileExplorerCommandId::Refresh:
    case FileExplorerCommandId::SelectAll:
    case FileExplorerCommandId::InvertSelection:
    case FileExplorerCommandId::ToggleHiddenItems:
    case FileExplorerCommandId::ToggleFileExtensions:
    case FileExplorerCommandId::ViewDetails:
    case FileExplorerCommandId::ViewList:
    case FileExplorerCommandId::ViewGrid:
    case FileExplorerCommandId::ViewCards:
    case FileExplorerCommandId::ViewColumns:
    case FileExplorerCommandId::ViewAdaptive:
    case FileExplorerCommandId::TogglePreviewPane:
    case FileExplorerCommandId::CopyPath:
    case FileExplorerCommandId::CopyItemPath:
    case FileExplorerCommandId::Properties:
    case FileExplorerCommandId::NewFolder:
    case FileExplorerCommandId::WriteFile:
    case FileExplorerCommandId::Rename:
    case FileExplorerCommandId::Delete:
    case FileExplorerCommandId::ToggleDualPane:
    case FileExplorerCommandId::Back:
    case FileExplorerCommandId::Forward:
    case FileExplorerCommandId::Up:
        break;
    }

    return enabledState(entry);
}

QString FileExplorerCommandRegistry::commandIdName(const FileExplorerCommandId id) {
    switch (id) {
    case FileExplorerCommandId::Open:
        return QStringLiteral("open");
    case FileExplorerCommandId::OpenInNewTab:
        return QStringLiteral("open-new-tab");
    case FileExplorerCommandId::OpenInSecondPane:
        return QStringLiteral("open-second-pane");
    case FileExplorerCommandId::Back:
        return QStringLiteral("back");
    case FileExplorerCommandId::Forward:
        return QStringLiteral("forward");
    case FileExplorerCommandId::Up:
        return QStringLiteral("up");
    case FileExplorerCommandId::Home:
        return QStringLiteral("home");
    case FileExplorerCommandId::Refresh:
        return QStringLiteral("refresh");
    case FileExplorerCommandId::CopyPath:
        return QStringLiteral("copy-path");
    case FileExplorerCommandId::CopyItemPath:
        return QStringLiteral("copy-item-path");
    case FileExplorerCommandId::Preview:
        return QStringLiteral("preview");
    case FileExplorerCommandId::Properties:
        return QStringLiteral("properties");
    case FileExplorerCommandId::SelectAll:
        return QStringLiteral("select-all");
    case FileExplorerCommandId::ClearSelection:
        return QStringLiteral("clear-selection");
    case FileExplorerCommandId::InvertSelection:
        return QStringLiteral("invert-selection");
    case FileExplorerCommandId::NewFolder:
        return QStringLiteral("new-folder");
    case FileExplorerCommandId::WriteFile:
        return QStringLiteral("write-file");
    case FileExplorerCommandId::Rename:
        return QStringLiteral("rename");
    case FileExplorerCommandId::Delete:
        return QStringLiteral("delete");
    case FileExplorerCommandId::ToggleHiddenItems:
        return QStringLiteral("toggle-hidden-items");
    case FileExplorerCommandId::ToggleFileExtensions:
        return QStringLiteral("toggle-file-extensions");
    case FileExplorerCommandId::ViewDetails:
        return QStringLiteral("view-details");
    case FileExplorerCommandId::ViewList:
        return QStringLiteral("view-list");
    case FileExplorerCommandId::ViewGrid:
        return QStringLiteral("view-grid");
    case FileExplorerCommandId::ViewCards:
        return QStringLiteral("view-cards");
    case FileExplorerCommandId::ViewColumns:
        return QStringLiteral("view-columns");
    case FileExplorerCommandId::ViewAdaptive:
        return QStringLiteral("view-adaptive");
    case FileExplorerCommandId::TogglePreviewPane:
        return QStringLiteral("toggle-preview-pane");
    case FileExplorerCommandId::ToggleDualPane:
        return QStringLiteral("toggle-dual-pane");
    }

    return QStringLiteral("unknown");
}

}  // namespace sak
