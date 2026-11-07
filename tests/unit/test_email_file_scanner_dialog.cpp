// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_email_file_scanner_dialog.cpp
/// @brief The email file scanner looks in the backup folders of every writable drive, the
///        system drive included.

#include "sak/email_file_scanner_dialog.h"
#include "sak/format_utils.h"

#include <QDir>
#include <QStorageInfo>
#include <QtTest/QtTest>

class EmailFileScannerDialogTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commonScanPathsIncludeTheSystemDrivesBackups();
    void aScanListsEveryKindOfMailFileTheAppOpens();
    void sizesMatchTheRestOfEmailTools();
};

// Review 3 (F38): the scan read "60 KB" where Email Inspector and the OST converter say "60.0 KB".
void EmailFileScannerDialogTests::sizesMatchTheRestOfEmailTools() {
    QCOMPARE(sak::EmailFileScannerDialog::sizeText(61'440), QStringLiteral("60.0 KB"));
    QCOMPARE(sak::EmailFileScannerDialog::sizeText(61'440), sak::formatBytes(qint64{61'440}));
}

void EmailFileScannerDialogTests::aScanListsEveryKindOfMailFileTheAppOpens() {
    const QStringList patterns = sak::EmailFileScannerDialog::mailFilePatterns();
    for (const char* pattern : {"*.pst", "*.ost", "*.nst", "*.mbox"}) {
        QVERIFY2(patterns.contains(QLatin1String(pattern)), pattern);
    }
}

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
