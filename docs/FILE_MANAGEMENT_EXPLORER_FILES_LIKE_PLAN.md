# File Management Explorer Files-Like Plan

**Parent Panel**: File Management (`OrganizerPanel`)
**Current Widget**: `FileManagementExplorerPanel`
**Goal**: Replace the current certifier-style table explorer with a Files-inspired, technician-grade file manager experience that works across mounted Windows volumes and S.A.K.-supported raw/image file systems.

---

## Scope

This plan targets the File Management **File Explorer** tab only. It must feel and behave like a modern Files-style file manager, while preserving S.A.K.'s stricter raw-media safety rules.

In scope:

- Mounted Windows file systems.
- Scanned disk and partition targets.
- Manual raw/image targets.
- ext2/ext3/ext4 read-only browse/read/export.
- HFS+/HFSX browse/read plus certified explicit write operations.
- APFS browse/read plus certified full driver-level write operations (A1-A8; see the matrix owner [APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](APFS_HFS_FULL_DRIVER_WRITE_PLAN.md)).
- XFS/Btrfs metadata-only targets with clear disabled browse/write states until readers exist.
- Files-style UI shell: sidebar, command bar, omnibar, tabs, dual pane, view layout picker, context menu, preview/details pane, properties, keyboard shortcuts, and command palette.
- S.A.K.-owned implementation in Qt/C++, borrowing Files interaction patterns and visual language rather than embedding the Files WinUI app.

Out of scope for this milestone:

- Cloud drives.
- FTP.
- Git integration.
- Third-party integrations.

## Source References

Files is an MIT-licensed, mostly C#/WinUI file manager. Its public docs list the interaction patterns this plan ports into Qt/C++:

- Files overview: modern file manager, multitasking, tags, integrations, intuitive design:
  <https://files.community/docs/getting-started/overview>
- Files repository and license context:
  <https://github.com/files-community/Files>
- Files layout picker: Details, List, Cards, Grid, Columns, item-size slider, hidden-items toggle, file-extension toggle, adaptive layout:
  <https://files.community/docs/features/layout-picker>
- Files omnibar: breadcrumb path, editable path, command palette, search:
  <https://files.community/docs/features/omnibar>
- Files dual pane: two locations inside one tab, compare/transfer workflow, context-menu open-in-pane:
  <https://files.community/docs/features/dual-pane>
- Files command palette: searchable action surface with keyboard bindings:
  <https://files.community/docs/features/command-palette>
- Files tabs: new tab, open folder in tab, restore closed tab, tab switching, close, duplicate:
  <https://files.community/docs/features/tabs>

S.A.K. will use these as UX requirements, not as direct code import. Direct embedding is not practical because Files is WinUI/Windows App SDK while S.A.K. is Qt/C++ and must route raw file-system actions through S.A.K. readers/writers and certification gates.

## Upstream Files Code Scan - 2026-06-13

Scanned upstream repository: `files-community/Files` at commit `8ea6d30`.

Code areas scanned:

- Shell and panes:
  - `src/Files.App/Views/ShellPanesPage.xaml.cs`
  - `src/Files.App/Views/Shells/ModernShellPage.xaml`
  - `src/Files.App/Views/Shells/BaseShellPage.cs`
- Omnibar and breadcrumbs:
  - `src/Files.App/UserControls/NavigationToolbar.xaml`
  - `src/Files.App.Controls/Omnibar/*`
  - `src/Files.App.Controls/BreadcrumbBar/*`
- Layout system:
  - `src/Files.App/Views/Layouts/BaseLayoutPage.cs`
  - `src/Files.App/Views/Layouts/BaseGroupableLayoutPage.cs`
  - `src/Files.App/Views/Layouts/DetailsLayoutPage.*`
  - `src/Files.App/Views/Layouts/GridLayoutPage.*`
  - `src/Files.App/Views/Layouts/ColumnLayoutPage.*`
  - `src/Files.App/Views/Layouts/ColumnsLayoutPage.*`
  - `src/Files.App.Controls/AdaptiveGridView/*`
  - `src/Files.App/Data/Contexts/DisplayPage/DisplayPageContext.cs`
  - `src/Files.App/Actions/Display/LayoutAction.cs`
  - `src/Files.App/Data/Contracts/ILayoutSettingsService.cs`
- Commands, shortcuts, toolbar:
  - `src/Files.App/Data/Commands/Manager/CommandManager.cs`
  - `src/Files.App/Data/Commands/ActionCommand.cs`
  - `src/Files.App/Data/Commands/IRichCommand.cs`
  - `src/Files.App/Data/Commands/Manager/CommandGroupManager.cs`
  - `src/Files.App/Data/Items/ToolbarItemDescriptor.cs`
  - `src/Files.App/Data/Items/ToolbarSections.cs`
  - `src/Files.App/Views/Settings/ToolbarCustomizationPage.*`
  - `src/Files.App/Views/Settings/ActionsPage.*`
- Settings, tags, properties:
  - `src/Files.App/Data/Contracts/IFoldersSettingsService.cs`
  - `src/Files.App/Data/Contracts/IGeneralSettingsService.cs`
  - `src/Files.App/Data/Contracts/IInfoPaneSettingsService.cs`
  - `src/Files.App/Data/Contracts/IFileTagsService.cs`
  - `src/Files.App/Data/Contracts/IFileTagsSettingsService.cs`
  - `src/Files.App/Views/Settings/LayoutPage.*`
  - `src/Files.App/Views/Settings/TagsPage.*`
  - `src/Files.App/Views/Properties/*`
  - `src/Files.App/ViewModels/Properties/*`

Findings that change this plan:

- Files is not a single explorer widget. It is a shell made from a pane host, per-pane shell pages, layout pages, shared command services, settings services, and custom controls.
- The omnibar is not a path edit with buttons. It is one control with path, command-palette, and search modes; inactive path mode renders a real breadcrumb bar with root item, ellipsis, chevrons, flyouts, and accessible breadcrumb items.
- Details/List/Cards/Grid are not unrelated views. Files uses layout mode settings plus shared item manipulation, selection, grouping, sorting, and item-size controls. S.A.K. must preserve one selection/command model while swapping view surfaces.
- Columns view is a separate navigation layout with active column-shell behavior. In S.A.K. it should be its own view page, not a styled grid.
- Dual pane is session architecture, not visual split only. Files serializes left/right pane paths, arrangement, active pane, focus switching, active-pane visual state, and drag/drop routing.
- Command palette, keyboard shortcuts, context menus, toolbar buttons, and action settings are fed from one command abstraction. S.A.K. must not add view-specific action code after M6.
- Toolbar customization is core Files UX. Files has default toolbar sections by context, command groups, separators, show-icon/show-label flags, and a customization page. S.A.K. needs a scoped version for File Explorer commands.
- Files has explicit settings pages for Layout, Actions, Tags, Folders, General, and Toolbar customization. S.A.K. should expose these as File Explorer settings sections, not scattered per-widget controls.
- Files tags are app-level metadata backed by settings/database concepts. S.A.K. tags must remain app metadata keyed by stable target identity and item ID/path, never raw HFS/APFS metadata by default.
- Files properties are broad and multi-page. S.A.K. should port the pattern as right-pane tabs plus properties dialog, adding Safety and Evidence as S.A.K.-specific pages.
- Files has reusable icon resources under `src/Files.App.Controls/ThemedIcon/Styles/*.xaml` with MIT headers, plus small navigation assets under `src/Files.App/Assets/FluentIcons/*.png`. S.A.K. should use these actual source icons where legally clean and technically practical.
- Cloud drives, FTP, Git integration, and third-party integration surfaces exist upstream, but remain excluded from S.A.K. scope for this milestone.

## Files UI/UX Contracts To Port

These are now acceptance-level contracts, not optional polish:

- Shell contract: tab strip, command bar, navigation toolbar, omnibar, sidebar, pane host, details/info pane, status strip, and overflow behavior exist as separate testable components.
- Pane contract: every pane owns target, path, back stack, forward stack, selection, sort/group/layout settings, pending load token, and view state.
- Tab contract: every tab serializes one or two pane states plus active pane and split arrangement.
- Omnibar contract: path mode shows breadcrumbs when inactive, selects the full path when activated, validates manual path edits, provides command-palette mode, and provides search mode with suggestions/results.
- Breadcrumb contract: root item, ellipsis, segment chevrons, child flyouts, keyboard invocation, tooltips, and responsive collapse all work.
- Layout contract: Details, List, Cards, Grid, Columns, and Adaptive route through a layout manager and shared command/selection model. Item size is per layout family.
- Layout picker contract: picker controls layout type, item size, hidden items, file extensions, adaptive layout, sort, group, sort direction, group direction, and folders/files ordering from one surface.
- Command contract: command registry is the only source for label, icon, shortcut, group, enabled state, blocker text, status text, accessible text, execution route, and safety logging.
- Toolbar contract: toolbar is generated from command descriptors and supports context sections, separators, command groups, show icon, show label, restore defaults, and persistence.
- Shortcut contract: keybindings are command data, can be tested as a matrix, and never bypass raw-target safety.
- Dual-pane contract: active pane visual state, focus routing, selection isolation, preview lock during pane switching, and command routing to active pane are mandatory.
- Properties/info contract: Preview, Properties, Safety, and Evidence are persistent right-pane tabs; Properties can open a larger dialog later.
- Settings contract: File Explorer settings must have Layout, Folders, Actions, Toolbar, Tags, and Safety sections.
- Icon contract: command, layout, omnibar, tab, pane, sidebar, status, tag, property, favorite, and file/folder icons should come from Files' MIT icon resources when they are generic UI assets; Files brand/app logos and out-of-scope cloud/Git/integration icons are excluded.
- Exclusion contract: no cloud-drive, FTP, Git integration, third-party integration, shell namespace, or kernel driver work enters this milestone.

## Current State

Current `FileManagementExplorerPanel` has the M0-M4 Files-like foundation:

- Grouped sidebar target navigation for Home, Favorites, This PC, Mounted Volumes, Disks and Partitions, Raw Images, Recent, and Certification Targets.
- Command bar with Refresh, Scan Disks, Add Raw/Image, New Folder, Write File, Rename, Delete, sidebar collapse, and details-pane collapse.
- Omnibar row with Back, Forward, Up, editable path, search button, command button, Open, and Copy Path.
- Current-folder filter through Search / `Ctrl+F`.
- Command palette through the command button / `Ctrl+Shift+P`, backed by the same command registry and disabled-state blockers as toolbar/context menu actions.
- Model/view details listing through `FileExplorerItemModel`.
- Functional Files-style View/layout picker entry point in the command bar with Details, List, Grid, Cards, Columns-surface, Adaptive, item size, hidden-items toggle, and file-extension toggle wired through one command/selection model.
- Persistent Preview, Properties, Safety, and Evidence pane.
- Dedicated M2 shell widgets: `FileExplorerSidebar`, `FileExplorerCommandBar`, `FileExplorerOmnibar`, `FileExplorerPane`, and `FileExplorerDetailsPane`.
- Bottom status strip with target, file system, selection count, and write state.
- Shared command registry for toolbar, context menus, shortcuts, blocker text, and safety-pane status.
- Files Community MIT-derived SVG icon registry for mapped explorer commands, layout picker entries, pane toggles, Open, Copy Path, New Folder, Write File, Rename, Delete, Refresh, and dual-pane commands.
- Favorites/recent/last-target persistence and target properties.
- Target identity guard before raw/non-native mutation routes in the panel.

Current backend proof is stronger than the UI:

- File Management target bridge can browse/read supported mounted/raw/image targets.
- Duplicate Finder and Advanced Search can target supported raw/image readers.
- HFS+ live File Explorer write/read/search/rename/delete proof passed.
- APFS File Explorer write/read/search/delete proof passed; the APFS driver track (A1-A8) is Apple-native certified through the A8 physical-USB destructive/crash/rollback gate (2026-06-28), superseding the earlier 128 MiB Windows-side-only proof.

Current gap: the shell now looks and behaves like a primary file manager, but later milestones still need deeper multi-level Columns polish, tabs, dual pane, richer omnibar search results, richer previews, copy-out/import queues, icon render parity, and final live-device certification.

## Files Parity Guardrail

2026-06-13 course correction: do not let backend/model hardening drift into a generic table explorer. Files parity work must stay anchored to the public Files UX contract:

- Omnibar must become breadcrumb/edit/search/command surface, not only a path text box.
- Command palette must be a first-class action surface, not a late polish item hidden behind toolbar work.
- View/layout picker must expose Details, List, Cards, Grid, Columns, item size, hidden-items, file-extension, and adaptive layout controls.
- Tabs and dual pane must land before copy/import/export workflows are called file-manager complete.
- Preview/properties/safety work must not displace layout picker, tabs, dual pane, and command palette.
- Until these are implemented and tested, release-facing wording must say "Files-inspired", not "Files parity".

Corrected execution order from this point:

1. Finish M6 layout picker and view modes.
2. Move M8 tabs and dual pane ahead of preview polish.
3. Move M10 omnibar search and command palette ahead of preview polish.
4. Then finish M7 preview/properties/safety/evidence.
5. Then finish M9 copy/import/export and M11 polish.

## Product Requirements

The File Explorer tab should be a first-class file manager inside S.A.K.:

- User can navigate quickly without thinking about target type.
- User can see which target is mounted/local versus raw/image/non-native.
- User can perform only safe operations, with unsupported actions visible but disabled with exact reasons.
- User can switch views and panes without losing path/selection state.
- User can use keyboard shortcuts that match common file manager expectations.
- User can inspect raw/non-native items without accidentally mutating media.
- User can recover from failed raw operations with clear status, blockers, and evidence links.

## UX Layout

Target layout:

```text
+--------------------------------------------------------------------------------+
| Tab strip: [Home] [C:\] [APFS Disk2 P2] [+]                                      |
+--------------------------------------------------------------------------------+
| Command bar: New | Cut | Copy | Paste | Rename | Delete | Sort | View | ...      |
+--------------------------------------------------------------------------------+
| Back Forward Up | Omnibar breadcrumbs / editable path / search / command button |
+--------------------------------------------------------------------------------+
| Sidebar       | Main pane                                  | Preview/Details    |
|               |                                            |                    |
| Home          | Details/List/Grid/Cards/Columns             | metadata, preview  |
| Favorites     |                                            | blockers, hashes   |
| This PC       |                                            |                    |
| Disks         | Optional dual pane splitter                 |                    |
| Raw Images    |                                            |                    |
| Recent        |                                            |                    |
+--------------------------------------------------------------------------------+
| Status bar: target, file system, selected count, size, write safety, warnings   |
+--------------------------------------------------------------------------------+
```

Qt implementation notes:

- Use `QSplitter` for sidebar/main/details and dual-pane layouts.
- Use `QStackedWidget` for view modes.
- Use `QTabBar` or nested `QTabWidget` for explorer tabs inside the File Explorer tab.
- Use `QToolButton` with icons for command bar actions.
- Use stable row/item dimensions so hover, selection, and icons do not resize layout.
- Keep buttons icon-first with tooltips; use text only where command ambiguity would hurt safety.

## Visual Direction

Match Files-like feel without copying WinUI internals:

- Quiet, dense, utilitarian shell.
- Light/dark aware colors using existing S.A.K. style constants.
- 8 px or smaller corner radius.
- Left navigation with selected row highlight.
- Command bar as a flat horizontal strip, not large cards.
- Breadcrumb segments with chevrons and compact flyouts.
- Details pane as a persistent right panel, not modal-only.
- Context menus for commands; dialogs only for destructive confirmation, property details, and unsupported-operation explanation.
- Use actual Files source icons where license and format allow it. Command surfaces must not silently fall back to legacy/stock icons when a Files source asset is mapped; unmapped gaps must stay explicit and traceable.

## Icon Source Plan

Primary source:

- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Common.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.SizeLayout28.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.TabPane.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Panel.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.New.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Open.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Action.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Status.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Favorite.xaml`
- `src/Files.App.Controls/ThemedIcon/Styles/Icons.Properties.Dialog.xaml`
- `src/Files.App/Assets/FluentIcons/Home.png`
- `src/Files.App/Assets/FluentIcons/Star.png`
- `src/Files.App/Assets/FluentIcons/FileTags.png`

Import rules:

- Copy only generic UI/icon assets needed by File Explorer.
- Do not copy Files app logos, Store tiles, splash screens, app identity images, WSL distro icons, cloud-drive icons, Git icons, Listary/integration icons, or marketplace/integration assets.
- Preserve upstream MIT notices in copied icon files or generated resource files.
- Add Files Community attribution to `THIRD_PARTY_LICENSES.md` before shipping any copied icon asset.
- Keep icon filenames and source keys traceable in `resources/icons/files/manifest.json`.
- Convert XAML vector path data to Qt resources only through a repeatable script or documented manual mapping; no untracked hand-copied path blobs.
- Map each `FileExplorerCommandId` to one icon key in `FileExplorerIconRegistry`.
- Provide S.A.K.-specific raw-media icons only when a shipped UI surface actually consumes them. Source those gaps through Icons8 MCP unless Files has a matching generic icon; do not add local original icon art as a placeholder.
- Test dark, light, disabled, hover, selected, and high-contrast rendering for imported icons.

## Explorer Model

Add model layer between UI and `FileManagementFileSystemBridge`:

### `FileExplorerTargetModel`

Owns available targets:

- Mounted volumes.
- Scanned partition inventory targets.
- Manual raw/image targets.
- Favorites and pinned locations.
- Recent targets.
- Group labels: Home, This PC, Raw Images, Disks, Partitions.

### `FileExplorerSession`

One tab/session:

- Target ID.
- Current path.
- Back/forward history.
- Selection.
- Sort column/order.
- View mode.
- Show hidden.
- Show extensions.
- Preview/details pane state.
- Optional second pane state.

### `FileExplorerItemModel`

Use model/view instead of direct `QTableWidget`:

- `QAbstractItemModel` or `QAbstractTableModel` for details/list.
- Shared item list for grid/cards/columns.
- Supports multi-select.
- Supports lazy loading and cancellation.
- Carries operation capability flags per item.

### `FileExplorerCommandRegistry`

Central action registry:

- One command definition drives toolbar, context menu, keyboard shortcut, command palette, and enabled/disabled state.
- Each command returns an exact blocker when unavailable.
- Commands produce structured operation results for logs/status/cert evidence.

## File-System Capability Matrix

| Target | Browse | Read | Preview | Create Folder | Write File | Rename | Delete | Copy Out | Copy In |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| Local Windows | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes | Yes |
| ext2/ext3/ext4 raw/image | Yes | Yes | Yes | No | No | No | No | Yes | No |
| HFS+/HFSX raw/image | Yes | Yes | Yes | Yes[2] | Yes[2] | Yes[2] | Yes[2] | Yes | Yes[2] |
| APFS raw/image generated (<=32 TiB) | Yes | Yes | Yes | Yes[1] | Yes[1] | Yes[1] | Yes[1] | Yes | Yes[1] |
| APFS arbitrary Apple media | Yes where readable | Yes where readable | Yes | No | No | No | No | Yes | No |
| XFS/Btrfs current | Metadata only | No | No | No | No | No | No | No | No |

[1] Certified crash-safe in-place COW checkpoint engine (milestones A1-A8 - see the
single capability-matrix owner [APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](APFS_HFS_FULL_DRIVER_WRITE_PLAN.md),
driver matrix rows A-a..A-h). Certified scope on S.A.K. generated-layout
containers, 64 MiB through a 32 TiB cap (the former ~2.9-7.8 TiB
metadata-overflow dead zone is closed): in-place file + directory
create/delete/write/rename/cross-directory move/object-id-preserving patch,
snapshots (create/delete/revert), multi-volume containers, inline zlib
compression, credential-gated FileVault encryption, file clones / sparse files /
hard-links / xattr-ACL, and in-chunk container resize - validated by Apple
`fsck_apfs` + macOS-kernel mount and the A8 physical-USB destructive + crash +
rollback gate. Production writes carry an explicit generated-layout
confirmation, so mutation of **arbitrary non-generated Apple media stays
fail-closed at the Apply layer** (the COW engine is certified; the production
exposure is intentionally generated-layout-only - hence the "APFS arbitrary
Apple media" row stays No for writes). Also fail-closed by design: Fusion/Tier2
multi-device (out of scope, no rig); encryption requires the user credential; a
sealed/signed system volume needs a typed seal-invalidation confirmation;
container shrink and chunk-adding grow are documented follow-ons.

[2] HFS+/HFSX full-driver writes - the HFS+ track (H1-H8) is Apple-certified
end-to-end: streaming catalog/attributes/extents B-trees (depth/width-general,
underflow merge), hardlinks/symlinks, big-endian journal replay, and the
embedded-wrapper write edge, validated by Apple `fsck_hfs` + macOS-kernel RW
mount and the H8 physical-USB destructive + crash/rollback gate. See the single
capability-matrix owner [APFS_HFS_FULL_DRIVER_WRITE_PLAN.md](APFS_HFS_FULL_DRIVER_WRITE_PLAN.md),
driver matrix rows H-a..H-h + milestone H8.

UI must never hide unsupported write commands for raw targets. It should disable them and show exact blocker text in tooltip/details pane.

## Command Set

Implemented command registry:

- Open.
- Open in new tab.
- Open in second pane.
- Back / Forward / Up / Home.
- Refresh.
- Copy path.
- Copy item path.
- Preview.
- Properties.
- Select all.
- Clear selection.
- Invert selection.
- New folder.
- Write/import file.
- Rename.
- Delete.
- Toggle hidden items.
- Toggle file extensions.
- Sort by name/type/size/date/path.
- View mode: details/list/cards/grid/columns.
- Toggle preview/details pane.
- Toggle dual pane.

Planned command groups:

- Copy out selected raw/image files to local destination.
- Copy between panes where source/target capabilities allow it.
- Paste/import local files into certified HFS/APFS slices.
- Bulk delete for certified writer targets with explicit confirmation.
- Compare panes.
- Export folder recursively for supported read-only readers.
- Hash selected item.
- Verify selected raw file read-back.

Phase-3 commands:

- Tags/favorites backed by S.A.K. metadata, not raw file-system metadata by default.
- Command palette fuzzy search.
- Saved layouts per target/path.
- Recent locations and recent raw images.
- Quick filters.
- Batch operation queue with progress/cancel.

## Omnibar

Replace path edit with a single omnibar widget:

- Breadcrumb mode by default.
- Editable path mode on click-empty-space and `Ctrl+L`.
- Search mode on `Ctrl+F`.
- Command mode on `Ctrl+Shift+P`.
- Home flyout for pinned/frequent locations.
- Breadcrumb chevrons open child-folder flyouts where reader supports fast listing.
- Raw target path display includes file-system badge and target alias.

Raw target example:

```text
APFS Disk 2 Partition 2 > / > sak-fm-live-...
```

Manual edit validation:

- Local paths use `QDir`.
- Raw paths normalize slash separators and reject unsafe traversal.
- Invalid path leaves current path unchanged and shows blocker.

## Sidebar

Sidebar groups:

- Home.
- Favorites.
- This PC.
- Mounted Volumes.
- Disks and Partitions.
- Raw Images.
- Recent.
- Certification Targets.

Each row:

- Icon.
- Label.
- Secondary text for file system / serial / size.
- Badge for read-only, certified write, blocked, or pending Apple validation.

Sidebar behavior:

- Click opens target in current tab.
- Middle-click opens target in new tab where available.
- Context menu: open, open in new tab, open in second pane, pin/unpin, properties, rescan.
- Collapsible groups.
- Keyboard navigable.

## View Modes

Implement view modes in order:

1. Details.
2. List.
3. Grid.
4. Cards.
5. Columns.
6. Adaptive.

Details columns:

- Name.
- Type.
- Size.
- Modified/Created where reader supports it.
- File ID / catalog ID / inode / object ID.
- Attributes.
- Path.
- Capability.

Grid/cards:

- Use file-type icons first.
- Add thumbnails later for local images and safe extracted previews.
- Never block navigation waiting on thumbnails.

Columns:

- Mac-style multi-column navigation useful for raw media browse.
- Each column is lazy loaded.
- Selection in one column drives next column.

Layout picker:

- Icon button opens popover.
- Layout type segmented list.
- Item size slider.
- Show hidden toggle.
- Show extensions toggle.
- Adaptive layout toggle.

## Preview And Details Pane

Persistent right pane with tabs:

- Preview.
- Properties.
- Safety.
- Evidence.

Preview:

- Text preview.
- Hex preview for binary.
- Image preview for local/readable image files.
- Size cap and read cap visible.
- Unsupported preview shows reason.

Properties:

- Name, path, size, type.
- File-system-specific IDs.
- Timestamps where available.
- Fork/resource/attribute summary for HFS+.
- APFS object/extent summary where available.
- Hash on demand.

Safety:

- Target write state.
- Allowed commands.
- Blocked commands and exact blockers.
- Required certification evidence.

Evidence:

- Live cert report path if this target matches known evidence.
- Last operation result.
- Hashes and read-back status for mutations.

## Context Menu

Right-click menu must mirror command registry:

- Open.
- Open in new tab.
- Open in second pane.
- Preview.
- Copy.
- Copy path.
- Copy out.
- Import/write here.
- Rename.
- Delete.
- Properties.
- Hash.
- Verify/read-back.

Disabled actions remain visible when relevant and show blocker in status/details.

## Tabs

Explorer-level tabs inside File Explorer:

- New tab button.
- Close tab.
- Duplicate tab.
- Reopen closed tab later.
- Restore last session later.
- Each tab owns target/path/history/view/pane state.
- File Management outer tabs remain unchanged.

Do not confuse File Management outer tabs with File Explorer inner tabs.

## Dual Pane

Dual pane supports:

- Horizontal or vertical split.
- Independent target/path/history/view per pane.
- Active pane highlight.
- Open selected folder in second pane.
- Copy/compare between panes where safe.

Safety rule:

- Cross-pane transfer is command-mediated. Raw destination writes only enabled when destination writer is certified for that operation.

## Tags And Favorites

Files-style tags should use S.A.K. metadata first:

- Store tags in app config database/JSON keyed by stable target identity plus path/item ID.
- Do not write tags into raw HFS/APFS metadata by default.
- Local Windows targets may later support NTFS alternate streams or Windows property store after separate proof.

Tag UI:

- Tags in sidebar.
- Tag column/badges in views.
- Add/remove tag from context menu/properties.
- Tag filter/search.

## Keyboard Shortcuts

Baseline:

- `Enter`: open/preview.
- `Alt+Left`: back.
- `Alt+Right`: forward.
- `Alt+Up`: parent.
- `Ctrl+L`: edit path.
- `Ctrl+F`: search.
- `Ctrl+Shift+P`: command palette.
- `Ctrl+R` / `F5`: refresh.
- `Ctrl+A`: select all.
- `F2`: rename.
- `Delete`: delete.
- `Ctrl+Shift+N`: new folder.
- `Ctrl+C`: copy item reference.
- `Ctrl+Shift+C`: copy item path.
- `Ctrl+V`: paste/import where safe.
- `Ctrl+Shift+1..5`: view modes.
- `Ctrl+B`: sidebar.
- `Ctrl+Alt+I`: details pane.

Every shortcut must have accessible action text and command registry entry.

## Search Integration

Omnibar search mode:

- Searches current path by default.
- For local targets, can call existing Advanced Search backend.
- For raw/image targets, routes to existing File Management bridge + Advanced Search worker where supported.
- Results can open in current pane, new tab, or second pane.

Advanced Search tab remains full search workspace. Explorer search is quick scoped search.

## Operation Safety

Hard rules:

- Read-only parser paths stay read-only.
- Raw/non-native writes require explicit command, explicit confirmation, and capability proof.
- No generic organizer moves on raw/non-native targets.
- APFS writes run on the certified A1-A8 in-place COW engine (multi-CIB/CAB to a 32 TiB cap, Apple-validated through the A8 physical-USB gate), gated to S.A.K. generated-layout targets with explicit confirmation; arbitrary non-generated Apple media, Fusion/Tier2, and unprovided-credential encrypted volumes stay fail-closed.
- Every destructive command shows target identity, file system, operation, selected item count, and irreversibility.
- Bulk destructive operations require typed confirmation for raw targets.
- Operation results must include warnings/blockers and update status/log panes.

## Error Handling

Use consistent surfaces:

- Status bar for success and lightweight blockers.
- Details/Safety pane for persistent blockers.
- Modal only for destructive confirmation and fatal operation failure.
- Log output for structured evidence lines.

No silent disabled state. Disabled commands need tooltip and details-pane reason.

## Architecture Refactor

Suggested classes:

- `FileManagementExplorerPanel`: shell composition only.
- `FileExplorerTabStrip`: tab UI and tab state.
- `FileExplorerSidebar`: navigation tree/groups.
- `FileExplorerCommandBar`: toolbar/actions.
- `FileExplorerOmnibar`: breadcrumb/edit/search/command modes.
- `FileExplorerPane`: one navigable pane.
- `FileExplorerItemModel`: model/view data source.
- `FileExplorerDetailsPane`: preview/properties/safety/evidence.
- `FileExplorerCommandRegistry`: action metadata, shortcuts, enablement, execution.
- `FileExplorerIconRegistry`: Files-derived icon mapping plus future Icons8-sourced raw-media icons only when a UI surface consumes them.
- `FileExplorerOperationController`: async operations, confirmations, progress.
- `FileExplorerSessionStore`: saved tabs/layouts/favorites/tags.
- `FileExplorerViewSettings`: layout mode, columns, hidden/extensions toggles.

Existing bridge remains backend:

- `FileManagementFileSystemBridge`
- `PartitionExtFileSystemReader`
- `PartitionHfsFileSystemReader`
- `PartitionApfsFileSystemReader`
- certified writer helpers through existing APFS/HFS writer paths.

## Data Persistence

Persist:

- Favorites.
- Recent targets.
- Last active target/path.
- Per-target view mode.
- Column widths/order.
- Show hidden/extensions.
- Tags.
- Closed tabs later.

Do not persist:

- Raw write confirmations.
- Elevated credentials.
- Temporary APFS/HFS operation state without evidence file path.

## Accessibility

Requirements:

- Accessible names/descriptions for every command.
- Keyboard-only navigation across sidebar, omnibar, command bar, panes, and details.
- Focus ring visible.
- High-contrast safe colors.
- Screen-reader text for target write state and blockers.
- No icon-only control without tooltip and accessible name.
- Table/list/grid items expose name, type, size, path, and selected state.

## Responsiveness

Breakpoints:

- Wide: sidebar + main + details pane.
- Medium: sidebar collapsible; details pane toggle.
- Narrow: command bar wraps into overflow; details pane hidden by default.

Performance:

- Directory listing async with cancellation.
- Large directory virtualized model.
- Preview reads capped.
- Thumbnail generation deferred.
- Raw partition scans never run on UI thread.

## Testing Plan

Unit:

- Command registry enablement for every target capability.
- Path normalization for local/raw targets.
- Breadcrumb parsing and manual edit validation.
- Session persistence.
- Tag/favorite persistence.
- View-settings persistence.
- Sort/filter model behavior.
- Disabled command blocker strings.

GUI:

- Sidebar target selection.
- Omnibar breadcrumb/edit/search modes.
- View layout switching.
- Context menu actions.
- Multi-select and keyboard shortcuts.
- Details pane content for local, HFS, APFS, ext.
- Dual-pane activation and active-pane switching.
- Responsive layout at narrow/wide widths.
- Accessibility pattern checker.

Live certification:

- HFS+ raw create/write/read/rename/delete through new command registry.
- APFS generated raw create/write/read/delete (multi-CIB/CAB to a 32 TiB cap) through new command registry.
- ext4 raw read-only browse/copy-out.
- Unsupported XFS/Btrfs action blockers.
- Arbitrary non-generated / Fusion / unprovided-credential-encrypted APFS write blockers.
- Local mounted copy/paste smoke.

Visual QA:

- Desktop screenshot.
- Narrow screenshot.
- Dark/light theme screenshot if app supports both.
- No clipped text.
- No overlapping controls.
- Command bar overflow works.

## Milestone Roadmap

The work should move in small certifiable steps. Each milestone must preserve
current HFS/APFS/ext raw browse behavior and must not widen write support without
new evidence.

| Milestone | Name | User-visible result | Main risk | Required proof |
|---|---|---|---|---|
| M0 | Baseline and design lock | Current explorer documented, screenshots captured, no UX claim inflation | Building against stale assumptions | Doc update plus baseline tests |
| M1 | Explorer domain model | Commands, sessions, targets, and items have testable non-UI contracts | UI code keeps owning business rules | Unit tests for state and capability mapping |
| M2 | Files-like shell | Sidebar, command bar, omnibar, main pane, details pane frame exists | Visual churn breaks current actions | GUI smoke and accessibility checks |
| M3 | Command registry | Toolbar, context menu, shortcuts, and later palette share one action source | Disabled raw actions become inconsistent | Unit tests for every command capability |
| M4 | Target navigation | Mounted volumes, raw disks/partitions, manual images, favorites/recent targets in sidebar | Wrong target identity on raw media | Target identity tests and manual raw target smoke |
| M5 | Model/view details pane | `QTableWidget` replaced with model/view details list and multi-select | Large folders block UI | Model tests plus large-directory smoke |
| M6 | View modes, layout picker, and Files icons | Details/List/Grid/Cards plus layout picker, Files-derived generic icons, and persisted options | View duplication or icon drift creates inconsistent behavior | View-mode GUI tests plus icon manifest/render tests |
| M7 | Preview/details/safety/evidence pane | Persistent pane replaces modal-only preview for normal flow | Preview reads too much or hides blockers | Preview cap tests and blocker tests |
| M8 | Tabs and dual pane | Explorer tabs and optional split pane | State leaks between panes/tabs | Session tests and cross-pane command tests |
| M9 | Copy/import/export workflows | Copy out from raw, import into certified writers, safe local copy/paste | Accidental raw writes | Live HFS/APFS cert through command route |
| M10 | Omnibar search and command palette | `Ctrl+F` search mode and `Ctrl+Shift+P` command palette | Palette bypasses safety blockers | Command registry and search worker tests |
| M11 | Tags, favorites, recent, polish | Files-like daily-use polish without cloud/FTP/Git/integrations | Tags accidentally mutate raw FS metadata or copied brand assets leak in | Persistence tests plus resource audit |
| M12 | Certification and release gate | Docs, tests, screenshots, live evidence, icon attribution synced | Claims exceed evidence | Full CTest, release readiness, live certification, license gate |

## Step-By-Step Checklist

### M0 - Baseline And Design Lock

Goal: freeze current behavior and define exact UX target before refactor.

Files likely touched:

- `docs/FILE_MANAGEMENT_EXPLORER_FILES_LIKE_PLAN.md`
- `README.md`
- `tests/README.md`
- Optional screenshot artifacts under `artifacts/file-management-explorer-baseline/`

Checklist:

- [x] Capture current File Explorer desktop screenshot: `artifacts/file-management-explorer-baseline/desktop.png`.
- [x] Capture current File Explorer narrow-width screenshot: `artifacts/file-management-explorer-baseline/narrow.png`.
- [x] List current controls and shortcuts in this doc.
- [x] Record current backend capability matrix in this doc.
- [x] Confirm out-of-scope items: cloud drives, FTP, Git integration, third-party integrations.
- [x] Add no README claim that Files-like parity is complete.
- [x] Run current focused tests:
  - [x] `ctest --test-dir build -C Release --output-on-failure -R "test_file_explorer_types|test_file_explorer_item_model|test_file_management_explorer_panel|test_file_management_file_system|test_duplicate_finder_worker|test_organizer_worker|test_advanced_search_worker|test_advanced_search_controller"`
  - [x] `git diff --check`

Exit gate:

- [x] Baseline doc complete.
- [x] Current tests pass or blockers documented.

### M1 - Explorer Domain Model

Goal: move state and capability decisions out of widget code before UI rebuild.

New or changed production types:

- [x] `include/sak/file_explorer_types.h`
- [x] `src/core/file_explorer_types.cpp`
- [x] `include/sak/file_explorer_command_registry.h`
- [x] `src/core/file_explorer_command_registry.cpp`
- [x] Session state is currently represented by `FileExplorerPaneState` and `FileExplorerTabState`; a separate persisted session store remains scheduled for M8/M11.

Core structs:

- [x] `FileExplorerTargetId`
- [x] `FileExplorerLocation`
- [x] `FileManagementEntry` remains the backend item record for M0-M5; item-level capability mapping now lives in `FileExplorerItemCapabilities`.
- [x] `FileExplorerSelection`
- [x] `FileExplorerPaneState` / `FileExplorerTabState`
- [x] `FileExplorerViewSettings`
- [x] `FileExplorerPaneState`
- [x] `FileExplorerTabState`
- [x] `FileExplorerCommand`
- [x] `FileExplorerCommandState`
- [x] Operation results continue to use `FileManagementMutationResult` until the async operation-controller milestone.

Checklist:

- [x] Define stable target identity from bridge-provided IDs only; targets without explicit IDs are invalid instead of receiving synthetic identities.
- [x] Define location as target ID plus normalized path.
- [x] Define pane state: location, history, selection, sort, view settings.
- [x] Define tab state as one or two pane states plus active pane.
- [x] Define target capability flags from `FileManagementTarget`.
- [x] Define item capability flags from target plus item type.
- [x] Define command IDs for the implemented registry commands.
- [x] Implement command enablement without UI dependencies.
- [x] Return exact blocker text for disabled raw write commands.
- [x] Keep APFS 64-128 MiB generated-layout write gate centralized in `FileManagementFileSystemBridge`.
- [x] Keep HFS+/HFSX certified-write state centralized in `FileManagementFileSystemBridge`.

Tests:

- [x] Add `tests/unit/test_file_explorer_types.cpp`.
- [x] Add command enablement tests for local, ext4, HFS+, HFSX, certified APFS, blocked APFS, XFS, and Btrfs capability states.
- [x] Add path normalization tests for local and raw slash paths.
- [x] Add session history tests for back/forward/up.
- [x] Add selection tests for empty, single, and multi-select.

Exit gate:

- [x] No UI behavior changed except internal wiring if required.
- [x] New model tests pass.
- [x] Existing File Management tests pass.

### M2 - Files-Like Shell

Goal: replace certifier-style layout with Files-like frame while keeping one pane and current actions.

New or changed UI classes:

- [x] `include/sak/file_management_explorer_panel.h`
- [x] `src/gui/file_management_explorer_panel.cpp`
- [x] `include/sak/file_explorer_sidebar.h`
- [x] `src/gui/file_explorer_sidebar.cpp`
- [x] `include/sak/file_explorer_command_bar.h`
- [x] `src/gui/file_explorer_command_bar.cpp`
- [x] `include/sak/file_explorer_omnibar.h`
- [x] `src/gui/file_explorer_omnibar.cpp`
- [x] `include/sak/file_explorer_pane.h`
- [x] `src/gui/file_explorer_pane.cpp`
- [x] `include/sak/file_explorer_details_pane.h`
- [x] `src/gui/file_explorer_details_pane.cpp`

Checklist:

- [x] Replace top target combo row with sidebar target navigation.
- [x] Add command bar above content.
- [x] Add omnibar row with back, forward, up, editable path, search button, command button.
- [x] Add back/forward/up plus editable path row as the first omnibar slice.
- [x] Add main pane with current details listing.
- [x] Add persistent right details pane with Preview, Properties, Safety, and Evidence tabs.
- [x] Add bottom status strip for target, file system, selection count, write state, warnings.
- [x] Keep Refresh / Scan Disks / Add Raw or Image reachable through command bar or sidebar.
- [x] Keep current open, copy path, preview, new folder, write file, rename, delete behavior working.
- [x] Add collapse button for sidebar.
- [x] Add collapse button for details pane.
- [x] Ensure narrow width hides/collapses right pane before controls overlap.
- [x] Add accessible names/descriptions for new controls.
- [x] Avoid nested cards and oversized hero-like content.

Tests:

- [x] Add GUI test for shell creation.
- [x] Add GUI test for target selection from sidebar.
- [x] Add GUI test for command bar button states.
- [x] Add GUI test for omnibar path load.
- [x] Add accessibility pattern check.
- [x] Run `git diff --check`.

Exit gate:

- [x] UI looks like file manager shell, not utility form.
- [x] No clipped text at normal and narrow widths in the captured baseline.
- [x] Current live-certified operations still reachable.

### M3 - Command Registry Integration

Goal: one command registry drives toolbar, context menu, shortcuts, and later command palette.

Checklist:

- [x] Move open command into registry.
- [x] Move open in new tab command into registry, initially disabled until M8.
- [x] Move open in second pane command into registry, initially disabled until M8.
- [x] Move back/forward/up/home commands into registry.
- [x] Move refresh command into registry.
- [x] Move copy path and copy item path commands into registry.
- [x] Move preview command into registry.
- [x] Move properties command into registry.
- [x] Move select all, clear selection, invert selection into registry.
- [x] Move new folder command into registry.
- [x] Move write/import file command into registry.
- [x] Move rename command into registry.
- [x] Move delete command into registry.
- [x] Move hidden/extensions toggles into registry.
- [x] Move view-mode commands into registry.
- [x] Move pane/tab commands into registry, disabled until M8.
- [x] Assign default keyboard shortcuts.
- [x] Add command status text and accessible text.
- [x] Add blocker propagation to command bar tooltips and Safety details pane.
- [x] Add blocker propagation to context menu status.
- [x] Add blocker propagation to status bar.

Context menu checklist:

- [x] Right-click blank area menu.
- [x] Right-click file item menu.
- [x] Right-click directory item menu.
- [x] Right-click sidebar target menu.
- [x] Disabled raw actions visible with blocker text.

Tests:

- [x] Unit command-state snapshot for representative local, ext4, HFS+, HFSX, APFS, XFS, and Btrfs states.
- [x] GUI context menu action count and enabled-state tests.
- [x] GUI shortcut smoke tests for Refresh and details-pane toggle.
- [x] Keyboard shortcut tests cover non-modal safe routes now; modal/destructive shortcut execution stays manual/live-certification gated.

Exit gate:

- [x] No duplicate enablement logic in toolbar/context/shortcut handlers.
- [x] Unsupported raw writes show same blocker everywhere.

### M4 - Target Navigation And Sidebar Completion

Goal: sidebar becomes primary way to navigate targets.

Checklist:

- [x] Add Home row.
- [x] Add Favorites group.
- [x] Add This PC group.
- [x] Add Mounted Volumes group.
- [x] Add Disks and Partitions group from `StorageInventoryWorker`.
- [x] Add Raw Images group.
- [x] Add Recent group.
- [x] Add Certification Targets group for write-capable APFS/HFS/HFSX raw targets.
- [x] Add icons for local disk, raw partition/image, folder/home, and command surfaces.
- [x] Add secondary text: file system, source, mounted/raw state, and capability tooltip.
- [x] Add text badges: Read-only, Write certified, Blocked, Writable.
- [x] Add pin/unpin favorite.
- [x] Add recent target insertion on navigation.
- [x] Add rescan disks command.
- [x] Add manual raw/image target command.
- [x] Add properties command for target.
- [x] Add target identity validation before every raw write.

Persistence checklist:

- [x] Persist favorites.
- [x] Persist recent targets.
- [x] Persist last target.
- [x] Reject stale UI/raw target mismatch before mutation by checking current pane target identity; disk serial/size mismatch hard-fail remains part of live raw-operation preflight.

Tests:

- [x] Unit target ID stability tests.
- [x] Unit stale raw target rejection covered by target-identity command/mutation guard path and live raw-operation preflight.
- [x] GUI sidebar grouping tests.
- [x] GUI pin/unpin action exposure tests.
- [ ] Manual smoke for scanned APFS/HFS partitions remains pending until next live-device pass.

Exit gate:

- [x] User can reach local volumes and raw targets without top combo.
- [x] Wrong pane/target mismatch cannot write silently; full disk-renumber proof remains part of M12 live certification.

### M5 - Model/View Details List And Multi-Select

Goal: replace `QTableWidget` with scalable model/view and selection support.

Status: complete for current backend fields. Local mounted targets now expose
modified/created timestamps through `FileManagementEntry`; raw/image readers keep
those fields empty until each reader can provide trustworthy metadata.

New or changed classes:

- [x] `FileExplorerItemModel`
- [x] `FileExplorerSortFilterModel`
- [x] `FileExplorerDetailsView`

Checklist:

- [x] Implement item model from `FileManagementListResult`.
- [x] Add roles for name, type, size, path, ID, directory flag, regular-file flag, symlink flag, link target, and attribute summary.
- [x] Add sortable columns.
- [x] Add safe multi-select to the current details table.
- [x] Add baseline keyboard navigation through Qt model/view and command shortcuts.
- [x] Add selection summary in status bar.
- [x] Add virtualized/lazy behavior where Qt model supports it.
- [x] Add cancellation or stale-result discard for slow listing.
- [x] Add loading state.
- [x] Add empty-folder state.
- [x] Add error state that still preserves target/path.

Details columns:

- [x] Name.
- [x] Type.
- [x] Size.
- [x] Modified/Created where available.
- [x] File-system ID.
- [x] Attributes/capability.
- [x] Full path.

Tests:

- [x] Unit model row/column/role tests.
- [x] Unit sort tests.
- [x] Unit timestamp role/display tests.
- [x] Unit hidden-item and file-extension display tests.
- [x] Unit multi-select command-state tests.
- [x] GUI details table exposes extended selection mode.
- [x] GUI double-click/open-route tests.
- [x] GUI details column resize persistence tests.

Exit gate:

- [x] Details view supports multi-select and sorting.
- [x] Large directory listing does not freeze UI.

### M6 - View Modes And Layout Picker

Goal: add Files-style view switching.

Icon checklist:

- [x] Add `FileExplorerIconRegistry`.
- [x] Import approved Files layout icons from `Icons.SizeLayout.xaml`.
- [x] Import approved 28 px Files layout icons from `Icons.SizeLayout28.xaml` for larger picker previews.
- [x] Import approved Files common command icons from `Icons.Common.xaml`.
- [x] Import approved Files tab/pane icons from `Icons.TabPane.xaml` and `Icons.Panel.xaml`.
- [x] Import approved Files new/open icons from `Icons.New.xaml` and `Icons.Open.xaml`.
- [x] Import approved Files favorite/status/property icons from `Icons.Favorite.xaml`, `Icons.Status.xaml`, and `Icons.Properties.Dialog.xaml`.
- [x] Confirm M6 has no consuming UI for S.A.K.-specific APFS/HFS/ext/raw/evidence/safety icons; defer them to the first consuming safety/evidence/transfer milestone and source them through Icons8 MCP.
- [x] Add icon source manifest with upstream commit, source file, source key, target resource path, and license notice.
- [x] Add registry/resource tests that verify mapped commands resolve bundled Files-derived assets.
- [x] Add icon render tests for normal, disabled, active/hover-equivalent, selected, and command-bar sizes.

Checklist:

- [x] Add details view.
- [x] Add list view.
- [x] Add grid view.
- [x] Add cards view.
- [x] Add columns view with current-folder column plus lazy selected-directory child preview column.
- [x] Add adaptive view command.
- [x] Add Files-style View/layout picker command-bar entry point.
- [x] Add layout picker menu with Details/List/Grid/Cards/Columns and view-related commands.
- [x] Keep unsupported future-only commands disabled with product-grade blockers.
- [x] Add functional layout picker popover.
- [x] Add item-size slider.
- [x] Add show hidden toggle.
- [x] Add show file extensions toggle.
- [x] Add per-target/path view settings persistence through new location-scoped settings only; no legacy/global view fallback.
- [x] Ensure view modes share same model and command registry.
- [x] Ensure selection survives view switching.
- [x] Ensure current path survives view switching.

View-specific requirements:

- [x] Details: dense row list with columns.
- [x] List: single-column compact file list.
- [x] Grid: icon grid with stable cell size.
- [x] Cards: larger row/card with metadata, no nested page cards.
- [x] Columns: current path and selected child directory in side-by-side columns with lazy child loading.

Tests:

- [x] GUI layout picker exposes Files view modes without milestone labels or unfinished command text.
- [x] GUI switch details/list/grid/cards/adaptive.
- [x] GUI Columns view exposes current and child-preview columns.
- [x] Selection survives layout switch.
- [x] Hidden/extensions toggles affect display only, not raw data.
- [x] View settings persist and restore per target/path.
- [x] Narrow-width layout picker remains usable.
- [x] Imported Files icons render without clipping at 16 px, 20 px, 24 px, and command-bar size.

Exit gate:

- [x] Details/List/Grid/Cards share one command/selection model and are usable anywhere the File Management bridge can list.
- [x] Layout picker does not overlap or clip text in desktop and narrow GUI tests.
- [x] Icon source manifest and third-party notice entries are ready for any copied Files icon asset.

### M7 - Preview, Properties, Safety, Evidence Pane

Goal: make the right pane useful and remove modal-only preview as primary flow.

Checklist:

- [ ] Add Preview tab.
- [ ] Add Properties tab.
- [ ] Add Safety tab.
- [ ] Add Evidence tab.
- [ ] Add text preview with read cap.
- [ ] Add hex preview for binary files.
- [ ] Add local image preview.
- [ ] Add raw-readable image preview if read cap and decoder are safe.
- [ ] Add unsupported preview blocker.
- [ ] Add hash-on-demand command.
- [ ] Add file-system-specific metadata.
- [ ] Add HFS+ fork/resource/attribute summary where reader exposes it.
- [ ] Add APFS object/extent summary where reader exposes it.
- [ ] Add target write state.
- [ ] Add allowed/blocked command list.
- [ ] Add evidence report links/paths where known.
- [ ] Add last operation result with hashes and warnings.

Safety pane checklist:

- [ ] Shows raw target identity.
- [ ] Shows file system.
- [ ] Shows write capability.
- [ ] Shows why APFS large target is read-only.
- [ ] Shows why arbitrary APFS is read-only.
- [ ] Shows why XFS/Btrfs browse/write is blocked.
- [ ] Shows destructive-operation confirmation requirements.

Tests:

- [ ] Unit preview cap test.
- [ ] GUI preview text file.
- [ ] GUI preview binary hex.
- [ ] GUI properties for local file.
- [ ] GUI safety pane for ext4, HFS, APFS 128 MiB, APFS large, XFS/Btrfs.
- [ ] Evidence path rendering test.

Exit gate:

- [ ] User can understand target safety without opening modal dialogs.
- [ ] Preview cannot read beyond configured cap.

### M8 - Explorer Tabs And Dual Pane

Goal: add Files-style multitasking.

Tabs checklist:

- [ ] Add explorer tab strip inside File Explorer tab.
- [ ] Add new tab button.
- [ ] Add close tab.
- [ ] Add duplicate tab.
- [ ] Add open target in new tab.
- [ ] Add open folder in new tab.
- [ ] Add active tab state persistence.
- [ ] Add closed-tab restore; M8 remains open until the action is real or removed from visible UI.
- [ ] Ensure File Management outer tabs remain unaffected.

Dual-pane checklist:

- [ ] Add second pane toggle.
- [ ] Add vertical split.
- [ ] Add horizontal split if practical.
- [ ] Add active pane highlight.
- [ ] Add open folder in second pane.
- [ ] Add independent target/path/history per pane.
- [ ] Add independent view mode per pane.
- [ ] Add command routing to active pane.
- [ ] Add status bar active pane summary.

Tests:

- [ ] Unit tab session serialization.
- [ ] Unit pane state isolation.
- [ ] GUI create/close/duplicate tab.
- [ ] GUI dual-pane toggle.
- [ ] GUI open folder in second pane.
- [ ] GUI command affects active pane only.

Exit gate:

- [ ] Tabs and dual pane do not corrupt target/path/session state.
- [ ] Raw write commands still validate destination target identity.

### M9 - Copy, Import, Export, And Transfer

Goal: implement actual file-manager transfer workflows while preserving raw safety.

Copy-out checklist:

- [ ] Copy selected local files to clipboard/reference.
- [ ] Copy selected raw readable file out to local destination.
- [ ] Copy selected raw folder out recursively where reader supports export.
- [ ] Add progress and cancel.
- [ ] Add overwrite/collision policy.
- [ ] Add read-back/hash proof for raw copy-out where useful.

Copy-in/import checklist:

- [ ] Paste/import local file into local folder.
- [ ] Paste/import local file into certified HFS+/HFSX target.
- [ ] Paste/import local file into certified 64-128 MiB generated APFS target.
- [ ] Block paste/import into ext raw/image.
- [ ] Block paste/import into APFS large target.
- [ ] Block paste/import into arbitrary APFS.
- [ ] Block paste/import into XFS/Btrfs.
- [ ] Add typed confirmation for raw destination writes.
- [ ] Add target identity validation immediately before write.
- [ ] Add operation result with hash/read-back.

Cross-pane checklist:

- [ ] Copy from pane A to pane B when B is writable.
- [ ] Copy from raw pane to local pane.
- [ ] Copy from local pane to certified HFS/APFS pane.
- [ ] Compare folder contents between panes.
- [ ] Show blocked transfer reasons in command tooltip and Safety pane.

Tests:

- [ ] Unit transfer command enablement matrix.
- [ ] Local copy/paste smoke.
- [ ] ext4 raw copy-out smoke.
- [ ] HFS+ live import/write/read/delete certification through command route.
- [ ] APFS 128 MiB live import/write/read/delete certification through command route.
- [ ] APFS large paste blocker test.

Exit gate:

- [ ] File transfer feels like file manager.
- [ ] No unsupported raw write path exists.

### M10 - Omnibar Search And Command Palette

Goal: add Files-style discovery and quick search without replacing Advanced Search.

Omnibar search checklist:

- [x] Add current-folder filter on `Ctrl+F`.
- [x] Filter current path by default.
- [ ] Add current target badge in search field.
- [ ] Add suggestions/flyout if cheap.
- [ ] Route local search to existing worker.
- [ ] Route raw/image search to File Management bridge and Advanced Search worker where supported.
- [ ] Add search result list.
- [ ] Add open result.
- [ ] Add open result location.
- [ ] Add clear search.
- [ ] Preserve search history if existing settings support it.

Command palette checklist:

- [x] Add command mode on `Ctrl+Shift+P`.
- [x] List commands from command registry.
- [x] Filter commands by typed text.
- [x] Show shortcut and disabled reason.
- [x] Execute enabled command.
- [x] Block disabled command with exact reason, not silent close.
- [ ] Add command groups: Navigation, File, View, Pane, Target, Safety.

Tests:

- [ ] Unit command palette filter.
- [x] GUI `Ctrl+Shift+P` opens palette.
- [ ] GUI disabled command shows blocker.
- [x] GUI `Ctrl+F` filters current folder.
- [ ] GUI raw search uses existing supported target path.

Exit gate:

- [ ] User can discover commands without toolbar.
- [ ] Search is useful for current location and does not duplicate full Advanced Search UI.

### M11 - Tags, Favorites, Recent, And Polish

Goal: close Files-like daily-use gaps that do not require cloud/FTP/Git/integration scope.

Favorites checklist:

- [ ] Pin folder/target to sidebar.
- [ ] Unpin folder/target.
- [ ] Reorder favorites if simple.
- [ ] Persist favorites.
- [ ] Show stale favorite warning if target identity no longer matches.

Recent checklist:

- [ ] Track recent local paths.
- [ ] Track recent raw image paths.
- [ ] Track recent scanned partition targets by identity.
- [ ] Clear recent list command.
- [ ] Do not persist sensitive transient evidence paths unless user opens them.

Tags checklist:

- [ ] Define app-level tag storage.
- [ ] Tag selected item.
- [ ] Remove tag.
- [ ] Show tag badges.
- [ ] Add tag column in details view.
- [ ] Add tag group in sidebar.
- [ ] Filter by tag.
- [ ] Do not write tags into raw HFS/APFS metadata.

Polish checklist:

- [ ] Review spacing and density against Files reference.
- [ ] Ensure command bar icons and labels are consistent.
- [ ] Ensure imported Files icons are used for generic commands before custom S.A.K. icons are considered.
- [ ] Ensure Files brand/app logos and excluded integration icons are absent from S.A.K. resources.
- [ ] Ensure all icon-only commands have tooltips and accessible names.
- [ ] Add empty states.
- [ ] Add loading states.
- [ ] Add blocked states.
- [ ] Add final dark/light theme pass if supported.
- [ ] Add desktop/narrow screenshots to artifacts.

Tests:

- [ ] Favorites persistence tests.
- [ ] Recent persistence tests.
- [ ] Tags persistence tests.
- [ ] GUI tag add/remove/filter.
- [ ] Visual QA screenshots.
- [ ] Accessibility checks.

Exit gate:

- [ ] Explorer feels complete for local and supported raw browse/write workflows.
- [ ] Tags/favorites do not mutate raw file-system metadata.

### M12 - Certification, Release Gate, And Docs

Goal: prove implementation end to end and align public claims with evidence.

Certification checklist:

- [ ] Run full local CTest.
- [ ] Run focused File Management tests.
- [ ] Run GUI tests for explorer shell.
- [ ] Run accessibility pattern check.
- [ ] Run PowerShell syntax check.
- [ ] Run filesystem manifest check.
- [ ] Run `git diff --check`.
- [ ] Run release readiness or document unrelated blocker.
- [ ] Run HFS+ destructive live File Explorer command-route certification.
- [ ] Run APFS 128 MiB destructive live File Explorer command-route certification.
- [ ] Run ext4 read-only raw copy-out certification.
- [ ] Run XFS/Btrfs blocker proof.
- [ ] Run large APFS write blocker proof.
- [ ] Run local mounted copy/paste smoke.
- [ ] Capture desktop screenshot.
- [ ] Capture narrow screenshot.
- [ ] Capture dual-pane screenshot.
- [ ] Capture details pane screenshot.
- [ ] Capture icon comparison screenshot against Files reference surfaces: command bar, layout picker, omnibar, sidebar, tabs, dual pane, details pane.

Documentation checklist:

- [ ] Update README File Explorer section with only completed capabilities.
- [x] Update CHANGELOG Unreleased with evidence paths.
- [x] Update `THIRD_PARTY_LICENSES.md` with Files Community MIT attribution for any copied icon/source asset.
- [ ] Update `docs/PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md` capability matrix.
- [ ] Update `docs/PARTITION_MANAGER_CERTIFICATION.md` live proof section.
- [ ] Update `docs/RELEASE_READINESS.md` release gate notes.
- [ ] Update `tests/README.md` with new tests and live cert lanes.
- [ ] Mark plan checklist items complete only when proof exists.

Release wording gate:

- [ ] Do not say "Files parity" unless acceptance criteria pass.
- [ ] Say "Files-inspired" until tabs, dual pane, layout picker, command palette, and details pane exist.
- [ ] Keep cloud drives, FTP, Git integration, and third-party integrations explicitly out of scope.
- [ ] Keep APFS large/arbitrary write blockers visible.
- [ ] Keep Apple-native APFS validation status accurate.

Exit gate:

- [ ] Tests pass.
- [ ] Live proof pass.
- [ ] Docs match evidence.
- [ ] No release-facing claim exceeds certification.

## Cross-Cutting Implementation Rules

- [ ] Keep current backend bridge stable until replacement has tests.
- [ ] Prefer extracting state/model code before moving UI.
- [ ] Every new UI command must enter command registry first.
- [ ] Every command must have enabled state, disabled blocker, shortcut metadata, accessible text, and status text.
- [ ] Every raw write command must validate target identity immediately before write.
- [ ] Every destructive raw command must show file system, target identity, item count, and irreversible warning.
- [ ] Every disabled raw command must be visible somewhere with exact reason.
- [ ] Every async operation must support stale-result discard at minimum; cancellation where practical.
- [ ] No UI thread raw partition scans.
- [ ] No hidden broadening of APFS/HFS writer scope.
- [ ] No copied Files app branding, Store tile, splash screen, cloud-drive, Git, FTP, or third-party integration icon assets.
- [ ] Any copied Files icon/source asset must have manifest traceability and third-party license attribution before release.
- [ ] No cloud/FTP/Git/third-party integration work in this milestone.

## Build Order Within Each Milestone

Use this loop for each milestone:

1. Add or update model/types first.
2. Add unit tests for model/types.
3. Add UI shell or command wiring.
4. Add GUI tests for visible behavior.
5. Update docs in same change.
6. Run focused tests.
7. Run `git diff --check`.
8. If milestone touches raw writes, run live cert before marking complete.
9. Update checklist boxes only after proof exists.

## Milestone Dependency Graph

```text
M0
 |
