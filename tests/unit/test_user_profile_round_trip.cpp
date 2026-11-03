// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_round_trip.cpp
/// @brief A real backup, written by the backup worker and read back from its manifest.json,
///        restored by the restore worker. Each worker is tested on its own elsewhere; what only
///        this file can catch is the two of them disagreeing about what a backup is.

#include "sak/app_data_sources.h"
#include "sak/backup_file_codec.h"
#include "sak/user_profile_backup_worker.h"
#include "sak/user_profile_restore_worker.h"
#include "sak/user_profile_types.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <windows.h>

using sak::BackupManifest;
using sak::ConflictResolution;
using sak::MergeMode;
using sak::PermissionMode;
using sak::UserProfileBackupWorker;
using sak::UserProfileRestoreWorker;

namespace {

constexpr int kWorkerTimeoutMs = 30'000;
constexpr int kContentRepeats = 200;
const QString kUser = QStringLiteral("sakroundtripuser");
const QString kPassword = QStringLiteral("round_trip_password");

/// Compresses well, so a compressed run really does store something different from the source.
QByteArray documentContent() {
    return QByteArrayLiteral("round trip document line, repeated so it compresses\n")
        .repeated(kContentRepeats);
}

bool writeFile(const QString& path, const QByteArray& content) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}

QByteArray readFile(const QString& path) {
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

bool setModified(const QString& path, QDate date) {
    QFile file(path);
    return file.open(QIODevice::ReadWrite) &&
           file.setFileTime(QDateTime(date, QTime(12, 0)), QFileDevice::FileModificationTime);
}

sak::FolderSelection documentsFolder() {
    sak::FolderSelection folder;
    folder.type = sak::FolderType::Documents;
    folder.display_name = QStringLiteral("Documents");
    folder.relative_path = QStringLiteral("Documents");
    folder.selected = true;
    return folder;
}

sak::UserProfile sourceProfile(const QString& profile_path) {
    sak::UserProfile user;
    user.username = kUser;
    user.profile_path = profile_path;
    user.is_selected = true;
    user.folder_selections = {documentsFolder()};
    return user;
}

sak::UserMapping mappingTo(MergeMode mode) {
    sak::UserMapping mapping;
    mapping.source_username = kUser;
    mapping.destination_username = kUser;
    mapping.mode = mode;
    mapping.selected = true;
    return mapping;
}

struct RunResult {
    bool completed{false};
    bool success{false};
    QString message;
    QStringList warnings;
    QStringList notes;
};

/// Run a backup with @p options, then read the manifest back from disk, as a later restore does.
RunResult runBackup(const sak::UserProfile& user,
                    const QString& destination,
                    const UserProfileBackupWorker::BackupOptions& options,
                    const QVector<sak::AppDataSourceInfo>& app_data = {},
                    const sak::SmartFilter& filter = sak::SmartFilter{}) {
    if (!QDir().mkpath(destination)) {
        return {};
    }
    UserProfileBackupWorker worker;
    QSignalSpy complete(&worker, &UserProfileBackupWorker::backupComplete);
    QSignalSpy log(&worker, &UserProfileBackupWorker::logMessage);
    BackupManifest manifest;
    manifest.app_data_sources = app_data;
    worker.startBackup(manifest, {user}, destination, filter, options);
    const bool finished = QTest::qWaitFor([&complete]() { return complete.count() == 1; },
                                          kWorkerTimeoutMs);
    worker.wait(kWorkerTimeoutMs);
    RunResult result;
    result.completed = finished && complete.count() == 1;
    if (result.completed) {
        result.success = complete.first().at(0).toBool();
        result.message = complete.first().at(1).toString();
    }
    for (const auto& entry : log) {
        (entry.at(1).toBool() ? result.warnings : result.notes).append(entry.at(0).toString());
    }
    return result;
}

RunResult runRestore(const QString& backup_path,
                     const QVector<sak::UserMapping>& mappings,
                     const UserProfileRestoreWorker::RestoreConfig& config,
                     const UserProfileRestoreWorker::RestoreSelections& selections = {}) {
    const BackupManifest manifest =
        BackupManifest::loadFromFile(backup_path + QStringLiteral("/manifest.json"));
    UserProfileRestoreWorker worker;
    QSignalSpy complete(&worker, &UserProfileRestoreWorker::restoreComplete);
    QSignalSpy log(&worker, &UserProfileRestoreWorker::logMessage);
    worker.startRestore(backup_path, manifest, mappings, config, selections);
    const bool finished = QTest::qWaitFor([&complete]() { return complete.count() == 1; },
                                          kWorkerTimeoutMs);
    worker.wait(kWorkerTimeoutMs);
    RunResult result;
    result.completed = finished && complete.count() == 1;
    if (result.completed) {
        result.success = complete.first().at(0).toBool();
        result.message = complete.first().at(1).toString();
    }
    for (const auto& entry : log) {
        if (entry.at(1).toBool()) {
            result.warnings.append(entry.at(0).toString());
        }
    }
    return result;
}

UserProfileBackupWorker::BackupOptions backupOptions(bool compress, bool encrypt) {
    UserProfileBackupWorker::BackupOptions options;
    options.permission_mode = PermissionMode::PreserveOriginal;
    options.compress = compress;
    options.encrypt = encrypt;
    options.password = encrypt ? kPassword : QString();
    return options;
}

UserProfileRestoreWorker::RestoreConfig verifiedRestore(ConflictResolution conflict, bool encrypt) {
    UserProfileRestoreWorker::RestoreConfig config;
    config.conflict_mode = conflict;
    config.perm_mode = PermissionMode::PreserveOriginal;
    config.verify = true;
    config.password = encrypt ? kPassword : QString();
    return config;
}

sak::AppDataSourceInfo appDataSource(const QString& relative_path) {
    sak::AppDataSourceInfo source;
    source.name = relative_path;
    source.category = QStringLiteral("Test");
    source.relative_path = relative_path;
    source.selected = true;
    return source;
}

const QString kChromePath = QStringLiteral("AppData/Local/Google/Chrome/User Data");
const QString kSignaturesPath = QStringLiteral("AppData/Roaming/Microsoft/Signatures");
const QString kStickyNotesPath = QStringLiteral(
    "AppData/Local/Packages/Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe/LocalState");
const QString kExtensionsPath = QStringLiteral(".vscode/extensions");
const QString kGitConfigPath = QStringLiteral(".gitconfig");

/// A profile shaped like a real one, with the files a person moves a browser profile for and the
/// ones a backup should leave behind.
bool seedAppData(const QString& profile) {
    const QDir root(profile);
    return writeFile(root.filePath(kChromePath + QStringLiteral("/Default/Bookmarks")),
                     QByteArrayLiteral("my bookmarks")) &&
           writeFile(root.filePath(kChromePath + QStringLiteral("/Local State")),
                     QByteArrayLiteral("{}")) &&
           writeFile(root.filePath(kChromePath + QStringLiteral("/Default/Cache/Cache_Data/f_1")),
                     QByteArray(4096, 'c')) &&
           writeFile(root.filePath(kSignaturesPath + QStringLiteral("/Work.htm")),
                     QByteArrayLiteral("<p>Regards</p>")) &&
           writeFile(root.filePath(kStickyNotesPath + QStringLiteral("/plum.sqlite")),
                     QByteArrayLiteral("a sticky note")) &&
           writeFile(root.filePath(kExtensionsPath +
                                   QStringLiteral("/some.ext/node_modules/dep/index.js")),
                     QByteArrayLiteral("module.exports = 1;")) &&
           writeFile(root.filePath(kGitConfigPath), QByteArrayLiteral("[user]\nname = Pat\n"));
}

const QString kOutlookPath = QStringLiteral("AppData/Local/Microsoft/Outlook");
constexpr int kArchiveBytes = 4096;
constexpr int kOlderPstBytes = 1024;
constexpr int kPstBytes = kArchiveBytes + kOlderPstBytes;

bool seedOutlook(const QString& profile) {
    const QDir root(QDir(profile).filePath(kOutlookPath));
    return writeFile(root.filePath(QStringLiteral("archive.pst")),
                     QByteArray(kArchiveBytes, 'p')) &&
           writeFile(root.filePath(QStringLiteral("older/2019.PST")),
                     QByteArray(kOlderPstBytes, 'q')) &&
           writeFile(root.filePath(QStringLiteral("me@example.com.ost")), QByteArray(8192, 'o')) &&
           writeFile(root.filePath(QStringLiteral("profile.xml")), QByteArrayLiteral("<xml/>"));
}

/// The two Outlook rows as the Application Data page offers them, only the ones asked for.
QVector<sak::AppDataSourceInfo> outlookRows(bool archives, bool cache) {
    QVector<sak::AppDataSourceInfo> rows;
    for (const sak::AppDataSourceInfo& source : sak::commonAppDataSources()) {
        if ((archives && source.name == QStringLiteral("Outlook Archives (PST)")) ||
            (cache && source.name == QStringLiteral("Outlook Cache (OST)"))) {
            rows.append(source);
            rows.last().selected = true;
        }
    }
    return rows;
}

QVector<sak::AppDataSourceInfo> seededSources() {
    return {appDataSource(kChromePath),
            appDataSource(kSignaturesPath),
            appDataSource(kStickyNotesPath),
            appDataSource(kExtensionsPath),
            appDataSource(kGitConfigPath)};
}

QString profileUnder(const QTemporaryDir& system_drive) {
    return system_drive.filePath(QStringLiteral("Users/") + kUser);
}

}  // namespace

class UserProfileRoundTripTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // Verification is on by default in the wizard, so every one of these is the default path.
    void verifiedRestoreRoundTrips_data();
    void verifiedRestoreRoundTrips();
    void verifiedOverwriteRoundTrips_data();
    void verifiedOverwriteRoundTrips();
    void wrongPasswordLeavesTheOriginalInPlace();
    void aCorruptedContainerIsRefused();
    void verificationComparesWithTheDecodedContent();

    // The conflict rules read a stored file's size and date, which a container changes.
    void keepLargerComparesTheOriginalSize_data();
    void keepLargerComparesTheOriginalSize();
    void keepNewerComparesTheOriginalDate_data();
    void keepNewerComparesTheOriginalDate();
    void restoredFileKeepsItsOriginalDate_data();
    void restoredFileKeepsItsOriginalDate();

    // A source chosen on the Application Data page is really copied and really comes back.
    void appDataRoundTrips_data();
    void appDataRoundTrips();
    void appDataAbsentForAUserIsNotAFailure();
    void appDataUntickedAtRestoreStaysOut();
    void appDataReplacesWhatIsThereAndKeepsIt();
    void appDataInsideASelectedFolderIsNotRestoredTwice();
    void appDataPastMaxPathRoundTrips();
    void lockedFileIsNamedAndTheRestIsCopied();

    // A technician ticking through a migration: a PST travels, the cache beside it does not.
    void outlookDefaultsCopyThePstAndLeaveTheCache();
    void outlookBothRowsRestoreEachFileOnce();
    void outlookRowUntickedAtRestoreKeepsTheOtherRow();
    void outlookLockedPstIsNamedAndTheRunSaysSo();
    void pstUnderDocumentsIsCoveredByTheUserFolders();

    // What the Select Folders page does must not look like damage to the backup.
    void untickedFolderStaysOutAndTheManifestStillVerifies();
    void manifestListsOnlyWhatWasCopied();

    // The wizard's own defaults, which every other case here avoids.
    void defaultPermissionsRoundTrip();

    // A person's name can be in any alphabet, and so can their files.
    void namesOutsideTheCodePageRoundTrip_data();
    void namesOutsideTheCodePageRoundTrip();

    // Controls on the backup wizard that used to change nothing.
    void verifyAfterBackupChecksEveryStoredFile_data();
    void verifyAfterBackupChecksEveryStoredFile();
    void verifyOffChecksNothing();
    void storedFileCheckCatchesADamagedCopy();
    void folderOverTheLimitIsWarnedAboutAndStillCopied();
    void excludeBoxesKeepOrDropTheirFiles_data();
    void excludeBoxesKeepOrDropTheirFiles();

