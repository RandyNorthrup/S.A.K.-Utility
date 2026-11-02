// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_app_restore_flow.cpp
/// @brief A scanned application reaches the restore page as a real install offer: scan result,
///        backup page, installed_apps.json on disk, restore page, list handed to Chocolatey.

#include "sak/app_data_program_prompt.h"
#include "sak/backup_file_codec.h"
#include "sak/elevation_manager.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_user_scanner.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
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
    void appDataSizeTravelsToTheManifestAndTheRestorePage();
    void restorePageOffersOnlyStoredAppData();
    void restoreFolderPageLeavesAppDataToItsOwnPage();
    void restoreFolderPageRecordsTheChoiceWithoutTouchingTheManifest();
    void runningProgramStopsTheBackupUntilAnswered();
    void pickingAnAccountLandsOnMergeNotReplace();
    void restoreStartsOnlyFromItsNamedLastStep();
    void lastStepSaysWhatHappensToExistingFiles();
    void lastStepRefusesAWrongPassword();
    void lastStepRefusesToAssignToAFolderWithNoAccount();
    void lastStepAsksForAdministratorForNetworkSettings();

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

// The page dropped the size when it committed its selection, so every row showed 0.0 MB on restore.
void UserProfileAppRestoreFlowTests::appDataSizeTravelsToTheManifestAndTheRestorePage() {
    constexpr qint64 kChromeBytes = 5 * sak::kBytesPerMB;
    sak::UserProfileBackupWizard backup_wizard;
    auto* app_data_page = qobject_cast<sak::UserProfileBackupAppDataPage*>(
        backup_wizard.page(sak::UserProfileBackupWizard::Page_AppData));
    QVERIFY(app_data_page != nullptr);
    sak::AppDataSourceInfo chrome;
    chrome.name = QStringLiteral("Chrome Profiles");
    chrome.category = QStringLiteral("Browsers");
    chrome.relative_path = QStringLiteral("AppData/Local/Google/Chrome/User Data");
    chrome.size_bytes = kChromeBytes;
    chrome.exists = true;
    app_data_page->populateTree({chrome});

    QCOMPARE(backup_wizard.appDataSources().size(), 1);
    QCOMPARE(backup_wizard.appDataSources().first().size_bytes, kChromeBytes);

    sak::BackupManifest manifest;
    sak::UserProfileBackupExecutePage execute_page(manifest, {}, QString());
    execute_page.recordNetworkSelectionsInManifest(&backup_wizard);
    QCOMPARE(manifest.app_data_sources.size(), 1);

    sak::FolderSelection stored;
    stored.type = sak::FolderType::ApplicationData;
    stored.relative_path = chrome.relative_path;
    stored.size_bytes = kChromeBytes;
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    user.backed_up_folders = {stored};
    manifest.users = {user};
    manifest.manifest_checksum = manifest.computeManifestChecksum();

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    auto* restore_page = restore_wizard.findChild<sak::UserProfileRestoreAppDataPage*>();
    QVERIFY(restore_page != nullptr);
    restore_page->loadAppDataSources();
    const QTreeWidgetItem* row = findRow(restore_page->m_appDataTree,
                                         QStringLiteral("Chrome Profiles"));
    QVERIFY(row != nullptr);
    QCOMPARE(row->text(kPackageColumn), sak::formatBytes(kChromeBytes));
}

// A backup from a version that recorded the choice and copied nothing must not offer it.
void UserProfileAppRestoreFlowTests::restorePageOffersOnlyStoredAppData() {
    sak::AppDataSourceInfo chrome;
    chrome.name = QStringLiteral("Chrome Profiles");
    chrome.relative_path = QStringLiteral("AppData/Local/Google/Chrome/User Data");
    sak::BackupManifest manifest;
    manifest.app_data_sources = {chrome};
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    manifest.users = {user};
    manifest.manifest_checksum = manifest.computeManifestChecksum();

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    auto* page = restore_wizard.findChild<sak::UserProfileRestoreAppDataPage*>();
    QVERIFY(page != nullptr);
    page->loadAppDataSources();
    QCOMPARE(page->m_appDataTree->topLevelItemCount(), 0);
    QVERIFY2(page->m_statusLabel->text().contains(QStringLiteral("older version")),
             qPrintable(page->m_statusLabel->text()));
}

