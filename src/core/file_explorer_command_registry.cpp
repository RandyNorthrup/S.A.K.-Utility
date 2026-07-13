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

// Single source of truth mapping each command to its palette section.
FileExplorerCommandGroup groupFor(const FileExplorerCommandId id) {
    using Id = FileExplorerCommandId;
    using Group = FileExplorerCommandGroup;
    static constexpr auto kGroups = std::to_array<std::pair<Id, Group>>({
        {Id::Open, Group::Navigation},
        {Id::OpenInNewTab, Group::Navigation},
        {Id::Back, Group::Navigation},
        {Id::Forward, Group::Navigation},
        {Id::Up, Group::Navigation},
        {Id::Home, Group::Navigation},
        {Id::Refresh, Group::Navigation},
        {Id::NewFolder, Group::File},
        {Id::WriteFile, Group::File},
        {Id::Rename, Group::File},
        {Id::Delete, Group::File},
        {Id::DeletePermanently, Group::File},
        {Id::SelectAll, Group::File},
        {Id::ClearSelection, Group::File},
        {Id::InvertSelection, Group::File},
        {Id::ViewDetails, Group::View},
        {Id::ViewList, Group::View},
        {Id::ViewGrid, Group::View},
        {Id::ViewCards, Group::View},
        {Id::ViewColumns, Group::View},
        {Id::ViewAdaptive, Group::View},
        {Id::ToggleHiddenItems, Group::View},
        {Id::ToggleFileExtensions, Group::View},
        {Id::OpenInSecondPane, Group::Pane},
        {Id::TogglePreviewPane, Group::Pane},
        {Id::ToggleDualPane, Group::Pane},
        {Id::DuplicateTab, Group::Pane},
        {Id::ReopenClosedTab, Group::Pane},
        {Id::Properties, Group::Target},
        {Id::CopyPath, Group::Target},
        {Id::CopyItemPath, Group::Target},
        {Id::Preview, Group::Safety},
        {Id::Hash, Group::Safety},
        {Id::CopyOut, Group::File},
        {Id::CopyItems, Group::File},
        {Id::Paste, Group::File},
        {Id::CopyToOtherPane, Group::Pane},
        {Id::ComparePanes, Group::Pane},
        {Id::ToggleSelect, Group::File},
        {Id::CopyItemPathQuoted, Group::Target},
        {Id::IncreaseSize, Group::View},
        {Id::DecreaseSize, Group::View},
        {Id::FocusOtherPane, Group::Pane},
        {Id::CutItems, Group::File},
        {Id::PasteIntoSelection, Group::File},
        {Id::CreateFolderWithSelection, Group::File},
        {Id::RemoveTags, Group::File},
        {Id::OpenInTerminal, Group::Navigation},
        {Id::EditInNotepad, Group::File},
    });
    const auto it = std::ranges::find(kGroups, id, &std::pair<Id, Group>::first);
    return it != kGroups.end() ? it->second : Group::Navigation;
}

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
                               groupFor(id),
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

