// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_data_manager.cpp
/// @brief Unit tests for UserDataManager data locations and checksums (TST-04)

#include "sak/user_data_manager.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
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
};

QTEST_MAIN(UserDataManagerTests)
#include "test_user_data_manager.moc"