void UserProfileAppRestoreFlowTests::restoreFolderPageLeavesAppDataToItsOwnPage() {
    sak::FolderSelection stored;
    stored.type = sak::FolderType::ApplicationData;
    stored.display_name = QStringLiteral("Chrome Profiles");
    stored.relative_path = QStringLiteral("AppData/Local/Google/Chrome/User Data");
    sak::AppDataSourceInfo chrome;
    chrome.name = stored.display_name;
    chrome.relative_path = stored.relative_path;
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    user.backed_up_folders = {stored};
    sak::BackupManifest manifest;
    manifest.users = {user};
    manifest.app_data_sources = {chrome};

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    sak::UserMapping mapping;
    mapping.source_username = user.username;
    restore_wizard.setUserMappings({mapping});
    auto* page = restore_wizard.findChild<sak::UserProfileRestoreFolderSelectionPage*>();
    QVERIFY(page != nullptr);
    page->initializePage();
    QCOMPARE(page->m_folderTable->rowCount(), 0);
    // Nothing to tick here, and the application data page still has something to restore.
    QVERIFY(page->validatePage());
}

void UserProfileAppRestoreFlowTests::restoreFolderPageRecordsTheChoiceWithoutTouchingTheManifest() {
    sak::FolderSelection documents;
    documents.type = sak::FolderType::Documents;
    documents.display_name = QStringLiteral("Documents");
    documents.relative_path = QStringLiteral("Documents");
    documents.selected = true;
    sak::FolderSelection pictures = documents;
    pictures.type = sak::FolderType::Pictures;
    pictures.display_name = QStringLiteral("Pictures");
    pictures.relative_path = QStringLiteral("Pictures");
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    user.backed_up_folders = {documents, pictures};
    sak::BackupManifest manifest;
    manifest.users = {user};
    manifest.manifest_checksum = manifest.computeManifestChecksum();

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    sak::UserMapping mapping;
    mapping.source_username = user.username;
    restore_wizard.setUserMappings({mapping});
    auto* page = restore_wizard.findChild<sak::UserProfileRestoreFolderSelectionPage*>();
    QVERIFY(page != nullptr);
    page->initializePage();
    QCOMPARE(page->m_folderTable->rowCount(), 2);
    page->m_folderTable->item(1, 0)->setCheckState(Qt::Unchecked);
    QVERIFY(page->validatePage());

    const auto choices = restore_wizard.folderChoices();
    QCOMPARE(choices.size(), 2);
    QCOMPARE(choices.at(1).relative_path, QStringLiteral("Pictures"));
    QVERIFY(!choices.at(1).selected);
    QVERIFY(restore_wizard.manifest().verifyManifestChecksum());
}

// A real process with a name on the list: a program's own copy of ping renamed, so nothing depends
// on what this machine has installed or running.
void UserProfileAppRestoreFlowTests::runningProgramStopsTheBackupUntilAnswered() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString fake_vlc = dir.filePath(QStringLiteral("vlc.exe"));
    const QString ping = QString::fromLocal8Bit(qgetenv("SystemRoot")) +
                         QStringLiteral("/System32/PING.EXE");
    QVERIFY(QFile::copy(ping, fake_vlc));
    QProcess running;
    running.start(fake_vlc,
                  {QStringLiteral("-n"), QStringLiteral("60"), QStringLiteral("127.0.0.1")});
    QVERIFY(running.waitForStarted());

    sak::AppDataSourceInfo vlc;
    vlc.name = QStringLiteral("VLC Settings");
    vlc.relative_path = QStringLiteral("AppData/Roaming/vlc");
    vlc.selected = true;

    QString question;
    QMessageBox::StandardButton answer = QMessageBox::Cancel;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&question, &answer]() {
        auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget());
        if (box != nullptr) {
            question = box->text();
            box->button(answer)->click();
        }
    });
    poll.start();

    QVERIFY(!sak::askToCloseProgramsHolding(nullptr, {vlc}));
    QVERIFY2(question.contains(vlc.name), qPrintable(question));

    answer = QMessageBox::Ignore;
    QVERIFY(sak::askToCloseProgramsHolding(nullptr, {vlc}));

    // Nothing selected, nothing to ask.
    question.clear();
    vlc.selected = false;
    QVERIFY(sak::askToCloseProgramsHolding(nullptr, {vlc}));
    QVERIFY(question.isEmpty());

    running.kill();
    QVERIFY(running.waitForFinished());
}

