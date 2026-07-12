# File Explorer: Files-Community Functional Clone Plan (C-Track)

Mandate (user, 2026-07-12): the File Management Explorer tab must be a
FUNCTIONAL CLONE of the Files app (github.com/files-community/files) - UI and
features - derived from the actual Files source, not approximated from memory.
Overriding constraint: every file-management feature must work through
FileManagementFileSystemBridge on NON-MOUNTED raw APFS/HFS+ targets wherever
the operation is expressible with the certified driver ops; local-mounted
behavior alone does not satisfy parity. That is the whole point of the
APFS/HFS driver program.

Source of truth: a local shallow clone of files-community/Files kept as a
sibling checkout of this repository (`../files-community-Files`, path
recorded in the developer's local memory notes). Every replicated region cites its
source file. Do not restyle from memory; re-read the XAML when in doubt.

Bridge op surface available for raw targets (file_management_file_system.h):
listDirectory, readFile, hashFile, renderPreview, createDirectory,
deleteDirectory, writeFile, writeFileFromHostPath, copyFileToHost,
exportDirectoryToHost, deleteFile, renameEntry (rename = move within target).
Raw compositions used by this plan (no new driver work): in-target copy =
readFile/copyFileToHost(temp) + writeFile; cross-target move = copy + delete;
recursive host-dir import = walk + createDirectory + writeFileFromHostPath;
compress/extract = stage via temp dir + import/export.

## 1. Shell anatomy (source: src/Files.App/Views/MainPage.xaml)

Rows of the page grid (MainPage.xaml 140-145):
- Row 0 Auto: TabBar (tab strip; in Files it doubles as the title bar).
- Row 1 Auto: NavigationToolbar (address bar), FULL WIDTH, above the sidebar,
  ShowOngoingTasks=true (status-center button lives here).
- Row 2 *: SidebarView wrapping the InnerContent grid.

InnerContent grid (MainPage.xaml 196-311), columns `* | Auto | Auto | Auto`
(content | splitter | info pane | shelf), rows:
- Row 0: uc:Toolbar (command toolbar), Margin 0,0,0,4, spans content+info cols.
- Row 2: PageContent | GridSplitter | InfoPane (right dock).
- Row 4: InfoPane when docked bottom (InfoPanePositionStates VSM 461-496).
- Row 5: StatusBar spanning content+info cols.
Sidebar footer: Settings item (346-372). Sidebar background is a distinct
brush (App.Theme.Sidebar.BackgroundBrush); content areas are cards.

S.A.K. mapping: the explorer panel root becomes: [tab row][address row]
[splitter: sidebar | (command toolbar / panes / status bar / info pane)].
Info pane dockable right or bottom with a persisted setting. Shelf pane:
EXCLUDED (Windows-shell drag shelf; no bridge meaning) - documented.

Metrics (NavigationToolbar.xaml, Toolbar.xaml, TabBar.xaml): address row
height 48, padding 4,0,4,0, spacing 4; address buttons 36x32 transparent;
command toolbar CornerRadius 8, border 1px, bottom margin 4; tab buttons
30x30; status bar strip height 32, git buttons 24; dividers 1x18.

## 2. Tab bar (src/Files.App/UserControls/TabBar/TabBar.xaml)

- Tab = icon + label + close button (close glyph E711); drag reorder.
- Custom new-tab button (30x30, plus glyph) in the strip footer.
- Tab-actions button LEFT of tabs (30x30): menu = New window (EXCLUDED,
  app-level) / Split pane vertically / horizontally / Arrange panes
  vertically / horizontally (toggles) / Close pane.
- Tab context menu order (TabBar.xaml 21-58): New tab, Duplicate tab,
  Move tab to new window (EXCLUDED), Close tabs to the left, Close tabs to
  the right, Close other tabs, Reopen tab.

## 3. Address toolbar (src/Files.App/UserControls/NavigationToolbar.xaml)

Left (order): sidebar toggle (hamburger) | Back (right-click = back-history
flyout, MaxHeight 400) | [narrow-only overflow "see more"] | Forward
(history flyout) | Up | Refresh. Wide state (>=540px) shows
Forward/Up/Refresh inline; narrow collapses them into the overflow button.
No Home button - Home is the breadcrumb root.

Center: Omnibar with THREE MODES (mode chips at left of the field,
src/Files.App.Controls/Omnibar/Omnibar.cs):
- Path mode (default): inactive content = BreadcrumbBar with Home root icon;
  each crumb has a chevron dropdown listing SIBLING folders and supports
  drag/drop. Active = editable path TextBox with live navigation
  suggestions. Enter = navigate. Ctrl+L / Alt+D focuses it (EditPath).
- Command palette mode (Ctrl+Shift+P): fuzzy command list with hotkey chips,
  executes commands. Placeholder "Find features and commands...".
- Search mode (Ctrl+F / F3): live search of current folder w/ recent
  searches; suggestion rows = icon + name + description.

Right: status-center button (progress ring + info badge; flyout hosts the
ops cards) | update button (EXCLUDED, app-level).

## 4. Command toolbar (src/Files.App/UserControls/Toolbar.xaml +
Data/Items/ToolbarSections.cs)

