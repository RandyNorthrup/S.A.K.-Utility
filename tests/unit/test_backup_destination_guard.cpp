// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/backup_destination_guard.h"

#include <QtTest/QtTest>

#include <initializer_list>

using sak::BackupDestinationCheck;
using sak::backupPathIsWithin;
using sak::screenBackupDestination;

namespace {

// The two profiles a technician typically has selected on the settings page, with the destination
// subfolder name the worker derives for each ("<destination>/<username>").
const QStringList kSourceRoots = {QStringLiteral("C:\\Users\\Username"),
                                  QStringLiteral("C:\\Users\\Public")};
const QStringList kFolderNames = {QStringLiteral("Username"), QStringLiteral("Public")};

BackupDestinationCheck screen(const QString& destination) {
    return screenBackupDestination(destination, kSourceRoots, kFolderNames);
}

}  // namespace

class BackupDestinationGuardTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void acceptsAbsoluteFolderOutsideEverySource();
    void trimsSurroundingWhitespaceOnAcceptedPath();
    void rejectsEmptyText();
    void rejectsWhitespaceOnlyText();
    void rejectsRelativePath();
    void rejectsDriveRelativePath();
    void rejectsDestinationEqualToASource();
    void rejectsDestinationInsideASource();
    void rejectsDestinationWhoseUserSubfolderIsASource();
    void allowsAncestorThatDoesNotCollideWithAUserSubfolder();
    void separatorAndCaseTricksDoNotEvadeOverlap();
    void siblingWithSharedPrefixIsNotTreatedAsNested();
    void unselectedSourcesDoNotConstrainTheDestination();
    void refusalIsNeverAccompaniedByAPath();
};

void BackupDestinationGuardTests::acceptsAbsoluteFolderOutsideEverySource() {
    const BackupDestinationCheck check = screen(QStringLiteral("D:\\Backups\\Profiles"));
    QVERIFY(check.accepted);
    QCOMPARE(check.path, QStringLiteral("D:\\Backups\\Profiles"));
    QVERIFY(check.refusal.isEmpty());
}

void BackupDestinationGuardTests::trimsSurroundingWhitespaceOnAcceptedPath() {
    // Surrounding whitespace is the only canonicalization allowed, and the trimmed form is what
    // the caller must store so the screened value is the value that gets created.
    const BackupDestinationCheck check = screen(QStringLiteral("  D:\\Backups\\Profiles \t"));
    QVERIFY(check.accepted);
    QCOMPARE(check.path, QStringLiteral("D:\\Backups\\Profiles"));
}

void BackupDestinationGuardTests::rejectsEmptyText() {
    const BackupDestinationCheck check = screen(QString());
    QVERIFY(!check.accepted);
    QVERIFY(!check.refusal.isEmpty());
}

void BackupDestinationGuardTests::rejectsWhitespaceOnlyText() {
    // The pre-fix check was QLineEdit::text().isEmpty(), which is false for "   ", so a
    // whitespace-only destination started a run and produced a junk folder.
    for (const QString& blank :
         {QStringLiteral("   "), QStringLiteral("\t"), QStringLiteral(" \t \n ")}) {
        const BackupDestinationCheck check = screen(blank);
        QVERIFY2(!check.accepted, qPrintable(QStringLiteral("accepted blank: [%1]").arg(blank)));
        QVERIFY(check.path.isEmpty());
    }
}

void BackupDestinationGuardTests::rejectsRelativePath() {
    // A relative path resolves against the process working directory, so the backup lands
    // somewhere the technician never chose.
    for (const QString& relative : {QStringLiteral("Backups"),
                                    QStringLiteral(".\\Backups"),
                                    QStringLiteral("..\\Backups"),
                                    QStringLiteral("Backups\\Profiles")}) {
        const BackupDestinationCheck check = screen(relative);
        QVERIFY2(!check.accepted,
                 qPrintable(QStringLiteral("accepted relative: [%1]").arg(relative)));
    }
}

void BackupDestinationGuardTests::rejectsDriveRelativePath() {
    // "C:Backups" is drive-relative on Windows, not absolute.
    QVERIFY(!screen(QStringLiteral("C:Backups")).accepted);
}

void BackupDestinationGuardTests::rejectsDestinationEqualToASource() {
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Username")).accepted);
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Public")).accepted);
}

void BackupDestinationGuardTests::rejectsDestinationInsideASource() {
    // The copy walks the source while the backup grows inside it: the run recurses into its own
    // output and never terminates.
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Username\\Desktop\\Backup")).accepted);
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Public\\Documents")).accepted);
}

void BackupDestinationGuardTests::rejectsDestinationWhoseUserSubfolderIsASource() {
    // Destination "C:\Users" is not itself inside a profile, but the worker writes
    // "C:\Users\Username" -- straight into the profile it is copying.
    QVERIFY(!screen(QStringLiteral("C:\\Users")).accepted);
}

void BackupDestinationGuardTests::allowsAncestorThatDoesNotCollideWithAUserSubfolder() {
    // "C:\" is an ancestor of both profiles, but the subtrees it produces ("C:\Username",
    // "C:\Public") do not overlap them, so banning every ancestor would be over-reach.
    QVERIFY(screen(QStringLiteral("C:\\")).accepted);
}

void BackupDestinationGuardTests::separatorAndCaseTricksDoNotEvadeOverlap() {
    QVERIFY(!screen(QStringLiteral("c:/users/username")).accepted);
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Username\\")).accepted);
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Username\\Documents\\..")).accepted);
    QVERIFY(!screen(QStringLiteral("C:\\Users\\Username\\.")).accepted);
}

void BackupDestinationGuardTests::siblingWithSharedPrefixIsNotTreatedAsNested() {
    // "D:\Profiles\Alice2" merely starts with "D:\Profiles\Alice"; it is a different folder.
    // A synthetic container is used here because the sibling has to share a prefix with a
    // selected source, which the placeholder profile names cannot express.
    const QStringList roots = {QStringLiteral("D:\\Profiles\\Alice")};
    const QStringList names = {QStringLiteral("Alice")};
    QVERIFY(screenBackupDestination(QStringLiteral("D:\\Profiles\\Alice2"), roots, names).accepted);
    QVERIFY(backupPathIsWithin(QStringLiteral("C:\\A\\B"), QStringLiteral("C:\\A")));
    QVERIFY(!backupPathIsWithin(QStringLiteral("C:\\AB"), QStringLiteral("C:\\A")));
}

void BackupDestinationGuardTests::unselectedSourcesDoNotConstrainTheDestination() {
    // Only selected profiles are copied, so only they may collide with the destination.
    const BackupDestinationCheck check =
        screenBackupDestination(QStringLiteral("C:\\Users\\Username\\Backup"), {}, {});
    QVERIFY(check.accepted);
}

void BackupDestinationGuardTests::refusalIsNeverAccompaniedByAPath() {
    // Fail closed: a refusal must not hand back a "close enough" substitute for the caller to use.
    for (const QString& bad : {QString(),
                               QStringLiteral("  "),
                               QStringLiteral("Backups"),
                               QStringLiteral("C:\\Users\\Username")}) {
        const BackupDestinationCheck check = screen(bad);
        QVERIFY(!check.accepted);
        QVERIFY(check.path.isEmpty());
        QVERIFY(!check.refusal.isEmpty());
    }
}

QTEST_GUILESS_MAIN(BackupDestinationGuardTests)
#include "test_backup_destination_guard.moc"
