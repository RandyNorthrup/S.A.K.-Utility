// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_backup_offline_source.cpp
/// @brief The wizard's offline source step and profile list: where the flow goes, what an
///        unreadable volume says, what a rescue stamps on the manifest, and that the recovery
///        key reaches the unlock seam and nothing else. Every disk, elevation and process touch
///        is replaced, so none of this needs a drive, a helper or administrator rights.

#include "sak/app_paths.h"
#include "sak/offline_profile_source.h"
#include "sak/offline_profile_source_page.h"
#include "sak/user_profile_backup_wizard.h"

#include <QApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QSysInfo>
#include <QTableWidgetItem>
#include <QtTest/QtTest>

#include <optional>

using namespace sak;

namespace {

const QString kGuidA = QStringLiteral("\\\\?\\Volume{aaaa0000-0000-0000-0000-000000000001}");
const QString kGuidB = QStringLiteral("\\\\?\\Volume{bbbb0000-0000-0000-0000-000000000002}");
const QString kTypedKey = QStringLiteral("000000-111111-222222-333333-444444-555555-666666-777777");

OfflineProfileEntry makeEntry(const QString& folder,
                              const QString& sid,
                              bool sid_from_hive,
                              bool included,
                              const QString& skip_reason = QString()) {
    OfflineProfileEntry entry;
    entry.folder_name = folder;
    entry.profile_path = QStringLiteral("E:\\Users\\") + folder;
    entry.sid = sid;
    entry.sid_from_hive = sid_from_hive;
    entry.included = included;
    entry.skip_reason = skip_reason;
    entry.estimated_size_bytes = 2048;
    return entry;
}

OfflineVolumeSource makeReadableVolume() {
    OfflineVolumeSource source;
    source.volume_name = kGuidA;
    source.root_path = QStringLiteral("E:\\");
    source.display_name = QStringLiteral("Dead Laptop (E:)");
    source.windows_found = true;
    source.readable = true;
    source.computer_name = QStringLiteral("OLD-PC-NAME");
    source.machine_sid = QStringLiteral("S-1-5-21-1-2-3");
    // The skip reasons are the core seam's own words (OfflineProfileSource::profileSkipReason),
    // so the page is proved against what the scan really says, not against invented text.
    source.profiles = {
        makeEntry(QStringLiteral("bob"), QStringLiteral("S-1-5-21-1-2-3-1000"), true, true),
        makeEntry(QStringLiteral("carol"), QStringLiteral("S-1-5-21-1-2-3-1001"), true, true),
        makeEntry(QStringLiteral("Public"),
                  QString(),
                  false,
                  false,
                  QStringLiteral("Built-in Windows folder, not a user account.")),
        makeEntry(QStringLiteral("CORP\\alice"),
                  QStringLiteral("S-1-5-21-9-9-9-1001"),
                  true,
                  false,
                  QStringLiteral(
                      "Domain or network-account profile; offline rescue covers local accounts "
                      "only.")),
    };
    return source;
}

OfflineVolumeSource makeLockedVolume() {
    OfflineVolumeSource source;
    source.volume_name = kGuidB;
    source.display_name = QStringLiteral("Encrypted Disk");
    source.readable = false;
    source.unread_reason =
        QStringLiteral("This volume could not be read. It is most likely BitLocker-locked.");
    return source;
}

/// @brief Seam bookkeeping: how often each touch point ran, with what, and what it returned.
struct SeamLog {
    int scan_calls = 0;
    int rescan_calls = 0;
    int prepare_calls = 0;
    int unlock_calls = 0;
    bool last_scan_elevated = false;
    QString prepared_volume;
    QString unlocked_volume;
    QString unlocked_key;
};

OfflineSourceDiscovery makeDiscovery(const QVector<OfflineVolumeSource>& volumes) {
    OfflineSourceDiscovery discovery;
    discovery.enumeration_ok = true;
    discovery.volumes = volumes;
    return discovery;
}

UserProfileBackupOfflineProfilesPage* offlinePage(UserProfileBackupWizard& wizard) {
    return qobject_cast<UserProfileBackupOfflineProfilesPage*>(
        wizard.page(UserProfileBackupWizard::Page_OfflineProfiles));
}

UserProfileBackupSourcePage* sourcePage(UserProfileBackupWizard& wizard) {
    return qobject_cast<UserProfileBackupSourcePage*>(
        wizard.page(UserProfileBackupWizard::Page_Source));
}

}  // namespace

