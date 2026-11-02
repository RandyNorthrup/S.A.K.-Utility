// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_data_sources.cpp
/// @brief What the Application Data pages offer: the list, the sizes, and which programs hold it.

#include "sak/app_data_sources.h"
#include "sak/user_profile_backup_worker.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

namespace {

bool writeFile(const QString& path, qsizetype size) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(QByteArray(size, 'x')) == size;
}

const sak::AppDataSourceInfo* findPath(const QVector<sak::AppDataSourceInfo>& sources,
                                       const QString& relative_path) {
    const auto it = std::ranges::find_if(sources, [&](const sak::AppDataSourceInfo& source) {
        return source.relative_path == relative_path;
    });
    return it != sources.end() ? &(*it) : nullptr;
}

sak::UserProfile selectedUser(const QString& name, const QString& profile_path) {
    sak::UserProfile user;
    user.username = name;
    user.profile_path = profile_path;
    user.is_selected = true;
    return user;
}

const QString kChrome = QStringLiteral("AppData/Local/Google/Chrome/User Data");

}  // namespace

class AppDataSourcesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void listIncludesWhatTheWizardPromises();
    void everySourceIsAPathTheWorkerAccepts();
    void secretsAreOptIn();
    void runningProgramsNameTheirSources();
    void scanAddsUpSizesAndLeavesCachesOut();
    void scanSkipsUsersNotSelected();
    void applicationDataRulesKeepWhatDocumentRulesSkip();
    void storedSourcesAreTheOnesWithDataBehindThem();
};

// The first wizard page promises browser bookmarks and email signatures. Bookmarks live in the
// browser profiles; Outlook keeps signatures in their own folder, which the list did not have.
void AppDataSourcesTests::listIncludesWhatTheWizardPromises() {
    const auto sources = sak::commonAppDataSources();
    QVERIFY(findPath(sources, kChrome) != nullptr);
    QVERIFY(findPath(sources, QStringLiteral("AppData/Roaming/Microsoft/Signatures")) != nullptr);
    // profiles.ini sits beside Profiles and names which profile to open.
    QVERIFY(findPath(sources, QStringLiteral("AppData/Roaming/Mozilla/Firefox")) != nullptr);
    QVERIFY(findPath(sources, QStringLiteral("AppData/Roaming/Thunderbird")) != nullptr);
}

void AppDataSourcesTests::everySourceIsAPathTheWorkerAccepts() {
    for (const auto& source : sak::commonAppDataSources()) {
        QVERIFY2(sak::UserProfileBackupWorker::isSafeRelativePath(source.relative_path),
                 qPrintable(source.relative_path));
    }
}

void AppDataSourcesTests::secretsAreOptIn() {
    const auto sources = sak::commonAppDataSources();
    const auto* ssh = findPath(sources, QStringLiteral(".ssh"));
    QVERIFY(ssh != nullptr);
    QVERIFY(!ssh->selected);
    const auto* outlook_data = findPath(sources, QStringLiteral("AppData/Local/Microsoft/Outlook"));
    QVERIFY(outlook_data != nullptr);
    QVERIFY(!outlook_data->selected);
    QVERIFY(findPath(sources, kChrome)->selected);
    QVERIFY(findPath(sources, QStringLiteral("AppData/Roaming/Microsoft/Signatures"))->selected);
}

void AppDataSourcesTests::runningProgramsNameTheirSources() {
    auto sources = sak::commonAppDataSources();
    const QStringList running = {QStringLiteral("explorer.exe"), QStringLiteral("CHROME.EXE")};
    const QStringList held = sak::appDataSourcesHeldOpen(sources, running);
    QCOMPARE(held, QStringList({QStringLiteral("Chrome Profiles")}));

    // Outlook holds two sources, and the new Outlook runs under a different name.
    for (auto& source : sources) {
        source.selected = true;
    }
    const QStringList outlook = sak::appDataSourcesHeldOpen(sources, {QStringLiteral("olk.exe")});
    QCOMPARE(outlook,
             QStringList({QStringLiteral("Outlook Data"), QStringLiteral("Outlook Signatures")}));

    // A source that is not selected does not need its program closed.
    for (auto& source : sources) {
        source.selected = false;
    }
    QVERIFY(sak::appDataSourcesHeldOpen(sources, running).isEmpty());
}

