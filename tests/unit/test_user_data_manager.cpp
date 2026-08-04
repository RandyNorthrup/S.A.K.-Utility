// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_data_manager.cpp
/// @brief Unit tests for UserDataManager data locations and checksums (TST-04)

#include "sak/user_data_manager.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest/QtTest>

using namespace sak;

class UserDataManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commonDataLocationsNotEmpty() {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        QVERIFY(!locations.empty());
    }

    void commonDataLocationsHaveDescriptions() {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        for (const auto& loc : locations) {
            QVERIFY2(!loc.description.isEmpty(),
                     qPrintable("Missing description for pattern: " + loc.pattern));
        }
    }

    void checksumDeterministic() {
        QTemporaryFile tmp;
        QVERIFY(tmp.open());
        tmp.write("hello world checksum test");
        tmp.flush();

        UserDataManager mgr;
        QString hash1 = mgr.generateChecksum(tmp.fileName());
        QString hash2 = mgr.generateChecksum(tmp.fileName());

        QVERIFY(!hash1.isEmpty());
        QCOMPARE(hash1, hash2);
    }

    void checksumDifferentForDifferentContent() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        tmp1.write("content A");
        tmp2.write("content B");
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QString hash1 = mgr.generateChecksum(tmp1.fileName());
        QString hash2 = mgr.generateChecksum(tmp2.fileName());

        QVERIFY(!hash1.isEmpty());
        QVERIFY(!hash2.isEmpty());
        QVERIFY(hash1 != hash2);
    }

    void compareChecksumsMatch() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        QByteArray content("identical content for comparison");
        tmp1.write(content);
        tmp2.write(content);
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QVERIFY(mgr.compareChecksums(tmp1.fileName(), tmp2.fileName()));
    }

    void compareChecksumsDoNotMatch() {
        QTemporaryFile tmp1, tmp2;
        QVERIFY(tmp1.open());
        QVERIFY(tmp2.open());
        tmp1.write("file one");
        tmp2.write("file two");
        tmp1.flush();
        tmp2.flush();

        UserDataManager mgr;
        QVERIFY(!mgr.compareChecksums(tmp1.fileName(), tmp2.fileName()));
    }

    void checksumNonexistentFileReturnsEmpty() {
        UserDataManager mgr;
        QString hash = mgr.generateChecksum("C:\\nonexistent\\path\\file.dat");
        QVERIFY(hash.isEmpty());
    }

    void calculateSizeOnTempFiles() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        QFile f1(QDir(tmpDir.path()).filePath("a.txt"));
        QVERIFY(f1.open(QIODevice::WriteOnly));
        f1.write(QByteArray(100, 'A'));
        f1.close();

        QFile f2(QDir(tmpDir.path()).filePath("b.txt"));
        QVERIFY(f2.open(QIODevice::WriteOnly));
        f2.write(QByteArray(200, 'B'));
        f2.close();

        UserDataManager mgr;
        QStringList paths = {tmpDir.path()};
        qint64 size = mgr.calculateSize(paths);

        // Should be at least 300 bytes
        QVERIFY(size >= 300);
    }

    void listBackupsOnEmptyDir() {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        UserDataManager mgr;
        auto backups = mgr.listBackups(tmpDir.path());
        QVERIFY(backups.empty());
    }

    // --- Helpers -------------------------------------------------------------

    static QString makeSourceDir(const QTemporaryDir& parent,
                                 const QString& name,
                                 const QString& file_name,
                                 const QByteArray& content) {
        const QString dir = QDir(parent.path()).filePath(name);
        if (!QDir().mkpath(dir)) {
            return {};
        }
        QFile f(QDir(dir).filePath(file_name));
        if (f.open(QIODevice::WriteOnly)) {
            f.write(content);
            f.close();
        }
        return dir;
    }

    // --- Regression tests (P06-18/19/20/21/22/23) ----------------------------

    // P06-18: the metadata sidecar must persist the real checksum so restore
    // and verifyBackup actually integrity-check the archive.
    void checksumPersistedAndVerified() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload bytes");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(!e->checksum.isEmpty());

        QFile meta(e->backup_path + ".json");
        QVERIFY(meta.open(QIODevice::ReadOnly));
        const auto obj = QJsonDocument::fromJson(meta.readAll()).object();
        QVERIFY(!obj.value("checksum").toString().isEmpty());
        QCOMPARE(obj.value("checksum").toString(), e->checksum);

        // Corrupt the archive: verifyBackup must now fail closed.
        QFile arc(e->backup_path);
        QVERIFY(arc.open(QIODevice::Append));
        arc.write("corruption");
        arc.close();
        QVERIFY(!mgr.verifyBackup(e->backup_path));
    }

    // P06-22: a same-named file across two sources collides in the flat
    // destination, so the copy primitive (and the backup) must fail closed.
    void copyDirectoryFailClosedOnCollision() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString a = makeSourceDir(work, "a", "same.txt", "from a");
        const QString b = makeSourceDir(work, "b", "same.txt", "from b");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {a, b}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
    }

    // P06-19: encryption requested with no password must not emit plaintext.
    void encryptWithoutPasswordFailsClosed() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "secret");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.encrypt = true;
        cfg.password = "";
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
        QCOMPARE(QDir(backupDir.path()).entryList({"*.zip"}, QDir::Files).size(), 0);
    }

    // P06-19: encryption without compression would leave a plaintext copy dir.
    void encryptRequiresCompression() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "secret");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        cfg.encrypt = true;
        cfg.password = "pw";
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(!e.has_value());
    }

    // P06-21: two backups sharing a one-second timestamp must both survive.
    void twoBackupsSameSecondBothKept() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        auto e1 = mgr.backupAppData("Foo", {src}, backupDir.path(), cfg);
        auto e2 = mgr.backupAppData("Foo", {src}, backupDir.path(), cfg);
        QVERIFY(e1.has_value());
        QVERIFY(e2.has_value());
        QVERIFY(e1->backup_path != e2->backup_path);
        QVERIFY(QFileInfo::exists(e1->backup_path));
        QVERIFY(QFileInfo::exists(e2->backup_path));
        QCOMPARE(static_cast<int>(mgr.listBackups(backupDir.path()).size()), 2);
    }

    // P06-20: an uncompressed backup is a directory; metadata must be truthful
    // and the payload must round-trip through list + restore.
    void uncompressedBackupRoundTrip() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "round trip");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(!e->backup_path.endsWith(".zip"));
        QVERIFY(QFileInfo(e->backup_path).isDir());
        QCOMPARE(static_cast<int>(mgr.listBackups(backupDir.path()).size()), 1);

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
        QVERIFY(QFileInfo::exists(QDir(restoreDir.path()).filePath("data.txt")));
    }

    // P06-24: a directory junction inside the source must NOT be followed --
    // otherwise the recursive copy pulls in data from outside the source root
    // (or loops forever on a junction that targets an ancestor).
    void backupSkipsJunctionReparsePoints() {
        QTemporaryDir work;
        QVERIFY(work.isValid());

        // External content the junction points at -- must never be backed up.
        const QString external = QDir(work.path()).filePath("external");
        QVERIFY(QDir().mkpath(external));
        QFile ext(QDir(external).filePath("secret.txt"));
        QVERIFY(ext.open(QIODevice::WriteOnly));
        ext.write("EXTERNAL SECRET");
        ext.close();

        // Source tree: a real file, a real nested subdir, and a junction.
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QVERIFY(!src.isEmpty());
        const QString real_sub = QDir(src).filePath("real");
        QVERIFY(QDir().mkpath(real_sub));
        QFile inner(QDir(real_sub).filePath("inner.txt"));
        QVERIFY(inner.open(QIODevice::WriteOnly));
        inner.write("inner");
        inner.close();

        // mklink /J creates a junction without elevation (unlike symlinks).
        const QString junction = QDir(src).filePath("jn");
        QProcess mklink;
        mklink.start("cmd",
                     {"/c",
                      "mklink",
                      "/J",
                      QDir::toNativeSeparators(junction),
                      QDir::toNativeSeparators(external)});
        QVERIFY(mklink.waitForFinished());
        QVERIFY2(mklink.exitCode() == 0, "junction creation failed");
        QVERIFY(QFileInfo(junction).isDir());

        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        const QDir root(e->backup_path);
        // Real content copied through.
        QVERIFY(root.exists("data.txt"));
        QVERIFY(QFileInfo::exists(root.filePath("real/inner.txt")));
        // Junction not followed: neither the junction dir nor its external
        // payload is present in the backup.
        QVERIFY(!QFileInfo::exists(root.filePath("jn")));
        QVERIFY(!QFileInfo::exists(root.filePath("jn/secret.txt")));
    }

    // --- B7-31: deleteBackup must remove a DIRECTORY payload, not just a file ---
    void deleteBackupRemovesDirectoryPayload() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;  // uncompressed -> the payload is a directory
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(QFileInfo(e->backup_path).isDir());

        QVERIFY(mgr.deleteBackup(e->backup_path));
        QVERIFY(!QFileInfo::exists(e->backup_path));  // QFile::remove could never delete a dir
        QVERIFY(!QFileInfo::exists(e->backup_path + ".json"));
    }

    // --- CR2: deleteBackup must not recursively erase an arbitrary tree ---
    // Pure decision-seam coverage for the confinement / identity / screen gate.
    void deletionRefusalScreensDriveRoot() {
        const QString r = UserDataManager::backupDeletionRefusal(
            QStringLiteral("C:/"), std::optional<QString>(QStringLiteral("C:/")));
        QVERIFY(!r.isEmpty());  // a drive root is refused even with matching metadata
    }

    void deletionRefusalNeedsMetadataSidecar() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::nullopt);
        QVERIFY(!r.isEmpty());  // no sidecar -> unmanaged directory -> refused
    }

    void deletionRefusalNeedsIdentityMatch() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString other = QDir(work.path()).filePath("Somewhere_else");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::optional<QString>(other));
        QVERIFY(!r.isEmpty());  // sidecar records a DIFFERENT object -> refused
    }

    void deletionRefusalAllowsManagedBackup() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString p = QDir(work.path()).filePath("App_backup");
        const QString r = UserDataManager::backupDeletionRefusal(p, std::optional<QString>(p));
        QVERIFY2(r.isEmpty(), qPrintable(r));  // safe local path + matching sidecar -> allowed
    }

    // --- CODEX_REVIEW_4 M-A1-20: entry guards were Q_ASSERT_X (release no-op) so an empty
    // path ran CWD-relative. allPathsPresent is the release-effective fail-closed replacement.
    void allPathsPresentRejectsEmpty() {
        QVERIFY(UserDataManager::allPathsPresent({QStringLiteral("a"), QStringLiteral("b")}));
        QVERIFY(!UserDataManager::allPathsPresent({QStringLiteral(""), QStringLiteral("x")}));
        QVERIFY(!UserDataManager::allPathsPresent({QStringLiteral("x"), QStringLiteral("")}));
        QVERIFY(!UserDataManager::allPathsPresent({QString()}));
    }

    // --- CODEX_REVIEW_4 M-A1-25: cap the encrypted-archive read before readAll() so an
    // attacker file cannot OOM the app ahead of the (post-decrypt) zip-bomb preflight.
    void encryptedArchiveSizeCapBoundary() {
        constexpr qint64 kCap = 4LL * 1024 * 1024 * 1024;
        QVERIFY(UserDataManager::encryptedArchiveSizeOk(0));
        QVERIFY(UserDataManager::encryptedArchiveSizeOk(kCap));
        QVERIFY(!UserDataManager::encryptedArchiveSizeOk(-1));        // unreadable -> fail closed
        QVERIFY(!UserDataManager::encryptedArchiveSizeOk(kCap + 1));  // oversized -> fail closed
    }

    // --- CODEX_REVIEW_4 M-A1-26: atomicReplaceFile renames the original aside first (never a
    // delete-then-rename that could destroy the original on a rename failure).
    void atomicReplaceFileSwapsWithoutDataLossWindow() {
        QTemporaryDir work;
        QVERIFY(work.isValid());

        // Replacing an existing target: content becomes the staged one; tmp + the aside-copy gone.
        const QString target = QDir(work.path()).filePath("data.bin");
        const QString tmp = QDir(work.path()).filePath("data.bin.new");
        {
            QFile t(target);
            QVERIFY(t.open(QIODevice::WriteOnly));
            t.write("old");
        }
        {
            QFile s(tmp);
            QVERIFY(s.open(QIODevice::WriteOnly));
            s.write("new");
        }
        QVERIFY(UserDataManager::atomicReplaceFile(tmp, target));
        {
            QFile r(target);
            QVERIFY(r.open(QIODevice::ReadOnly));
            QCOMPARE(r.readAll(), QByteArray("new"));
        }
        QVERIFY(!QFileInfo::exists(tmp));
        QVERIFY(!QFileInfo::exists(target + ".sak_old"));

        // Replacing a not-yet-existing target: the staged file is simply moved into place.
        const QString target2 = QDir(work.path()).filePath("fresh.bin");
        const QString tmp2 = QDir(work.path()).filePath("fresh.bin.new");
        {
            QFile s2(tmp2);
            QVERIFY(s2.open(QIODevice::WriteOnly));
            s2.write("hello");
        }
        QVERIFY(UserDataManager::atomicReplaceFile(tmp2, target2));
        QVERIFY(QFileInfo::exists(target2));
        QVERIFY(!QFileInfo::exists(tmp2));
    }

    // End-to-end: an arbitrary directory with no sidecar is left intact.
    void deleteBackupRefusesUnmanagedDirectory() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString victim = QDir(work.path()).filePath("not_a_backup");
        QVERIFY(QDir().mkpath(victim));
        QFile f(QDir(victim).filePath("important.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("keep me");
        f.close();

        UserDataManager mgr;
        QVERIFY(!mgr.deleteBackup(victim));  // no metadata sidecar -> refused
        QVERIFY(QFileInfo::exists(victim));  // tree left fully intact
        QVERIFY(QFileInfo::exists(QDir(victim).filePath("important.txt")));
    }

    // End-to-end: a forged sidecar that points elsewhere must not authorize deletion.
    void deleteBackupRefusesForgedSidecarMismatch() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString victim = QDir(work.path()).filePath("victim");
        QVERIFY(QDir().mkpath(victim));
        QJsonObject obj;
        obj["app_name"] = QStringLiteral("X");
        obj["backup_path"] = QDir(work.path()).filePath("elsewhere");
        QFile meta(victim + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(QJsonDocument(obj).toJson());
        meta.close();

        UserDataManager mgr;
        QVERIFY(!mgr.deleteBackup(victim));  // sidecar identity mismatch -> refused
        QVERIFY(QFileInfo::exists(victim));
    }

    // --- B7-30: a stray metadata .json without its payload must NOT verify ---
    void verifyBackupFailsWhenPayloadMissing() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(mgr.verifyBackup(e->backup_path));   // valid while the payload is present

        QVERIFY(QFile::remove(e->backup_path));      // delete payload, keep .json
        QVERIFY(!mgr.verifyBackup(e->backup_path));  // metadata alone must not verify
    }

    // --- B7-30: two unreadable files both hash to "" -- that is NOT "equal" ---
    void compareChecksumsRejectsUnreadablePair() {
        UserDataManager mgr;
        QVERIFY(!mgr.compareChecksums(QStringLiteral("C:/nope/a.dat"),
                                      QStringLiteral("C:/nope/b.dat")));
    }

    // --- B7-09: directory restore must honor overwrite_existing ---
    void dirRestoreHonorsOverwriteFlag() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "NEW");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = false;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        const auto seedThenRestore = [&](bool overwrite) -> QByteArray {
            QTemporaryDir restoreDir;
            QFile old(QDir(restoreDir.path()).filePath("data.txt"));
            [&] {
                QVERIFY(old.open(QIODevice::WriteOnly));
            }();
            old.write("OLD");
            old.close();
            UserDataManager::RestoreConfig rcfg;
            rcfg.verify_checksum = false;
            rcfg.create_backup = false;
            rcfg.overwrite_existing = overwrite;
            [&] {
                QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
            }();
            QFile f(QDir(restoreDir.path()).filePath("data.txt"));
            [&] {
                QVERIFY(f.open(QIODevice::ReadOnly));
            }();
            return f.readAll();
        };
        QCOMPARE(seedThenRestore(false), QByteArray("OLD"));  // existing file preserved
        QCOMPARE(seedThenRestore(true), QByteArray("NEW"));   // existing file replaced
    }

    // --- B7-11: an ENCRYPTED backup must round-trip (decrypt temp needs .zip ext) ---
    void encryptedBackupRestoreRoundTrip() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "secret.txt", "TOP SECRET DATA");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());

        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.encrypt = true;
        cfg.password = QStringLiteral("hunter2");
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(e->backup_path.endsWith(".zip"));

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        rcfg.overwrite_existing = true;
        rcfg.password = QStringLiteral("hunter2");
        // Before the .zip-suffix fix, Expand-Archive rejected the extensionless decrypted temp and
        // this returned false -- encrypted backups were unrestorable.
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));

        QDirIterator it(restoreDir.path(),
                        {QStringLiteral("secret.txt")},
                        QDir::Files,
                        QDirIterator::Subdirectories);
        QVERIFY(it.hasNext());
        QFile restored(it.next());
        QVERIFY(restored.open(QIODevice::ReadOnly));
        QCOMPARE(restored.readAll(), QByteArray("TOP SECRET DATA"));
    }

    // --- CR3-1: a COMPRESSED backup must honor exclude_patterns ---
    // Previously Compress-Archive ran over the raw sources and ignored exclusions, so a
    // "secret" file the user asked to drop was archived anyway. It must now be filtered.
    void compressedBackupHonorsExcludePatterns() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "keep.txt", "KEEP");
        QVERIFY(!src.isEmpty());
        QFile secret(QDir(src).filePath("password.key"));
        QVERIFY(secret.open(QIODevice::WriteOnly));
        secret.write("SECRET");
        secret.close();

        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = false;
        cfg.exclude_patterns = QStringList{QStringLiteral("*.key")};
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = false;
        rcfg.create_backup = false;
        rcfg.overwrite_existing = true;
        QVERIFY(mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));

        QDirIterator kept(restoreDir.path(),
                          {QStringLiteral("keep.txt")},
                          QDir::Files,
                          QDirIterator::Subdirectories);
        QVERIFY(kept.hasNext());  // non-excluded file present
        QDirIterator leaked(restoreDir.path(),
                            {QStringLiteral("password.key")},
                            QDir::Files,
                            QDirIterator::Subdirectories);
        QVERIFY(!leaked.hasNext());  // excluded secret was NOT archived
    }

    // --- CR3-3: an emptied sidecar checksum must not disable verification for a .zip ---
    void restoreRejectsCompressedBackupWithEmptiedChecksum() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "payload");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        cfg.compress = true;
        cfg.verify_checksum = true;
        auto e = mgr.backupAppData("App", {src}, backupDir.path(), cfg);
        QVERIFY(e.has_value());
        QVERIFY(!e->checksum.isEmpty());

        // Tamper: empty the sidecar checksum to try to skip integrity checking.
        const QString metaPath = e->backup_path + ".json";
        QFile in(metaPath);
        QVERIFY(in.open(QIODevice::ReadOnly));
        QJsonObject obj = QJsonDocument::fromJson(in.readAll()).object();
        in.close();
        obj["checksum"] = QString();
        QFile out(metaPath);
        QVERIFY(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
        out.write(QJsonDocument(obj).toJson());
        out.close();

        QTemporaryDir restoreDir;
        QVERIFY(restoreDir.isValid());
        UserDataManager::RestoreConfig rcfg;
        rcfg.verify_checksum = true;  // integrity is REQUIRED
        rcfg.create_backup = false;
        // A compressed archive with no checksum cannot be verified -> fail closed.
        QVERIFY(!mgr.restoreAppData(e->backup_path, restoreDir.path(), rcfg));
    }

    // --- CR3-7: strict metadata parse rejects a wrong-typed / oversized sidecar ---
    void listBackupsRejectsWrongTypedChecksumSidecar() {
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        const QString payload = QDir(backupDir.path()).filePath("App_20250101_000000.zip");
        QFile pf(payload);
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.write("PK\x03\x04");
        pf.close();
        QJsonObject obj;
        obj["app_name"] = QStringLiteral("App");
        obj["backup_path"] = payload;
        obj["checksum"] = 12'345;  // wrong type: must not coerce to "" (which disables verify)
        QFile meta(payload + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(QJsonDocument(obj).toJson());
        meta.close();

        UserDataManager mgr;
        QVERIFY(mgr.listBackups(backupDir.path()).empty());  // rejected, not silently coerced
    }

    void listBackupsRejectsOversizedSidecar() {
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        const QString payload = QDir(backupDir.path()).filePath("App_big.zip");
        QFile pf(payload);
        QVERIFY(pf.open(QIODevice::WriteOnly));
        pf.write("PK");
        pf.close();
        QByteArray big = "{\"app_name\":\"App\",\"backup_path\":\"" + payload.toUtf8() +
                         "\",\"checksum\":\"\",\"pad\":\"";
        big += QByteArray(1024 * 1024 + 16, 'a');  // push the sidecar past the 1 MiB cap
        big += "\"}";
        QFile meta(payload + ".json");
        QVERIFY(meta.open(QIODevice::WriteOnly));
        meta.write(big);
        meta.close();

        UserDataManager mgr;
        QVERIFY(mgr.listBackups(backupDir.path()).empty());  // oversized sidecar refused
    }

    // --- CR3-8: empty app_name / backup_dir fail closed at RUNTIME (not debug-only) ---
    void backupFailsClosedOnEmptyInputs() {
        QTemporaryDir work;
        QVERIFY(work.isValid());
        const QString src = makeSourceDir(work, "src", "data.txt", "x");
        QTemporaryDir backupDir;
        QVERIFY(backupDir.isValid());
        UserDataManager mgr;
        UserDataManager::BackupConfig cfg;
        QVERIFY(!mgr.backupAppData(QString(), {src}, backupDir.path(), cfg).has_value());
        QVERIFY(!mgr.backupAppData(QStringLiteral("App"), {src}, QString(), cfg).has_value());
    }
};

QTEST_MAIN(UserDataManagerTests)
#include "test_user_data_manager.moc"
