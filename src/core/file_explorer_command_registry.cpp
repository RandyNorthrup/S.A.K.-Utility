// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_command_registry.h"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace sak {
namespace {

struct MakeCommandFlags {
    bool destructive = false;
    bool selection_required = false;
    bool write_operation = false;
};

FileExplorerCommand makeCommand(const FileExplorerCommandId id,
                                const QString& text,
                                const QString& status_text,
                                const QString& shortcut = {},
                                const MakeCommandFlags flags = {}) {
    return FileExplorerCommand{id,
                               text,
                               text,
                               status_text,
                               shortcut,
                               flags.destructive,
                               flags.selection_required,
                               flags.write_operation};
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

// Build-time availability overrides (tabs / dual pane unavailable in this build).
std::optional<FileExplorerCommandState> buildAvailabilityState(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context,
    const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (id == OpenInNewTab && !context.can_create_tabs) {
        return disabledState(entry, QStringLiteral("Explorer tabs are unavailable in this build."));
    }
    if ((id == OpenInSecondPane || id == ToggleDualPane) && !context.can_use_dual_pane) {
        return disabledState(entry, QStringLiteral("Dual pane is unavailable in this build."));
    }
    return std::nullopt;
}

// Navigation commands resolve from pane history/location independent of selection.
std::optional<FileExplorerCommandState> navigationOverrideState(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context,
    const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    switch (id) {
    case Back:
        return context.pane.canGoBack()
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("No previous location."));
    case Forward:
        return context.pane.canGoForward()
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("No next location."));
    case Up:
        if (!hasSelectedTarget(context)) {
            return disabledState(entry, QStringLiteral("No File Explorer target selected."));
        }
        return !context.pane.location.atRoot(context.target.local_file_system)
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("Already at target root."));
    case ClearSelection:
        return context.pane.selection.isEmpty()
                   ? disabledState(entry, QStringLiteral("No selection to clear."))
                   : enabledState(entry);
    default:
        return std::nullopt;
    }
}

bool requiresSingleSelection(const FileExplorerCommandId id) {
    using enum FileExplorerCommandId;
    static constexpr auto kSingleSelectionCommands =
        std::to_array({Open, OpenInNewTab, OpenInSecondPane, Preview, Properties, Rename});
    return std::ranges::find(kSingleSelectionCommands, id) != kSingleSelectionCommands.end();
}

// Selection-count requirements for commands that act on selected items.
std::optional<FileExplorerCommandState> selectionRequirementState(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context,
    const FileExplorerCommand& entry) {
    if (entry.selection_required && context.pane.selection.isEmpty()) {
        return disabledState(entry, QStringLiteral("Select an item first."));
    }
    if (requiresSingleSelection(id) && entry.selection_required &&
        !context.pane.selection.hasSingleEntry()) {
        return disabledState(entry, QStringLiteral("Select one item."));
    }
    return std::nullopt;
}

// Target capability requirements (write / browse / read).
std::optional<FileExplorerCommandState> capabilityState(const FileExplorerCommandId id,
                                                        const FileExplorerCommandContext& context,
                                                        const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (entry.write_operation && !context.target.can_write_files) {
        return disabledState(entry, writeBlocker(context.target));
    }
    if (id == Open || id == OpenInNewTab || id == OpenInSecondPane) {
        if (!context.target.can_browse) {
            return disabledState(entry, browseBlocker(context.target));
        }
    } else if (id == Preview && !context.target.can_read_files) {
        return disabledState(entry, readBlocker(context.target));
    }
    return std::nullopt;
}