void AppDataSourcesTests::scanAddsUpSizesAndLeavesCachesOut() {
    QTemporaryDir alice;
    QTemporaryDir bob;
    QVERIFY(alice.isValid() && bob.isValid());
    QVERIFY(writeFile(alice.filePath(kChrome + QStringLiteral("/Default/Bookmarks")), 1000));
    QVERIFY(writeFile(alice.filePath(kChrome + QStringLiteral("/Default/Cache/data_0")), 500'000));
    QVERIFY(writeFile(bob.filePath(kChrome + QStringLiteral("/Default/Bookmarks")), 234));
    QVERIFY(writeFile(bob.filePath(QStringLiteral(".gitconfig")), 40));

    const auto found = sak::scanAppDataSources({selectedUser(QStringLiteral("Alice"), alice.path()),
                                                selectedUser(QStringLiteral("Bob"), bob.path())},
                                               sak::SmartFilter{},
                                               nullptr);
    QCOMPARE(found.size(), 2);  // one row per source, not one per user

    const auto* chrome = findPath(found, kChrome);
    QVERIFY(chrome != nullptr);
    QVERIFY(chrome->exists);
    QCOMPARE(chrome->size_bytes, qint64(1234));

    const auto* git = findPath(found, QStringLiteral(".gitconfig"));
    QVERIFY(git != nullptr);
    QCOMPARE(git->size_bytes, qint64(40));
}

void AppDataSourcesTests::scanSkipsUsersNotSelected() {
    QTemporaryDir alice;
    QVERIFY(alice.isValid());
    QVERIFY(writeFile(alice.filePath(kChrome + QStringLiteral("/Default/Bookmarks")), 1000));

    sak::UserProfile user = selectedUser(QStringLiteral("Alice"), alice.path());
    user.is_selected = false;
    QVERIFY(sak::scanAppDataSources({user}, sak::SmartFilter{}, nullptr).isEmpty());
}

void AppDataSourcesTests::applicationDataRulesKeepWhatDocumentRulesSkip() {
    QTemporaryDir profile;
    QVERIFY(profile.isValid());
    const QString note =
        profile.filePath(QStringLiteral("AppData/Local/Packages/Sticky/LocalState/plum.sqlite"));
    const QString cache = profile.filePath(QStringLiteral("AppData/Local/App/Cache/data_0"));
    QVERIFY(writeFile(note, 10));
    QVERIFY(writeFile(cache, 10));

    const sak::SmartFilter rules;
    const sak::SmartFileFilter for_documents(rules);
    const sak::SmartFileFilter for_app_data(rules.forApplicationData());
    QVERIFY(for_documents.shouldExcludeFile(QFileInfo(note), profile.path()));
    QVERIFY(!for_app_data.shouldExcludeFile(QFileInfo(note), profile.path()));
    QVERIFY(for_app_data.shouldExcludeFile(QFileInfo(cache), profile.path()));
}

void AppDataSourcesTests::storedSourcesAreTheOnesWithDataBehindThem() {
    sak::AppDataSourceInfo chrome;
    chrome.name = QStringLiteral("Chrome Profiles");
    chrome.relative_path = kChrome;
    chrome.size_bytes = 999;  // what the page estimated; the stored size wins
    sak::AppDataSourceInfo notes;
    notes.name = QStringLiteral("Notepad++ Settings");
    notes.relative_path = QStringLiteral("AppData/Roaming/Notepad++");
    notes.size_bytes = 7;
    sak::AppDataSourceInfo steam;
    steam.name = QStringLiteral("Steam Config");
    steam.relative_path = QStringLiteral("AppData/Local/Steam");

    sak::BackupManifest manifest;
    manifest.app_data_sources = {chrome, notes, steam};

    sak::FolderSelection stored_chrome;
    stored_chrome.type = sak::FolderType::ApplicationData;
    stored_chrome.relative_path = kChrome;
    stored_chrome.size_bytes = 100;
    sak::FolderSelection roaming;  // the old workaround: the whole Roaming folder, ticked
    roaming.type = sak::FolderType::AppData_Roaming;
    roaming.relative_path = QStringLiteral("AppData\\Roaming");
    sak::BackupUserData alice;
    alice.backed_up_folders = {stored_chrome, roaming};
    sak::BackupUserData bob;
    sak::FolderSelection bobs_chrome = stored_chrome;
    bobs_chrome.size_bytes = 23;
    bob.backed_up_folders = {bobs_chrome};
    manifest.users = {alice, bob};

    const auto stored = sak::appDataStoredInBackup(manifest);
    QCOMPARE(stored.size(), 2);
    QCOMPARE(findPath(stored, kChrome)->size_bytes, qint64(123));
    QCOMPARE(findPath(stored, notes.relative_path)->size_bytes, qint64(7));
    QVERIFY(findPath(stored, steam.relative_path) == nullptr);

    // What an older version wrote: every choice recorded, nothing copied.
    sak::BackupManifest older;
    older.app_data_sources = {chrome, steam};
    sak::FolderSelection documents;
    documents.relative_path = QStringLiteral("Documents");
    sak::BackupUserData user;
    user.backed_up_folders = {documents};
    older.users = {user};
    QVERIFY(sak::appDataStoredInBackup(older).isEmpty());
}

QTEST_GUILESS_MAIN(AppDataSourcesTests)
#include "test_app_data_sources.moc"