private:
    QByteArray m_saved_system_drive;
};

void UserProfileRoundTripTests::initTestCase() {
    m_saved_system_drive = qgetenv("SystemDrive");
}

void UserProfileRoundTripTests::cleanupTestCase() {
    qputenv("SystemDrive", m_saved_system_drive);
}

void UserProfileRoundTripTests::verifiedRestoreRoundTrips_data() {
    QTest::addColumn<bool>("compress");
    QTest::addColumn<bool>("encrypt");
    QTest::newRow("plain copy") << false << false;
    QTest::newRow("compressed") << true << false;
    QTest::newRow("encrypted") << false << true;
    QTest::newRow("compressed and encrypted") << true << true;
}

void UserProfileRoundTripTests::verifiedRestoreRoundTrips() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);

    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QByteArray content = documentContent();
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), content));

    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(compress, encrypt));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    // The case this file exists for only arises when the stored file differs from the original.
    const QString stored = backup.filePath(kUser + QStringLiteral("/Documents/report.txt"));
    QCOMPARE(readFile(stored) == content, !compress && !encrypt);

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, encrypt));
    QVERIFY2(restored.completed, "the restore never finished");
    QVERIFY2(restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));

    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QCOMPARE(readFile(landed), content);
}

void UserProfileRoundTripTests::verifiedOverwriteRoundTrips_data() {
    verifiedRestoreRoundTrips_data();
}

// The damaging form of the same fault: a failed verify rolls the overwrite back, so every file the
// person asked to replace stays as it was while the wizard reports the restore failed.
void UserProfileRoundTripTests::verifiedOverwriteRoundTrips() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);

    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QByteArray content = documentContent();
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), content));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(compress, encrypt));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QVERIFY(writeFile(landed, QByteArrayLiteral("older, shorter")));

    const RunResult restored = runRestore(backup.path(),
                                          {mappingTo(MergeMode::MergeIntoDestination)},
                                          verifiedRestore(ConflictResolution::KeepLarger, encrypt));
    QVERIFY2(restored.completed, "the restore never finished");
    QVERIFY2(restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));
    QCOMPARE(readFile(landed), content);

    const QStringList leftovers = QDir(QFileInfo(landed).absolutePath())
                                      .entryList({QStringLiteral("report.txt.sak*")},
                                                 QDir::Files | QDir::Hidden);
    QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));
}

void UserProfileRoundTripTests::wrongPasswordLeavesTheOriginalInPlace() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), documentContent()));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(false, true));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QByteArray original = QByteArrayLiteral("older, shorter");
    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QVERIFY(writeFile(landed, original));

    auto config = verifiedRestore(ConflictResolution::KeepLarger, true);
    config.password = QStringLiteral("not the password");
    const RunResult restored =
        runRestore(backup.path(), {mappingTo(MergeMode::MergeIntoDestination)}, config);
    QVERIFY(restored.completed);
    QVERIFY2(!restored.success, "a wrong password must not read as a clean restore");
    QCOMPARE(readFile(landed), original);
}

// Proves verification still has teeth after the fix: damaging one stored file must stop the
// restore, not slip through because nothing compares against the stored bytes any more.
void UserProfileRoundTripTests::aCorruptedContainerIsRefused() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), documentContent()));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(true, false));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QString stored = backup.filePath(kUser + QStringLiteral("/Documents/report.txt"));
    QByteArray damaged = readFile(stored);
    QVERIFY(damaged.size() > 64);
    damaged[damaged.size() - 8] = static_cast<char>(damaged.at(damaged.size() - 8) ^ 0x5A);
    QVERIFY(writeFile(stored, damaged));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, false));
    QVERIFY(restored.completed);
    QVERIFY2(!restored.success, "a damaged stored file must not read as a clean restore");
    QVERIFY(!QFile::exists(system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"))));
}

// The rest of this file proves a good restore passes. This is what proves the check can fail: a
// restored file that differs from the decoded content, or from a verbatim source, is caught.
void UserProfileRoundTripTests::verificationComparesWithTheDecodedContent() {
    using Check = UserProfileRestoreWorker::RestoredFileCheck;
    QTemporaryDir work;
    QVERIFY(work.isValid());
    const QByteArray content = documentContent();
    const QByteArray digest = QCryptographicHash::hash(content, QCryptographicHash::Sha256);
    const QString stored = work.filePath(QStringLiteral("stored.bin"));
    const QString restored = work.filePath(QStringLiteral("restored.txt"));
    QVERIFY(writeFile(stored, QByteArrayLiteral("SAKBFC1 container bytes that differ")));

    QVERIFY(writeFile(restored, content));
    QCOMPARE(UserProfileRestoreWorker::verifyRestoredFile(stored, restored, digest),
             Check::Matches);

    QVERIFY(writeFile(restored, content + "damaged"));
    QCOMPARE(UserProfileRestoreWorker::verifyRestoredFile(stored, restored, digest),
             Check::Differs);

    QVERIFY(writeFile(stored, content));
    QCOMPARE(UserProfileRestoreWorker::verifyRestoredFile(stored, restored, QByteArray()),
             Check::Differs);
    QVERIFY(writeFile(restored, content));
    QCOMPARE(UserProfileRestoreWorker::verifyRestoredFile(stored, restored, QByteArray()),
             Check::Matches);

    QVERIFY(QFile::remove(restored));
    QCOMPARE(UserProfileRestoreWorker::verifyRestoredFile(stored, restored, digest),
             Check::Unreadable);
}

void UserProfileRoundTripTests::keepLargerComparesTheOriginalSize_data() {
    QTest::addColumn<bool>("compress");
    QTest::addColumn<bool>("encrypt");
    QTest::newRow("compressed") << true << false;
    QTest::newRow("compressed and encrypted") << true << true;
}

// The existing file is larger than the compressed entry and smaller than the original. Comparing
// the entry would keep the existing file; the rule says keep the larger, and that is the backup's.
void UserProfileRoundTripTests::keepLargerComparesTheOriginalSize() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QByteArray content = documentContent();
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), content));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(compress, encrypt));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QString stored = backup.filePath(kUser + QStringLiteral("/Documents/report.txt"));
    const qint64 existing_size = content.size() / 2;
    QVERIFY(QFileInfo(stored).size() < existing_size);

    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QVERIFY(writeFile(landed, QByteArray(existing_size, 'e')));

    const RunResult restored = runRestore(backup.path(),
                                          {mappingTo(MergeMode::MergeIntoDestination)},
                                          verifiedRestore(ConflictResolution::KeepLarger, encrypt));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
    QCOMPARE(readFile(landed), content);
}

