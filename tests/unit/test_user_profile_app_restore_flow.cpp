// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_app_restore_flow.cpp
/// @brief A scanned application reaches the restore page as a real install offer: scan result,
///        backup page, installed_apps.json on disk, restore page, list handed to Chocolatey.

#include "sak/elevation_manager.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"

#include <QApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest/QtTest>

#include <vector>

namespace {

constexpr int kDialogPollMs = 20;
constexpr int kDialogTimeoutMs = 5000;
constexpr int kPackageColumn = 2;

sak::AppScanner::AppInfo desktopApp(const QString& name) {
    sak::AppScanner::AppInfo app;
    app.name = name;
    return app;
}

sak::AppScanner::AppInfo chocolateyApp(const QString& package) {
    sak::AppScanner::AppInfo app;
    app.name = package;
    app.choco_package = package;
    app.choco_available = true;
    app.source = sak::AppScanner::AppInfo::Source::Chocolatey;
    return app;
}

sak::AppScanner::AppInfo storeApp(const QString& name) {
    sak::AppScanner::AppInfo app;
    app.name = name;
    app.source = sak::AppScanner::AppInfo::Source::AppX;
    return app;
}

QTreeWidgetItem* findRow(const QTreeWidget* tree, const QString& name) {
    for (int category = 0; category < tree->topLevelItemCount(); ++category) {
        QTreeWidgetItem* parent = tree->topLevelItem(category);
        for (int child = 0; child < parent->childCount(); ++child) {
            if (parent->child(child)->text(0) == name) {
                return parent->child(child);
            }
        }
    }
    return nullptr;
}

QStringList packagesOf(const QVector<sak::RestoreAppInfo>& apps) {
    QStringList packages;
    for (const auto& app : apps) {
        packages << app.choco_package;
    }
    packages.sort();
    return packages;
}

bool writeInstalledAppsSidecar(const QString& directory, const QJsonArray& apps) {
    QFile file(directory + QStringLiteral("/installed_apps.json"));
    return file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(apps).toJson()) > 0;
}

}  // namespace

class UserProfileAppRestoreFlowTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void scanToRestoreOffersOnlyRealInstalls();
    void deselectedAppIsNotInTheBackup();
    void restoreIgnoresAnIdChocolateyWouldRefuse();
    void restoreInstallsAPackageOnce();
    void restoreInstallAsksForAdministratorFirst();

private:
    static sak::UserProfileRestoreAppRestorePage* restorePage(
        sak::UserProfileRestoreWizard& wizard);
};

sak::UserProfileRestoreAppRestorePage* UserProfileAppRestoreFlowTests::restorePage(
    sak::UserProfileRestoreWizard& wizard) {
    return wizard.findChild<sak::UserProfileRestoreAppRestorePage*>();
}

// The defect this file exists for: the scan recorded no id, the page dropped any id it was given,
// and the restore page therefore listed every application as "(no match)".
void UserProfileAppRestoreFlowTests::scanToRestoreOffersOnlyRealInstalls() {
    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());

    sak::UserProfileBackupWizard backup_wizard;
    auto* apps_page = qobject_cast<sak::UserProfileBackupInstalledAppsPage*>(
        backup_wizard.page(sak::UserProfileBackupWizard::Page_InstalledApps));
    QVERIFY(apps_page != nullptr);
    apps_page->populateTree(sak::UserProfileBackupInstalledAppsPage::inventoryFromScan(
        {desktopApp(QStringLiteral("7-Zip 22.01 (x64)")),
         desktopApp(QStringLiteral("Mozilla Firefox (x64 en-US)")),
         desktopApp(QStringLiteral("Solar Fire v9")),
         storeApp(QStringLiteral("SpotifyAB.SpotifyMusic")),
         chocolateyApp(QStringLiteral("ripgrep")),
         chocolateyApp(QStringLiteral("chocolatey"))}));

    sak::BackupManifest manifest;
    sak::UserProfileBackupExecutePage execute_page(manifest, {}, backup_dir.path());
    QVERIFY(execute_page.saveInstalledAppsToBackup(backup_wizard.installedApps()));

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setBackupPath(backup_dir.path());
    auto* restore_page = restorePage(restore_wizard);
    QVERIFY(restore_page != nullptr);
    restore_page->loadApps();

    const QTreeWidget* tree = restore_page->m_appTree;
    const QTreeWidgetItem* zip = findRow(tree, QStringLiteral("7-Zip 22.01 (x64)"));
    QVERIFY(zip != nullptr);
    QCOMPARE(zip->text(kPackageColumn), QStringLiteral("7zip"));
    QVERIFY((zip->flags() & Qt::ItemIsEnabled) != 0);
    QCOMPARE(zip->checkState(0), Qt::Checked);

    for (const QString& unmatched : {QStringLiteral("Solar Fire v9"),
                                     QStringLiteral("SpotifyAB.SpotifyMusic"),
                                     QStringLiteral("chocolatey")}) {
        const QTreeWidgetItem* row = findRow(tree, unmatched);
        QVERIFY2(row != nullptr, qPrintable(unmatched));
        QCOMPARE(row->text(kPackageColumn), QStringLiteral("(no match)"));
        QVERIFY((row->flags() & Qt::ItemIsEnabled) == 0);
        QCOMPARE(row->checkState(0), Qt::Unchecked);
    }

    QCOMPARE(packagesOf(restore_page->collectSelectedApps()),
             QStringList(
                 {QStringLiteral("7zip"), QStringLiteral("firefox"), QStringLiteral("ripgrep")}));
}

