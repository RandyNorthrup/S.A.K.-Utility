// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_profile_round_trip.cpp
/// @brief A real backup, written by the backup worker and read back from its manifest.json,
///        restored by the restore worker. Each worker is tested on its own elsewhere; what only
///        this file can catch is the two of them disagreeing about what a backup is.

#include "sak/backup_file_codec.h"
#include "sak/user_profile_backup_worker.h"
#include "sak/user_profile_restore_worker.h"
#include "sak/user_profile_types.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest/QtTest>

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
};

/// Run a backup with @p options, then read the manifest back from disk, as a later restore does.
RunResult runBackup(const sak::UserProfile& user,
                    const QString& destination,
                    const UserProfileBackupWorker::BackupOptions& options) {
    if (!QDir().mkpath(destination)) {
        return {};
    }
    UserProfileBackupWorker worker;
    QSignalSpy complete(&worker, &UserProfileBackupWorker::backupComplete);
    QSignalSpy log(&worker, &UserProfileBackupWorker::logMessage);
    worker.startBackup(BackupManifest{}, {user}, destination, sak::SmartFilter{}, options);
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

QTEST_GUILESS_MAIN(UserProfileRoundTripTests)
#include "test_user_profile_round_trip.moc"
