// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_restore_selection.cpp
/// @brief Unit tests for restore folder-selection -> manifest application
/// (P12-25: unchecked folders must not be restored).

#include "sak/user_profile_restore_selection.h"

#include <QtTest/QtTest>

using namespace sak;

namespace {
FolderSelection makeFolder(const QString& relPath, bool selected) {
    FolderSelection f;
    f.display_name = relPath;
    f.relative_path = relPath;
    f.selected = selected;
    return f;
}

BackupManifest makeManifest() {
    BackupManifest m;
    BackupUserData alice;
    alice.username = QStringLiteral("Alice");
    alice.backed_up_folders = {makeFolder("Documents", true),
                               makeFolder("Desktop", true),
                               makeFolder("Downloads", true)};
    BackupUserData bob;
    bob.username = QStringLiteral("Bob");
    bob.backed_up_folders = {makeFolder("Pictures", true)};
    m.users = {alice, bob};
    return m;
}
}  // namespace

class RestoreSelectionTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void uncheckingAFolderClearsOnlyThatFolder();
    void unmatchedFolderKeepsExistingSelection();
    void selectionIsScopedPerUser();
    void selectedFolderCountReflectsChoices();
    void absentUserCountsZero();
};

// The core P12-25 guarantee: unchecking exactly one folder clears only its
// `selected` flag; sibling folders stay selected.
void RestoreSelectionTests::uncheckingAFolderClearsOnlyThatFolder() {
    BackupManifest m = makeManifest();
    const QVector<FolderRestoreChoice> choices = {
        {QStringLiteral("Alice"), QStringLiteral("Documents"), true},
        {QStringLiteral("Alice"), QStringLiteral("Desktop"), false},
        {QStringLiteral("Alice"), QStringLiteral("Downloads"), true},
        {QStringLiteral("Bob"), QStringLiteral("Pictures"), true}};
    applyFolderRestoreSelections(m, choices);

    QCOMPARE(m.users[0].backed_up_folders[0].selected, true);   // Documents
    QCOMPARE(m.users[0].backed_up_folders[1].selected, false);  // Desktop (unchecked)
    QCOMPARE(m.users[0].backed_up_folders[2].selected, true);   // Downloads
    QCOMPARE(m.users[1].backed_up_folders[0].selected, true);   // Bob/Pictures
}

// A folder with no matching choice keeps its prior selection (never silently
// dropped just because the UI omitted a row).
void RestoreSelectionTests::unmatchedFolderKeepsExistingSelection() {
    BackupManifest m = makeManifest();
    m.users[0].backed_up_folders[1].selected = false;  // Desktop preset unchecked
    const QVector<FolderRestoreChoice> choices = {
        {QStringLiteral("Alice"), QStringLiteral("Documents"), true}};
    applyFolderRestoreSelections(m, choices);

    QCOMPARE(m.users[0].backed_up_folders[0].selected, true);   // matched -> true
    QCOMPARE(m.users[0].backed_up_folders[1].selected, false);  // unmatched -> preserved
    QCOMPARE(m.users[0].backed_up_folders[2].selected, true);   // unmatched -> preserved default
}

// A choice for one user must not toggle an identically-named folder of another
// user (match keys on username as well as relative path).
void RestoreSelectionTests::selectionIsScopedPerUser() {
    BackupManifest m = makeManifest();
    m.users[1].backed_up_folders.append(makeFolder("Documents", true));  // Bob/Documents
    const QVector<FolderRestoreChoice> choices = {
        {QStringLiteral("Alice"), QStringLiteral("Documents"), false}};
    applyFolderRestoreSelections(m, choices);

    QCOMPARE(m.users[0].backed_up_folders[0].selected, false);  // Alice/Documents cleared
    QCOMPARE(m.users[1].backed_up_folders[1].selected, true);   // Bob/Documents untouched
}

void RestoreSelectionTests::selectedFolderCountReflectsChoices() {
    BackupManifest m = makeManifest();
    const QVector<FolderRestoreChoice> choices = {
        {QStringLiteral("Alice"), QStringLiteral("Desktop"), false},
        {QStringLiteral("Alice"), QStringLiteral("Downloads"), false}};
    applyFolderRestoreSelections(m, choices);

    QCOMPARE(selectedFolderCount(m, QStringLiteral("Alice")), 1);  // only Documents left
    QCOMPARE(selectedFolderCount(m, QStringLiteral("Bob")), 1);
}

void RestoreSelectionTests::absentUserCountsZero() {
    const BackupManifest m = makeManifest();
    QCOMPARE(selectedFolderCount(m, QStringLiteral("Nobody")), 0);
}

QTEST_MAIN(RestoreSelectionTests)
#include "test_user_profile_restore_selection.moc"