void UserProfileRoundTripTests::keepNewerComparesTheOriginalDate_data() {
    keepLargerComparesTheOriginalSize_data();
    QTest::newRow("encrypted") << false << true;
}

// The original dates from 2020 and the existing file from 2023. A container written today must not
// count as the newer of the two.
void UserProfileRoundTripTests::keepNewerComparesTheOriginalDate() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QString original = profile.filePath(QStringLiteral("Documents/report.txt"));
    QVERIFY(writeFile(original, documentContent()));
    QVERIFY(setModified(original, QDate(2020, 1, 1)));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(compress, encrypt));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QByteArray existing = QByteArrayLiteral("newer than the backup");
    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QVERIFY(writeFile(landed, existing));
    QVERIFY(setModified(landed, QDate(2023, 1, 1)));

    const RunResult restored = runRestore(backup.path(),
                                          {mappingTo(MergeMode::MergeIntoDestination)},
                                          verifiedRestore(ConflictResolution::KeepNewer, encrypt));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
    QCOMPARE(readFile(landed), existing);
}

void UserProfileRoundTripTests::restoredFileKeepsItsOriginalDate_data() {
    verifiedRestoreRoundTrips_data();
}

void UserProfileRoundTripTests::restoredFileKeepsItsOriginalDate() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QString original = profile.filePath(QStringLiteral("Documents/report.txt"));
    QVERIFY(writeFile(original, documentContent()));
    QVERIFY(setModified(original, QDate(2020, 1, 1)));
    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(compress, encrypt));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, encrypt));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));

    const QString landed = system_drive.filePath(QStringLiteral("Users/") + kUser +
                                                 QStringLiteral("/Documents/report.txt"));
    QCOMPARE(QFileInfo(landed).lastModified().date(), QDate(2020, 1, 1));
}

void UserProfileRoundTripTests::appDataRoundTrips_data() {
    verifiedRestoreRoundTrips_data();
}

void UserProfileRoundTripTests::appDataRoundTrips() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(seedAppData(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();  // Nothing but the application data page's choices.
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(compress, encrypt), seededSources());
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));

    // The cache is what makes a browser profile enormous, and it is worth nothing on another PC.
    const QDir stored(backup.filePath(kUser));
    QVERIFY(QFile::exists(stored.filePath(kChromePath + QStringLiteral("/Default/Bookmarks"))));
    QVERIFY(!QFile::exists(stored.filePath(kChromePath + QStringLiteral("/Default/Cache"))));

    const BackupManifest manifest =
        BackupManifest::loadFromFile(backup.filePath(QStringLiteral("manifest.json")));
    QCOMPARE(manifest.users.size(), 1);
    QCOMPARE(manifest.users.first().backed_up_folders.size(), seededSources().size());
    for (const auto& folder : manifest.users.first().backed_up_folders) {
        QCOMPARE(folder.type, sak::FolderType::ApplicationData);
        QVERIFY2(folder.size_bytes > 0, qPrintable(folder.relative_path));
    }

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, encrypt));
    QVERIFY2(restored.completed && restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));

    const QDir landed(profileUnder(system_drive));
    QCOMPARE(readFile(landed.filePath(kChromePath + QStringLiteral("/Default/Bookmarks"))),
             QByteArrayLiteral("my bookmarks"));
    QCOMPARE(readFile(landed.filePath(kSignaturesPath + QStringLiteral("/Work.htm"))),
             QByteArrayLiteral("<p>Regards</p>"));
    // Under a folder called Packages, which the document folders' rules skip.
    QCOMPARE(readFile(landed.filePath(kStickyNotesPath + QStringLiteral("/plum.sqlite"))),
             QByteArrayLiteral("a sticky note"));
    // An extension will not run without its node_modules.
    QCOMPARE(readFile(landed.filePath(kExtensionsPath +
                                      QStringLiteral("/some.ext/node_modules/dep/index.js"))),
             QByteArrayLiteral("module.exports = 1;"));
    // One file rather than a folder, and it must come back as a file.
    QVERIFY(QFileInfo(landed.filePath(kGitConfigPath)).isFile());
    QCOMPARE(readFile(landed.filePath(kGitConfigPath)), QByteArrayLiteral("[user]\nname = Pat\n"));
}

void UserProfileRoundTripTests::appDataAbsentForAUserIsNotAFailure() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), documentContent()));

    const RunResult backed_up = runBackup(sourceProfile(profile.path()),
                                          backup.path(),
                                          backupOptions(false, false),
                                          {appDataSource(kChromePath)});
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const BackupManifest manifest =
        BackupManifest::loadFromFile(backup.filePath(QStringLiteral("manifest.json")));
    QCOMPARE(manifest.users.first().backed_up_folders.size(), 1);
    QCOMPARE(manifest.users.first().backed_up_folders.first().relative_path,
             QStringLiteral("Documents"));
}