class TestOfflineSourceWizard : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void sourceStepDefaultsToThisPc();
    void offlineSourceSkipsThisPcPages();
    void unelevatedRefusesWithoutTouchingADisk();
    void lockedVolumeIsListedWithItsReasonAndOfferedForUnlock();
    void unlockSendsTheKeyToTheUnlockSeamOnly();
    void wrongKeyRefusesAndKeepsTheRowUnreadable();
    void preparedSourceNamesTheMachineAndFeedsTheUserList();
    void goingBackReleasesTheMount();
    void repeatedNextTakesThePreviousMountDownFirst();
    void skippedFolderCannotBeSelectedAndNeverReachesTheBackup();
    void recoveryKeyDialogRefusesAWrongShapeWithoutClosing();
    void manifestStampsTheOfflineMachineName();
};

void TestOfflineSourceWizard::sourceStepDefaultsToThisPc() {
    UserProfileBackupWizard wizard;
    auto* source = sourcePage(wizard);
    QVERIFY(source != nullptr);
    // Today's flow is the default: a technician who clicks Next without choosing gets a live scan.
    QCOMPARE(static_cast<int>(source->selectedSource()),
             static_cast<int>(UserProfileBackupSourcePage::SourceThisPc));
    QCOMPARE(source->nextId(), static_cast<int>(UserProfileBackupWizard::Page_SelectUsers));
    QVERIFY(!wizard.isOfflineSource());

    source->setSourceSelected(UserProfileBackupSourcePage::SourceOtherWindows);
    QCOMPARE(static_cast<int>(source->selectedSource()),
             static_cast<int>(UserProfileBackupSourcePage::SourceOtherWindows));
    QCOMPARE(source->nextId(), static_cast<int>(UserProfileBackupWizard::Page_OfflineProfiles));
    QVERIFY(wizard.isOfflineSource());
}

void TestOfflineSourceWizard::offlineSourceSkipsThisPcPages() {
    UserProfileBackupWizard wizard;
    auto* customize = qobject_cast<const UserProfileBackupCustomizeDataPage*>(
        wizard.page(UserProfileBackupWizard::Page_CustomizeData));
    auto* filters = qobject_cast<const UserProfileBackupSmartFiltersPage*>(
        wizard.page(UserProfileBackupWizard::Page_SmartFilters));
    QVERIFY(customize != nullptr);
    QVERIFY(filters != nullptr);

    // Live: the flow that shipped, page for page.
    QCOMPARE(customize->nextId(), static_cast<int>(UserProfileBackupWizard::Page_InstalledApps));
    QCOMPARE(filters->nextId(), static_cast<int>(UserProfileBackupWizard::Page_KnownNetworks));

    sourcePage(wizard)->setSourceSelected(UserProfileBackupSourcePage::SourceOtherWindows);
    auto* offline = offlinePage(wizard);
    QVERIFY(offline != nullptr);
    QCOMPARE(offline->nextId(), static_cast<int>(UserProfileBackupWizard::Page_CustomizeData));
    // Installed apps, app data, Wi-Fi and adapters describe the PC running S.A.K., so an
    // offline run must not walk through them.
    QCOMPARE(customize->nextId(), static_cast<int>(UserProfileBackupWizard::Page_SmartFilters));
    QCOMPARE(filters->nextId(), static_cast<int>(UserProfileBackupWizard::Page_BackupSettings));
}

void TestOfflineSourceWizard::unelevatedRefusesWithoutTouchingADisk() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool elevated) {
             ++log.scan_calls;
             log.last_scan_elevated = elevated;
             OfflineSourceDiscovery refused;
             refused.error = OfflineProfileSource::elevationRequiredMessage();
             return refused;
         },
         [&](const QString&, bool) { return OfflineVolumeSource{}; },
         [&](const QString&) { return std::unexpected(QStringLiteral("must not run")); },
         [&](const QString&, const QString&) {
             return std::unexpected(QStringLiteral("must not run"));
         },
         [](const QString&, QWidget*) { return std::optional<QString>{}; },
         [] { return false; }});

    page->initializePage();
    QCOMPARE(log.scan_calls, 1);
    QVERIFY(!log.last_scan_elevated);  // the page tells the seam it is not elevated
    QCOMPARE(page->statusText(), OfflineProfileSource::elevationRequiredMessage());
    QVERIFY(!page->isComplete());
    QVERIFY(!page->validatePage());
    QCOMPARE(log.prepare_calls, 0);
    QVERIFY(wizard.scannedUsers().isEmpty());
}

