// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_user_data_manager.cpp
/// @brief Unit tests for UserDataManager data locations and checksums (TST-04)

#include <QtTest/QtTest>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QFile>
#include "sak/user_data_manager.h"

using namespace sak;

class UserDataManagerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void commonDataLocationsNotEmpty()
    {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        QVERIFY(!locations.empty());
    }

    void commonDataLocationsHaveDescriptions()
    {
        UserDataManager mgr;
        auto locations = mgr.getCommonDataLocations();
        for (const auto& loc : locations) {
            QVERIFY2(!loc.description.isEmpty(),
                     qPrintable("Missing description for pattern: " + loc.pattern));
        }
    }

    void checksumDeterministic()
    {
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

    void checksumDifferentForDifferentContent()
    {
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

    void compareChecksumsMatch()
    {
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

    void compareChecksumsDoNotMatch()
    {
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

    void checksumNonexistentFileReturnsEmpty()
    {
        UserDataManager mgr;
        QString hash = mgr.generateChecksum("C:\\nonexistent\\path\\file.dat");
        QVERIFY(hash.isEmpty());
    }

    void calculateSizeOnTempFiles()
    {
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

    void listBackupsOnEmptyDir()
    {
        QTemporaryDir tmpDir;
        QVERIFY(tmpDir.isValid());

        UserDataManager mgr;
        auto backups = mgr.listBackups(tmpDir.path());
        QVERIFY(backups.empty());
    }
};

QTEST_MAIN(UserDataManagerTests)
#include "test_user_data_manager.moc"
