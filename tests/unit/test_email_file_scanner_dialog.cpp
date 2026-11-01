// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_file_scanner_dialog.cpp
/// @brief The email file scanner looks in the backup folders of every writable drive, the
///        system drive included.

#include "sak/email_file_scanner_dialog.h"

#include <QDir>
#include <QStorageInfo>
#include <QtTest/QtTest>

class EmailFileScannerDialogTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commonScanPathsIncludeTheSystemDrivesBackups();
};

// C: was skipped on the claim that the profile paths already covered it, but those are folders
// under the user's profile, so C:\Backups and C:\Email Backups were never looked at.
void EmailFileScannerDialogTests::commonScanPathsIncludeTheSystemDrivesBackups() {
    const QStorageInfo system_volume(QDir::homePath());
    QVERIFY(system_volume.isValid() && system_volume.isReady());
    const QString root = system_volume.rootPath();
    const QStringList paths = sak::EmailFileScannerDialog::commonScanPaths();
    QVERIFY2(paths.contains(root + QStringLiteral("Backups")),
             qPrintable(paths.join(QStringLiteral(", "))));
    QVERIFY(paths.contains(root + QStringLiteral("Email Backups")));
}

QTEST_MAIN(EmailFileScannerDialogTests)
#include "test_email_file_scanner_dialog.moc"