void UserProfileRoundTripTests::appDataUntickedAtRestoreStaysOut() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(seedAppData(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), seededSources());
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    QVector<sak::AppDataSourceInfo> choices = seededSources();
    choices[0].selected = false;  // Chrome
    UserProfileRestoreWorker::RestoreSelections selections;
    selections.app_data_sources = choices;
    const RunResult restored = runRestore(backup.path(),
                                          {mappingTo(MergeMode::CreateNewUser)},
                                          verifiedRestore(ConflictResolution::SkipDuplicate, false),
                                          selections);
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));

    const QDir landed(profileUnder(system_drive));
    QVERIFY(!QFile::exists(landed.filePath(kChromePath)));
    QVERIFY(QFile::exists(landed.filePath(kSignaturesPath + QStringLiteral("/Work.htm"))));
}

// A renamed or skipped Bookmarks leaves the browser reading the old file, so the rule the person
// chose for their documents cannot apply here. Nothing is lost: the old file stays beside it.
void UserProfileRoundTripTests::appDataReplacesWhatIsThereAndKeepsIt() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(seedAppData(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), {appDataSource(kChromePath)});
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QString landed = QDir(profileUnder(system_drive))
                               .filePath(kChromePath + QStringLiteral("/Default/Bookmarks"));
    QVERIFY(writeFile(landed, QByteArrayLiteral("the new PC's bookmarks")));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::MergeIntoDestination)},
                   verifiedRestore(ConflictResolution::RenameWithSuffix, false));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
    QCOMPARE(readFile(landed), QByteArrayLiteral("my bookmarks"));
    QCOMPARE(readFile(landed + QStringLiteral(".sakbak")),
             QByteArrayLiteral("the new PC's bookmarks"));
    QVERIFY(
        !QFile::exists(QFileInfo(landed).absolutePath() + QStringLiteral("/Bookmarks_backup1")));
}

// Documents is selected by default and the PowerShell profile lives inside it. Copying or restoring
// it twice would refuse, or conflict with itself and leave a renamed second copy.
void UserProfileRoundTripTests::appDataInsideASelectedFolderIsNotRestoredTwice() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QString nested = QStringLiteral("Documents/PowerShell");
    QVERIFY(writeFile(profile.filePath(nested + QStringLiteral("/profile.ps1")),
                      QByteArrayLiteral("Set-Location ~")));
    const RunResult backed_up = runBackup(sourceProfile(profile.path()),
                                          backup.path(),
                                          backupOptions(false, false),
                                          {appDataSource(nested)});
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::RenameWithSuffix, false));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));

    const QDir landed(QDir(profileUnder(system_drive)).filePath(nested));
    QCOMPARE(landed.entryList(QDir::Files), QStringList({QStringLiteral("profile.ps1")}));
    QCOMPARE(readFile(landed.filePath(QStringLiteral("profile.ps1"))),
             QByteArrayLiteral("Set-Location ~"));
}

// Browser extensions nest deeply, and the paths grow again under a backup folder.
void UserProfileRoundTripTests::appDataPastMaxPathRoundTrips() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QString deep = kChromePath + QStringLiteral("/Default/Extensions/") +
                         QStringLiteral("nested-folder-name/").repeated(14) +
                         QStringLiteral("deep.json");
    QVERIFY(profile.filePath(deep).size() > 260);
    QVERIFY(writeFile(profile.filePath(deep), QByteArrayLiteral("deep")));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), {appDataSource(kChromePath)});
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, false));
    QVERIFY2(restored.completed && restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));
    QCOMPARE(readFile(QDir(profileUnder(system_drive)).filePath(deep)), QByteArrayLiteral("deep"));
}

// What a running browser does to its Cookies database: open it so that even a shared read is
// refused. The backup must say which file it could not copy, keep going, and not call it clean.
// What the Application Data page starts with on a PC holding these files.
void UserProfileRoundTripTests::outlookDefaultsCopyThePstAndLeaveTheCache() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(seedOutlook(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const QVector<sak::AppDataSourceInfo> ticked = outlookRows(true, false);
    QCOMPARE(ticked.size(), 1);
    const RunResult backed_up = runBackup(user, backup.path(), backupOptions(false, false), ticked);
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QDir stored(QDir(backup.path()).filePath(kUser + QLatin1Char('/') + kOutlookPath));
    QVERIFY(QFile::exists(stored.filePath(QStringLiteral("archive.pst"))));
    QVERIFY(QFile::exists(stored.filePath(QStringLiteral("older/2019.PST"))));
    QVERIFY(!QFile::exists(stored.filePath(QStringLiteral("me@example.com.ost"))));
    QVERIFY(!QFile::exists(stored.filePath(QStringLiteral("profile.xml"))));

    // And it comes back, with what the page will offer agreeing.
    const sak::BackupManifest manifest =
        BackupManifest::loadFromFile(backup.filePath(QStringLiteral("manifest.json")));
    const QVector<sak::AppDataSourceInfo> offered = sak::appDataStoredInBackup(manifest);
    QCOMPARE(offered.size(), 1);
    QCOMPARE(offered.first().name, QStringLiteral("Outlook Archives (PST)"));
    QCOMPARE(offered.first().size_bytes, qint64(kPstBytes));
    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, false));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
    const QDir landed(QDir(profileUnder(system_drive)).filePath(kOutlookPath));
    QCOMPARE(readFile(landed.filePath(QStringLiteral("archive.pst"))).size(), kArchiveBytes);
    QVERIFY(!QFile::exists(landed.filePath(QStringLiteral("me@example.com.ost"))));
}

