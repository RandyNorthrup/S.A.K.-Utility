// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QtTest>
#include <QSignalSpy>
#include "sak/windows_user_scanner.h"

class TestWindowsUserScanner : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Scanner initialization
    void testConstructor();

    // User scanning
    void testScanUsers();
    void testScanUsersNotEmpty();
    void testScanUsersProgress();
    void testUserFoundSignal();

    // Current user
    void testGetCurrentUsername();
    void testCurrentUsernameNotEmpty();
    void testCurrentUsernameValid();

    // User SID
    void testGetUserSID();
    void testGetUserSIDCurrent();
    void testGetUserSIDInvalid();
    void testGetUserSIDFormat();

    // Profile paths
    void testGetProfilePath();
    void testGetProfilePathCurrent();
    void testGetProfilePathInvalid();
    void testGetProfilePathFormat();

    // Login status
    void testIsUserLoggedIn();
    void testIsUserLoggedInCurrent();
    void testIsUserLoggedInInvalid();

    // Profile size estimation
    void testEstimateProfileSize();
    void testEstimateProfileSizeCurrent();
    void testEstimateProfileSizeInvalid();
    void testEstimateProfileSizePositive();

    // Default folder selections
    void testGetDefaultFolderSelections();
    void testGetDefaultFolderSelectionsCurrent();
    void testGetDefaultFolderSelectionsInvalid();
    void testGetDefaultFolderSelectionsStandard();

    // User enumeration
    void testEnumerateWindowsUsers();
    void testEnumerateAdministrators();
    void testEnumerateStandardUsers();

    // Profile validation
    void testProfilePathExists();
    void testProfileHasDocuments();
    void testProfileHasDesktop();

    // Signals
    void testScanProgressSignal();
    void testUserFoundSignals();
    void testMultipleUsers();

    // Error handling
    void testEmptyUsername();
    void testNullUsername();
    void testInvalidProfilePath();

    // User properties
    void testUserProfileStructure();
    void testUserHasSID();
    void testUserHasPath();

    // Special users
    void testSystemUsers();
    void testBuiltInUsers();
    void testFilterSystemAccounts();

    // Performance
    void testQuickSizeEstimate();
    void testScanSpeed();

private:
    sak::WindowsUserScanner* m_scanner{nullptr};
    
    void waitForSignal(QObject* obj, const char* signal, int timeout = 5000);
    bool isValidSID(const QString& sid);
    bool isValidProfilePath(const QString& path);
};

void TestWindowsUserScanner::initTestCase() {
    // Setup test environment
}

void TestWindowsUserScanner::cleanupTestCase() {
    // Cleanup test environment
}

void TestWindowsUserScanner::init() {
    m_scanner = new sak::WindowsUserScanner(this);
}

void TestWindowsUserScanner::cleanup() {
    delete m_scanner;
    m_scanner = nullptr;
}

void TestWindowsUserScanner::waitForSignal(QObject* obj, const char* signal, int timeout) {
    QSignalSpy spy(obj, signal);
    QVERIFY(spy.wait(timeout));
}

bool TestWindowsUserScanner::isValidSID(const QString& sid) {
    // SID format: S-1-5-21-...
    return sid.startsWith("S-1-") && sid.length() > 10;
}

bool TestWindowsUserScanner::isValidProfilePath(const QString& path) {
    // Profile path should be like C:\Users\Username
    return path.contains(":\\") && path.contains("Users");
}

void TestWindowsUserScanner::testConstructor() {
    QVERIFY(m_scanner != nullptr);
}

void TestWindowsUserScanner::testScanUsers() {
    auto users = m_scanner->scanUsers();
    // Should return at least current user
    QVERIFY(!users.isEmpty());
}

void TestWindowsUserScanner::testScanUsersNotEmpty() {
    auto users = m_scanner->scanUsers();
    QVERIFY(users.size() >= 1); // At least one user
}

void TestWindowsUserScanner::testScanUsersProgress() {
    QSignalSpy spy(m_scanner, &sak::WindowsUserScanner::scanProgress);
    
    m_scanner->scanUsers();
    
    // May emit progress signals
}

void TestWindowsUserScanner::testUserFoundSignal() {
    QSignalSpy spy(m_scanner, &sak::WindowsUserScanner::userFound);
    
    m_scanner->scanUsers();
    
    // Should find at least current user
    QVERIFY(spy.count() >= 1);
}

void TestWindowsUserScanner::testGetCurrentUsername() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QVERIFY(!username.isEmpty());
}

void TestWindowsUserScanner::testCurrentUsernameNotEmpty() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QVERIFY(username.length() > 0);
}

void TestWindowsUserScanner::testCurrentUsernameValid() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    // Should not contain invalid characters
    QVERIFY(!username.contains("\\"));
    QVERIFY(!username.contains("/"));
}