// Open + pane navigation commands (resolve from selection / pane history).
QVector<FileExplorerCommand> openAndNavigationCommands() {
    return {
        makeCommand(FileExplorerCommandId::Open,
                    QStringLiteral("Open"),
                    QStringLiteral("Open selected item."),
                    QStringLiteral("Enter"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::OpenInNewTab,
                    QStringLiteral("Open in New Tab"),
                    QStringLiteral("Open selected item in a new explorer tab."),
                    {},
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::OpenInSecondPane,
                    QStringLiteral("Open in Second Pane"),
                    QStringLiteral("Open selected item in the inactive pane."),
                    {},
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::Back,
                    QStringLiteral("Back"),
                    QStringLiteral("Go to previous location."),
                    QStringLiteral("Alt+Left")),
        makeCommand(FileExplorerCommandId::Forward,
                    QStringLiteral("Forward"),
                    QStringLiteral("Go to next location."),
                    QStringLiteral("Alt+Right")),
        makeCommand(FileExplorerCommandId::Up,
                    QStringLiteral("Up"),
                    QStringLiteral("Go to parent folder."),
                    QStringLiteral("Alt+Up")),
        makeCommand(FileExplorerCommandId::Home,
                    QStringLiteral("Home"),
                    QStringLiteral("Go to target root."),
                    QStringLiteral("Alt+Home")),
        makeCommand(FileExplorerCommandId::Refresh,
                    QStringLiteral("Refresh"),
                    QStringLiteral("Refresh current folder."),
                    QStringLiteral("F5")),
    };
}

// Clipboard, preview, and selection commands.
QVector<FileExplorerCommand> clipboardAndSelectionCommands() {
    return {
        makeCommand(FileExplorerCommandId::CopyPath,
                    QStringLiteral("Copy Path"),
                    QStringLiteral("Copy current folder path.")),
        makeCommand(FileExplorerCommandId::CopyItemPath,
                    QStringLiteral("Copy Item Path"),
                    QStringLiteral("Copy selected item path."),
                    QStringLiteral("Ctrl+Shift+C"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::Preview,
                    QStringLiteral("Preview"),
                    QStringLiteral("Preview selected item."),
                    QStringLiteral("Space"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::Properties,
                    QStringLiteral("Properties"),
                    QStringLiteral("Show selected item properties."),
                    QStringLiteral("Alt+Enter"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::SelectAll,
                    QStringLiteral("Select All"),
                    QStringLiteral("Select every item in the current folder."),
                    QStringLiteral("Ctrl+A")),
        makeCommand(FileExplorerCommandId::ClearSelection,
                    QStringLiteral("Clear Selection"),
                    QStringLiteral("Clear current selection."),
                    QStringLiteral("Esc")),
        makeCommand(FileExplorerCommandId::InvertSelection,
                    QStringLiteral("Invert Selection"),
                    QStringLiteral("Invert current folder selection.")),
    };
}

// Write / destructive commands that mutate the target.
QVector<FileExplorerCommand> writeCommands() {
    return {
        makeCommand(FileExplorerCommandId::NewFolder,
                    QStringLiteral("New Folder"),
                    QStringLiteral("Create a folder in the current location."),
                    QStringLiteral("Ctrl+Shift+N"),
                    {.write_operation = true}),
        makeCommand(FileExplorerCommandId::WriteFile,
                    QStringLiteral("Write File"),
                    QStringLiteral("Import or write a file into the current location."),
                    QStringLiteral("Ctrl+V"),
                    {.write_operation = true}),
        makeCommand(FileExplorerCommandId::Rename,
                    QStringLiteral("Rename"),
                    QStringLiteral("Rename selected item."),
                    QStringLiteral("F2"),
                    {.selection_required = true, .write_operation = true}),
        makeCommand(FileExplorerCommandId::Delete,
                    QStringLiteral("Delete"),
                    QStringLiteral("Delete selected item."),
                    QStringLiteral("Delete"),
                    {.destructive = true, .selection_required = true, .write_operation = true}),
    };
}

// View / display toggle commands.
QVector<FileExplorerCommand> viewCommands() {
    return {
        makeCommand(FileExplorerCommandId::ToggleHiddenItems,
                    QStringLiteral("Hidden Items"),
                    QStringLiteral("Toggle hidden item display."),
                    QStringLiteral("Ctrl+H")),
        makeCommand(FileExplorerCommandId::ToggleFileExtensions,
                    QStringLiteral("File Extensions"),
                    QStringLiteral("Toggle file extension display.")),
        makeCommand(FileExplorerCommandId::ViewDetails,
                    QStringLiteral("Details"),
                    QStringLiteral("Switch to details view."),
                    QStringLiteral("Ctrl+Shift+1")),
        makeCommand(FileExplorerCommandId::ViewList,
                    QStringLiteral("List"),
                    QStringLiteral("Switch to list view."),
                    QStringLiteral("Ctrl+Shift+2")),
        makeCommand(FileExplorerCommandId::ViewGrid,
                    QStringLiteral("Grid"),
                    QStringLiteral("Switch to grid view."),
                    QStringLiteral("Ctrl+Shift+3")),
        makeCommand(FileExplorerCommandId::ViewCards,
                    QStringLiteral("Cards"),
                    QStringLiteral("Switch to cards view."),
                    QStringLiteral("Ctrl+Shift+4")),
        makeCommand(FileExplorerCommandId::ViewColumns,
                    QStringLiteral("Columns"),
                    QStringLiteral("Switch to columns view."),
                    QStringLiteral("Ctrl+Shift+5")),
        makeCommand(FileExplorerCommandId::ViewAdaptive,
                    QStringLiteral("Adaptive"),
                    QStringLiteral("Switch to adaptive icon view."),
                    QStringLiteral("Ctrl+Shift+6")),
        makeCommand(FileExplorerCommandId::TogglePreviewPane,
                    QStringLiteral("Preview Pane"),
                    QStringLiteral("Toggle preview and details pane."),
                    QStringLiteral("Ctrl+Alt+I")),
        makeCommand(FileExplorerCommandId::ToggleDualPane,
                    QStringLiteral("Dual Pane"),
                    QStringLiteral("Toggle dual-pane explorer layout."),
                    QStringLiteral("Ctrl+Alt+D")),
    };
}

}  // namespace

QVector<FileExplorerCommand> FileExplorerCommandRegistry::commands() {
    QVector<FileExplorerCommand> registry;
    registry.append(openAndNavigationCommands());
    registry.append(clipboardAndSelectionCommands());
    registry.append(writeCommands());
    registry.append(viewCommands());
    return registry;
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
    const FileExplorerCommandId id, const FileExplorerCommandContext& context) {
    const FileExplorerCommand entry = command(id);
    if (const auto availability = buildAvailabilityState(id, context, entry)) {
        return *availability;
    }
    if (const auto navigation = navigationOverrideState(id, context, entry)) {
        return *navigation;
    }
    if (!hasSelectedTarget(context)) {
        return disabledState(entry, QStringLiteral("No File Explorer target selected."));
    }
    if (const auto selection = selectionRequirementState(id, context, entry)) {
        return *selection;
    }
    if (const auto capability = capabilityState(id, context, entry)) {
        return *capability;
    }
    return enabledState(entry);
}

QString FileExplorerCommandRegistry::commandIdName(const FileExplorerCommandId id) {
    using Id = FileExplorerCommandId;
    static constexpr auto kNames = std::to_array<std::pair<Id, const char*>>({
        {Id::Open, "open"},
        {Id::OpenInNewTab, "open-new-tab"},
        {Id::OpenInSecondPane, "open-second-pane"},
        {Id::Back, "back"},
        {Id::Forward, "forward"},
        {Id::Up, "up"},
        {Id::Home, "home"},
        {Id::Refresh, "refresh"},
        {Id::CopyPath, "copy-path"},
        {Id::CopyItemPath, "copy-item-path"},
        {Id::Preview, "preview"},
        {Id::Properties, "properties"},
        {Id::SelectAll, "select-all"},
        {Id::ClearSelection, "clear-selection"},
        {Id::InvertSelection, "invert-selection"},
        {Id::NewFolder, "new-folder"},
        {Id::WriteFile, "write-file"},
        {Id::Rename, "rename"},
        {Id::Delete, "delete"},
        {Id::ToggleHiddenItems, "toggle-hidden-items"},
        {Id::ToggleFileExtensions, "toggle-file-extensions"},
        {Id::ViewDetails, "view-details"},
        {Id::ViewList, "view-list"},
        {Id::ViewGrid, "view-grid"},
        {Id::ViewCards, "view-cards"},
        {Id::ViewColumns, "view-columns"},
        {Id::ViewAdaptive, "view-adaptive"},
        {Id::TogglePreviewPane, "toggle-preview-pane"},
        {Id::ToggleDualPane, "toggle-dual-pane"},
    });
    const auto it = std::ranges::find(kNames, id, &std::pair<Id, const char*>::first);
    return it != kNames.end() ? QString::fromLatin1(it->second) : QStringLiteral("unknown");
}

}  // namespace sak
