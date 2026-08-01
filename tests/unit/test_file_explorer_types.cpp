// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_types.cpp
/// @brief Unit tests for File Explorer state and command registry.

#include "sak/file_explorer_command_registry.h"
#include "sak/file_explorer_layout_metrics.h"
#include "sak/file_explorer_status_center.h"
#include "sak/file_explorer_storage_history.h"

#include <QtTest/QtTest>

namespace {

sak::FileManagementTarget writableLocalTarget() {
    sak::FileManagementTarget target;
    target.id = QStringLiteral("local:C:/fixture");
    target.label = QStringLiteral("Local Fixture");
    target.root_path = QStringLiteral("C:/fixture");
    target.file_system = QStringLiteral("NTFS");
    target.local_file_system = true;
    target.read_only = false;
    target.can_browse = true;
    target.can_read_files = true;
    target.can_write_files = true;
    target.can_organize = true;
    return target;
}

sak::FileManagementTarget readOnlyRawTarget(const QString& blocker) {
    sak::FileManagementTarget target;
    target.id = QStringLiteral("disk:4:partition:2");
    target.label = QStringLiteral("ext4 raw fixture");
    target.root_path = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk4\\Partition2");
    target.file_system = QStringLiteral("ext4");
    target.local_file_system = false;
    target.read_only = true;
    target.can_browse = true;
    target.can_read_files = true;
    target.can_write_files = false;
    target.can_organize = false;
    target.blockers.append(blocker);
    return target;
}

sak::FileManagementTarget rawTarget(const QString& file_system,
                                    const bool can_browse,
                                    const bool can_read,
                                    const bool can_write,
                                    const QString& blocker = {}) {
    sak::FileManagementTarget target;
    target.id = QStringLiteral("raw:%1:%2")
                    .arg(file_system, can_write ? QStringLiteral("rw") : QStringLiteral("ro"));
    target.label = QStringLiteral("%1 fixture").arg(file_system);
    target.root_path = QStringLiteral("\\\\?\\GLOBALROOT\\Device\\Harddisk7\\Partition1");
    target.file_system = file_system;
    target.source = QStringLiteral("Unit fixture");
    target.local_file_system = false;
    target.read_only = !can_write;
    target.can_browse = can_browse;
    target.can_read_files = can_read;
    target.can_write_files = can_write;
    target.can_organize = false;
    if (!blocker.isEmpty()) {
        target.blockers.append(blocker);
    }
    return target;
}

sak::FileManagementEntry selectedFile() {
    sak::FileManagementEntry entry;
    entry.name = QStringLiteral("note.txt");
    entry.path = QStringLiteral("/note.txt");
    entry.type = QStringLiteral("file");
    entry.size_bytes = 42;
    entry.regular_file = true;
    return entry;
}

sak::FileManagementEntry selectedDirectory() {
    sak::FileManagementEntry entry;
    entry.name = QStringLiteral("folder");
    entry.path = QStringLiteral("/folder");
    entry.type = QStringLiteral("directory");
    entry.directory = true;
    return entry;
}

sak::FileExplorerCommandContext contextFor(const sak::FileManagementTarget& target,
                                           const bool with_selection) {
    sak::FileExplorerCommandContext context;
    context.target = target;
    context.pane.location.target_id = sak::FileExplorerTargetId::fromTarget(target);
    context.pane.location.path = sak::FileExplorerLocation::normalizePath(target.local_file_system
                                                                              ? target.root_path
                                                                              : QStringLiteral("/"),
                                                                          target.local_file_system);
    if (with_selection) {
        context.pane.selection.entries.append(selectedFile());
    }
    return context;
}

}  // namespace

class FileExplorerTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void targetIdsAndRawPathsAreStable() {
        auto target = readOnlyRawTarget(QStringLiteral("read-only fixture"));
        const auto target_id = sak::FileExplorerTargetId::fromTarget(target);

        QCOMPARE(target_id.value, QStringLiteral("disk:4:partition:2"));
        QVERIFY(!target_id.isEmpty());
        target.id.clear();
        const auto missing_id = sak::FileExplorerTargetId::fromTarget(target);
        QVERIFY(missing_id.isEmpty());
        QCOMPARE(sak::FileExplorerLocation::normalizePath(
                     QStringLiteral("\\Users\\Randy\\..\\Test"), false),
                 QStringLiteral("/Users/Test"));
        QCOMPARE(sak::FileExplorerLocation::normalizePath(QStringLiteral("C:\\fixture\\.\\child"),
                                                          true),
                 QStringLiteral("C:/fixture/child"));
        QCOMPARE(sak::FileExplorerLocation::parentPath(QStringLiteral("/Users/Test"), false),
                 QStringLiteral("/Users"));