// Ticking both copies both, and each pass restores its own files. A second pass over the same files
// would conflict with the first and leave a .sakbak of what it had just restored.
void UserProfileRoundTripTests::outlookBothRowsRestoreEachFileOnce() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(seedOutlook(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), outlookRows(true, true));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const RunResult restored =
        runRestore(backup.path(),
                   {mappingTo(MergeMode::CreateNewUser)},
                   verifiedRestore(ConflictResolution::SkipDuplicate, false));
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
    const QDir landed(QDir(profileUnder(system_drive)).filePath(kOutlookPath));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("archive.pst"))));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("older/2019.PST"))));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("me@example.com.ost"))));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("profile.xml"))));
    QDirIterator all(landed.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    int files = 0;
    while (all.hasNext()) {
        const QString file = all.next();
        QVERIFY2(!file.endsWith(QStringLiteral(".sakbak")), qPrintable(file));
        ++files;
    }
    QCOMPARE(files, 4);
}

// The restore page lists the two rows apart, so either can stay behind while the other comes.
void UserProfileRoundTripTests::outlookRowUntickedAtRestoreKeepsTheOtherRow() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(seedOutlook(profile.path()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), outlookRows(true, true));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    // Neither row: no pass runs, so not even their folder appears.
    {
        QTemporaryDir system_drive;
        QVERIFY(system_drive.isValid());
        qputenv("SystemDrive", system_drive.path().toLocal8Bit());
        UserProfileRestoreWorker::RestoreSelections selections;
        selections.app_data_sources = outlookRows(true, true);
        selections.app_data_sources[0].selected = false;
        selections.app_data_sources[1].selected = false;
        const RunResult restored =
            runRestore(backup.path(),
                       {mappingTo(MergeMode::CreateNewUser)},
                       verifiedRestore(ConflictResolution::SkipDuplicate, false),
                       selections);
        QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
        QVERIFY(!QDir(QDir(profileUnder(system_drive)).filePath(kOutlookPath)).exists());
    }

    for (const bool keep_pst : {true, false}) {
        QTemporaryDir system_drive;
        QVERIFY(system_drive.isValid());
        qputenv("SystemDrive", system_drive.path().toLocal8Bit());
        UserProfileRestoreWorker::RestoreSelections selections;
        selections.app_data_sources = outlookRows(true, true);
        selections.app_data_sources[0].selected = keep_pst;
        selections.app_data_sources[1].selected = !keep_pst;
        const RunResult restored =
            runRestore(backup.path(),
                       {mappingTo(MergeMode::CreateNewUser)},
                       verifiedRestore(ConflictResolution::SkipDuplicate, false),
                       selections);
        QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));
        const QDir landed(QDir(profileUnder(system_drive)).filePath(kOutlookPath));
        QCOMPARE(QFile::exists(landed.filePath(QStringLiteral("archive.pst"))), keep_pst);
        QCOMPARE(QFile::exists(landed.filePath(QStringLiteral("older/2019.PST"))), keep_pst);
        QCOMPARE(QFile::exists(landed.filePath(QStringLiteral("me@example.com.ost"))), !keep_pst);
        QCOMPARE(QFile::exists(landed.filePath(QStringLiteral("profile.xml"))), !keep_pst);
    }
}

// Outlook left open holds its PST, and the prompt can be declined. Then the file is named and the
// backup says it did not copy everything.
void UserProfileRoundTripTests::outlookLockedPstIsNamedAndTheRunSaysSo() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(seedOutlook(profile.path()));
    const QString locked_path =
        QDir(profile.path()).filePath(kOutlookPath + QStringLiteral("/archive.pst"));
    const HANDLE held = CreateFileW(reinterpret_cast<LPCWSTR>(locked_path.utf16()),
                                    GENERIC_READ | GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    QVERIFY(held != INVALID_HANDLE_VALUE);
    const auto release = qScopeGuard([held]() { CloseHandle(held); });

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), outlookRows(true, false));
    QVERIFY(backed_up.completed);
    QVERIFY2(!backed_up.success, qPrintable(backed_up.message));
    QVERIFY2(backed_up.message.contains(QStringLiteral("not everything was copied")),
             qPrintable(backed_up.message));
    QVERIFY2(backed_up.warnings.join(QLatin1Char('\n')).contains(QStringLiteral("archive.pst")),
             qPrintable(backed_up.warnings.join(QLatin1Char('\n'))));
    QVERIFY(QFile::exists(QDir(backup.path())
                              .filePath(kUser + QLatin1Char('/') + kOutlookPath +
                                        QStringLiteral("/older/2019.PST"))));
}

// Since Outlook 2010 a new PST goes under Documents\Outlook Files, and the filters must let it
// pass.
void UserProfileRoundTripTests::pstUnderDocumentsIsCoveredByTheUserFolders() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    const QString relative = QStringLiteral("Documents/Outlook Files/customer.pst");
    QVERIFY(writeFile(QDir(profile.path()).filePath(relative), QByteArray(4096, 'p')));

    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(false, false));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));
    QVERIFY(QFile::exists(QDir(backup.path()).filePath(kUser + QLatin1Char('/') + relative)));
}

void UserProfileRoundTripTests::lockedFileIsNamedAndTheRestIsCopied() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(seedAppData(profile.path()));
    const QString locked_path =
        profile.filePath(kChromePath + QStringLiteral("/Default/Bookmarks"));

    HANDLE lock =
        CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(locked_path).utf16()),
                    GENERIC_READ | GENERIC_WRITE,
                    0,
                    nullptr,
                    OPEN_EXISTING,
                    FILE_ATTRIBUTE_NORMAL,
                    nullptr);
    QVERIFY(lock != INVALID_HANDLE_VALUE);
    QVERIFY(UserProfileBackupWorker::isLockedByAnotherProgram(locked_path));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.clear();
    const RunResult backed_up =
        runBackup(user, backup.path(), backupOptions(false, false), {appDataSource(kChromePath)});
    CloseHandle(lock);
    QVERIFY(!UserProfileBackupWorker::isLockedByAnotherProgram(locked_path));

    QVERIFY(backed_up.completed);
    QVERIFY2(!backed_up.success, "a file left behind must not read as a clean backup");
    QCOMPARE(backed_up.message.section(QLatin1Char('\n'), 0, 0),
             QStringLiteral("Backup finished, but not everything was copied."));
    const QString named = QStringLiteral("In use by another program, not backed up: ") +
                          locked_path;
    QVERIFY2(backed_up.warnings.contains(named), qPrintable(backed_up.warnings.join('\n')));

    const QDir stored(backup.filePath(kUser));
    QVERIFY(QFile::exists(stored.filePath(kChromePath + QStringLiteral("/Local State"))));
    QVERIFY(!QFile::exists(stored.filePath(kChromePath + QStringLiteral("/Default/Bookmarks"))));
    QVERIFY(QFile::exists(backup.filePath(QStringLiteral("manifest.json"))));
}

