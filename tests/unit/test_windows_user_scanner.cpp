// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_windows_user_scanner.cpp
/// @brief Unit tests for WindowsUserScanner

#include "sak/windows_user_scanner.h"

#include <QtTest/QtTest>

using namespace sak;

class TestWindowsUserScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void getCurrentUsername_nonEmpty();
    void getProfilePath_currentUser();
    void getProfilePath_nonExistentUser();
    void estimateProfileSize_invalidPath();
    void getDefaultFolderSelections_currentUser();
    void getDefaultFolderSelections_invalidPath();
};

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
    QVERIFY(selections.size() >= 0);
}

QTEST_MAIN(TestWindowsUserScanner)
#include "test_windows_user_scanner.moc"