// Replace overwrites someone's files, so it is something to ask for, not what picking an account
// does on its own. It used to be exactly that, and Auto-Map picks accounts for every matching row.
void UserProfileAppRestoreFlowTests::pickingAnAccountLandsOnMergeNotReplace() {
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    sak::BackupManifest manifest;
    manifest.users = {user};
    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    auto* page = restore_wizard.findChild<sak::UserProfileRestoreUserMappingPage*>();
    QVERIFY(page != nullptr);
    page->initializePage();

    auto* destination = qobject_cast<QComboBox*>(page->m_mappingTable->cellWidget(0, 3));
    auto* mode = qobject_cast<QComboBox*>(page->m_mappingTable->cellWidget(0, 4));
    QVERIFY(destination != nullptr && mode != nullptr);
    QCOMPARE(static_cast<sak::MergeMode>(mode->currentData().toInt()),
             sak::MergeMode::CreateNewUser);

    destination->addItem(QStringLiteral("Pat"), QStringLiteral("Pat"));
    destination->setCurrentIndex(destination->count() - 1);
    QCOMPARE(static_cast<sak::MergeMode>(mode->currentData().toInt()),
             sak::MergeMode::MergeIntoDestination);
}

// Next on the last settings page used to start the restore with no warning at all.
void UserProfileAppRestoreFlowTests::restoreStartsOnlyFromItsNamedLastStep() {
    sak::UserProfileRestoreWizard restore_wizard;
    auto* last_step = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    auto* execute = restore_wizard.findChild<sak::UserProfileRestoreExecutePage*>();
    QVERIFY(last_step != nullptr && execute != nullptr);
    QVERIFY(last_step->isCommitPage());
    QCOMPARE(last_step->buttonText(QWizard::CommitButton), QStringLiteral("Start Restore"));

    // No other page may lead straight into the run.
    const QList<int> ids = restore_wizard.pageIds();
    QCOMPARE(restore_wizard.page(ids.at(ids.size() - 2)), static_cast<QWizardPage*>(last_step));
    QCOMPARE(restore_wizard.page(ids.last()), static_cast<QWizardPage*>(execute));
}

void UserProfileAppRestoreFlowTests::lastStepSaysWhatHappensToExistingFiles() {
    sak::UserMapping replace;
    replace.source_username = QStringLiteral("Pat");
    replace.destination_username = QStringLiteral("Pat");
    replace.mode = sak::MergeMode::ReplaceDestination;
    sak::UserMapping merge;
    merge.source_username = QStringLiteral("Sam");
    merge.destination_username = QStringLiteral("Sam");
    merge.mode = sak::MergeMode::MergeIntoDestination;
    sak::UserMapping new_folder;
    new_folder.source_username = QStringLiteral("Alex");
    new_folder.mode = sak::MergeMode::CreateNewUser;

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setUserMappings({replace, merge, new_folder});
    auto* page = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    QVERIFY(page != nullptr);
    page->initializePage();

    const QString summary = page->m_summaryLabel->text();
    QVERIFY2(summary.contains(QStringLiteral("<b>overwrite</b>")), qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("follow the conflict rule")), qPrintable(summary));
    QVERIFY2(summary.contains(QStringLiteral("no Windows account")), qPrintable(summary));
}