// The manifest's checksum covers each folder's selected flag, and the wizard used to write the
// person's choice into it. Unticking any folder then made the restore refuse the backup as damaged.
void UserProfileRoundTripTests::untickedFolderStaysOutAndTheManifestStillVerifies() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), documentContent()));
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Pictures/photo.jpg")), documentContent()));

    sak::UserProfile user = sourceProfile(profile.path());
    sak::FolderSelection pictures = documentsFolder();
    pictures.type = sak::FolderType::Pictures;
    pictures.display_name = QStringLiteral("Pictures");
    pictures.relative_path = QStringLiteral("Pictures");
    user.folder_selections.append(pictures);
    const RunResult backed_up = runBackup(user, backup.path(), backupOptions(false, false));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    UserProfileRestoreWorker::RestoreSelections selections;
    selections.folder_choices = {{kUser, QStringLiteral("Pictures"), false}};
    const RunResult restored = runRestore(backup.path(),
                                          {mappingTo(MergeMode::CreateNewUser)},
                                          verifiedRestore(ConflictResolution::SkipDuplicate, false),
                                          selections);
    QVERIFY2(restored.completed && restored.success, qPrintable(restored.message));

    const QDir landed(profileUnder(system_drive));
    QVERIFY(QFile::exists(landed.filePath(QStringLiteral("Documents/report.txt"))));
    QVERIFY(!QFile::exists(landed.filePath(QStringLiteral("Pictures"))));
}

void UserProfileRoundTripTests::manifestListsOnlyWhatWasCopied() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), documentContent()));
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Videos/movie.mp4")), documentContent()));

    sak::UserProfile user = sourceProfile(profile.path());
    sak::FolderSelection videos = documentsFolder();
    videos.type = sak::FolderType::Videos;
    videos.display_name = QStringLiteral("Videos");
    videos.relative_path = QStringLiteral("Videos");
    videos.selected = false;
    user.folder_selections.append(videos);
    const RunResult backed_up = runBackup(user, backup.path(), backupOptions(false, false));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const BackupManifest manifest =
        BackupManifest::loadFromFile(backup.filePath(QStringLiteral("manifest.json")));
    QCOMPARE(manifest.users.first().backed_up_folders.size(), 1);
    QCOMPARE(manifest.users.first().backed_up_folders.first().relative_path,
             QStringLiteral("Documents"));
}

// Both wizards default to Strip All, which rewrites each file's permissions on the way through.
void UserProfileRoundTripTests::defaultPermissionsRoundTrip() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());
    const QByteArray content = documentContent();
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/report.txt")), content));

    auto options = backupOptions(true, false);
    options.permission_mode = PermissionMode::StripAll;
    const RunResult backed_up = runBackup(sourceProfile(profile.path()), backup.path(), options);
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));

    auto config = verifiedRestore(ConflictResolution::RenameWithSuffix, false);
    config.perm_mode = PermissionMode::StripAll;
    const RunResult restored =
        runRestore(backup.path(), {mappingTo(MergeMode::CreateNewUser)}, config);
    QVERIFY2(restored.completed && restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));
    QCOMPARE(readFile(
                 QDir(profileUnder(system_drive)).filePath(QStringLiteral("Documents/report.txt"))),
             content);
}

void UserProfileRoundTripTests::namesOutsideTheCodePageRoundTrip_data() {
    verifiedRestoreRoundTrips_data();
}

void UserProfileRoundTripTests::namesOutsideTheCodePageRoundTrip() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QTemporaryDir system_drive;
    QVERIFY(profile.isValid() && backup.isValid() && system_drive.isValid());
    qputenv("SystemDrive", system_drive.path().toLocal8Bit());

    const QString name = QString::fromUtf8("\xE5\x86\x99\xE7\x9C\x9F");  // photos
    const QByteArray content = documentContent();
    const QString relative = QStringLiteral("Documents/") + name + QStringLiteral("/") + name +
                             QStringLiteral(".txt");
    QVERIFY(writeFile(profile.filePath(relative), content));

    sak::UserProfile user = sourceProfile(profile.path());
    user.username = name;
    auto options = backupOptions(compress, encrypt);
    options.verify = true;
    const RunResult backed_up = runBackup(user, backup.path(), options);
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));

    sak::UserMapping mapping = mappingTo(MergeMode::CreateNewUser);
    mapping.source_username = name;
    mapping.destination_username = name;
    const RunResult restored = runRestore(
        backup.path(), {mapping}, verifiedRestore(ConflictResolution::SkipDuplicate, encrypt));
    QVERIFY2(restored.completed && restored.success,
             qPrintable(restored.message + QLatin1Char('\n') + restored.warnings.join('\n')));
    QCOMPARE(readFile(system_drive.filePath(QStringLiteral("Users/") + name + QLatin1Char('/') +
                                            relative)),
             content);
}

void UserProfileRoundTripTests::verifyAfterBackupChecksEveryStoredFile_data() {
    verifiedRestoreRoundTrips_data();
}