void TestOfflineSourceWizard::lockedVolumeIsListedWithItsReasonAndOfferedForUnlock() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool) { return makeDiscovery({makeReadableVolume(), makeLockedVolume()}); },
         [&](const QString&, bool) { return OfflineVolumeSource{}; },
         [&](const QString&) { return std::unexpected(QStringLiteral("must not run")); },
         [&](const QString&, const QString&) {
             return std::unexpected(QStringLiteral("must not run"));
         },
         [](const QString&, QWidget*) { return std::optional<QString>{}; },
         [] { return true; }});

    page->initializePage();
    QVERIFY(page->volumeTable() != nullptr);
    QCOMPARE(page->volumeTable()->rowCount(), 2);
    // The locked row says WHY. A row that read "0 profiles" would be an empty success.
    const QString locked_status = page->volumeTable()->item(1, 2)->text();
    QVERIFY(locked_status.contains(QStringLiteral("BitLocker-locked"), Qt::CaseInsensitive));
    // The readable row is picked first, so its folders are listed and no unlock is offered.
    QCOMPARE(page->profileTable()->rowCount(), 4);
    QVERIFY(!page->unlockOffered());

    // Pick the locked one: it lists nothing, offers the unlock, and refuses to be Next'd.
    page->volumeTable()->selectRow(1);
    QCOMPARE(page->profileTable()->rowCount(), 0);
    QVERIFY(page->unlockOffered());
    QVERIFY(!page->isComplete());
    QVERIFY(!page->validatePage());
    QVERIFY(page->statusText().contains(QStringLiteral("recovery key"), Qt::CaseInsensitive));
    QCOMPARE(log.prepare_calls, 0);
}

void TestOfflineSourceWizard::unlockSendsTheKeyToTheUnlockSeamOnly() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool) { return makeDiscovery({makeLockedVolume()}); },
         [&](const QString& volume_name, bool) {
             ++log.rescan_calls;
             OfflineVolumeSource after = makeReadableVolume();
             after.volume_name = volume_name;
             after.display_name = QStringLiteral("Encrypted Disk");
             after.root_path = QStringLiteral("F:\\");
             return after;
         },
         [&](const QString&) { return std::unexpected(QStringLiteral("must not run")); },
         [&](const QString& volume_name, const QString& typed_key) {
             ++log.unlock_calls;
             log.unlocked_volume = volume_name;
             log.unlocked_key = typed_key;
             return QStringLiteral("SAK-OFFLINE-UNLOCK-OK");
         },
         [](const QString&, QWidget*) { return std::optional<QString>(kTypedKey); },
         [] { return true; }});

    page->initializePage();
    page->volumeTable()->selectRow(0);
    // onUnlockClicked is a private slot; the meta-object is how a test presses a button.
    QMetaObject::invokeMethod(page, "onUnlockClicked");

    QCOMPARE(log.unlock_calls, 1);
    QCOMPARE(log.unlocked_volume, kGuidB);
    QCOMPARE(log.unlocked_key, kTypedKey);  // as typed: the core seam normalizes it, not the page
    QCOMPARE(log.rescan_calls, 1);
    QVERIFY(page->statusText().contains(QStringLiteral("unlocked"), Qt::CaseInsensitive));
    QVERIFY(page->isComplete());

    // The digits must appear nowhere the technician did not type them.
    QVERIFY(!page->statusText().contains(QStringLiteral("444444")));
    for (QLabel* label : page->findChildren<QLabel*>()) {
        QVERIFY2(!label->text().contains(QStringLiteral("444444")), qPrintable(label->text()));
    }
}