Left CommandBar, AlwaysVisible context, exact order:
1. New (icon+label, flyout: New folder / New file / Create shortcut /
   separator / shell templates (EXCLUDED)).
2. separator
3. Cut, Copy, Paste, Rename, Share (EXCLUDED - Windows share sheet),
   Delete, Properties - all icon-only.
Context sections appended after separators when active: Extract group for
archives (Extract files / here smart / here / to subfolder); Edit in
Notepad (bat), Run with PowerShell (ps1), image Set-as/Rotate (LOCAL-ONLY),
media Play (EXCLUDED), font/driver/cert install (EXCLUDED), RecycleBin
section (LOCAL-ONLY: Empty / Restore all / Restore).

Right CommandBar (static, icon-only): Filter header toggle | Selection
options (Select all / Invert selection / Clear selection) | Sort flyout
(Sort by: Name, Date modified, Date created, Size, Type, [Sync status
EXCLUDED], Tags, Path; separator; Ascending/Descending; separator; Sort
folders first / files first / together) | Group by flyout (None, Name, Date
modified Y/M/D, Date created Y/M/D, Size, Type, Tags, Folder path;
Ascending/Descending) | Layout flyout (5 radio buttons 76x72 Details/List/
Cards/Grid/Columns + per-layout size Slider + toggles Show hidden items /
File extensions / Adaptive) | Info-pane toggle.

## 5. Status bar (src/Files.App/UserControls/StatusBar.xaml)

Inside content area, bottom. Col0: "N items" | divider | "N items selected"
| divider | selection size. Col1/2: git actions + branch (EXCLUDED - git
integration out of scope for the S.A.K. tab; documented). Height 32.

## 6. Info pane (src/Files.App/UserControls/Pane/InfoPane.xaml)

Card (radius 8). Top: segmented Details | Preview. Preview = file preview /
"No item selected" / "Preview not available" / loading ring. Details =
file name header, drive details w/ StorageBar for drives, property
name/value repeater, tag pills + Edit tags flyout, Open properties button.
Dock right or bottom (setting), resizable splitter. S.A.K. keeps its extra
Safety and Evidence surfaces inside the Details scroller (S.A.K.-specific
additions, clearly grouped, not replacing Files anatomy).

## 7. Sidebar (Files.App.Controls/Sidebar/*, ViewModels/UserControls/
SidebarViewModel.cs)

Sections in fixed order, each toggleable from the sidebar background context
menu: Home, Pinned, [Libraries EXCLUDED], Drives, [Cloud EXCLUDED],
[Network EXCLUDED], [WSL EXCLUDED], Tags. S.A.K. section mapping: Home,
Pinned (favorites incl. offline pins), Drives (mounted volumes), Disks and
Partitions (raw), Raw Images, Recent, Certification Targets, Tags - order
fixed, each toggleable, same context-menu pattern.
Row = 3px accent selection bar | indent (level*16) | chevron 16 | icon 16 |
text (SemiBold headers) | decorator 28 (eject button for removable drives
only). NO capacity bar in sidebar rows - capacity is the row TOOLTIP
"{free} free of {capacity}"; capacity bars belong to drive cards/details.
Item context menus per section (SidebarViewModel.GetLocationItemMenuItems):
Open in new tab / new window (EXCLUDED) / new pane split V/H / other pane,
Copy, Properties, Pin/Unpin, Reorder sidebar items dialog, Hide section,
Eject, Format (EXCLUDED), Open terminal (LOCAL-ONLY). Drag onto Pinned
header pins folders; drag onto folder/drive = move/copy per modifiers; drag
onto a tag applies the tag. Footer: Settings. Compact rail 48-56px wide,
Ctrl+B toggles, width resizable + persisted (default 240, max 480).

## 8. Layouts (src/Files.App/Views/Layouts/*,
Helpers/Layout/LayoutSizeKindHelper.cs)

