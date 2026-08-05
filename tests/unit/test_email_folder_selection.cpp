// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_folder_selection.cpp
/// @brief The rule behind "Export ALL Mail Folders" and the Email Inspector's folder tree.
///
/// These two must agree exactly. If the export walked folders the tree hides, a technician
/// asking for EML would receive calendar entries and contact cards written out as .eml
/// files; if it walked fewer, mail would be silently missing from an export the user was
/// told covered everything. One function decides both, and this pins its behaviour.

#include "sak/email_folder_selection.h"
#include "sak/email_types.h"

#include <QTest>

namespace {

sak::PstFolder makeFolder(uint64_t node_id,
                          const QString& name,
                          const QString& container_class = QStringLiteral("IPF.Note")) {
    sak::PstFolder folder;
    folder.node_id = node_id;
    folder.display_name = name;
    folder.container_class = container_class;
    return folder;
}

}  // namespace

class TestEmailFolderSelection : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // ====================================================================
    // isMailFolder
    // ====================================================================

    void ordinaryMailFoldersAreIncluded() {
        QVERIFY(sak::isMailFolder(QStringLiteral("Inbox"), QStringLiteral("IPF.Note")));
        QVERIFY(sak::isMailFolder(QStringLiteral("Sent Items"), QStringLiteral("IPF.Note")));
        // An empty container class is the common case in damaged stores; a folder is not
        // excluded merely because its class could not be read.
        QVERIFY(sak::isMailFolder(QStringLiteral("Recovered"), QString()));
    }

    void nonMailContainerClassesAreExcluded() {
        QVERIFY(!sak::isMailFolder(QStringLiteral("Contacts"), QStringLiteral("IPF.Contact")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Calendar"), QStringLiteral("IPF.Appointment")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Tasks"), QStringLiteral("IPF.Task")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Notes"), QStringLiteral("IPF.StickyNote")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Journal"), QStringLiteral("IPF.Journal")));
        QVERIFY(
            !sak::isMailFolder(QStringLiteral("Settings"), QStringLiteral("IPF.Configuration")));
    }

    // Outlook appends variants to the container class, so the match is by PREFIX. An
    // exact-match rule would let these through and export contact cards as email.
    void containerClassVariantsAreExcluded() {
        QVERIFY(!sak::isMailFolder(QStringLiteral("IM Contacts"),
                                   QStringLiteral("IPF.Contact.MOC.ImContactList")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Birthdays"),
                                   QStringLiteral("IPF.Appointment.Birthday")));
    }

    // IPF.Configuration is matched exactly, not by prefix, so a mail folder whose class
    // merely starts with the same letters is not swept up.
    void configurationIsMatchedExactly() {
        QVERIFY(sak::isMailFolder(QStringLiteral("Configured Mail"),
                                  QStringLiteral("IPF.ConfigurationNotes")));
    }

    void bookkeepingFolderNamesAreExcluded() {
        QVERIFY(!sak::isMailFolder(QStringLiteral("Sync Issues"), QStringLiteral("IPF.Note")));
        QVERIFY(!sak::isMailFolder(QStringLiteral("Conflicts"), QStringLiteral("IPF.Note")));
        QVERIFY(
            !sak::isMailFolder(QStringLiteral("Conversation History"), QStringLiteral("IPF.Note")));
        // Name matching is case-insensitive: the store's casing varies by Outlook version.
        QVERIFY(!sak::isMailFolder(QStringLiteral("sync issues"), QStringLiteral("IPF.Note")));
    }

    // ====================================================================
    // findIpmSubtree
    // ====================================================================

    // An OST carries TWO IPM_SUBTREE nodes: an empty one under Root - Public and the real
    // one under Root - Mailbox. Picking the empty one would export nothing at all.
    void populatedIpmSubtreeWinsOverEmptyOne() {
        sak::PstFolder empty_subtree = makeFolder(10, QStringLiteral("IPM_SUBTREE"));
        sak::PstFolder public_root = makeFolder(1, QStringLiteral("Root - Public"));
        public_root.children.append(empty_subtree);

        sak::PstFolder real_subtree = makeFolder(20, QStringLiteral("IPM_SUBTREE"));
        real_subtree.children.append(makeFolder(21, QStringLiteral("Inbox")));
        sak::PstFolder mailbox_root = makeFolder(2, QStringLiteral("Root - Mailbox"));
        mailbox_root.children.append(real_subtree);

        const QVector<sak::PstFolder> tree{public_root, mailbox_root};
        const sak::PstFolder* found = sak::findIpmSubtree(tree);
        QVERIFY(found != nullptr);
        QCOMPARE(found->node_id, static_cast<uint64_t>(20));
    }

    void topOfInformationStoreIsRecognised() {
        sak::PstFolder subtree = makeFolder(30, QStringLiteral("Top of Information Store"));
        subtree.children.append(makeFolder(31, QStringLiteral("Inbox")));
        const QVector<sak::PstFolder> tree{subtree};
        const sak::PstFolder* found = sak::findIpmSubtree(tree);
        QVERIFY(found != nullptr);
        QCOMPARE(found->node_id, static_cast<uint64_t>(30));
    }

    void noSubtreeYieldsNullptr() {
        const QVector<sak::PstFolder> tree{makeFolder(1, QStringLiteral("Inbox"))};
        QCOMPARE(sak::findIpmSubtree(tree), nullptr);
    }

    // ====================================================================
    // mailFolderNodeIds
    // ====================================================================

    void collectsEveryMailFolderUnderTheSubtree() {
        sak::PstFolder inbox = makeFolder(100, QStringLiteral("Inbox"));
        inbox.children.append(makeFolder(101, QStringLiteral("Projects")));
        inbox.children.append(makeFolder(102, QStringLiteral("Receipts")));

        sak::PstFolder subtree = makeFolder(10, QStringLiteral("IPM_SUBTREE"));
        subtree.children.append(inbox);
        subtree.children.append(makeFolder(103, QStringLiteral("Sent Items")));

        const QVector<sak::PstFolder> tree{subtree};
        const QVector<uint64_t> ids = sak::mailFolderNodeIds(tree);
        QCOMPARE(ids, (QVector<uint64_t>{100, 101, 102, 103}));
    }

    // The IPM_SUBTREE node itself is a container, not a mail folder the user sees, so its
    // id must NOT appear -- exporting it would be exporting a folder the tree never shows.
    void theSubtreeNodeItselfIsNotExported() {
        sak::PstFolder subtree = makeFolder(10, QStringLiteral("IPM_SUBTREE"));
        subtree.children.append(makeFolder(11, QStringLiteral("Inbox")));
        const QVector<uint64_t> ids = sak::mailFolderNodeIds(QVector<sak::PstFolder>{subtree});
        QVERIFY(!ids.contains(static_cast<uint64_t>(10)));
        QCOMPARE(ids, (QVector<uint64_t>{11}));
    }

    // A non-mail folder takes its whole subtree with it: a subfolder of Contacts is not
    // mail either. This mirrors the tree, where a hidden folder's children never appear.
    void nonMailFoldersExcludeTheirDescendants() {
        sak::PstFolder contacts =
            makeFolder(200, QStringLiteral("Contacts"), QStringLiteral("IPF.Contact"));
        contacts.children.append(
            makeFolder(201, QStringLiteral("Suppliers"), QStringLiteral("IPF.Contact")));
        // Even a child that LOOKS like mail is not exported, because the user cannot see it.
        contacts.children.append(makeFolder(202, QStringLiteral("Notes to self")));

        sak::PstFolder subtree = makeFolder(10, QStringLiteral("IPM_SUBTREE"));
        subtree.children.append(makeFolder(203, QStringLiteral("Inbox")));
        subtree.children.append(contacts);

        const QVector<uint64_t> ids = sak::mailFolderNodeIds(QVector<sak::PstFolder>{subtree});
        QCOMPARE(ids, (QVector<uint64_t>{203}));
    }

    // With no IPM_SUBTREE the roots are used directly, so a store whose tree does not carry
    // one still exports rather than silently producing nothing.
    void fallsBackToTheRootsWithoutASubtree() {
        const QVector<sak::PstFolder> tree{makeFolder(1, QStringLiteral("Inbox")),
                                           makeFolder(2, QStringLiteral("Archive"))};
        QCOMPARE(sak::mailFolderNodeIds(tree), (QVector<uint64_t>{1, 2}));
    }

    void emptyTreeYieldsNoIds() {
        QVERIFY(sak::mailFolderNodeIds(QVector<sak::PstFolder>{}).isEmpty());
    }

    // A store holding only contacts and calendar folders yields nothing, which is what lets
    // the panel refuse with an accurate message instead of opening a directory picker for
    // an export that could only produce an empty folder.
    void storeWithNoMailYieldsNoIds() {
        sak::PstFolder subtree = makeFolder(10, QStringLiteral("IPM_SUBTREE"));
        subtree.children.append(
            makeFolder(11, QStringLiteral("Contacts"), QStringLiteral("IPF.Contact")));
        subtree.children.append(
            makeFolder(12, QStringLiteral("Calendar"), QStringLiteral("IPF.Appointment")));
        QVERIFY(sak::mailFolderNodeIds(QVector<sak::PstFolder>{subtree}).isEmpty());
    }
};

QTEST_MAIN(TestEmailFolderSelection)

#include "test_email_folder_selection.moc"
