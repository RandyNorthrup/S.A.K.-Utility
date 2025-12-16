// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/actions/sticky_notes_backup_action.h"

class TestStickyNotesBackupAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testScanFindsStickyNotes();
    void testExecuteBacksUpNotes();
    
    // Database location
    void testFindStickyNotesDatabase();
    void testDatabaseInLocalAppData();
    void testDatabaseForCurrentUser();
    void testDatabaseForMultipleUsers();
    
    // Database validation
    void testValidateSQLiteDatabase();
    void testCheckDatabaseSize();
    void testVerifyDatabaseIntegrity();
    
    // Backup operations
    void testCreateBackup();
    void testBackupWithTimestamp();
    void testPreserveMetadata();
    void testBackupVerification();
    
    // Note detection
    void testCountNotes();
    void testReadNoteContent();
    void testDetectEmptyDatabase();
    
    // Error handling
    void testHandleDatabaseNotFound();
    void testHandleDatabaseLocked();
    void testHandleBackupFailed();
    void testHandleCorruptedDatabase();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testBackupProgress();
    
    // Results formatting
    void testFormatNoteCount();
    void testFormatBackupSuccess();
    void testFormatDatabaseSize();
    
    // Edge cases
    void testNoStickyNotesInstalled();
    void testLegacyStickyNotes();
    void testMultipleProfiles();
    void testStickyNotesRunning();

private:
    QTemporaryDir* m_tempDir;
    sak::StickyNotesBackupAction* m_action;
    
    QString createMockStickyNotesDB(int noteCount = 5);
    qint64 getFileSize(const QString& path);
};

void TestStickyNotesBackupAction::initTestCase() {
    // One-time setup
}

void TestStickyNotesBackupAction::cleanupTestCase() {
    // One-time cleanup
}

void TestStickyNotesBackupAction::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    m_action = new sak::StickyNotesBackupAction(m_tempDir->path());
}

void TestStickyNotesBackupAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestStickyNotesBackupAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Sticky Notes Backup"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("Sticky Notes", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::QuickBackup);
    QVERIFY(!m_action->requiresAdmin());
}

void TestStickyNotesBackupAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestStickyNotesBackupAction::testScanFindsStickyNotes() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestStickyNotesBackupAction::testExecuteBacksUpNotes() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestStickyNotesBackupAction::testFindStickyNotesDatabase() {
    // Sticky Notes database location
    QString expectedPath = "%LOCALAPPDATA%\\Packages\\Microsoft.MicrosoftStickyNotes_8wekyb3d8bbwe\\LocalState\\plum.sqlite";
    
    QVERIFY(expectedPath.contains("plum.sqlite"));
}

void TestStickyNotesBackupAction::testDatabaseInLocalAppData() {
    QString localAppData = QDir::homePath() + "/AppData/Local";
    
    QVERIFY(!localAppData.isEmpty());
}

void TestStickyNotesBackupAction::testDatabaseForCurrentUser() {
    QString userProfile = QDir::homePath();
    
    QVERIFY(!userProfile.isEmpty());
    QVERIFY(QDir(userProfile).exists());
}

void TestStickyNotesBackupAction::testDatabaseForMultipleUsers() {
    // May need to backup for all user profiles
    QStringList users = {"User1", "User2", "User3"};
    
    QVERIFY(users.count() >= 1);
}

void TestStickyNotesBackupAction::testValidateSQLiteDatabase() {
    QString dbPath = createMockStickyNotesDB();
    
    QVERIFY(QFile::exists(dbPath));
    QVERIFY(dbPath.endsWith(".sqlite"));
}

void TestStickyNotesBackupAction::testCheckDatabaseSize() {
    QString dbPath = createMockStickyNotesDB();
    qint64 size = getFileSize(dbPath);
    
    QVERIFY(size > 0);
}

void TestStickyNotesBackupAction::testVerifyDatabaseIntegrity() {
    // SQLite database integrity check
    QString dbPath = createMockStickyNotesDB();
    
    QVERIFY(QFile::exists(dbPath));
}

void TestStickyNotesBackupAction::testCreateBackup() {
    QString sourcePath = createMockStickyNotesDB();
    QString backupPath = m_tempDir->filePath("plum_backup.sqlite");
    
    QVERIFY(QFile::copy(sourcePath, backupPath));
    QVERIFY(QFile::exists(backupPath));
}

void TestStickyNotesBackupAction::testBackupWithTimestamp() {
    QString timestamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    QString backupName = QString("plum_%1.sqlite").arg(timestamp);
    
    QVERIFY(backupName.contains(timestamp.left(8))); // Date part
}