        sak::FileExplorerLocation root;
        root.target_id = target_id;
        root.path = QStringLiteral("/");
        QVERIFY(root.atRoot(false));
    }

    void paneNavigationMaintainsBackForwardHistory() {
        sak::FileExplorerPaneState pane;
        sak::FileExplorerLocation root;
        root.target_id.value = QStringLiteral("raw:apfs");
        root.path = QStringLiteral("/");
        sak::FileExplorerLocation users = root;
        users.path = QStringLiteral("/Users");
        sak::FileExplorerLocation randy = root;
        randy.path = QStringLiteral("/Users/Randy");

        pane.navigateTo(root, false);
        pane.navigateTo(users, false);
        pane.navigateTo(randy, false);

        QCOMPARE(pane.location.path, QStringLiteral("/Users/Randy"));
        QVERIFY(pane.canGoBack());
        QVERIFY(pane.goBack());
        QCOMPARE(pane.location.path, QStringLiteral("/Users"));
        QVERIFY(pane.canGoForward());
        QVERIFY(pane.goForward());
        QCOMPARE(pane.location.path, QStringLiteral("/Users/Randy"));
        QVERIFY(pane.goUp(false));
        QCOMPARE(pane.location.path, QStringLiteral("/Users"));
    }

    void dualPaneStatesTrackIndependentHistories() {
        // Each explorer pane owns a full FileExplorerPaneState; navigating one
        // must never disturb the other (the basis for dual-pane isolation).
        sak::FileExplorerPaneState left;
        sak::FileExplorerPaneState right;

        sak::FileExplorerLocation la;
        la.target_id.value = QStringLiteral("raw:left");
        la.path = QStringLiteral("/");
        sak::FileExplorerLocation la2 = la;
        la2.path = QStringLiteral("/one");

        sak::FileExplorerLocation rb;
        rb.target_id.value = QStringLiteral("raw:right");
        rb.path = QStringLiteral("/");
        sak::FileExplorerLocation rb2 = rb;
        rb2.path = QStringLiteral("/two");

        left.navigateTo(la, false);
        left.navigateTo(la2, false);
        right.navigateTo(rb, false);
        right.navigateTo(rb2, false);

        QCOMPARE(left.location.path, QStringLiteral("/one"));
        QCOMPARE(right.location.path, QStringLiteral("/two"));

        // Going back in the left pane leaves the right pane untouched.
        QVERIFY(left.goBack());
        QCOMPARE(left.location.path, QStringLiteral("/"));
        QCOMPARE(right.location.path, QStringLiteral("/two"));
        QCOMPARE(left.location.target_id.value, QStringLiteral("raw:left"));
        QCOMPARE(right.location.target_id.value, QStringLiteral("raw:right"));
    }

    void selectionSummariesExposeItemShape() {
        sak::FileExplorerSelection selection;
        selection.entries.append(selectedFile());

        QVERIFY(selection.hasSingleEntry());
        QCOMPARE(selection.count(), 1);

        selection.entries.append(selectedDirectory());

        QCOMPARE(selection.count(), 2);
        QVERIFY(selection.containsRegularFile());
        QVERIFY(selection.containsDirectory());
        QCOMPARE(selection.totalRegularFileBytes(), 42ULL);
        QCOMPARE(selection.paths(),
                 QStringList({QStringLiteral("/note.txt"), QStringLiteral("/folder")}));

        selection.clear();
        QVERIFY(selection.isEmpty());
    }

    void itemCapabilitiesFollowTargetAndEntryShape() {
        const auto writable = sak::FileExplorerItemCapabilities::fromTargetAndEntry(
            writableLocalTarget(), selectedFile());

        QVERIFY(writable.can_open);
        QVERIFY(writable.can_preview);
        QVERIFY(writable.can_copy_path);
        QVERIFY(writable.can_rename);
        QVERIFY(writable.can_delete);
        QVERIFY(writable.blockers.isEmpty());

        const auto read_only = sak::FileExplorerItemCapabilities::fromTargetAndEntry(
            readOnlyRawTarget(QStringLiteral("raw fixture write blocker")), selectedFile());

        QVERIFY(read_only.can_open);
        QVERIFY(read_only.can_preview);
        QVERIFY(read_only.can_copy_path);
        QVERIFY(!read_only.can_rename);
        QVERIFY(!read_only.can_delete);
        QVERIFY(read_only.blockers.join(QStringLiteral("; "))
                    .contains(QStringLiteral("write blocker")));
    }

    void registryEnablesWritableLocalCommands() {
        const auto context = contextFor(writableLocalTarget(), true);

        const auto new_folder =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::NewFolder, context);
        const auto rename =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Rename, context);
        const auto delete_item =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Delete, context);
        const auto preview =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Preview, context);

        QVERIFY2(new_folder.enabled, qPrintable(new_folder.blocker));
        QVERIFY2(rename.enabled, qPrintable(rename.blocker));
        QVERIFY2(delete_item.enabled, qPrintable(delete_item.blocker));
        QVERIFY(delete_item.command.destructive);
        QVERIFY2(preview.enabled, qPrintable(preview.blocker));
    }

    void registryBlocksReadOnlyRawWritesButAllowsPreview() {
        const auto target = readOnlyRawTarget(QStringLiteral("ext raw target is read-only"));
        const auto context = contextFor(target, true);

        const auto write_file =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::WriteFile, context);
        const auto delete_item =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Delete, context);
        const auto preview =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Preview, context);

        QVERIFY(!write_file.enabled);
        QVERIFY(write_file.blocker.contains(QStringLiteral("read-only")));
        QVERIFY(!delete_item.enabled);
        QVERIFY(delete_item.blocker.contains(QStringLiteral("read-only")));
        QVERIFY2(preview.enabled, qPrintable(preview.blocker));
    }

    void registryCoversSupportedAndBlockedTargetTypes() {
        const QVector<sak::FileManagementTarget> writable_targets{
            writableLocalTarget(),
            rawTarget(QStringLiteral("HFS+"), true, true, true),
            rawTarget(QStringLiteral("HFSX"), true, true, true),
            rawTarget(QStringLiteral("APFS"), true, true, true),
        };
        for (const auto& target : writable_targets) {
            const auto context = contextFor(target, true);
            QVERIFY2(sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Preview,
                                                             context)
                         .enabled,
                     qPrintable(target.file_system));
            QVERIFY2(sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::WriteFile,
                                                             context)
                         .enabled,
                     qPrintable(target.file_system));
        }

        const QVector<sak::FileManagementTarget> blocked_targets{
            rawTarget(QStringLiteral("ext4"),
                      true,
                      true,
                      false,
                      QStringLiteral("ext4 raw target is read-only")),
            rawTarget(QStringLiteral("APFS"),
                      true,
                      true,
                      false,
                      QStringLiteral("APFS target is outside certified write envelope")),
            rawTarget(QStringLiteral("XFS"),
                      false,
                      false,
                      false,
                      QStringLiteral("XFS directory reader unavailable")),
            rawTarget(QStringLiteral("Btrfs"),
                      false,
                      false,
                      false,
                      QStringLiteral("Btrfs directory reader unavailable")),
        };
        for (const auto& target : blocked_targets) {
            const auto context = contextFor(target, true);
            const auto write = sak::FileExplorerCommandRegistry::state(
                sak::FileExplorerCommandId::WriteFile, context);
            QVERIFY(!write.enabled);
            QVERIFY2(!write.blocker.isEmpty(), qPrintable(target.file_system));
        }
    }

    void registryHashNeedsSingleReadableSelection() {
        using sak::FileExplorerCommandId;
        // No selection: hashing has nothing to act on.
        QVERIFY(!sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::Hash,
                                                         contextFor(writableLocalTarget(), false))
                     .enabled);
        // A single readable file, even on a read-only raw target, can be hashed.
        QVERIFY(sak::FileExplorerCommandRegistry::state(
                    FileExplorerCommandId::Hash,
                    contextFor(readOnlyRawTarget(QStringLiteral("read-only raw fixture")), true))
                    .enabled);
        // A target with no read capability blocks hashing with a reason.
        const auto blocked = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::Hash,
            contextFor(rawTarget(QStringLiteral("xfs"), false, false, false), true));
        QVERIFY(!blocked.enabled);
        QVERIFY(!blocked.blocker.isEmpty());
    }

    void registryCopyOutNeedsSingleReadableSelection() {
        using sak::FileExplorerCommandId;
        // No selection: nothing to copy out.
        QVERIFY(!sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CopyOut,
                                                         contextFor(writableLocalTarget(), false))
                     .enabled);
        // A single readable file, even on a read-only raw target, can be copied out.
        QVERIFY(sak::FileExplorerCommandRegistry::state(
                    FileExplorerCommandId::CopyOut,
                    contextFor(readOnlyRawTarget(QStringLiteral("read-only raw fixture")), true))
                    .enabled);
        // A target with no read capability blocks copy-out with a reason.
        const auto blocked = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::CopyOut,
            contextFor(rawTarget(QStringLiteral("xfs"), false, false, false), true));
        QVERIFY(!blocked.enabled);
        QVERIFY(!blocked.blocker.isEmpty());
    }

    void registryCopyItemsNeedsReadableSelection() {
        using sak::FileExplorerCommandId;
        // No selection: nothing to copy.
        QVERIFY(!sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CopyItems,
                                                         contextFor(writableLocalTarget(), false))
                     .enabled);
        // Multi-select is allowed: two readable files can go to the clipboard together.
        auto multi = contextFor(readOnlyRawTarget(QStringLiteral("read-only raw fixture")), true);
        auto second = selectedFile();
        second.name = QStringLiteral("other.txt");
        second.path = QStringLiteral("/other.txt");
        multi.pane.selection.entries.append(second);
        QVERIFY(sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CopyItems, multi)
                    .enabled);
        // A target with no read capability blocks copy with a reason.
        const auto blocked = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::CopyItems,
            contextFor(rawTarget(QStringLiteral("btrfs"), false, false, false), true));
        QVERIFY(!blocked.enabled);
        QVERIFY(!blocked.blocker.isEmpty());
    }

    void registryPasteNeedsClipboardFilesAndWritableTarget() {
        using sak::FileExplorerCommandId;
        // Writable target but empty clipboard: paste stays disabled with the clipboard reason.
        auto context = contextFor(writableLocalTarget(), false);
        const auto no_clipboard =
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::Paste, context);
        QVERIFY(!no_clipboard.enabled);
        QVERIFY(no_clipboard.blocker.contains(QStringLiteral("clipboard"), Qt::CaseInsensitive));
        // Clipboard files + writable target: paste enables.
        context.clipboard_has_files = true;
        QVERIFY(
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::Paste, context).enabled);
        // Read-only raw target reports its real write blocker even with clipboard files.
        auto read_only = contextFor(readOnlyRawTarget(QStringLiteral("ext4 is read-only")), false);
        read_only.clipboard_has_files = true;
        const auto blocked = sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::Paste,
                                                                     read_only);
        QVERIFY(!blocked.enabled);
        QCOMPARE(blocked.blocker, QStringLiteral("ext4 is read-only"));
    }

    void registryCrossPaneCommandsNeedSplitAndWritableDestination() {
        using sak::FileExplorerCommandId;
        // No active split: both cross-pane commands explain the dual-pane requirement.
        auto context = contextFor(writableLocalTarget(), true);
        context.can_use_dual_pane = true;
        const auto no_split = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::CopyToOtherPane, context);
        QVERIFY(!no_split.enabled);
        QVERIFY(no_split.blocker.contains(QStringLiteral("dual pane"), Qt::CaseInsensitive));
        QVERIFY(
            !sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::ComparePanes, context)
                 .enabled);

        // Split active + writable other pane: copy and compare both enable.
        context.dual_pane_active = true;
        context.other_pane_target = writableLocalTarget();
        QVERIFY(
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CopyToOtherPane, context)
                .enabled);
        QVERIFY(
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::ComparePanes, context)
                .enabled);

        // Read-only other pane blocks the copy with the destination's real write blocker,
        // while compare stays available.
        context.other_pane_target = readOnlyRawTarget(QStringLiteral("ext4 is read-only"));
        const auto blocked = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::CopyToOtherPane, context);
        QVERIFY(!blocked.enabled);
        QCOMPARE(blocked.blocker, QStringLiteral("ext4 is read-only"));
        QVERIFY(
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::ComparePanes, context)
                .enabled);

        // Raw source + raw destination is allowed: the transfer stages through a
        // scratch host folder between the certified reader and writer.
        auto raw_source = contextFor(rawTarget(QStringLiteral("apfs"), true, true, true), true);
        raw_source.can_use_dual_pane = true;
        raw_source.dual_pane_active = true;
        raw_source.other_pane_target = rawTarget(QStringLiteral("hfsplus"), true, true, true);
        const auto raw_raw = sak::FileExplorerCommandRegistry::state(
            FileExplorerCommandId::CopyToOtherPane, raw_source);
        QVERIFY(raw_raw.enabled);
    }

    void registryGatesTabAndDualPaneCommandsByBuildAvailability() {
        // Tabs and dual pane are shipped features, but the registry still exposes
        // a build-availability gate: when a host reports it cannot host tabs or a
        // second pane, the commands must disable with a clear reason.
        auto context = contextFor(writableLocalTarget(), true);

        const auto new_tab = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::OpenInNewTab, context);
        const auto dual_pane = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::ToggleDualPane, context);

        QVERIFY(!new_tab.enabled);
        QVERIFY(new_tab.blocker.contains(QStringLiteral("unavailable")));
        QVERIFY(!dual_pane.enabled);
        QVERIFY(dual_pane.blocker.contains(QStringLiteral("unavailable")));

        context.can_create_tabs = true;
        context.can_use_dual_pane = true;
        QVERIFY(sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::OpenInNewTab,
                                                        context)
                    .enabled);
        QVERIFY(sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::ToggleDualPane,
                                                        context)
                    .enabled);
    }

    void registryEnablesLayoutAndViewToggleCommands() {
        const auto context = contextFor(writableLocalTarget(), true);

        const auto details = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::ViewDetails, context);
        const auto list =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::ViewList, context);
        const auto hidden = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::ToggleHiddenItems, context);

        // View layouts and toggles ship functional: in a normal writable-local
        // context they are enabled (the File Explorer panel exposes List/Grid/Cards/
        // Columns/Adaptive + Hidden Items as working actions, no milestone gating).
        QVERIFY2(details.enabled, qPrintable(details.blocker));
        QVERIFY2(list.enabled, qPrintable(list.blocker));
        QVERIFY2(hidden.enabled, qPrintable(hidden.blocker));
    }

    void registryReportsSelectionAndTargetBlockers() {
        const auto no_selection = contextFor(writableLocalTarget(), false);
        const auto rename = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::Rename, no_selection);
        QVERIFY(!rename.enabled);
        QCOMPARE(rename.blocker, QStringLiteral("Select an item first."));

        sak::FileExplorerCommandContext no_target;
        const auto refresh =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Refresh, no_target);
        QVERIFY(!refresh.enabled);
        QCOMPARE(refresh.blocker, QStringLiteral("No File Explorer target selected."));
    }

    void registryAllowsMultiDeleteButBlocksSingleItemCommands() {
        auto context = contextFor(writableLocalTarget(), true);
        context.pane.selection.entries.append(selectedDirectory());

        const auto rename =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Rename, context);
        const auto open = sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Open,
                                                                  context);
        const auto delete_item =
            sak::FileExplorerCommandRegistry::state(sak::FileExplorerCommandId::Delete, context);
        const auto copy_path = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::CopyItemPath, context);

        QVERIFY(!rename.enabled);
        QCOMPARE(rename.blocker, QStringLiteral("Select one item."));
        QVERIFY(!open.enabled);
        QCOMPARE(open.blocker, QStringLiteral("Select one item."));
        QVERIFY(delete_item.enabled);
        QVERIFY(copy_path.enabled);

        // Files DeleteItemPermanentlyAction: Shift+Delete, gated like Delete
        // (multi-select allowed, selection + write capability required).
        const auto permanent = sak::FileExplorerCommandRegistry::state(
            sak::FileExplorerCommandId::DeletePermanently, context);
        QVERIFY(permanent.enabled);
        QCOMPARE(permanent.command.shortcut, QStringLiteral("Shift+Del"));
        QVERIFY(permanent.command.destructive);
        auto empty_selection = contextFor(writableLocalTarget(), false);
        QVERIFY(!sak::FileExplorerCommandRegistry::state(
                     sak::FileExplorerCommandId::DeletePermanently, empty_selection)
                     .enabled);
    }

    void registryAssignsEveryCommandToAnOrderedGroup() {
        using Registry = sak::FileExplorerCommandRegistry;
        const auto order = Registry::groupOrder();
        QCOMPARE(order.size(), 6);

        // Every registered command resolves to a group that appears in groupOrder,
        // so the palette can never drop a command by leaving its section unrendered.
        for (const auto& command : Registry::commands()) {
            const auto group = Registry::group(command.id);
            QCOMPARE(command.group, group);
            QVERIFY2(order.contains(group), qPrintable(Registry::commandIdName(command.id)));
            QVERIFY(!Registry::groupName(group).isEmpty());
        }

        // Representative anchors for the section split.
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::Back),
                 sak::FileExplorerCommandGroup::Navigation);
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::Delete),
                 sak::FileExplorerCommandGroup::File);
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::ViewGrid),
                 sak::FileExplorerCommandGroup::View);
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::ToggleDualPane),
                 sak::FileExplorerCommandGroup::Pane);
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::Properties),
                 sak::FileExplorerCommandGroup::Target);
        QCOMPARE(Registry::group(sak::FileExplorerCommandId::Hash),
                 sak::FileExplorerCommandGroup::Safety);
    }

    void layoutMetricsMatchFilesSizeKindTables() {
        using sak::FileExplorerViewMode;

        // Files *ViewSizeKind enums: Details/List/Columns 1..5, Cards 1..4,
        // Grid 1..12; Adaptive shares the Grid size kind.
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::Details), 5);
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::List), 5);
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::Columns), 5);
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::Cards), 4);
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::Grid), 12);
        QCOMPARE(sak::fileExplorerSizeKindMax(FileExplorerViewMode::Adaptive), 12);

        // LayoutSizeKindHelper.GetDetailsViewRowHeight / GetListViewRowHeight.
        QCOMPARE(sak::fileExplorerRowHeight(FileExplorerViewMode::Details, 1), 28);
        QCOMPARE(sak::fileExplorerRowHeight(FileExplorerViewMode::Details, 5), 48);
        QCOMPARE(sak::fileExplorerRowHeight(FileExplorerViewMode::List, 1), 24);
        QCOMPARE(sak::fileExplorerRowHeight(FileExplorerViewMode::Columns, 5), 44);

        // GetGridViewItemWidth: 80 + 20 per kind up to 300.
        QCOMPARE(sak::fileExplorerGridItemWidth(1), 80);
        QCOMPARE(sak::fileExplorerGridItemWidth(8), 220);
        QCOMPARE(sak::fileExplorerGridItemWidth(12), 300);

        // GetIconSize: compact layouts 16/16/20/24/48, Cards 64/64/80/96,
        // Grid 96 then 128 through kind 8 then 256.
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::Details, 2), 16);
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::List, 5), 48);
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::Cards, 4), 96);
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::Grid, 1), 96);
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::Grid, 8), 128);
        QCOMPARE(sak::fileExplorerIconSize(FileExplorerViewMode::Grid, 9), 256);

        // Defaults: Details/List/Cards/Columns Small, Grid Large.
        const sak::FileExplorerLayoutSizes defaults;
        QCOMPARE(defaults.details, 2);
        QCOMPARE(defaults.list, 2);
        QCOMPARE(defaults.cards, 1);
        QCOMPARE(defaults.grid, 8);
        QCOMPARE(defaults.columns, 2);

        // Out-of-range kinds clamp instead of reading past the tables.
        sak::FileExplorerLayoutSizes wild;
        wild.grid = 99;
        wild.cards = 0;
        sak::clampFileExplorerLayoutSizes(wild);
        QCOMPARE(wild.grid, 12);
        QCOMPARE(wild.cards, 1);
    }

    void layoutCyclerRingMatchesFilesOrder() {
        using sak::FileExplorerViewMode;

        // Files LayoutCycler: Details -> List -> Cards -> Grid -> Columns,
        // wrapping in both directions; Adaptive resolves as Grid.
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Details, true),
                 FileExplorerViewMode::List);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::List, true),
                 FileExplorerViewMode::Cards);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Cards, true),
                 FileExplorerViewMode::Grid);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Grid, true),
                 FileExplorerViewMode::Columns);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Columns, true),
                 FileExplorerViewMode::Details);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Details, false),
                 FileExplorerViewMode::Columns);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Adaptive, true),
                 FileExplorerViewMode::Columns);
        QCOMPARE(sak::fileExplorerAdjacentLayout(FileExplorerViewMode::Adaptive, false),
                 FileExplorerViewMode::Cards);
    }

    void storageHistoryWrapperWalksAndTruncatesRedoBranch() {
        using Operation = sak::FileExplorerHistoryOperation;
        sak::FileExplorerStorageHistoryWrapper wrapper;
        QVERIFY(!wrapper.canUndo());
        QVERIFY(!wrapper.canRedo());
        QVERIFY(wrapper.undoTarget() == nullptr);
        QVERIFY(wrapper.redoTarget() == nullptr);

        const auto entry = [](const Operation operation, const QString& destination) {
            sak::FileExplorerStorageHistory history;
            history.operation = operation;
            history.items = {
                sak::FileExplorerHistoryItem{QStringLiteral("/src"), destination, false}};
            return history;
        };
        wrapper.add(entry(Operation::Rename, QStringLiteral("/renamed")));
        wrapper.add(entry(Operation::Copy, QStringLiteral("/copied")));
        QCOMPARE(wrapper.count(), 2);
        QVERIFY(wrapper.canUndo());
        QVERIFY(!wrapper.canRedo());
        QCOMPARE(wrapper.undoTarget()->operation, Operation::Copy);

        // Undo walks backward; the undone entry becomes the redo target.
        wrapper.markUndone();
        QCOMPARE(wrapper.undoTarget()->operation, Operation::Rename);
        QCOMPARE(wrapper.redoTarget()->operation, Operation::Copy);
        wrapper.markUndone();
        QVERIFY(!wrapper.canUndo());
        QCOMPARE(wrapper.redoTarget()->operation, Operation::Rename);
        wrapper.markRedone();
        QCOMPARE(wrapper.undoTarget()->operation, Operation::Rename);

        // Files AddHistory: a new operation after undos drops the stale redo
        // branch (the Copy entry disappears).
        wrapper.add(entry(Operation::Move, QStringLiteral("/moved")));
        QCOMPARE(wrapper.count(), 2);
        QVERIFY(!wrapper.canRedo());
        QCOMPARE(wrapper.undoTarget()->operation, Operation::Move);
        QCOMPARE(wrapper.undoTarget()->items.first().destination_path, QStringLiteral("/moved"));

        wrapper.clear();
        QCOMPARE(wrapper.count(), 0);
        QVERIFY(!wrapper.canUndo());
        QVERIFY(!wrapper.canRedo());
    }

    void statusCenterCardStringsMatchFilesTemplates() {
        using Op = sak::FileExplorerOperationType;
        using Result = sak::FileExplorerReturnResult;
        const auto header = [](const Op operation, const Result result) {
            sak::FileExplorerStatusCardRequest request;
            request.operation = operation;
            request.result = result;
            request.source = {QStringLiteral("C:/staging/report.txt")};
            request.destination = {QStringLiteral("C:/exports/Bundle")};
            request.items_count = 2;
            sak::FileExplorerStatusCenterItem item(request);
            return item.header();
        };
        // Files resw StatusCenter_* templates (terminal cards skip the
        // discovery substitution because they are not in progress).
        QCOMPARE(header(Op::Copy, Result::Success), QStringLiteral("Copied 2 items to \"Bundle\""));
        QCOMPARE(header(Op::Copy, Result::Failed),
                 QStringLiteral("Error copying 2 items to \"Bundle\""));
        QCOMPARE(header(Op::Copy, Result::Cancelled),
                 QStringLiteral("Canceled copying 2 items to \"Bundle\""));
        QCOMPARE(header(Op::Move, Result::Success), QStringLiteral("Moved 2 items to \"Bundle\""));
        // Delete/Recycle share the Delete headers and name the source folder.
        QCOMPARE(header(Op::Delete, Result::Success),
                 QStringLiteral("Deleted 2 items from \"staging\""));
        QCOMPARE(header(Op::Recycle, Result::Cancelled),
                 QStringLiteral("Canceled deleting 2 items from \"staging\""));
        QCOMPARE(header(Op::Compressed, Result::Success),
                 QStringLiteral("Compressed 2 items to \"Bundle\""));
        QCOMPARE(header(Op::Extract, Result::Success),
                 QStringLiteral("Extracted \"report.txt\" to \"Bundle\""));
        QCOMPARE(header(Op::Prepare, Result::InProgress),
                 QStringLiteral("Preparing the operation..."));

        // The in-progress card starts on the discovery header and the failed
        // card carries the Files failed subheader.
        sak::FileExplorerStatusCardRequest in_progress;
        in_progress.operation = Op::Copy;
        in_progress.result = Result::InProgress;
        in_progress.source = {QStringLiteral("C:/staging/report.txt")};
        in_progress.destination = {QStringLiteral("C:/exports/Bundle")};
        in_progress.items_count = 2;
        in_progress.can_provide_progress = true;
        sak::FileExplorerStatusCenterItem discovering(in_progress);
        QCOMPARE(discovering.header(), QStringLiteral("Discovered 2 items"));
        QCOMPARE(discovering.message(), QStringLiteral("Discovering items..."));
        QVERIFY(discovering.isInProgress());

        sak::FileExplorerStatusCardRequest failed = in_progress;
        failed.result = Result::Failed;
        sak::FileExplorerStatusCenterItem failed_item(failed);
        QCOMPARE(failed_item.subHeader(),
                 QStringLiteral("Failed to copy 2 items from \"C:/staging/report.txt\" to "
                                "\"C:/exports/Bundle\""));
        QCOMPARE(failed_item.kind(), sak::FileExplorerStatusItemKind::Error);
    }

    void statusCenterCardProgressReportsMatchFiles() {
        sak::FileExplorerStatusCardRequest request;
        request.operation = sak::FileExplorerOperationType::Copy;
        request.result = sak::FileExplorerReturnResult::InProgress;
        request.source = {QStringLiteral("C:/staging")};
        request.destination = {QStringLiteral("C:/exports/Bundle")};
        request.items_count = 3;
        request.can_provide_progress = true;
        request.cancelable = true;
        sak::FileExplorerStatusCenterItem item(request);
        QVERIFY(item.isSpeedAndProgressAvailable());
        QVERIFY(!item.isIndeterminate());
        QVERIFY(item.isCancelable());

        // First report with the enumeration done: the discovery phase ends
        // ("Processing items...") and the header gains the percentage.
        sak::FileExplorerStatusProgress progress;
        progress.enumeration_completed = true;
        progress.total_size = 1000;
        progress.processed_size = 250;
        progress.items_count = 3;
        progress.processed_items_count = 1;
        progress.file_name = QStringLiteral("a.txt");
        progress.size_speed = 500.0;
        item.reportProgress(progress);
        QCOMPARE(item.message(), QStringLiteral("Processing items..."));
        QCOMPARE(item.progressPercentage(), 25);
        QCOMPARE(item.speedText(), QStringLiteral("500 bytes/s"));
        QCOMPARE(item.currentItemName(), QStringLiteral("a.txt"));
        QCOMPARE(item.header(), QStringLiteral("Copying 3 items to \"Bundle\" (25%)"));

        // Later reports show the Files size footer and extend the graph.
        progress.processed_size = 750;
        progress.size_speed = 750.0;
        item.reportProgress(progress);
        QCOMPARE(item.message(), QStringLiteral("750 bytes of 1000 bytes processed"));
        QCOMPARE(item.progressPercentage(), 75);
        QCOMPARE(item.header(), QStringLiteral("Copying 3 items to \"Bundle\" (75%)"));
        QCOMPARE(item.graphPoints().size(), 2);
        QCOMPARE(item.graphPoints().last().x(), 75.0);

        // Delete-family cards report the items footer instead of sizes.
        sak::FileExplorerStatusCardRequest delete_request = request;
        delete_request.operation = sak::FileExplorerOperationType::Delete;
        sak::FileExplorerStatusCenterItem delete_item(delete_request);
        sak::FileExplorerStatusProgress delete_progress;
        delete_progress.enumeration_completed = true;
        delete_progress.items_count = 3;
        delete_progress.processed_items_count = 2;
        delete_item.reportProgress(delete_progress);
        delete_progress.processed_items_count = 3;
        delete_item.reportProgress(delete_progress);
        QCOMPARE(delete_item.message(), QStringLiteral("3/3 items processed"));
    }

    void statusCenterCardTransitionsOnTerminalProgress() {
        // B8-22: a terminal status arriving through reportProgress must leave the
        // in-progress state. Before the fix only m_return_result changed, so the
        // card spun forever, still counted as in-progress, and was never reaped.
        sak::FileExplorerStatusCardRequest request;
        request.operation = sak::FileExplorerOperationType::Copy;
        request.result = sak::FileExplorerReturnResult::InProgress;
        request.items_count = 2;
        request.can_provide_progress = true;
        request.cancelable = true;
        sak::FileExplorerStatusCenterItem item(request);
        QVERIFY(item.isInProgress());
        QCOMPARE(item.kind(), sak::FileExplorerStatusItemKind::InProgress);

        sak::FileExplorerStatusProgress progress;
        progress.status = sak::FileExplorerReturnResult::Success;
        progress.enumeration_completed = true;
        progress.items_count = 2;
        progress.processed_items_count = 2;
        item.reportProgress(progress);
        QVERIFY(!item.isInProgress());
        QVERIFY(!item.isCancelable());
        QCOMPARE(item.kind(), sak::FileExplorerStatusItemKind::Successful);
        QCOMPARE(item.iconKind(), sak::FileExplorerStatusIconKind::Successful);

        // A failure status maps to the error kind and also leaves in-progress.
        sak::FileExplorerStatusCenterItem failing(request);
        sak::FileExplorerStatusProgress fail_progress;
        fail_progress.status = sak::FileExplorerReturnResult::Failed;
        failing.reportProgress(fail_progress);
        QVERIFY(!failing.isInProgress());
        QCOMPARE(failing.kind(), sak::FileExplorerStatusItemKind::Error);
    }

    void registryReadGatesCompressAndExtract() {
        using sak::FileExplorerCommandId;
        // B8-22: compress reads its sources and extract reads the archive, so a
        // target that can WRITE but not READ must disable them. The only delta
        // between the two targets is can_read_files, so a change in the enabled
        // state isolates the read gate.
        const auto unreadable = rawTarget(QStringLiteral("apfs"), true, false, true);
        const auto readable = rawTarget(QStringLiteral("apfs"), true, true, true);
        const auto blocked = contextFor(unreadable, true);
        const auto ready = contextFor(readable, true);
        QVERIFY(!sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CompressIntoZip,
                                                         blocked)
                     .enabled);
        QVERIFY(
            sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::CompressIntoZip, ready)
                .enabled);
        QVERIFY(
            !sak::FileExplorerCommandRegistry::state(FileExplorerCommandId::ExtractHere, blocked)
                 .enabled);
    }

    void statusProgressReporterThrottlesAndComputesDeltaSpeed() {
        qint64 now_ms = 0;
        int delivered = 0;
        sak::FileExplorerStatusProgress last;
        sak::FileExplorerStatusProgressReporter reporter(
            [&delivered, &last](const sak::FileExplorerStatusProgress& snapshot) {
                ++delivered;
                last = snapshot;
            },
            [&now_ms]() { return now_ms; });

        reporter.setTotalSize(1000);
        reporter.setItemsCount(4);
        reporter.setProcessedSize(100);
        reporter.addProcessedItems(1);
        reporter.report();
        QCOMPARE(delivered, 1);
        // Files seeds the previous sample one second back: 100 bytes / 1s.
        QCOMPARE(last.size_speed, 100.0);
        QCOMPARE(last.items_speed, 1.0);

        // Inside the 100ms sampler window: suppressed.
        now_ms = 50;
        reporter.setProcessedSize(200);
        reporter.report();
        QCOMPARE(delivered, 1);

        // Past the window: delivered with the delta-over-elapsed speed.
        now_ms = 150;
        reporter.setProcessedSize(400);
        reporter.report();
        QCOMPARE(delivered, 2);
        QCOMPARE(last.processed_size, 400);
        QCOMPARE(last.size_speed, 2000.0);

        // Critical status change bypasses the throttle.
        now_ms = 160;
        reporter.setStatus(sak::FileExplorerReturnResult::Failed);
        reporter.report();
        QCOMPARE(delivered, 3);
        QCOMPARE(last.status, sak::FileExplorerReturnResult::Failed);

        // Files auto-success: fully processed after enumeration flips the
        // status without an explicit setStatus.
        int auto_delivered = 0;
        sak::FileExplorerStatusProgress auto_last;
        sak::FileExplorerStatusProgressReporter auto_reporter(
            [&auto_delivered, &auto_last](const sak::FileExplorerStatusProgress& snapshot) {
                ++auto_delivered;
                auto_last = snapshot;
            },
            [&now_ms]() { return now_ms; });
        auto_reporter.setTotalSize(100);
        auto_reporter.setItemsCount(2);
        auto_reporter.setProcessedSize(100);
        auto_reporter.addProcessedItems(2);
        auto_reporter.setEnumerationCompleted();
        auto_reporter.report();
        QCOMPARE(auto_delivered, 1);
        QCOMPARE(auto_last.status, sak::FileExplorerReturnResult::Success);
    }

    void statusCenterAggregatesAndBadgeMatrixMatchFiles() {
        sak::FileExplorerStatusCenterModel model;
        QCOMPARE(model.infoBadgeState(), 0);
        QCOMPARE(model.infoBadgeValue(), -1);
        QVERIFY(!model.showProgressRing());

        sak::FileExplorerStatusCardRequest request;
        request.operation = sak::FileExplorerOperationType::Copy;
        request.result = sak::FileExplorerReturnResult::InProgress;
        request.destination = {QStringLiteral("C:/exports/Bundle")};
        request.items_count = 1;
        request.can_provide_progress = true;
        auto* first = model.addItem(request);
        auto* second = model.addItem(request);
        // Newest first (Files inserts at index 0).
        QCOMPARE(model.items().first(), second);
        QCOMPARE(model.infoBadgeState(), 1);
        QCOMPARE(model.infoBadgeValue(), 2);
        QVERIFY(model.showProgressRing());

        sak::FileExplorerStatusProgress progress;
        progress.enumeration_completed = true;
        progress.total_size = 100;
        progress.processed_size = 40;
        first->reportProgress(progress);
        QCOMPARE(model.averageProgress(), 20);

        // A failure beside running work: state 2; only failures left: state 3.
        sak::FileExplorerStatusCardRequest failed = request;
        failed.result = sak::FileExplorerReturnResult::Failed;
        auto* failed_item = model.addItem(failed);
        QCOMPARE(model.infoBadgeState(), 2);
        model.removeItem(first);
        model.removeItem(second);
        QCOMPARE(model.infoBadgeState(), 3);
        QCOMPARE(model.infoBadgeValue(), -1);
        // Opening the flyout keeps the ring for the unreviewed error.
        model.flyoutOpened();
        QVERIFY(model.showProgressRing());

        // Clear completed removes only terminal cards; an empty model resets
        // the ring on the next flyout open.
        auto* running = model.addItem(request);
        model.removeAllCompletedItems();
        QCOMPARE(model.items().size(), 1);
        QCOMPARE(model.items().first(), running);
        QVERIFY(!model.items().contains(failed_item));
        model.removeItem(running);
        model.flyoutOpened();
        QVERIFY(!model.showProgressRing());
        QCOMPARE(model.infoBadgeState(), 0);
    }

    void statusCenterCancelFlagsCardCanceling() {
        sak::FileExplorerStatusCardRequest request;
        request.operation = sak::FileExplorerOperationType::Move;
        request.result = sak::FileExplorerReturnResult::InProgress;
        request.destination = {QStringLiteral("C:/exports/Bundle")};
        request.items_count = 1;
        request.can_provide_progress = true;
        request.cancelable = true;
        sak::FileExplorerStatusCenterItem item(request);
        QSignalSpy cancel_spy(&item, &sak::FileExplorerStatusCenterItem::cancelRequested);

        item.requestCancel();
        QCOMPARE(cancel_spy.count(), 1);
        QVERIFY(item.isCancelRequested());
        QVERIFY(!item.isCancelable());
        QVERIFY(item.isIndeterminate());
        QVERIFY(!item.isSpeedAndProgressAvailable());
        QVERIFY(item.header().startsWith(QStringLiteral("Canceling - ")));

        // Files ReportProgress bails after cancellation.
        const QString frozen_header = item.header();
        sak::FileExplorerStatusProgress progress;
        progress.total_size = 100;
        progress.processed_size = 50;
        item.reportProgress(progress);
        QCOMPARE(item.header(), frozen_header);
        QCOMPARE(item.progressPercentage(), 0);

        // A second cancel is a no-op.
        item.requestCancel();
        QCOMPARE(cancel_spy.count(), 1);
    }
};

QTEST_MAIN(FileExplorerTypesTests)
#include "test_file_explorer_types.moc"