Six modes: Details, List, Cards, Grid, Columns, Adaptive. Hotkeys
Ctrl+Shift+1..6. Zoom Ctrl+- / Ctrl++ steps the per-layout size kind.
Sizes (px, Compact/Small/Medium/Large/ExtraLarge):
- Details rows 28/36/40/44/48 (default 36); icons 16/16/20/24/32.
- List + Columns rows 24/32/36/40/44 (default 32); List cell width 260.
- Cards icons 64/64/80/96; card details box 196/240/280/320.
- Grid item width 80..300 in 12 steps (default 100), square thumb, name
  block height 60 centered wrap 2 lines.
Details view: icon col 24 fixed; default-ON columns Icon, Name, Tags, Date
modified, Type, Size; Date created default OFF; Path contextual. Header row
40px w/ select-all checkbox, per-column sort on click, splitter drag resize
+ double-click auto-fit, right-click column menu ending with "Size all
columns to fit" + "Set as default". Columns view = miller blades (200px,
right divider), folder rows get chevron E76C, selecting a folder opens a
blade to the right. Empty state: "This folder is empty." /
"No items found" (FolderEmptyIndicator).

## 9. Interactions (BaseLayoutPage.cs, GridLayoutPage.xaml.cs)

- Inline rename: F2, context menu, or slow-double-click on the NAME within
  1.5s (tapDebounceTimer). Textbox selects the base name without extension
  when extensions shown; Enter commits, Esc cancels, invalid chars stripped
  with a warning tip.
- Selection: Extended everywhere; Ctrl+A select all, Ctrl+Space toggle
  focused, Invert, Clear; optional per-item checkboxes (setting) + header
  select-all; rubber-band rectangle selection.
- Activation: double-click opens; Enter opens; Ctrl+Enter opens folder in
  new tab; Ctrl+Shift+Enter opens in other pane; Alt+Enter = Properties;
  double-click empty area = navigate up (setting); optional single-click
  open (setting).
- Drag/drop: modifier semantics - Alt or Ctrl+Shift = Link (EXCLUDED for
  raw; local creates shortcut), Ctrl = Copy, Shift = Move, default = Move
  same-target / Copy cross-target; drop captions "Copy to {name}" etc;
  hover a folder springs it open after a delay; drag out to OS apps
  (LOCAL + raw via temp materialization).
- Sort state per folder: option + direction + folders-first; header click
  sorts; group-by renders section headers.

## 10. Context menus (Data/Factories/ContentPageContextFlyoutFactory.cs)

Background menu order: [Close active pane] | Layout submenu | Sort by
submenu | Group by submenu | Refresh | sep | New submenu (Folder, File,
Shortcut LOCAL-ONLY) | [RecycleBin entries LOCAL-ONLY] | Open in terminal
(LOCAL-ONLY) | [Format EXCLUDED].
Item menu order: Open | Open with (LOCAL-ONLY) | Open in new tab | new
window (EXCLUDED) | new pane V/H | other pane | [image Set as / Rotate
LOCAL-ONLY] | [Run as admin LOCAL-ONLY] | sep | Cut, Copy, Paste-into,
Paste shortcut (LOCAL-ONLY) | Copy item path (Ctrl+Shift+C) | Create folder
with selection | Create shortcut (LOCAL-ONLY) | Rename | Delete |
Properties | Pin/Unpin sidebar | Compress submenu (zip) | Extract submenu |
Flatten folder | Edit in Notepad (LOCAL-ONLY) | sep | Open in terminal
(LOCAL-ONLY). Primary icon row: Cut Copy Paste Rename Delete Properties.
Shell-extension overflow: EXCLUDED.

## 11. Action parity matrix (raw-target-first)

Grades: HAVE (works today incl. raw where applicable), Cn (wave), LOCAL
(meaningful only on mounted local paths - still implemented), EXCLUDED
(documented non-goal: Windows-shell/app-level integration with no bridge
meaning: share sheet, git, cloud/network/WSL, shell templates+extensions,
set-as-background targets beyond local, install cert/font/driver, BitLocker,
Storage Sense, new window, compact overlay, update UI, shelf).

