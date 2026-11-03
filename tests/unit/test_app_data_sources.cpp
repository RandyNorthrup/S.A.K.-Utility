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

const sak::AppDataSourceInfo* findName(const QVector<sak::AppDataSourceInfo>& sources,
                                       const QString& name) {
    const auto it = std::ranges::find_if(sources, [&](const sak::AppDataSourceInfo& source) {
        return source.name == name;
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
const QString kOutlook = QStringLiteral("AppData/Local/Microsoft/Outlook");

}  // namespace

class AppDataSourcesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void listIncludesWhatTheWizardPromises();
    void everySourceIsAPathTheWorkerAccepts();
    void secretsAreOptIn();
    void outlookPstIsTickedAndItsCacheIsNot();
    void outlookRowsSplitTheFolderBetweenThem();
    void outlookPstRowAppearsOnlyWhereThereIsAPst();
    void splitRowsAreToldApartInABackup();
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
    QVERIFY(findPath(sources, kChrome)->selected);
    QVERIFY(findPath(sources, QStringLiteral("AppData/Roaming/Microsoft/Signatures"))->selected);
}

// A technician ticking through a migration must not leave a PST behind: in this folder it may be
// the only copy of a customer's mail. An OST beside it is what the mail server still has.
void AppDataSourcesTests::outlookPstIsTickedAndItsCacheIsNot() {
    const auto sources = sak::commonAppDataSources();
    const auto* pst = findName(sources, QStringLiteral("Outlook Archives (PST)"));
    const auto* cache = findName(sources, QStringLiteral("Outlook Cache (OST)"));
    QVERIFY(pst != nullptr && cache != nullptr);
    QVERIFY(pst->selected);
    QVERIFY(!cache->selected);
    QCOMPARE(pst->relative_path, kOutlook);
    QCOMPARE(cache->relative_path, kOutlook);
}

void AppDataSourcesTests::outlookRowsSplitTheFolderBetweenThem() {
    const auto sources = sak::commonAppDataSources();
    const auto* pst = findName(sources, QStringLiteral("Outlook Archives (PST)"));
    const auto* cache = findName(sources, QStringLiteral("Outlook Cache (OST)"));
    QVERIFY(pst != nullptr && cache != nullptr);
    const auto takes = [](const sak::AppDataSourceInfo& row, const QString& name) {
        return sak::appDataPatternsTakeFile(name, row.include_patterns, row.exclude_patterns);
    };
    for (const QString& name : {QStringLiteral("archive.pst"), QStringLiteral("OLD.PST")}) {
        QVERIFY2(takes(*pst, name), qPrintable(name));
        QVERIFY2(!takes(*cache, name), qPrintable(name));
    }
    for (const QString& name : {QStringLiteral("me@example.com.ost"),
                                QStringLiteral("group.nst"),
                                QStringLiteral("profile.xml"),
                                QStringLiteral("archive.pst.tmp")}) {
        QVERIFY2(!takes(*pst, name), qPrintable(name));
        QVERIFY2(takes(*cache, name), qPrintable(name));
    }

    // Each row's size is its own files', so the choice shows what it costs.
    QTemporaryDir alice;
    QVERIFY(alice.isValid());
    QVERIFY(writeFile(alice.filePath(kOutlook + QStringLiteral("/archive.pst")), 100));
    QVERIFY(writeFile(alice.filePath(kOutlook + QStringLiteral("/older/archive2.pst")), 20));
    QVERIFY(writeFile(alice.filePath(kOutlook + QStringLiteral("/me.ost")), 5000));
    QVERIFY(writeFile(alice.filePath(kOutlook + QStringLiteral("/profile.xml")), 7));
    const auto found = sak::scanAppDataSources(
        {selectedUser(QStringLiteral("Alice"), alice.path())}, sak::SmartFilter{}, nullptr);
    QCOMPARE(findName(found, QStringLiteral("Outlook Archives (PST)"))->size_bytes, qint64(120));
    QCOMPARE(findName(found, QStringLiteral("Outlook Cache (OST)"))->size_bytes, qint64(5007));
}

void AppDataSourcesTests::outlookPstRowAppearsOnlyWhereThereIsAPst() {
    QTemporaryDir alice;
    QVERIFY(alice.isValid());
    QVERIFY(writeFile(alice.filePath(kOutlook + QStringLiteral("/me.ost")), 5000));
    const auto found = sak::scanAppDataSources(
        {selectedUser(QStringLiteral("Alice"), alice.path())}, sak::SmartFilter{}, nullptr);
    QVERIFY(findName(found, QStringLiteral("Outlook Archives (PST)")) == nullptr);
    QVERIFY(findName(found, QStringLiteral("Outlook Cache (OST)")) != nullptr);
}

// Two rows on one path: each is stored only if its own copy is there, with its own size. A row's
// copy must not read as the other row's, or the restore page offers data the backup does not hold.
void AppDataSourcesTests::splitRowsAreToldApartInABackup() {
    const auto sources = sak::commonAppDataSources();
    const auto* pst = findName(sources, QStringLiteral("Outlook Archives (PST)"));
    const auto* cache = findName(sources, QStringLiteral("Outlook Cache (OST)"));
    QVERIFY(pst != nullptr && cache != nullptr);

    sak::BackupManifest manifest;
    sak::AppDataSourceInfo whole_folder;
    whole_folder.name = QStringLiteral("Outlook Data");
    whole_folder.relative_path = kOutlook;
    manifest.app_data_sources = {*pst, *cache, whole_folder};
    sak::FolderSelection stored;
    stored.type = sak::FolderType::ApplicationData;
    stored.relative_path = kOutlook;
    stored.include_patterns = pst->include_patterns;
    stored.exclude_patterns = pst->exclude_patterns;
    stored.size_bytes = 120;
    sak::BackupUserData user;
    user.backed_up_folders = {stored};
    manifest.users = {user};

    const auto listed = sak::appDataStoredInBackup(manifest);
    QCOMPARE(listed.size(), 1);
    QCOMPARE(listed.first().name, pst->name);
    QCOMPARE(listed.first().size_bytes, qint64(120));

    // The patterns survive the trip through the manifest, and a row with none writes none, which
    // keeps the checksum of a manifest from before they existed.
    const auto round_trip = sak::AppDataSourceInfo::fromJson(pst->toJson());
    QCOMPARE(round_trip.include_patterns, pst->include_patterns);
    QCOMPARE(round_trip.exclude_patterns, pst->exclude_patterns);
    const auto cache_round_trip = sak::AppDataSourceInfo::fromJson(cache->toJson());
    QCOMPARE(cache_round_trip.exclude_patterns, cache->exclude_patterns);
    const auto* chrome = findPath(sources, kChrome);
    QVERIFY(!chrome->toJson().contains(QStringLiteral("include_patterns")));
    QVERIFY(!chrome->toJson().contains(QStringLiteral("exclude_patterns")));
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
             QStringList({QStringLiteral("Outlook Archives (PST)"),
                          QStringLiteral("Outlook Cache (OST)"),
                          QStringLiteral("Outlook Signatures")}));

    // Two rows share a folder, and so a program, which is still named once.
    QCOMPARE(sak::appDataSourceProcessNames(kOutlook).size(), 2);

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