M1 ----+
 |     |
M2     |
 |     |
M3 <---+
 |
M4
 |
M5
 |
M6
 |
M7
 |
M8
 |
M9
 |
M10
 |
M11
 |
M12
```

M3 can start after M1 and continue while M2 shell work lands. M9 must wait for M8 if cross-pane transfer is included, but copy-out/import commands can start after M5 when model/selection state is stable.

## Acceptance Criteria

Do not call this Files-like complete until:

- Sidebar exists and is useful.
- Omnibar supports breadcrumb, edit path, search, command entry.
- Command bar and context menu share one command registry.
- At least Details/List/Grid/Cards view modes exist.
- Preview/details pane is persistent.
- Multi-select works.
- Keyboard shortcuts cover core actions.
- Tabs or dual pane exists; both preferred for parity.
- Generic command/layout/omnibar/sidebar icons are imported from approved Files source assets where possible.
- Files app branding, Store tile, splash screen, cloud-drive, Git, FTP, and third-party integration icons are absent.
- Copied Files icon/source assets have manifest entries and MIT attribution.
- Raw file-system blockers are visible, exact, and tested.
- HFS/APFS live destructive certification passes through new command route.
- Full local CTest passes.
- Release readiness passes or documented non-File-Explorer blockers remain.

## Documentation Updates Required During Build

- README File Explorer section: update only when phase exits pass.
- CHANGELOG Unreleased: describe each completed phase and evidence paths.
- `docs/PARTITION_MANAGER_CROSS_FILESYSTEM_PLAN.md`: keep raw FS capability matrix synced.
- `docs/PARTITION_MANAGER_CERTIFICATION.md`: add live proof only after destructive certification.
- `tests/README.md`: add unit/gui/live certification coverage.

## Open Questions

- Should explorer tabs persist across app restarts in first release or later?
- Should local mounted targets use native OS shell copy/paste, S.A.K. copy engine, or both?
- Should tags be global across targets or per-target identity only?
- Should APFS generated-target write commands stay visible in toolbar or move behind an "Advanced raw write" overflow group?
- Should file thumbnails be local-only in first pass, or support raw-image extracted previews?
