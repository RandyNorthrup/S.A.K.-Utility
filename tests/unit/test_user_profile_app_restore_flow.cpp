// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_app_restore_flow.cpp
/// @brief A scanned application reaches the restore page as a real install offer: scan result,
///        backup page, installed_apps.json on disk, restore page, list handed to Chocolatey.

#include "sak/app_data_program_prompt.h"
#include "sak/app_data_sources.h"
#include "sak/backup_file_codec.h"
#include "sak/elevation_manager.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_backup_worker.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_user_scanner.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QTreeWidget>
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

const QString kWalkAppData = QStringLiteral("AppData/Roaming/Test Program");

/// A real backup through the real worker, with one folder and one application data source.
bool makeWalkBackup(const QString& profile_path, const QString& backup_path) {
    const QDir profile(profile_path);
    if (!profile.mkpath(QStringLiteral("Documents")) || !profile.mkpath(kWalkAppData)) {
        return false;
    }
    for (const QString& relative :
         {QStringLiteral("Documents/report.txt"), kWalkAppData + QStringLiteral("/settings.ini")}) {
        QFile file(profile.filePath(relative));
        if (!file.open(QIODevice::WriteOnly) || file.write("content") <= 0) {
            return false;
        }
    }

    sak::FolderSelection documents;
    documents.type = sak::FolderType::Documents;
    documents.display_name = QStringLiteral("Documents");
    documents.relative_path = QStringLiteral("Documents");
    documents.selected = true;
    sak::UserProfile user;
    user.username = QStringLiteral("sakwizardwalkuser");
    user.profile_path = profile_path;
    user.is_selected = true;
    user.folder_selections = {documents};
    sak::AppDataSourceInfo source;
    source.name = QStringLiteral("Test Program");
    source.category = QStringLiteral("Test");
    source.relative_path = kWalkAppData;
    source.selected = true;
    sak::BackupManifest to_save;
    to_save.app_data_sources = {source};

    sak::UserProfileBackupWorker worker;
    QSignalSpy done(&worker, &sak::UserProfileBackupWorker::backupComplete);
    sak::UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = sak::PermissionMode::PreserveOriginal;
    worker.startBackup(to_save, {user}, backup_path, sak::SmartFilter{}, options);
    const bool finished = QTest::qWaitFor([&done]() { return done.count() == 1; }, 30'000);
    return finished && worker.wait(30'000) && done.first().at(0).toBool();
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
    void theWifiListSaysWhyItIsEmpty();
    void deselectedAppIsNotInTheBackup();
    void restoreIgnoresAnIdChocolateyWouldRefuse();
    void restoreInstallsAPackageOnce();
    void restoreInstallAsksForAdministratorFirst();
    void appDataSizeTravelsToTheManifestAndTheRestorePage();
    void restorePageOffersOnlyStoredAppData();
    void wizardPagesKeepARowsPatterns();
    void restoreFolderPageLeavesAppDataToItsOwnPage();
    void restoreFolderPageRecordsTheChoiceWithoutTouchingTheManifest();
    void runningProgramStopsTheBackupUntilAnswered();
    void pickingAnAccountLandsOnMergeNotReplace();
    void restoreStartsOnlyFromItsNamedLastStep();
    void lastStepSaysWhatHappensToExistingFiles();
    void lastStepRefusesAWrongPassword();
    void lastStepNotesHaveNoStandInMarks();
    void lastStepRefusesToAssignToAFolderWithNoAccount();
    void lastStepAsksForAdministratorForNetworkSettings();
    void filterBoxesReachTheRulesTheRunUses();
    void verifyBoxReachesTheRun();
    void destinationStartsEmpty();
    void skipDoesNotCarryOntoThePageAfter();
    void customFolderMustBeInsideTheProfile();
    void executePageRunsWhatTheWizardChose();
    void executePageShowsWhatWentWrong();
    void restorePageTellsFinishedWithProblemsFromStopped();
    void wholeRestoreWizardRunsOnARealBackup();
    void backupWizardWalksToItsLastPage();
    void aFinishedBackupStopsItsBars();
    void backupWizardReadsAsPlainText();

private:
    static sak::UserProfileRestoreAppRestorePage* restorePage(
        sak::UserProfileRestoreWizard& wizard);
};

sak::UserProfileRestoreAppRestorePage* UserProfileAppRestoreFlowTests::restorePage(
    sak::UserProfileRestoreWizard& wizard) {
    return wizard.findChild<sak::UserProfileRestoreAppRestorePage*>();
}

// Review 2 (F35): the Wi-Fi page's list was empty with nothing to say why, before a scan and after
// one that found no profiles (the capture VM has no Wi-Fi adapter).
void UserProfileAppRestoreFlowTests::theWifiListSaysWhyItIsEmpty() {
    sak::UserProfileBackupWizard backup_wizard;
    auto* page = qobject_cast<sak::UserProfileBackupKnownNetworksPage*>(
        backup_wizard.page(sak::UserProfileBackupWizard::Page_KnownNetworks));
    QVERIFY(page != nullptr);
    const QLabel* empty = page->m_networkTree->viewport()->findChild<QLabel*>();
    QVERIFY(empty != nullptr);
    QVERIFY2(empty->text().startsWith(QStringLiteral("Press Scan Networks")),
             qPrintable(empty->text()));
    page->populateTree({});
    QCOMPARE(empty->text(), QStringLiteral("This PC has no saved Wi-Fi profiles."));
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

// Each page rebuilds its rows from its tree. A row that lost its patterns there would copy the
// whole Outlook folder, cache and all, on a backup, and could not be told from its twin on a
// restore.
void UserProfileAppRestoreFlowTests::wizardPagesKeepARowsPatterns() {
    sak::AppDataSourceInfo archives;
    sak::AppDataSourceInfo cache;
    for (const sak::AppDataSourceInfo& source : sak::commonAppDataSources()) {
        if (source.name == QStringLiteral("Outlook Archives (PST)")) {
            archives = source;
        } else if (source.name == QStringLiteral("Outlook Cache (OST)")) {
            cache = source;
        }
    }
    QVERIFY(archives.splitsItsPath() && cache.splitsItsPath());

    sak::UserProfileBackupWizard backup_wizard;
    auto* backup_page = backup_wizard.findChild<sak::UserProfileBackupAppDataPage*>();
    QVERIFY(backup_page != nullptr);
    backup_page->populateTree({archives, cache});
    QCOMPARE(backup_wizard.appDataSources().size(), 1);  // the cache starts unticked
    QCOMPARE(backup_wizard.appDataSources().first().name, archives.name);
    QCOMPARE(backup_wizard.appDataSources().first().include_patterns, archives.include_patterns);

    sak::FolderSelection stored_archives;
    stored_archives.type = sak::FolderType::ApplicationData;
    stored_archives.relative_path = archives.relative_path;
    stored_archives.include_patterns = archives.include_patterns;
    sak::FolderSelection stored_cache = stored_archives;
    stored_cache.include_patterns.clear();
    stored_cache.exclude_patterns = cache.exclude_patterns;
    sak::BackupUserData user;
    user.username = QStringLiteral("Pat");
    user.backed_up_folders = {stored_archives, stored_cache};
    sak::BackupManifest manifest;
    manifest.app_data_sources = {archives, cache};
    manifest.users = {user};
    manifest.manifest_checksum = manifest.computeManifestChecksum();

    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setManifest(manifest);
    auto* restore_page = restore_wizard.findChild<sak::UserProfileRestoreAppDataPage*>();
    QVERIFY(restore_page != nullptr);
    restore_page->loadAppDataSources();
    QVERIFY(restore_page->validatePage());
    const QVector<sak::AppDataSourceInfo> chosen = restore_wizard.appDataSources();
    QCOMPARE(chosen.size(), 2);
    for (const sak::AppDataSourceInfo& row : chosen) {
        const sak::AppDataSourceInfo& expected = row.name == archives.name ? archives : cache;
        QCOMPARE(row.include_patterns, expected.include_patterns);
        QCOMPARE(row.exclude_patterns, expected.exclude_patterns);
    }
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

// UI review S5: "[OK]" and "(!)" stood in for icons in the last step's permission notes.
void UserProfileAppRestoreFlowTests::lastStepNotesHaveNoStandInMarks() {
    sak::UserMapping merge;
    merge.source_username = QStringLiteral("Sam");
    merge.destination_username = QStringLiteral("Sam");
    merge.mode = sak::MergeMode::MergeIntoDestination;
    sak::UserProfileRestoreWizard restore_wizard;
    restore_wizard.setUserMappings({merge});
    auto* page = restore_wizard.findChild<sak::UserProfileRestorePermissionSettingsPage*>();
    QVERIFY(page != nullptr);
    page->initializePage();
    QVERIFY(page->m_permissionModeCombo->count() > 1);
    for (int index = 0; index < page->m_permissionModeCombo->count(); ++index) {
        page->m_permissionModeCombo->setCurrentIndex(index);
        page->updateSummary();
        const QString summary = page->m_summaryLabel->text();
        for (const QString mark : {QStringLiteral("[OK]"), QStringLiteral("(!)")}) {
            QVERIFY2(!summary.contains(mark), qPrintable(summary));
        }
    }
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

void UserProfileAppRestoreFlowTests::filterBoxesReachTheRulesTheRunUses() {
    sak::UserProfileBackupWizard wizard;
    auto* page = qobject_cast<sak::UserProfileBackupSmartFiltersPage*>(
        wizard.page(sak::UserProfileBackupWizard::Page_SmartFilters));
    QVERIFY(page != nullptr);
    page->initializePage();
    QVERIFY(page->m_excludeCacheCheck->isChecked());
    QVERIFY(page->m_excludeTempCheck->isChecked());
    QVERIFY(page->m_excludeLockCheck->isChecked());
    QVERIFY(wizard.getSmartFilter().excludesCache());

    page->m_excludeCacheCheck->setChecked(false);
    QVERIFY(!wizard.getSmartFilter().excludesCache());
    QVERIFY(wizard.getSmartFilter().excludesTemporaryFiles());

    page->m_excludeTempCheck->setChecked(false);
    QVERIFY(!wizard.getSmartFilter().excludesTemporaryFiles());
    QVERIFY(wizard.getSmartFilter().excludesLockFiles());

    page->m_excludeLockCheck->setChecked(false);
    QVERIFY(!wizard.getSmartFilter().excludesLockFiles());

    // Back and forward again must show what was chosen, not reset it.
    page->initializePage();
    QVERIFY(!page->m_excludeCacheCheck->isChecked());
    QVERIFY(!page->m_excludeTempCheck->isChecked());
    QVERIFY(!page->m_excludeLockCheck->isChecked());

    page->onResetToDefaults();
    QVERIFY(page->m_excludeCacheCheck->isChecked());
    QVERIFY(wizard.getSmartFilter().excludesCache());

    page->m_enableFolderSizeLimitCheck->setChecked(true);
    QVERIFY(wizard.getSmartFilter().enable_folder_size_limit);

    // Reset means all of it, not just the lists.
    page->onResetToDefaults();
    QVERIFY(!wizard.getSmartFilter().enable_folder_size_limit);
    QVERIFY(!page->m_enableFolderSizeLimitCheck->isChecked());
}

void UserProfileAppRestoreFlowTests::verifyBoxReachesTheRun() {
    sak::UserProfileBackupWizard wizard;
    auto* page = qobject_cast<sak::UserProfileBackupSettingsPage*>(
        wizard.page(sak::UserProfileBackupWizard::Page_BackupSettings));
    QVERIFY(page != nullptr);
    QVERIFY(wizard.verifyAfterBackup());
    page->m_verifyCheck->setChecked(false);
    QVERIFY(!wizard.verifyAfterBackup());
    QVERIFY2(page->m_verifyCheck->text().contains(QStringLiteral("SHA-256")),
             qPrintable(page->m_verifyCheck->text()));
}

// The suggested folder was inside the home folder, which the same page then refused for anyone
// backing up their own account.
void UserProfileAppRestoreFlowTests::destinationStartsEmpty() {
    sak::UserProfileBackupWizard wizard;
    auto* page = qobject_cast<sak::UserProfileBackupSettingsPage*>(
        wizard.page(sak::UserProfileBackupWizard::Page_BackupSettings));
    QVERIFY(page != nullptr);
    page->initializePage();
    QVERIFY(page->m_destinationEdit->text().isEmpty());
}

// Skip was never reset, so it stayed on the button through pages with nothing to skip.
void UserProfileAppRestoreFlowTests::skipDoesNotCarryOntoThePageAfter() {
    sak::UserProfileBackupWizard wizard;
    wizard.setStartId(sak::UserProfileBackupWizard::Page_AppData);
    wizard.restart();
    QCOMPARE(wizard.currentId(), static_cast<int>(sak::UserProfileBackupWizard::Page_AppData));
    QCOMPARE(wizard.buttonText(QWizard::NextButton), QStringLiteral("Skip >"));

    wizard.next();
    QCOMPARE(wizard.currentId(), static_cast<int>(sak::UserProfileBackupWizard::Page_SmartFilters));
    QCOMPARE(wizard.buttonText(QWizard::NextButton), QStringLiteral("Next >"));

    // A page with its own Skip still shows it.
    wizard.next();
    QCOMPARE(wizard.currentId(),
             static_cast<int>(sak::UserProfileBackupWizard::Page_KnownNetworks));
    QCOMPARE(wizard.buttonText(QWizard::NextButton), QStringLiteral("Skip >"));
}

void UserProfileAppRestoreFlowTests::customFolderMustBeInsideTheProfile() {
    using Dialog = sak::PerUserCustomizationDialog;
    const QString profile = QStringLiteral("C:/Users/Username");
    QCOMPARE(Dialog::pathInsideProfile(profile, profile + QStringLiteral("/Projects/Site")),
             QStringLiteral("Projects/Site"));
    QVERIFY(Dialog::pathInsideProfile(profile, QStringLiteral("D:/Work")).isEmpty());
    QVERIFY(Dialog::pathInsideProfile(profile, profile + QStringLiteral("2/Documents")).isEmpty());
    QVERIFY(Dialog::pathInsideProfile(profile, profile).isEmpty());
}

// The whole backup path as the wizard drives it: what the pages chose reaches the worker.
void UserProfileAppRestoreFlowTests::executePageRunsWhatTheWizardChose() {
    QTemporaryDir profile;
    QTemporaryDir destination;
    QVERIFY(profile.isValid() && destination.isValid());
    const QString app_data = QStringLiteral("AppData/Roaming/Test Program");
    const QString file_inside = app_data + QStringLiteral("/settings.ini");
    QVERIFY(QDir(profile.path()).mkpath(app_data));
    QFile file(profile.filePath(file_inside));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("setting=1");
    file.close();

    sak::UserProfile user;
    user.username = QStringLiteral("Pat");
    user.profile_path = profile.path();
    user.is_selected = true;

    sak::AppDataSourceInfo source;
    source.name = QStringLiteral("Test Program");
    source.relative_path = app_data;
    source.selected = true;

    sak::UserProfileBackupWizard wizard;
    wizard.setAppDataSources({source});
    const QString backup_path = destination.filePath(QStringLiteral("backup"));
    auto* page = qobject_cast<sak::UserProfileBackupExecutePage*>(
        wizard.page(sak::UserProfileBackupWizard::Page_Execute));
    QVERIFY(page != nullptr);
    page->setRunInputs({user}, backup_path);

    page->onStartBackup();
    QTRY_VERIFY_WITH_TIMEOUT(page->isComplete(), 30'000);
    QVERIFY2(QFile::exists(backup_path + QStringLiteral("/Pat/") + file_inside),
             qPrintable(page->m_logEdit->toPlainText()));
    QVERIFY2(page->m_logEdit->toPlainText().contains(
                 QStringLiteral("Checked 1 stored file(s) against their originals")),
             qPrintable(page->m_logEdit->toPlainText()));
    // The page and the worker both wrote the start and the outcome, so in the capture VM's log
    // each appeared twice, and "=== Backup Complete ===" stood over "=== Backup Failed ===".
    const QString log = page->m_logEdit->toPlainText();
    for (const QString& once : {QStringLiteral("=== Backup Started ==="),
                                QStringLiteral("Users to backup: 1"),
                                QStringLiteral("=== Backup Complete ==="),
                                QStringLiteral("Files copied: 1\n")}) {
        QVERIFY2(log.count(once) == 1, qPrintable(once + QStringLiteral(" in\n") + log));
    }

    const auto stored = sak::BackupManifest::loadFromFile(backup_path + "/manifest.json");
    QCOMPARE(stored.users.size(), 1);
    QCOMPARE(stored.users.first().backed_up_folders.size(), 1);
    QCOMPARE(stored.users.first().backed_up_folders.first().type, sak::FolderType::ApplicationData);
}

// "Backup failed!" said nothing, and was also what a backup missing one locked file got.
void UserProfileAppRestoreFlowTests::executePageShowsWhatWentWrong() {
    sak::BackupManifest manifest;
    sak::UserProfileBackupExecutePage page(manifest, {}, QString());
    page.onBackupComplete(false,
                          QStringLiteral("Backup finished, but not everything was copied.\n"
                                         "Files copied: 3"));
    QCOMPARE(page.m_statusLabel->text(),
             QStringLiteral("Backup finished, but not everything was copied."));
    page.onBackupComplete(true, QStringLiteral("Backup complete!\nFiles copied: 3"));
    QCOMPARE(page.m_statusLabel->text(), QStringLiteral("Backup completed successfully!"));
}

// Review 3: every file came back and one netsh call failed, and the page said "Restore failed",
// "Restore Stopped" and "The restore did not finish." A restore that ran every step finished, with
// what it could not do named; only one that stopped part way says it stopped.
void UserProfileAppRestoreFlowTests::restorePageTellsFinishedWithProblemsFromStopped() {
    sak::UserProfileRestoreExecutePage finished;
    finished.onRestoreRanToEnd(0, 1);
    finished.onRestoreComplete(
        false,
        QStringLiteral("Restore finished, but not everything was restored.\nFiles restored: 1198"));
    QCOMPARE(finished.title(), QStringLiteral("Restore Finished with Problems"));
    QCOMPARE(finished.subTitle(),
             QStringLiteral("The restore finished, but not everything was restored."));
    QCOMPARE(finished.m_statusLabel->text(),
             QStringLiteral("Restore finished with problems: 1 network setting was not applied"));
    QCOMPARE(finished.m_currentOperationLabel->text(), QStringLiteral("Current: done"));
    QCOMPARE(finished.m_overallProgressBar->value(), finished.m_overallProgressBar->maximum());

    sak::UserProfileRestoreExecutePage both;
    both.onRestoreRanToEnd(3, 2);
    both.onRestoreComplete(false, QString());
    QCOMPARE(both.m_statusLabel->text(),
             QStringLiteral("Restore finished with problems: 3 files or folders could not be "
                            "restored; 2 network settings were not applied"));

    sak::UserProfileRestoreExecutePage stopped;
    stopped.onRestoreComplete(false, QStringLiteral("Restore cancelled"));
    QCOMPARE(stopped.title(), QStringLiteral("Restore Stopped"));
    QCOMPARE(stopped.subTitle(), QStringLiteral("The restore did not finish."));
    QCOMPARE(stopped.m_statusLabel->text(), QStringLiteral("Restore failed"));
}

// The Select Folders crash sat on the only route through this wizard and no test walked it. Every
// page opens here in order, on a real backup, and then the page that does the work runs.
void UserProfileAppRestoreFlowTests::wholeRestoreWizardRunsOnARealBackup() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    const QByteArray saved_drive = qgetenv("SystemDrive");
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    const auto restore_drive =
        qScopeGuard([&saved_drive]() { qputenv("SystemDrive", saved_drive); });

    QVERIFY2(makeWalkBackup(profile.path(), backup.path()), "the backup for this walk failed");

    sak::UserProfileRestoreWizard wizard;
    wizard.restart();
    auto* backup_path = wizard.currentPage()->findChild<QLineEdit*>();
    QVERIFY(backup_path != nullptr);
    backup_path->setText(backup.path());

    const QList<int> ids = wizard.pageIds();
    while (wizard.currentId() != ids.at(ids.size() - 2)) {
        const int before = wizard.currentId();
        wizard.next();
        QVERIFY2(wizard.currentId() != before, qPrintable(wizard.currentPage()->title()));
    }
    QCOMPARE(wizard.currentPage()->title(), QStringLiteral("Review and Start Restore"));
    QCOMPARE(wizard.backupPath(), backup.path());
    QCOMPARE(wizard.userMappings().size(), 1);
    QCOMPARE(wizard.folderChoices().size(), 1);
    QCOMPARE(wizard.appDataSources().size(), 1);

    // Start Restore's own checks are covered above, and this account has no rights to pass them
    // with a new folder. The run is what is left.
    auto* last = qobject_cast<sak::UserProfileRestorePermissionSettingsPage*>(wizard.currentPage());
    QVERIFY(last != nullptr);
    auto* execute = wizard.findChild<sak::UserProfileRestoreExecutePage*>();
    QVERIFY(execute != nullptr);
    wizard.setPermissionMode(sak::PermissionMode::PreserveOriginal);
    execute->onStartRestore();
    QTRY_VERIFY_WITH_TIMEOUT(execute->isComplete(), 30'000);
    QVERIFY2(execute->m_restoreSuccess, qPrintable(execute->m_logText->toPlainText()));
    // The finished page says so everywhere: the summary once, both bars full, "Current:" and the
    // subtitle moved on. The log printed the summary twice and the file bar stopped at 94%.
    const QString log = execute->m_logText->toPlainText();
    QCOMPARE(log.count(QStringLiteral("Files restored:")), 1);
    QCOMPARE(execute->m_currentProgressBar->value(), execute->m_currentProgressBar->maximum());
    QVERIFY(execute->m_currentProgressBar->maximum() > 0);
    QCOMPARE(execute->m_currentOperationLabel->text(), QStringLiteral("Current: done"));
    QCOMPARE(execute->subTitle(), QStringLiteral("The restore finished."));
    // Review 2: the header still read "Restore in Progress", and both Cancel buttons were live.
    QCOMPARE(execute->title(), QStringLiteral("Restore Complete"));
    QVERIFY(!execute->m_cancelButton->isEnabled());
    QVERIFY(!wizard.button(QWizard::CancelButton)->isEnabled());

    const QDir landed(system_drive.filePath(QStringLiteral("Users/sakwizardwalkuser")));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("Documents/report.txt"))));
    QVERIFY(QFile::exists(landed.filePath(kWalkAppData + QStringLiteral("/settings.ini"))));
}

// Walks the wizard with the buttons a person sees. wizard.next() alone reached the last page while
// the window itself showed Finish on Backup Settings, and Finish closed it with nothing backed up.
void UserProfileAppRestoreFlowTests::backupWizardWalksToItsLastPage() {
    QTemporaryDir destination;
    QVERIFY(destination.isValid());
    sak::UserProfileBackupWizard wizard;
    wizard.restart();
    while (wizard.currentId() != sak::UserProfileBackupWizard::Page_BackupSettings) {
        const int before = wizard.currentId();
        wizard.next();
        QVERIFY2(wizard.currentId() != before, qPrintable(wizard.currentPage()->title()));
    }
    auto* settings = qobject_cast<sak::UserProfileBackupSettingsPage*>(wizard.currentPage());
    QVERIFY(settings != nullptr);
    QVERIFY(!settings->isFinalPage());
    QVERIFY(!wizard.button(QWizard::NextButton)->isHidden());
    QVERIFY(wizard.button(QWizard::FinishButton)->isHidden());

    const QString backup_path = destination.filePath(QStringLiteral("backup"));
    settings->m_destinationEdit->setText(backup_path);
    wizard.button(QWizard::NextButton)->click();
    QCOMPARE(wizard.currentId(), static_cast<int>(sak::UserProfileBackupWizard::Page_Execute));
    auto* execute = qobject_cast<sak::UserProfileBackupExecutePage*>(wizard.currentPage());
    QVERIFY(execute != nullptr);
    // The screened destination reached the page that runs the backup.
    QVERIFY2(execute->m_destinationPath.endsWith(QStringLiteral("backup")),
             qPrintable(execute->m_destinationPath));
}

// UI re-check N10: after the backup finished, its subtitle still read "Backup in progress..." and
// the current-operation bar kept its busy animation.
void UserProfileAppRestoreFlowTests::aFinishedBackupStopsItsBars() {
    sak::BackupManifest manifest;
    sak::UserProfileBackupExecutePage page(manifest, {}, QString());
    page.m_currentProgress->setRange(0, 0);  // as it is while the worker runs
    page.onBackupComplete(true, QStringLiteral("Backup finished"));
    QVERIFY(page.m_currentProgress->maximum() > 0);
    QCOMPARE(page.m_currentProgress->value(), page.m_currentProgress->maximum());
    QCOMPARE(page.subTitle(), QStringLiteral("The backup finished."));
}

// UI review S5, S17, DF13: "[shield]", "[save]" and "(i)" stood in for icons, the users table's
// Select column was headed "?", and each settings row sized its own label, so the fields after them
// started at 139, 141 and 149 px.
void UserProfileAppRestoreFlowTests::backupWizardReadsAsPlainText() {
    sak::UserProfileBackupWizard wizard;
    wizard.restart();
    const QStringList marks = {QStringLiteral("[shield]"),
                               QStringLiteral("[save]"),
                               QStringLiteral("[FOLDER]"),
                               QStringLiteral("(i)"),
                               QStringLiteral("(!)"),
                               QStringLiteral("[OK]"),
                               QStringLiteral("[x]")};
    QStringList found;
    const auto scan = [&]() {
        for (const QLabel* label : wizard.findChildren<QLabel*>()) {
            for (const QString& mark : marks) {
                if (label->text().contains(mark)) {
                    found << label->text();
                }
            }
        }
        for (const QTableWidget* table : wizard.findChildren<QTableWidget*>()) {
            for (int column = 0; column < table->columnCount(); ++column) {
                const QTableWidgetItem* header = table->horizontalHeaderItem(column);
                if (header != nullptr && header->text().trimmed() == QStringLiteral("?")) {
                    found << QStringLiteral("a column headed \"?\"");
                }
            }
        }
    };
    scan();
    while (wizard.currentId() != sak::UserProfileBackupWizard::Page_BackupSettings) {
        const int before = wizard.currentId();
        wizard.next();
        QVERIFY2(wizard.currentId() != before, qPrintable(wizard.currentPage()->title()));
        scan();
    }
    found.removeDuplicates();
    QVERIFY2(found.isEmpty(), qPrintable(found.join(QStringLiteral("\n"))));

    auto* settings = qobject_cast<sak::UserProfileBackupSettingsPage*>(wizard.currentPage());
    QVERIFY(settings != nullptr);
    const int width = settings->m_destinationLabel->minimumWidth();
    QVERIFY(width >= settings->m_destinationLabel->sizeHint().width());
    QCOMPARE(settings->m_compressionLabel->minimumWidth(), width);
    QCOMPARE(settings->m_permissionLabel->minimumWidth(), width);
}

QTEST_MAIN(UserProfileAppRestoreFlowTests)
#include "test_user_profile_app_restore_flow.moc"