void UserProfileRoundTripTests::verifyAfterBackupChecksEveryStoredFile() {
    QFETCH(bool, compress);
    QFETCH(bool, encrypt);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/one.txt")), documentContent()));
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/two.txt")), documentContent()));

    auto options = backupOptions(compress, encrypt);
    options.verify = true;
    const RunResult backed_up = runBackup(sourceProfile(profile.path()), backup.path(), options);
    QVERIFY2(backed_up.completed && backed_up.success,
             qPrintable(backed_up.message + QLatin1Char('\n') + backed_up.warnings.join('\n')));
    QVERIFY2(backed_up.notes.contains(QStringLiteral("Checked 2 stored file(s) against their "
                                                     "originals")),
             qPrintable(backed_up.notes.join('\n')));
}

void UserProfileRoundTripTests::verifyOffChecksNothing() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/one.txt")), documentContent()));

    const RunResult backed_up =
        runBackup(sourceProfile(profile.path()), backup.path(), backupOptions(false, false));
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));
    for (const QString& note : backed_up.notes) {
        QVERIFY2(!note.startsWith(QStringLiteral("Checked")), qPrintable(note));
    }
}

// A run cannot damage a file between writing and checking it, so the check is tested directly:
// what the option promises is that a copy differing from its original is caught.
void UserProfileRoundTripTests::storedFileCheckCatchesADamagedCopy() {
    QTemporaryDir work;
    QVERIFY(work.isValid());
    const QByteArray content = documentContent();
    const QString original = work.filePath(QStringLiteral("original.txt"));
    const QString stored = work.filePath(QStringLiteral("stored.txt"));
    QVERIFY(writeFile(original, content));

    QVERIFY(writeFile(stored, content));
    QVERIFY(UserProfileBackupWorker::storedFileMatches(original, stored, {}, {}));
    QVERIFY(writeFile(stored, content + "damaged"));
    QVERIFY(!UserProfileBackupWorker::storedFileMatches(original, stored, {}, {}));

    sak::BackupCodecOptions codec;
    codec.compress = true;
    const QString container = work.filePath(QStringLiteral("container.sak"));
    const auto written = sak::writeBackupFile(original, container, codec);
    QVERIFY(written.has_value());
    QVERIFY(
        UserProfileBackupWorker::storedFileMatches(original, container, written->plain_sha256, {}));
    // Decodes cleanly, but holds something else.
    QVERIFY(!UserProfileBackupWorker::storedFileMatches(
        original, container, QCryptographicHash::hash("other", QCryptographicHash::Sha256), {}));
    QByteArray damaged = readFile(container);
    damaged[damaged.size() - 8] = static_cast<char>(damaged.at(damaged.size() - 8) ^ 0x5A);
    QVERIFY(writeFile(container, damaged));
    QVERIFY(!UserProfileBackupWorker::storedFileMatches(
        original, container, written->plain_sha256, {}));
}

void UserProfileRoundTripTests::folderOverTheLimitIsWarnedAboutAndStillCopied() {
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    QVERIFY(writeFile(profile.filePath(QStringLiteral("Documents/one.txt")), documentContent()));

    sak::UserProfile user = sourceProfile(profile.path());
    user.folder_selections.first().size_bytes = 2048;
    sak::SmartFilter filter;
    filter.max_folder_size_bytes = 1024;

    const QString warning = QStringLiteral(
        "Documents for sakroundtripuser is about 2.0 KB, over the 1.0 KB you set. It will still "
        "be backed up.");

    const RunResult off = runBackup(
        user, backup.filePath(QStringLiteral("off")), backupOptions(false, false), {}, filter);
    QVERIFY2(off.completed && off.success, qPrintable(off.message));
    QVERIFY(!off.warnings.contains(warning));

    filter.enable_folder_size_limit = true;
    const RunResult on = runBackup(
        user, backup.filePath(QStringLiteral("on")), backupOptions(false, false), {}, filter);
    QVERIFY2(on.completed && on.success, qPrintable(on.message));
    QVERIFY2(on.warnings.contains(warning), qPrintable(on.warnings.join('\n')));
    QVERIFY(QFile::exists(
        backup.filePath(QStringLiteral("on/") + kUser + QStringLiteral("/Documents/one.txt"))));
}

void UserProfileRoundTripTests::excludeBoxesKeepOrDropTheirFiles_data() {
    QTest::addColumn<bool>("cache");
    QTest::addColumn<bool>("temporary");
    QTest::addColumn<bool>("lock_files");
    QTest::newRow("all on") << true << true << true;
    QTest::newRow("cache off") << false << true << true;
    QTest::newRow("temporary off") << true << false << true;
    QTest::newRow("lock files off") << true << true << false;
}

void UserProfileRoundTripTests::excludeBoxesKeepOrDropTheirFiles() {
    QFETCH(bool, cache);
    QFETCH(bool, temporary);
    QFETCH(bool, lock_files);
    QTemporaryDir profile;
    QTemporaryDir backup;
    QVERIFY(profile.isValid() && backup.isValid());
    const QString cache_file = QStringLiteral("Documents/App/Cache/entry.bin");
    const QString temp_file = QStringLiteral("Documents/draft.tmp");
    const QString lock_file = QStringLiteral("Documents/db.lock");
    const QString kept = QStringLiteral("Documents/report.txt");
    for (const QString& path : {cache_file, temp_file, lock_file, kept}) {
        QVERIFY(writeFile(profile.filePath(path), documentContent()));
    }

    sak::SmartFilter filter;
    filter.setExclusionGroups(cache, temporary, lock_files);
    const RunResult backed_up = runBackup(
        sourceProfile(profile.path()), backup.path(), backupOptions(false, false), {}, filter);
    QVERIFY2(backed_up.completed && backed_up.success, qPrintable(backed_up.message));

    const QDir stored(backup.filePath(kUser));
    QVERIFY(QFile::exists(stored.filePath(kept)));
    QCOMPARE(QFile::exists(stored.filePath(cache_file)), !cache);
    QCOMPARE(QFile::exists(stored.filePath(temp_file)), !temporary);
    QCOMPARE(QFile::exists(stored.filePath(lock_file)), !lock_files);
}

QTEST_GUILESS_MAIN(UserProfileRoundTripTests)
#include "test_user_profile_round_trip.moc"