void UserProfileAppRestoreFlowTests::lastStepRefusesAWrongPassword() {
    QTemporaryDir backup_dir;
    QVERIFY(backup_dir.isValid());
    const QString source = backup_dir.filePath(QStringLiteral("source.txt"));
    QFile file(source);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(QByteArray(4096, 'x'));
    file.close();
    QVERIFY(QDir(backup_dir.path()).mkpath(QStringLiteral("Pat")));
    sak::BackupCodecOptions codec;
    codec.encrypt = true;
    codec.password = QStringLiteral("right password");
    QVERIFY(
        sak::writeBackupFile(source, backup_dir.filePath(QStringLiteral("Pat/stored.txt")), codec)
            .has_value());

    sak::UserMapping mapping;
    mapping.source_username = QStringLiteral("Pat");
    mapping.destination_username = sak::WindowsUserScanner::getCurrentUsername();
    mapping.mode = sak::MergeMode::MergeIntoDestination;

    sak::BackupManifest manifest;
    manifest.encrypted = true;
    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    restore_wizard.setBackupPath(backup_dir.path());
    restore_wizard.setUserMappings({mapping});
    auto* page = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    QVERIFY(page != nullptr);

    QString typed = QStringLiteral("not it");
    QString warning;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&typed, &warning]() {
        QWidget* modal = QApplication::activeModalWidget();
        if (auto* input = qobject_cast<QInputDialog*>(modal)) {
            input->setTextValue(typed);
            input->accept();
        } else if (auto* box = qobject_cast<QMessageBox*>(modal)) {
            warning = box->text();
            box->accept();
        }
    });
    poll.start();

    QVERIFY(!page->validatePage());
    QVERIFY2(warning.contains(QStringLiteral("does not open this backup")), qPrintable(warning));
    QVERIFY(restore_wizard.password().isEmpty());

    typed = QStringLiteral("right password");
    QVERIFY(page->validatePage());
    QCOMPARE(restore_wizard.password(), typed);
}

// Every file would otherwise fail one by one: there is no account to look up.
void UserProfileAppRestoreFlowTests::lastStepRefusesToAssignToAFolderWithNoAccount() {
    sak::UserMapping mapping;
    mapping.source_username = QStringLiteral("Pat");
    mapping.mode = sak::MergeMode::CreateNewUser;
    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setUserMappings({mapping});
    auto* page = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    QVERIFY(page != nullptr);
    page->m_permissionModeCombo->setCurrentIndex(page->m_permissionModeCombo->findData(
        static_cast<int>(sak::PermissionMode::AssignToDestination)));

    QString warning;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&warning]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            warning = box->text();
            box->accept();
        }
    });
    poll.start();

    QVERIFY(!page->validatePage());
    QVERIFY2(warning.contains(QStringLiteral("needs a Windows account")), qPrintable(warning));
}

// netsh refuses these without administrator rights, one failure at a time, after the files are
// already restored. Better to ask on the step where the person can still change their mind.
void UserProfileAppRestoreFlowTests::lastStepAsksForAdministratorForNetworkSettings() {
    sak::WifiProfileInfo wifi;
    wifi.profile_name = QStringLiteral("Home");
    wifi.selected = true;
    sak::UserMapping mapping;
    mapping.source_username = QStringLiteral("Pat");
    mapping.destination_username = sak::WindowsUserScanner::getCurrentUsername();
    mapping.mode = sak::MergeMode::MergeIntoDestination;

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setUserMappings({mapping});
    auto* page = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    QVERIFY(page != nullptr);

    QString question;
    QTimer poll;
    poll.setInterval(kDialogPollMs);
    connect(&poll, &QTimer::timeout, this, [&question]() {
        if (auto* box = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
            question = box->text();
            box->button(QMessageBox::No)->click();
        }
    });
    poll.start();

    // Nothing here needs the rights: restoring into your own account, no network settings.
    QVERIFY(page->validatePage());
    QVERIFY(question.isEmpty());

    restore_wizard.setWifiProfiles({wifi});
    const bool elevated = sak::ElevationManager::isElevated();
    QCOMPARE(page->validatePage(), elevated);
    QCOMPARE(question.contains(QStringLiteral("administrator")), !elevated);

    // Unticked in the wizard: no longer anything to ask about.
    question.clear();
    wifi.selected = false;
    restore_wizard.setWifiProfiles({wifi});
    QVERIFY(page->validatePage());
    QVERIFY(question.isEmpty());
}

QTEST_MAIN(UserProfileAppRestoreFlowTests)
#include "test_user_profile_app_restore_flow.moc"