void TestOfflineSourceWizard::wrongKeyRefusesAndKeepsTheRowUnreadable() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool) { return makeDiscovery({makeLockedVolume()}); },
         [&](const QString&, bool) { return OfflineVolumeSource{}; },
         [&](const QString&) { return std::unexpected(QStringLiteral("must not run")); },
         [&](const QString&, const QString&) {
             ++log.unlock_calls;  // the seam must record the call itself, not just answer it
             return std::unexpected(QStringLiteral("Windows refused the recovery key."));
         },
         [](const QString&, QWidget*) { return std::optional<QString>(kTypedKey); },
         [] { return true; }});

    page->initializePage();
    QMetaObject::invokeMethod(page, "onUnlockClicked");
    QCOMPARE(log.unlock_calls, 1);
    QCOMPARE(log.rescan_calls, 0);  // nothing was re-read, because nothing was unlocked
    QVERIFY(page->statusText().contains(QStringLiteral("refused"), Qt::CaseInsensitive));
    // A refusal is where a key leaks: Windows' reason is shown beside the attempt, so the typed
    // digits must appear in NO label on this page.
    QVERIFY(!page->statusText().contains(QStringLiteral("444444")));
    for (QLabel* label : page->findChildren<QLabel*>()) {
        QVERIFY2(!label->text().contains(QStringLiteral("444444")),
                 qPrintable(QStringLiteral("[%1] ").arg(label->text()) + label->text()));
    }
    QVERIFY(!page->isComplete());
}

void TestOfflineSourceWizard::preparedSourceNamesTheMachineAndFeedsTheUserList() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting({[&](bool) { return makeDiscovery({makeReadableVolume()}); },
                              [&](const QString&, bool) { return OfflineVolumeSource{}; },
                              [&](const QString& wanted) {
                                  ++log.prepare_calls;
                                  log.prepared_volume = wanted;
                                  OfflineVolumePrep prep;
                                  prep.source = makeReadableVolume();
                                  prep.source.computer_name = QStringLiteral("OLD-PC-NAME");
                                  return prep;
                              },
                              [&](const QString&, const QString&) {
                                  return std::unexpected(QStringLiteral("must not run"));
                              },
                              [](const QString&, QWidget*) { return std::optional<QString>{}; },
                              [] { return true; }});

    QVERIFY(page->sourceComputerName().isEmpty());  // nothing prepared yet, so nothing to stamp
    page->initializePage();
    QVERIFY(page->validatePage());
    QCOMPARE(log.prepare_calls, 1);
    QCOMPARE(log.prepared_volume, kGuidA);
    QVERIFY(page->preparedSource().has_value());
    QCOMPARE(page->sourceComputerName(), QStringLiteral("OLD-PC-NAME"));
    // An offline rescue stamps where the files came FROM, not this PC.
    QCOMPARE(wizard.manifestSourceMachine(), QStringLiteral("OLD-PC-NAME"));

    const QVector<UserProfile> users = wizard.scannedUsers();
    QCOMPARE(users.size(), 2);  // the skipped folders are listed on screen, never carried forward
    QCOMPARE(users[0].username, QStringLiteral("bob"));
    QCOMPARE(users[0].sid, QStringLiteral("S-1-5-21-1-2-3-1000"));
    QVERIFY(users[0].is_selected);
    QCOMPARE(users[1].username, QStringLiteral("carol"));
    QCOMPARE(users[1].sid, QStringLiteral("S-1-5-21-1-2-3-1001"));
    QVERIFY(users[1].is_selected);
}