// Tab-management commands act on the tab strip, independent of any target.
std::optional<FileExplorerCommandState> tabManagementState(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context,
    const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (id == DuplicateTab) {
        return context.can_create_tabs
                   ? enabledState(entry)
                   : disabledState(entry,
                                   QStringLiteral("Explorer tabs are unavailable in this build."));
    }
    if (id == ReopenClosedTab) {
        if (!context.can_create_tabs) {
            return disabledState(entry,
                                 QStringLiteral("Explorer tabs are unavailable in this build."));
        }
        return context.has_closed_tab
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("No recently closed tab to reopen."));
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
    static constexpr auto kSingleSelectionCommands = std::to_array(
        {Open, OpenInNewTab, OpenInSecondPane, Preview, Properties, Rename, Hash, CopyOut});
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
bool isBrowseCommand(const FileExplorerCommandId id) {
    using enum FileExplorerCommandId;
    return id == Open || id == OpenInNewTab || id == OpenInSecondPane;
}

bool isReadCommand(const FileExplorerCommandId id) {
    using enum FileExplorerCommandId;
    return id == Preview || id == Hash || id == CopyOut || id == CopyItems;
}

// Cross-pane copy needs a readable source and a writable destination. A raw source
// with a raw destination is allowed: the transfer stages through a scratch host
// folder between the certified reader and writer.
FileExplorerCommandState crossPaneCopyState(const FileExplorerCommandContext& context,
                                            const FileExplorerCommand& entry) {
    if (!context.target.can_read_files) {
        return disabledState(entry, readBlocker(context.target));
    }
    if (!context.other_pane_target.can_write_files) {
        return disabledState(entry, writeBlocker(context.other_pane_target));
    }
    return enabledState(entry);
}

// Cross-pane commands need an active split; all but pane focus additionally
// need a target in the inactive pane.
std::optional<FileExplorerCommandState> crossPaneState(const FileExplorerCommandId id,
                                                       const FileExplorerCommandContext& context,
                                                       const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (id != CopyToOtherPane && id != ComparePanes && id != FocusOtherPane) {
        return std::nullopt;
    }
    if (!context.dual_pane_active) {
        return disabledState(entry, QStringLiteral("Enable dual pane first."));
    }
    if (id == FocusOtherPane) {
        return enabledState(entry);
    }
    if (FileExplorerTargetId::fromTarget(context.other_pane_target).isEmpty()) {
        return disabledState(entry, QStringLiteral("Open a target in the other pane first."));
    }
    if (id == ComparePanes) {
        return enabledState(entry);
    }
    return crossPaneCopyState(context, entry);
}

// Files OpenTerminalAction: unmodified folder selections (or none) only.
FileExplorerCommandState openTerminalState(const FileExplorerCommandContext& context,
                                           const FileExplorerCommand& entry) {
    const auto& entries = context.pane.selection.entries;
    const bool all_folders = std::ranges::all_of(entries, [](const FileManagementEntry& item) {
        return item.directory;
    });
    if (!entries.isEmpty() && !all_folders) {
        return disabledState(entry,
                             QStringLiteral("Select folders (or nothing) to open a terminal."));
    }
    return enabledState(entry);
}

// Files EditInNotepadAction: script extensions (.bat/.cmd/.ahk) only.
FileExplorerCommandState editInNotepadState(const FileExplorerCommandContext& context,
                                            const FileExplorerCommand& entry) {
    const bool all_scripts =
        std::ranges::all_of(context.pane.selection.entries, [](const FileManagementEntry& item) {
            return item.regular_file &&
                   (item.name.endsWith(QStringLiteral(".bat"), Qt::CaseInsensitive) ||
                    item.name.endsWith(QStringLiteral(".cmd"), Qt::CaseInsensitive) ||
                    item.name.endsWith(QStringLiteral(".ahk"), Qt::CaseInsensitive));
        });
    if (!all_scripts) {
        return disabledState(entry, QStringLiteral("Select .bat, .cmd, or .ahk script files."));
    }
    return enabledState(entry);
}

// Local-tool commands (Files OpenTerminalAction / EditInNotepadAction) launch host
// programs against host paths, so they exist on mounted local volumes only - a raw
// APFS/HFS path has no host representation to hand the tool. RemoveTags additionally
// needs a selection that actually carries tags (Files RemoveTagsAction.IsExecutable).
std::optional<FileExplorerCommandState> localToolState(const FileExplorerCommandId id,
                                                       const FileExplorerCommandContext& context,
                                                       const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (id == RemoveTags) {
        return context.selection_has_tags
                   ? enabledState(entry)
                   : disabledState(entry, QStringLiteral("Selected items have no tags."));
    }
    if (id != OpenInTerminal && id != EditInNotepad) {
        return std::nullopt;
    }
    if (!context.target.local_file_system) {
        return disabledState(entry, QStringLiteral("Available on mounted local volumes only."));
    }
    return id == OpenInTerminal ? openTerminalState(context, entry)
                                : editInNotepadState(context, entry);
}

// Paste additionally needs pasteable file items on the clipboard. Checked after the
// write-capability gate so a read-only target reports its real write blocker first.
std::optional<FileExplorerCommandState> pasteClipboardState(
    const FileExplorerCommandId id,
    const FileExplorerCommandContext& context,
    const FileExplorerCommand& entry) {
    using enum FileExplorerCommandId;
    if (id != Paste && id != PasteIntoSelection) {
        return std::nullopt;
    }
    if (!context.clipboard_has_files) {
        return disabledState(entry, QStringLiteral("Copy files to the clipboard first."));
    }
    // Files PasteItemToSelectionAction: with a selection present it must be
    // exactly one folder; no selection falls back to the current folder.
    if (id == PasteIntoSelection && !context.pane.selection.isEmpty() &&
        (!context.pane.selection.hasSingleEntry() ||
         !context.pane.selection.entries.first().directory)) {
        return disabledState(entry, QStringLiteral("Select one folder to paste into."));
    }
    return std::nullopt;
}

std::optional<FileExplorerCommandState> capabilityState(const FileExplorerCommandId id,
                                                        const FileExplorerCommandContext& context,
                                                        const FileExplorerCommand& entry) {
    if (entry.write_operation && !context.target.can_write_files) {
        return disabledState(entry, writeBlocker(context.target));
    }
    if (isBrowseCommand(id) && !context.target.can_browse) {
        return disabledState(entry, browseBlocker(context.target));
    }
    if (isReadCommand(id) && !context.target.can_read_files) {
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
        // Files CopyItemPathWithQuotesAction: each selected path wrapped in
        // double quotes, newline-joined.
        makeCommand(FileExplorerCommandId::CopyItemPathQuoted,
                    QStringLiteral("Copy Item Path (Quoted)"),
                    QStringLiteral("Copy selected item path wrapped in quotes."),
                    QStringLiteral("Ctrl+Alt+C"),
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
        makeCommand(FileExplorerCommandId::Hash,
                    QStringLiteral("Hash"),
                    QStringLiteral("Compute the SHA-256 of the selected file."),
                    QStringLiteral("Ctrl+Shift+H"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::CopyOut,
                    QStringLiteral("Copy Out..."),
                    QStringLiteral("Copy the selected file out to a local destination."),
                    QStringLiteral("Ctrl+Shift+O"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::CopyItems,
                    QStringLiteral("Copy"),
                    QStringLiteral("Copy selected files for pasting into a writable folder."),
                    QStringLiteral("Ctrl+C"),
                    {.selection_required = true}),
        // Files CutItemAction: same clipboard package with a move operation;
        // the eventual paste deletes the source, so the source target must
        // be writable.
        makeCommand(FileExplorerCommandId::CutItems,
                    QStringLiteral("Cut"),
                    QStringLiteral("Cut selected files; pasting moves them."),
                    QStringLiteral("Ctrl+X"),
                    {.selection_required = true, .write_operation = true}),
        makeCommand(FileExplorerCommandId::SelectAll,
                    QStringLiteral("Select All"),
                    QStringLiteral("Select every item in the current folder."),
                    QStringLiteral("Ctrl+A")),
        // Files ToggleSelectAction: Ctrl+Space toggles the focused item.
        makeCommand(FileExplorerCommandId::ToggleSelect,
                    QStringLiteral("Toggle Selection"),
                    QStringLiteral("Toggle selection of the focused item."),
                    QStringLiteral("Ctrl+Space")),
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
        // Ctrl+Shift+I mirrors Files AddItemAction (new-item flow); Files
        // reserves Ctrl+Shift+V for paste-into-selection (C3).
        makeCommand(FileExplorerCommandId::WriteFile,
                    QStringLiteral("Write File"),
                    QStringLiteral("Import or write a file into the current location."),
                    QStringLiteral("Ctrl+Shift+I"),
                    {.write_operation = true}),
        makeCommand(FileExplorerCommandId::Paste,
                    QStringLiteral("Paste"),
                    QStringLiteral("Paste copied files into the current folder."),
                    QStringLiteral("Ctrl+V"),
                    {.write_operation = true}),
        // Files PasteItemToSelectionAction (Ctrl+Shift+V): paste into the
        // selected folder, or the current folder when nothing is selected.
        makeCommand(FileExplorerCommandId::PasteIntoSelection,
                    QStringLiteral("Paste Into Folder"),
                    QStringLiteral("Paste copied files into the selected folder."),
                    QStringLiteral("Ctrl+Shift+V"),
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
        // Files DeleteItemPermanentlyAction: Shift+Delete skips the Recycle Bin.
        makeCommand(FileExplorerCommandId::DeletePermanently,
                    QStringLiteral("Delete permanently"),
                    QStringLiteral("Delete selected item permanently (skips the Recycle Bin)."),
                    QStringLiteral("Shift+Del"),
                    {.destructive = true, .selection_required = true, .write_operation = true}),
        // Files CreateFolderWithSelectionAction: new folder that swallows the selection.
        makeCommand(FileExplorerCommandId::CreateFolderWithSelection,
                    QStringLiteral("Create folder with selection"),
                    QStringLiteral("Create a new folder and move the selected items into it."),
                    {},
                    {.selection_required = true, .write_operation = true}),
        // Files RemoveTagsAction: clears app-level tags; never touches the file system.
        makeCommand(FileExplorerCommandId::RemoveTags,
                    QStringLiteral("Remove tags"),
                    QStringLiteral("Remove all S.A.K. tags from the selected items."),
                    {},
                    {.selection_required = true}),
        // Files OpenTerminalAction: Ctrl+` opens a terminal in the current or
        // selected folder (mounted local volumes only).
        makeCommand(FileExplorerCommandId::OpenInTerminal,
                    QStringLiteral("Open in Windows Terminal"),
                    QStringLiteral("Open a terminal in the current or selected folder."),
                    QStringLiteral("Ctrl+`")),
        // Files EditInNotepadAction: script files (.bat/.cmd/.ahk) only.
        makeCommand(FileExplorerCommandId::EditInNotepad,
                    QStringLiteral("Edit in Notepad"),
                    QStringLiteral("Open the selected script files in Notepad."),
                    {},
                    {.selection_required = true}),
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
        // Files LayoutAction keys: 1 Details, 2 List, 3 Cards, 4 Grid,
        // 5 Columns, 6 Adaptive.
        makeCommand(FileExplorerCommandId::ViewGrid,
                    QStringLiteral("Grid"),
                    QStringLiteral("Switch to grid view."),
                    QStringLiteral("Ctrl+Shift+4")),
        makeCommand(FileExplorerCommandId::ViewCards,
                    QStringLiteral("Cards"),
                    QStringLiteral("Switch to cards view."),
                    QStringLiteral("Ctrl+Shift+3")),
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
        // Files LayoutIncreaseSize/LayoutDecreaseSize (Ctrl+= / Ctrl+-,
        // also Ctrl+mouse-wheel).
        makeCommand(FileExplorerCommandId::IncreaseSize,
                    QStringLiteral("Increase Size"),
                    QStringLiteral("Increase item size in the current view."),
                    QStringLiteral("Ctrl+=")),
        makeCommand(FileExplorerCommandId::DecreaseSize,
                    QStringLiteral("Decrease Size"),
                    QStringLiteral("Decrease item size in the current view."),
                    QStringLiteral("Ctrl+-")),
    };
}

// Pane and tab commands.
QVector<FileExplorerCommand> paneAndTabCommands() {
    return {
        makeCommand(FileExplorerCommandId::ToggleDualPane,
                    QStringLiteral("Dual Pane"),
                    QStringLiteral("Toggle dual-pane explorer layout."),
                    QStringLiteral("Ctrl+Shift+S")),
        makeCommand(FileExplorerCommandId::FocusOtherPane,
                    QStringLiteral("Focus Other Pane"),
                    QStringLiteral("Move focus to the other explorer pane."),
                    QStringLiteral("Ctrl+Shift+Right")),
        makeCommand(FileExplorerCommandId::DuplicateTab,
                    QStringLiteral("Duplicate Tab"),
                    QStringLiteral("Open a copy of the current tab."),
                    QStringLiteral("Ctrl+Shift+K")),
        makeCommand(FileExplorerCommandId::ReopenClosedTab,
                    QStringLiteral("Reopen Closed Tab"),
                    QStringLiteral("Reopen the most recently closed tab."),
                    QStringLiteral("Ctrl+Shift+T")),
        makeCommand(FileExplorerCommandId::CopyToOtherPane,
                    QStringLiteral("Copy to Other Pane"),
                    QStringLiteral("Copy the selected files into the other pane's folder."),
                    QStringLiteral("F6"),
                    {.selection_required = true}),
        makeCommand(FileExplorerCommandId::ComparePanes,
                    QStringLiteral("Compare Panes"),
                    QStringLiteral("Compare the two pane folders by name and size.")),
    };
}

}  // namespace

QVector<FileExplorerCommand> FileExplorerCommandRegistry::commands() {
    QVector<FileExplorerCommand> registry;
    registry.append(openAndNavigationCommands());
    registry.append(clipboardAndSelectionCommands());
    registry.append(writeCommands());
    registry.append(viewCommands());
    registry.append(paneAndTabCommands());
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
    if (const auto tabs = tabManagementState(id, context, entry)) {
        return *tabs;
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
    if (const auto paste = pasteClipboardState(id, context, entry)) {
        return *paste;
    }
    if (const auto cross_pane = crossPaneState(id, context, entry)) {
        return *cross_pane;
    }
    if (const auto local_tool = localToolState(id, context, entry)) {
        return *local_tool;
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
        {Id::DeletePermanently, "delete-permanently"},
        {Id::CreateFolderWithSelection, "create-folder-with-selection"},
        {Id::RemoveTags, "remove-tags"},
        {Id::OpenInTerminal, "open-in-terminal"},
        {Id::EditInNotepad, "edit-in-notepad"},
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
        {Id::DuplicateTab, "duplicate-tab"},
        {Id::ReopenClosedTab, "reopen-closed-tab"},
        {Id::Hash, "hash"},
        {Id::CopyOut, "copy-out"},
        {Id::CopyItems, "copy-items"},
        {Id::Paste, "paste"},
        {Id::CopyToOtherPane, "copy-to-other-pane"},
        {Id::ComparePanes, "compare-panes"},
        {Id::ToggleSelect, "toggle-select"},
        {Id::CopyItemPathQuoted, "copy-item-path-quoted"},
        {Id::IncreaseSize, "increase-size"},
        {Id::DecreaseSize, "decrease-size"},
        {Id::FocusOtherPane, "focus-other-pane"},
        {Id::CutItems, "cut-items"},
        {Id::PasteIntoSelection, "paste-into-selection"},
    });
    const auto it = std::ranges::find(kNames, id, &std::pair<Id, const char*>::first);
    return it != kNames.end() ? QString::fromLatin1(it->second) : QStringLiteral("unknown");
}

FileExplorerCommandGroup FileExplorerCommandRegistry::group(const FileExplorerCommandId id) {
    return groupFor(id);
}

QString FileExplorerCommandRegistry::groupName(const FileExplorerCommandGroup group) {
    using Group = FileExplorerCommandGroup;
    switch (group) {
    case Group::Navigation:
        return QStringLiteral("Navigation");
    case Group::File:
        return QStringLiteral("File");
    case Group::View:
        return QStringLiteral("View");
    case Group::Pane:
        return QStringLiteral("Pane");
    case Group::Target:
        return QStringLiteral("Target");
    case Group::Safety:
        return QStringLiteral("Safety");
    }
    return QStringLiteral("Other");
}

QVector<FileExplorerCommandGroup> FileExplorerCommandRegistry::groupOrder() {
    using Group = FileExplorerCommandGroup;
    return {Group::Navigation, Group::File, Group::View, Group::Pane, Group::Target, Group::Safety};
}

}  // namespace sak