Navigation: Back/Forward/Up/Refresh HAVE (add history flyouts C1; hotkeys
Alt+arrows/Backspace/F5/Ctrl+R C2; mouse4/5 C2). Tabs: new/close/duplicate/
reopen/next-prev HAVE (hotkeys C2); close left/right/others C1; tab-actions
menu C1. Panes: split V/H + arrange V/H + close pane + focus other
(Ctrl+Shift+Right) C1 (dual-pane exists; arrangement actions partial).
Open in new tab/pane/other pane C2 (folder param routing exists partially).
File ops (ALL must work on raw targets): Copy Ctrl+C HAVE; Cut Ctrl+X C3
(clipboard mark + move-on-paste via bridge copy+delete); Paste HAVE (raw
import HAVE; raw->raw C3); Paste into selection C3; Duplicate C3; Delete
HAVE (raw permanent w/ confirm; local recycle C3 + Shift+Del permanent);
Rename/F2 inline C2 (dialog HAVE); New folder HAVE (hotkey C2); New file
HAVE (Write File; add empty-file New>File C3); Create folder with selection
C3; Copy path variants HAVE/C2 (quotes variants C2); Undo/Redo C5 (journal:
rename/move/copy/create inverse ops; raw delete = no undo, matching Files
permanent-delete semantics); Flatten folder C5.
Archives: Compress to zip C4 (miniz/zlib; raw staged via temp); Extract
(Ctrl+E) / here / smart / to subfolder C4; 7z EXCLUDED (no lib in-tree).
Selection: Select all/Invert/Clear/Toggle C2. Display: 6 layouts HAVE
(hotkeys C2, Adaptive HAVE); zoom steps C2 (slider maps size kinds C5);
sort menu HAVE-partial C1; group-by C5; folders-first C1; hidden items
Ctrl+H HAVE (hotkey C2); file extensions HAVE; checkboxes setting C5.
Search: omnibar search mode C6 (dialog HAVE); filter header C6.
Command palette: omnibar mode C6 (dialog HAVE, Ctrl+Shift+P HAVE).
Properties: window Alt+Enter C4 (pane HAVE). Terminal: LOCAL C4.
Status center: C7 (progress + cancel + badge for import/export/copy/
compress incl. raw streams). Tags: HAVE (+ sidebar tag section HAVE,
drag-to-tag C5, Remove tags C4). Sidebar: pin/unpin HAVE (drag-pin C5,
reorder dialog C5, eject C5 LOCAL, section toggles C1, compact rail C1).
Theme toggle: HAVE app-level. Settings surface for explorer options: C6.

## 12. Waves

- C1 Shell re-anatomy to source: full-width 48px address row (hamburger,
  back+history, narrow overflow, forward/up/refresh, omnibar path mode w/
  crumb sibling dropdowns, status-center button placeholder), command
  toolbar into content column w/ exact button order + flyouts (New group,
  icon row, right base bar w/ selection/sort/group/layout flyouts), status
  bar into content column w/ counts, tab-actions button + full tab context
  menu, sidebar section toggles + settings footer + compact rail + Ctrl+B,
  info pane right/bottom dock + Details|Preview segmented.
- C2 Interactions: inline rename everywhere (F2/slow-double-click/menu,
  extension-aware selection), selection commands + Ctrl+Space, activation
  matrix (Enter/Ctrl+Enter/Ctrl+Shift+Enter/Alt+Enter, dbl-click-up
  setting), full default hotkey map from Actions/**, mouse4/5, header
  sort + auto-fit, zoom steps.
- C3 Transfer engine (raw-first): cut/copy/paste matrix across local/raw
  incl. raw->raw and cross-target move, duplicate, recursive import,
  paste-into-selection, local recycle-bin delete + Shift+Del, drop
  semantics w/ modifier captions + spring-open, drag-out materialization.
- C4 Context menus exact + Compress/Extract (zip) + Properties window +
  Remove tags + Edit in Notepad/terminal (LOCAL).
- C5 Layout fidelity: size-kind tables + per-layout sliders, group-by,
  checkboxes setting, Columns blade polish (chevron, blade widths),
  Cards/Grid exact cells, undo/redo journal, sidebar drag-pin/reorder/
  eject, drag-to-tag.
- C6 Omnibar command palette + search modes inline (replace dialogs),
  filter header, explorer settings page.
- C7 Status center: cards w/ progress/speed/cancel, toolbar badge states.
- C8 Certification: unit+GUI tests per wave feature on local AND raw
  fixtures (sparse APFS/HFS images), full CTest, live destructive proof of
  the transfer matrix on the physical drives, screenshot comparison vs
  Files, docs/CHANGELOG/README sync.

Every wave: implement -> clang-format/lizard/cppcheck gates -> tests ->
commit with source citations. No wave claims done without raw-target proof
where the feature touches file data.