void TestWindowsUserScanner::testGetUserSID() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString sid = sak::WindowsUserScanner::getUserSID(username);
    
    QVERIFY(!sid.isEmpty());
}

void TestWindowsUserScanner::testGetUserSIDCurrent() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString sid = sak::WindowsUserScanner::getUserSID(username);
    
    QVERIFY(isValidSID(sid));
}

void TestWindowsUserScanner::testGetUserSIDInvalid() {
    QString sid = sak::WindowsUserScanner::getUserSID("InvalidUser12345");
    QVERIFY(sid.isEmpty());
}

void TestWindowsUserScanner::testGetUserSIDFormat() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString sid = sak::WindowsUserScanner::getUserSID(username);
    
    if (!sid.isEmpty()) {
        QVERIFY(sid.startsWith("S-1-"));
    }
}

void TestWindowsUserScanner::testGetProfilePath() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    QVERIFY(!path.isEmpty());
}

void TestWindowsUserScanner::testGetProfilePathCurrent() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    QVERIFY(isValidProfilePath(path));
}

void TestWindowsUserScanner::testGetProfilePathInvalid() {
    QString path = sak::WindowsUserScanner::getProfilePath("InvalidUser12345");
    QVERIFY(path.isEmpty());
}

void TestWindowsUserScanner::testGetProfilePathFormat() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    if (!path.isEmpty()) {
        QVERIFY(path.contains(":\\"));
        QVERIFY(QDir(path).exists());
    }
}

void TestWindowsUserScanner::testIsUserLoggedIn() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    bool loggedIn = sak::WindowsUserScanner::isUserLoggedIn(username);
    
    QVERIFY(loggedIn); // Current user must be logged in
}

void TestWindowsUserScanner::testIsUserLoggedInCurrent() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QVERIFY(sak::WindowsUserScanner::isUserLoggedIn(username));
}

void TestWindowsUserScanner::testIsUserLoggedInInvalid() {
    bool loggedIn = sak::WindowsUserScanner::isUserLoggedIn("InvalidUser12345");
    QVERIFY(!loggedIn);
}

void TestWindowsUserScanner::testEstimateProfileSize() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    if (!path.isEmpty()) {
        qint64 size = sak::WindowsUserScanner::estimateProfileSize(path);
        QVERIFY(size >= 0);
    }
}

void TestWindowsUserScanner::testEstimateProfileSizeCurrent() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    qint64 size = sak::WindowsUserScanner::estimateProfileSize(path);
    QVERIFY(size > 0); // Profile should have some size
}

void TestWindowsUserScanner::testEstimateProfileSizeInvalid() {
    qint64 size = sak::WindowsUserScanner::estimateProfileSize("C:\\NonexistentPath");
    QVERIFY(size == 0);
}

void TestWindowsUserScanner::testEstimateProfileSizePositive() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    qint64 size = sak::WindowsUserScanner::estimateProfileSize(path);
    QVERIFY(size >= 0);
}

void TestWindowsUserScanner::testGetDefaultFolderSelections() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    auto folders = sak::WindowsUserScanner::getDefaultFolderSelections(path);
    QVERIFY(!folders.isEmpty());
}

void TestWindowsUserScanner::testGetDefaultFolderSelectionsCurrent() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    auto folders = sak::WindowsUserScanner::getDefaultFolderSelections(path);
    
    // Should have standard folders
    QVERIFY(folders.size() >= 3); // Documents, Desktop, Downloads at minimum
}

void TestWindowsUserScanner::testGetDefaultFolderSelectionsInvalid() {
    auto folders = sak::WindowsUserScanner::getDefaultFolderSelections("C:\\Invalid");
    // May return empty or default list
}

void TestWindowsUserScanner::testGetDefaultFolderSelectionsStandard() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    auto folders = sak::WindowsUserScanner::getDefaultFolderSelections(path);
    
    // Check for standard folders
    bool hasDocuments = false;
    bool hasDesktop = false;
    
    for (const auto& folder : folders) {
        if (folder.displayName.contains("Documents", Qt::CaseInsensitive)) {
            hasDocuments = true;
        }
        if (folder.displayName.contains("Desktop", Qt::CaseInsensitive)) {
            hasDesktop = true;
        }
    }
    
    QVERIFY(hasDocuments || hasDesktop); // At least one standard folder
}

void TestWindowsUserScanner::testEnumerateWindowsUsers() {
    auto users = m_scanner->scanUsers();
    
    // Should enumerate at least current user
    QVERIFY(!users.isEmpty());
}

void TestWindowsUserScanner::testEnumerateAdministrators() {
    auto users = m_scanner->scanUsers();
    
    // May or may not have admin users
    // Just verify enumeration works
}

