// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_windows_user_scanner.cpp
/// @brief Unit tests for WindowsUserScanner

#include "sak/windows_user_scanner.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestWindowsUserScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void getCurrentUsername_nonEmpty();
    void getProfilePath_currentUser();
    void getProfilePath_nonExistentUser();
    void getProfilePath_emptyUsernameFailsClosed();  // R5-G23-4 fail-closed guard
    void estimateProfileSize_invalidPath();
    void getDefaultFolderSelections_currentUser();
    void getDefaultFolderSelections_invalidPath();
    void sumFolderFileSizes_sumsRealFiles();  // B10-31 per-folder sizing
};

void TestWindowsUserScanner::sumFolderFileSizes_sumsRealFiles() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    // 5 + 3 bytes at two nesting levels; a per-folder selection must report the
    // real total (previously it called estimateProfileSize on the folder and got 0).
    QFile a(QDir(dir.path()).filePath("a.txt"));
    QVERIFY(a.open(QIODevice::WriteOnly));
    a.write("hello");  // 5
    a.close();
    QVERIFY(QDir(dir.path()).mkpath("sub"));
    QFile b(QDir(dir.path()).filePath("sub/b.txt"));
    QVERIFY(b.open(QIODevice::WriteOnly));
    b.write("abc");  // 3
    b.close();

    QCOMPARE(WindowsUserScanner::sumFolderFileSizes(dir.path(), 1000), static_cast<qint64>(8));
    // An absent folder sizes to 0.
    QCOMPARE(WindowsUserScanner::sumFolderFileSizes(QDir(dir.path()).filePath("nope"), 1000),
             static_cast<qint64>(0));
}

void TestWindowsUserScanner::construction_default() {
    WindowsUserScanner scanner;
    QVERIFY(dynamic_cast<QObject*>(&scanner) != nullptr);
}

void TestWindowsUserScanner::getCurrentUsername_nonEmpty() {
    const QString username = WindowsUserScanner::getCurrentUsername();
    QVERIFY(!username.isEmpty());
    QVERIFY(username.length() > 0);
}

void TestWindowsUserScanner::getProfilePath_currentUser() {
    const QString username = WindowsUserScanner::getCurrentUsername();
    const QString profile_path = WindowsUserScanner::getProfilePath(username);
    QVERIFY(!profile_path.isEmpty());
    QVERIFY(QDir(profile_path).exists());
}

void TestWindowsUserScanner::getProfilePath_nonExistentUser() {
    const QString profile_path =
        WindowsUserScanner::getProfilePath(QStringLiteral("NonExistentUser_XYZ_12345"));
    // Should return empty or non-existent path
    QVERIFY(profile_path.isEmpty() || !QDir(profile_path).exists());
}

void TestWindowsUserScanner::getProfilePath_emptyUsernameFailsClosed() {
    // R5-G23-4: an empty username must FAIL CLOSED (return an empty path). Without the guard,
    // the standard-location path "<SystemDrive>\Users\<name>" collapses to the profiles ROOT
    // ("<SystemDrive>\Users\") -- which exists -- so an empty name would resolve to the parent
    // of EVERY user's profile and be reported as a real, existing profile. The guard returns {}
    // before that. Deterministic and platform-independent (the empty-name check runs before any
    // registry/SystemDrive lookup); non-vacuous by construction -- if the guard were removed the
    // call would return a non-empty, existing directory here, turning this assertion red.
    const QString profile_path = WindowsUserScanner::getProfilePath(QString());
    QVERIFY2(profile_path.isEmpty(),
             qPrintable(QStringLiteral("empty username must not resolve to a profile, got: %1")
                            .arg(profile_path)));
}

void TestWindowsUserScanner::estimateProfileSize_invalidPath() {
    const qint64 size =
        WindowsUserScanner::estimateProfileSize(QStringLiteral("C:\\Invalid_Profile_Path_12345"));
    QCOMPARE(size, static_cast<qint64>(0));
}

void TestWindowsUserScanner::getDefaultFolderSelections_currentUser() {
    const QString username = WindowsUserScanner::getCurrentUsername();
    const QString profile_path = WindowsUserScanner::getProfilePath(username);
    if (profile_path.isEmpty()) {
        QSKIP("Could not resolve current user profile path");
    }
    const auto selections = WindowsUserScanner::getDefaultFolderSelections(profile_path);
    // Should have at least Documents, Desktop, etc.
    QVERIFY(!selections.isEmpty());
    QVERIFY(selections.size() >= 2);
}

void TestWindowsUserScanner::getDefaultFolderSelections_invalidPath() {
    const auto selections = WindowsUserScanner::getDefaultFolderSelections(
        QStringLiteral("C:\\Invalid_Profile_Path_12345"));
    // The fixed 9-entry folder catalog is returned unconditionally; for a path that exists for no
    // folder, every entry must size to zero. `size() >= 0` was a tautology (a container length is
    // never negative); pin the catalog count and the all-zero sizing an invalid path must yield, so
    // a regression that sized the wrong path or dropped the existence guard fails here.
    QCOMPARE(selections.size(), 9);
    for (const auto& sel : selections) {
        QVERIFY2(sel.size_bytes == 0 && sel.file_count == 0, qPrintable(sel.display_name));
    }
}

QTEST_MAIN(TestWindowsUserScanner)
#include "test_windows_user_scanner.moc"