void TestOfflineSourceWizard::goingBackReleasesTheMount() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());

    int unmount_attempts = 0;
    OfflineProfileSource::ElevatedScriptRunner mount_runner =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        if (script.contains(QStringLiteral("/p"))) {
            ++unmount_attempts;
            return QStringLiteral("SAK-OFFLINE-UNMOUNT-OK");
        }
        return QStringLiteral("SAK-OFFLINE-MOUNT-OK");
    };

    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool) { return makeDiscovery({makeReadableVolume()}); },
         [&](const QString&, bool) { return OfflineVolumeSource{}; },
         [&](const QString& wanted) -> std::expected<OfflineVolumePrep, QString> {
             ++log.prepare_calls;
             OfflineVolumePrep prep;
             prep.source = makeReadableVolume();
             prep.source.root_path = QStringLiteral("mounted");  // a mounted spelling
             auto mount = OfflineVolumeMount::acquire(wanted, mount_runner);
             if (!mount) {
                 return std::unexpected(mount.error());
             }
             prep.mount.emplace(std::move(*mount));  // emplace: the mount is move-only by design
             return prep;
         },
         [&](const QString&, const QString&) {
             return std::unexpected(QStringLiteral("must not run"));
         },
         [](const QString&, QWidget*) { return std::optional<QString>{}; },
         [] { return true; }});

    page->initializePage();
    QVERIFY(page->validatePage());
    QCOMPARE(log.prepare_calls, 1);
    QCOMPARE(unmount_attempts, 0);  // still mounted while the run needs it

    page->cleanupPage();            // what Back does to the page being left
    QCOMPARE(unmount_attempts, 1);
    QVERIFY(!page->preparedSource().has_value());
    QVERIFY(wizard.scannedUsers().isEmpty());
}

void TestOfflineSourceWizard::repeatedNextTakesThePreviousMountDownFirst() {
    // Pressing Next twice (the second time after a later page's refusal) replaces the prepared
    // source. The mount the FIRST press created must come down before the second is taken: an
    // abandoned offline mount is damage on the technician's PC, not a leftover to ignore.
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    QVERIFY(sak::app_paths::establishSessionTempDirectory().has_value());

    int unmount_attempts = 0;
    OfflineProfileSource::ElevatedScriptRunner mount_runner =
        [&](const QString& script, const QString&) -> std::expected<QString, QString> {
        if (script.contains(QStringLiteral("/p"))) {
            ++unmount_attempts;
            return QStringLiteral("SAK-OFFLINE-UNMOUNT-OK");
        }
        return QStringLiteral("SAK-OFFLINE-MOUNT-OK");
    };

    SeamLog log;
    page->setSeamsForTesting(
        {[&](bool) { return makeDiscovery({makeReadableVolume()}); },
         [&](const QString&, bool) { return OfflineVolumeSource{}; },
         [&](const QString& wanted) -> std::expected<OfflineVolumePrep, QString> {
             ++log.prepare_calls;
             OfflineVolumePrep prep;
             prep.source = makeReadableVolume();
             prep.source.root_path = QStringLiteral("mounted");
             auto mount = OfflineVolumeMount::acquire(wanted, mount_runner);
             if (!mount) {
                 return std::unexpected(mount.error());
             }
             prep.mount.emplace(std::move(*mount));
             return prep;
         },
         [&](const QString&, const QString&) {
             return std::unexpected(QStringLiteral("must not run"));
         },
         [](const QString&, QWidget*) { return std::optional<QString>{}; },
         [] { return true; }});

    page->initializePage();
    QVERIFY(page->validatePage());
    QVERIFY(page->validatePage());  // a second Next after a later refusal
    QCOMPARE(log.prepare_calls, 2);
    QCOMPARE(unmount_attempts, 1);  // the first mount came down; the second is still held
}

void TestOfflineSourceWizard::skippedFolderCannotBeSelectedAndNeverReachesTheBackup() {
    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    SeamLog log;
    page->setSeamsForTesting({[&](bool) { return makeDiscovery({makeReadableVolume()}); },
                              [&](const QString&, bool) { return OfflineVolumeSource{}; },
                              [&](const QString&) {
                                  OfflineVolumePrep prep;
                                  prep.source = makeReadableVolume();
                                  return prep;
                              },
                              [&](const QString&, const QString&) {
                                  return std::unexpected(QStringLiteral("must not run"));
                              },
                              [](const QString&, QWidget*) { return std::optional<QString>{}; },
                              [] { return true; }});

    page->initializePage();
    QCOMPARE(page->profileTable()->rowCount(), 4);
    QCOMPARE(page->rescuableFolders(),
             QStringList({QStringLiteral("bob"), QStringLiteral("carol")}));

    // A folder that cannot go in carries no checkable box: unticking it is not the decision,
    // it cannot go in -- and the reason stands in its own column.
    for (int skipped_row : {2, 3}) {
        auto* skipped_check = page->profileTable()->item(skipped_row, 0);
        QVERIFY(!skipped_check->flags().testFlag(Qt::ItemIsUserCheckable));
        QCOMPARE(skipped_check->checkState(), Qt::Unchecked);
        QVERIFY(!page->profileTable()->item(skipped_row, 4)->text().isEmpty());
    }
    QVERIFY(page->profileTable()->item(2, 4)->text().contains(QStringLiteral("Built-in"),
                                                              Qt::CaseInsensitive));
    QVERIFY(page->profileTable()->item(3, 4)->text().contains(QStringLiteral("local accounts"),
                                                              Qt::CaseInsensitive));

    // Untick carol: only bob remains, and that is what the wizard carries forward.
    auto* carol_check = page->profileTable()->item(1, 0);
    carol_check->setCheckState(Qt::Unchecked);
    QVERIFY(page->isComplete());
    QVERIFY(page->validatePage());
    QCOMPARE(wizard.scannedUsers().size(), 2);  // both rescuable folders listed, honest selection
    int selected = 0;
    for (const UserProfile& user : wizard.scannedUsers()) {
        if (user.is_selected) {
            ++selected;
            QCOMPARE(user.username, QStringLiteral("bob"));
        }
    }
    QCOMPARE(selected, 1);
}