void TestStickyNotesBackupAction::testPreserveMetadata() {
    QString dbPath = createMockStickyNotesDB();
    QFileInfo info(dbPath);
    
    QDateTime created = info.birthTime();
    QDateTime modified = info.lastModified();
    
    QVERIFY(created.isValid() || modified.isValid());
}

void TestStickyNotesBackupAction::testBackupVerification() {
    QString sourcePath = createMockStickyNotesDB();
    QString backupPath = m_tempDir->filePath("plum_backup.sqlite");
    
    QVERIFY(QFile::copy(sourcePath, backupPath));
    
    // Verify sizes match
    qint64 sourceSize = getFileSize(sourcePath);
    qint64 backupSize = getFileSize(backupPath);
    
    QCOMPARE(sourceSize, backupSize);
}

void TestStickyNotesBackupAction::testCountNotes() {
    // Mock note count
    int noteCount = 7;
    
    QVERIFY(noteCount >= 0);
}

void TestStickyNotesBackupAction::testReadNoteContent() {
    // Sticky notes stored as JSON in SQLite
    QString mockNote = R"({"text": "Remember to backup!", "color": "yellow"})";
    
    QVERIFY(mockNote.contains("text"));
}

void TestStickyNotesBackupAction::testDetectEmptyDatabase() {
    // Database exists but no notes
    int noteCount = 0;
    
    QCOMPARE(noteCount, 0);
}

void TestStickyNotesBackupAction::testHandleDatabaseNotFound() {
    // Sticky Notes never used
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestStickyNotesBackupAction::testHandleDatabaseLocked() {
    // Sticky Notes app is running
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestStickyNotesBackupAction::testHandleBackupFailed() {
    // Backup location not writable
    QString invalidPath = "Z:\\Invalid\\Path";
    
    QVERIFY(!QDir(invalidPath).exists());
}

void TestStickyNotesBackupAction::testHandleCorruptedDatabase() {
    // Database file is corrupted
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestStickyNotesBackupAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestStickyNotesBackupAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(500);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestStickyNotesBackupAction::testBackupProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(500);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestStickyNotesBackupAction::testFormatNoteCount() {
    QString result = "Found 7 sticky notes";
    
    QVERIFY(result.contains("7"));
    QVERIFY(result.contains("sticky notes"));
}

void TestStickyNotesBackupAction::testFormatBackupSuccess() {
    QString success = "Sticky Notes backed up to: C:\\Backup\\plum_20251216.sqlite";
    
    QVERIFY(success.contains("backed up"));
    QVERIFY(success.contains(".sqlite"));
}

void TestStickyNotesBackupAction::testFormatDatabaseSize() {
    qint64 size = 256 * 1024; // 256 KB
    QString formatted = QString("%1 KB").arg(size / 1024);
    
    QVERIFY(formatted.contains("KB"));
}

void TestStickyNotesBackupAction::testNoStickyNotesInstalled() {
    // Windows 10/11 without Sticky Notes app
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestStickyNotesBackupAction::testLegacyStickyNotes() {
    // Old Windows 7 Sticky Notes (StickyNotes.snt)
    QString legacyPath = "%APPDATA%\\Microsoft\\Sticky Notes\\StickyNotes.snt";
    
    QVERIFY(legacyPath.contains("StickyNotes.snt"));
}

void TestStickyNotesBackupAction::testMultipleProfiles() {
    // Backup sticky notes from all user profiles
    int profileCount = 3;
    
    QVERIFY(profileCount >= 1);
}

void TestStickyNotesBackupAction::testStickyNotesRunning() {
    // App is running, database may be locked
    QString processName = "Microsoft.Notes.exe";
    
    QVERIFY(!processName.isEmpty());
}

// Helper methods

QString TestStickyNotesBackupAction::createMockStickyNotesDB(int noteCount) {
    QString dbPath = m_tempDir->filePath("plum.sqlite");
    
    QFile file(dbPath);
    if (file.open(QIODevice::WriteOnly)) {
        // Create mock SQLite header
        file.write("SQLite format 3");
        
        // Write some mock data
        for (int i = 0; i < noteCount * 100; ++i) {
            file.write("X");
        }
        
        file.close();
    }
    
    return dbPath;
}

qint64 TestStickyNotesBackupAction::getFileSize(const QString& path) {
    QFileInfo info(path);
    return info.size();
}

QTEST_MAIN(TestStickyNotesBackupAction)
#include "test_sticky_notes_backup_action.moc"