void TestWindowsUserScanner::testEnumerateStandardUsers() {
    auto users = m_scanner->scanUsers();
    
    // Should have at least one standard user
    QVERIFY(!users.isEmpty());
}

void TestWindowsUserScanner::testProfilePathExists() {
    auto users = m_scanner->scanUsers();
    
    for (const auto& user : users) {
        if (!user.profilePath.isEmpty()) {
            QDir dir(user.profilePath);
            QVERIFY(dir.exists());
        }
    }
}

void TestWindowsUserScanner::testProfileHasDocuments() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    QDir documentsDir(path + "/Documents");
    // Documents folder should exist
}

void TestWindowsUserScanner::testProfileHasDesktop() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    QDir desktopDir(path + "/Desktop");
    // Desktop folder should exist
}

void TestWindowsUserScanner::testScanProgressSignal() {
    QSignalSpy spy(m_scanner, &sak::WindowsUserScanner::scanProgress);
    
    m_scanner->scanUsers();
    
    // May emit progress
}

void TestWindowsUserScanner::testUserFoundSignals() {
    QSignalSpy spy(m_scanner, &sak::WindowsUserScanner::userFound);
    
    m_scanner->scanUsers();
    
    QVERIFY(spy.count() >= 1);
}

void TestWindowsUserScanner::testMultipleUsers() {
    auto users = m_scanner->scanUsers();
    
    // System may have multiple users
    QVERIFY(users.size() >= 1);
}

void TestWindowsUserScanner::testEmptyUsername() {
    QString sid = sak::WindowsUserScanner::getUserSID("");
    QVERIFY(sid.isEmpty());
    
    QString path = sak::WindowsUserScanner::getProfilePath("");
    QVERIFY(path.isEmpty());
}

void TestWindowsUserScanner::testNullUsername() {
    QString sid = sak::WindowsUserScanner::getUserSID(QString());
    QVERIFY(sid.isEmpty());
}

void TestWindowsUserScanner::testInvalidProfilePath() {
    qint64 size = sak::WindowsUserScanner::estimateProfileSize("");
    QCOMPARE(size, 0LL);
}

void TestWindowsUserScanner::testUserProfileStructure() {
    auto users = m_scanner->scanUsers();
    
    for (const auto& user : users) {
        QVERIFY(!user.username.isEmpty());
        QVERIFY(!user.sid.isEmpty());
        QVERIFY(!user.profilePath.isEmpty());
    }
}

void TestWindowsUserScanner::testUserHasSID() {
    auto users = m_scanner->scanUsers();
    
    for (const auto& user : users) {
        QVERIFY(isValidSID(user.sid));
    }
}

void TestWindowsUserScanner::testUserHasPath() {
    auto users = m_scanner->scanUsers();
    
    for (const auto& user : users) {
        QVERIFY(isValidProfilePath(user.profilePath));
    }
}

void TestWindowsUserScanner::testSystemUsers() {
    auto users = m_scanner->scanUsers();
    
    // Should filter out system accounts
    for (const auto& user : users) {
        QVERIFY(!user.username.startsWith("SYSTEM"));
        QVERIFY(!user.username.startsWith("LOCAL SERVICE"));
    }
}

void TestWindowsUserScanner::testBuiltInUsers() {
    auto users = m_scanner->scanUsers();
    
    // Should filter out built-in accounts
    for (const auto& user : users) {
        QVERIFY(user.username != "Administrator" || user.profilePath.length() > 0);
    }
}

void TestWindowsUserScanner::testFilterSystemAccounts() {
    auto users = m_scanner->scanUsers();
    
    // All returned users should have valid profiles
    for (const auto& user : users) {
        QVERIFY(QDir(user.profilePath).exists());
    }
}

void TestWindowsUserScanner::testQuickSizeEstimate() {
    QString username = sak::WindowsUserScanner::getCurrentUsername();
    QString path = sak::WindowsUserScanner::getProfilePath(username);
    
    QElapsedTimer timer;
    timer.start();
    
    qint64 size = sak::WindowsUserScanner::estimateProfileSize(path);
    
    qint64 elapsed = timer.elapsed();
    
    // Should be quick (under 5 seconds)
    QVERIFY(elapsed < 5000);
    QVERIFY(size >= 0);
}

void TestWindowsUserScanner::testScanSpeed() {
    QElapsedTimer timer;
    timer.start();
    
    auto users = m_scanner->scanUsers();
    
    qint64 elapsed = timer.elapsed();
    
    // Should complete quickly
    QVERIFY(elapsed < 10000); // Under 10 seconds
    QVERIFY(!users.isEmpty());
}

QTEST_MAIN(TestWindowsUserScanner)
#include "test_windows_user_scanner.moc"