// Unchecking on the backup page takes the row, and its id, out of what restore can offer.
void UserProfileAppRestoreFlowTests::deselectedAppIsNotInTheBackup() {
    sak::UserProfileBackupWizard backup_wizard;
    auto* apps_page = qobject_cast<sak::UserProfileBackupInstalledAppsPage*>(
        backup_wizard.page(sak::UserProfileBackupWizard::Page_InstalledApps));
    QVERIFY(apps_page != nullptr);
    apps_page->populateTree(sak::UserProfileBackupInstalledAppsPage::inventoryFromScan(
        {desktopApp(QStringLiteral("Git")), desktopApp(QStringLiteral("Google Chrome"))}));

    QTreeWidgetItem* git = findRow(apps_page->m_appTree, QStringLiteral("Git"));
    QVERIFY(git != nullptr);
    git->setCheckState(0, Qt::Unchecked);

    const auto selected = backup_wizard.installedApps();
    QCOMPARE(selected.size(), 1);
    QCOMPARE(selected[0].choco_package, QStringLiteral("googlechrome"));
}

void UserProfileAppRestoreFlowTests::restoreIgnoresAnIdChocolateyWouldRefuse() {
    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    QVERIFY(writeInstalledAppsSidecar(
        backup_dir.path(),
        QJsonArray{QJsonObject{{"name", "Tampered"}, {"choco_package", "--source=evil"}},
                   QJsonObject{{"name", "Git"}, {"choco_package", "git"}}}));

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setBackupPath(backup_dir.path());
    auto* restore_page = restorePage(restore_wizard);
    QVERIFY(restore_page != nullptr);
    restore_page->loadApps();

    const QTreeWidgetItem* tampered = findRow(restore_page->m_appTree, QStringLiteral("Tampered"));
    QVERIFY(tampered != nullptr);
    QCOMPARE(tampered->text(kPackageColumn), QStringLiteral("(no match)"));
    QVERIFY((tampered->flags() & Qt::ItemIsEnabled) == 0);
    QCOMPARE(packagesOf(restore_page->collectSelectedApps()), QStringList({QStringLiteral("git")}));
}

// Two rows can name one package, as two installed Python versions do. Asking Chocolatey for it
// twice would report two installs for one.
void UserProfileAppRestoreFlowTests::restoreInstallsAPackageOnce() {
    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    QVERIFY(writeInstalledAppsSidecar(
        backup_dir.path(),
        QJsonArray{QJsonObject{{"name", "Python 3.13.16 (64-bit)"}, {"choco_package", "python"}},
                   QJsonObject{{"name", "Python 3.14.10 (64-bit)"}, {"choco_package", "Python"}},
                   QJsonObject{{"name", "Git"}, {"choco_package", "git"}}}));

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setBackupPath(backup_dir.path());
    auto* restore_page = restorePage(restore_wizard);
    QVERIFY(restore_page != nullptr);
    restore_page->loadApps();

    QCOMPARE(restore_page->collectSelectedApps().size(), 2);
}

// Nothing ever reached the install path while every row was "(no match)", so it never needed this
// gate. Installers need administrator rights, and without the gate every install would fail.
void UserProfileAppRestoreFlowTests::restoreInstallAsksForAdministratorFirst() {
    if (sak::ElevationManager::isElevated()) {
        QSKIP("Run unelevated: an elevated process has no gate to show and would really install");
    }
    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    QVERIFY(writeInstalledAppsSidecar(
        backup_dir.path(), QJsonArray{QJsonObject{{"name", "Git"}, {"choco_package", "git"}}}));

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setBackupPath(backup_dir.path());
    auto* restore_page = restorePage(restore_wizard);
    QVERIFY(restore_page != nullptr);
    restore_page->loadApps();

    QString question;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&question]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr) {
            question = box->text();
            box->done(QMessageBox::No);
        }
    });
    poll.start();
    QTimer::singleShot(kDialogTimeoutMs, &poll, &QTimer::stop);

    restore_page->onInstallApps();

    QVERIFY2(question.contains(QStringLiteral("administrator")), qPrintable(question));
    QVERIFY(!restore_page->m_installing);
    QVERIFY(restore_page->isComplete());
}

QTEST_MAIN(UserProfileAppRestoreFlowTests)
#include "test_user_profile_app_restore_flow.moc"
