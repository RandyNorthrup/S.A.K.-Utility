// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_file_explorer_types.cpp
/// @brief Unit tests for File Explorer state and command registry.

#include "sak/file_explorer_command_registry.h"

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
};

QTEST_MAIN(FileExplorerTypesTests)
#include "test_file_explorer_types.moc"