void TestOfflineSourceWizard::recoveryKeyDialogRefusesAWrongShapeWithoutClosing() {
    QVERIFY(BitLockerRecoveryKeyDialog::keyProblem(kTypedKey).isEmpty());
    QVERIFY(BitLockerRecoveryKeyDialog::keyProblem(QStringLiteral("00000-111111-222222-333333-"
                                                                  "444444-555555-666666-777777"))
                .contains(QStringLiteral("48")));
    QVERIFY(!BitLockerRecoveryKeyDialog::keyProblem(QStringLiteral("not a key")).isEmpty());

    BitLockerRecoveryKeyDialog dialog(QStringLiteral("Encrypted Disk"));
    dialog.findChild<QLineEdit*>()->setText(QStringLiteral("12345"));
    QMetaObject::invokeMethod(&dialog, "onAccept");
    QCOMPARE(dialog.result(), 0);  // refused on the spot: a bad key never leaves the window
    QCOMPARE(dialog.recoveryKey(), QString());
    auto* error = dialog.findChild<QLabel*>(QStringLiteral("bitlockerKeyError"));
    QVERIFY(error != nullptr);
    QVERIFY(error->text().contains(QStringLiteral("48")));
    // The reason must not repeat the digits that were typed.
    QVERIFY(!error->text().contains(QStringLiteral("12345")));

    dialog.findChild<QLineEdit*>()->setText(kTypedKey);
    QMetaObject::invokeMethod(&dialog, "onAccept");
    QCOMPARE(dialog.result(), QDialog::Accepted);
    QCOMPARE(dialog.recoveryKey(), kTypedKey);
}

void TestOfflineSourceWizard::manifestStampsTheOfflineMachineName() {
    // A this-PC run stamps this PC; an offline run must stamp the machine the files came FROM,
    // or the restore welcome page would call a dead laptop this machine.
    UserProfileBackupWizard this_pc;
    QCOMPARE(this_pc.manifestSourceMachine(), QSysInfo::machineHostName());

    UserProfileBackupWizard wizard;
    auto* page = offlinePage(wizard);
    page->setSeamsForTesting({[&](bool) { return makeDiscovery({makeReadableVolume()}); },
                              [&](const QString&, bool) { return OfflineVolumeSource{}; },
                              [&](const QString&) {
                                  OfflineVolumePrep prep;
                                  prep.source = makeReadableVolume();
                                  return prep;
                              },
                              [&](const QString&, const QString&) {
                                  return std::unexpected(QStringLiteral("must not run"));
                              },
                              [](const QString&, QWidget*) { return std::optional<QString>{}; },
                              [] { return true; }});
    page->initializePage();
    QVERIFY(page->validatePage());
    QCOMPARE(wizard.offlineSourceName(), QStringLiteral("OLD-PC-NAME"));
    QCOMPARE(wizard.manifestSourceMachine(), QStringLiteral("OLD-PC-NAME"));
}

QTEST_MAIN(TestOfflineSourceWizard)
#include "test_user_profile_backup_offline_source.moc"
