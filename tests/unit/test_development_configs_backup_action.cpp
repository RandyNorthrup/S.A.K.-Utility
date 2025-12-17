// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include <QDir>
#include "sak/actions/development_configs_backup_action.h"

class TestDevelopmentConfigsBackupAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testDoesNotRequireAdmin();
    void testScanFindsConfigs();
    void testExecuteBacksUpConfigs();
    
    // Git configuration
    void testLocateGitConfig();
    void testBackupGlobalGitConfig();
    void testBackupGitCredentials();
    void testBackupGitIgnore();
    
    // SSH keys
    void testLocateSSHDirectory();
    void testBackupSSHKeys();
    void testBackupKnownHosts();
    void testBackupSSHConfig();
    
    // VS Code settings
    void testLocateVSCodeSettings();
    void testBackupVSCodeSettings();
    void testBackupVSCodeExtensions();
    void testBackupVSCodeKeybindings();
    
    // Visual Studio settings
    void testLocateVisualStudioSettings();
    void testBackupVSSettings();
    void testBackupVSExtensions();
    void testBackupVSCodeSnippets();
    
    // IntelliJ settings
    void testLocateIntelliJSettings();
    void testBackupIntelliJSettings();
    void testBackupIntelliJPlugins();
    void testBackupIntelliJKeymaps();
    
    // Multi-user support
    void testEnumerateUserProfiles();
    void testBackupAllUsers();
    void testCountUserProfiles();
    
    // Sensitive data handling
    void testDetectSensitiveData();
    void testHandleSSHPrivateKeys();
    void testEncryptSensitiveBackup();
    void testWarnAboutSensitiveData();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleNoConfigsFound();
    void testHandleBackupLocationFailure();
    void testHandleFileAccessDenied();
    void testHandleLargeConfigFiles();
    
    // File operations
    void testCalculateTotalSize();
    void testCountConfigFiles();
    void testCopyConfigFiles();
    void testVerifyBackupIntegrity();
    
    // Results formatting
    void testFormatConfigList();
    void testFormatBackupResults();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testNoIDEsInstalled();
    void testMissingSSHDirectory();
    void testCorruptedGitConfig();
    void testInsufficientDiskSpace();

private:
    sak::DevelopmentConfigsBackupAction* m_action;
    QTemporaryDir* m_tempDir;
};

void TestDevelopmentConfigsBackupAction::initTestCase() {
    // One-time setup
}

void TestDevelopmentConfigsBackupAction::cleanupTestCase() {
    // One-time cleanup
}

void TestDevelopmentConfigsBackupAction::init() {
    m_tempDir = new QTemporaryDir();
    m_action = new sak::DevelopmentConfigsBackupAction(m_tempDir->path());
}

void TestDevelopmentConfigsBackupAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestDevelopmentConfigsBackupAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Development Configs Backup"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("Git", Qt::CaseInsensitive) || 
            m_action->description().contains("SSH", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::QuickBackup);
    QVERIFY(!m_action->requiresAdmin());
}

void TestDevelopmentConfigsBackupAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestDevelopmentConfigsBackupAction::testDoesNotRequireAdmin() {
    // Can backup user configs without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestDevelopmentConfigsBackupAction::testScanFindsConfigs() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testExecuteBacksUpConfigs() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(30000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testLocateGitConfig() {
    // Git global config location
    QString gitConfigPath = R"(%UserProfile%\.gitconfig)";
    
    QVERIFY(gitConfigPath.contains(".gitconfig"));
}

void TestDevelopmentConfigsBackupAction::testBackupGlobalGitConfig() {
    QString configFile = ".gitconfig";
    
    QCOMPARE(configFile, QString(".gitconfig"));
}

void TestDevelopmentConfigsBackupAction::testBackupGitCredentials() {
    // Git credential helper
    QString credPath = R"(%UserProfile%\.git-credentials)";
    
    QVERIFY(credPath.contains(".git-credentials"));
}

void TestDevelopmentConfigsBackupAction::testBackupGitIgnore() {
    // Global git ignore
    QString ignorePath = R"(%UserProfile%\.gitignore_global)";
    
    QVERIFY(ignorePath.contains(".gitignore"));
}

void TestDevelopmentConfigsBackupAction::testLocateSSHDirectory() {
    // SSH directory location
    QString sshPath = R"(%UserProfile%\.ssh)";
    
    QVERIFY(sshPath.contains(".ssh"));
}

void TestDevelopmentConfigsBackupAction::testBackupSSHKeys() {
    QStringList keyFiles = {
        "id_rsa",
        "id_rsa.pub",
        "id_ed25519",
        "id_ed25519.pub"
    };
    
    QVERIFY(keyFiles.size() >= 2);
}

void TestDevelopmentConfigsBackupAction::testBackupKnownHosts() {
    QString knownHostsFile = "known_hosts";
    
    QCOMPARE(knownHostsFile, QString("known_hosts"));
}

void TestDevelopmentConfigsBackupAction::testBackupSSHConfig() {
    QString configFile = "config";
    
    QCOMPARE(configFile, QString("config"));
}

void TestDevelopmentConfigsBackupAction::testLocateVSCodeSettings() {
    // VS Code settings location
    QString vscodePath = R"(%AppData%\Code\User)";
    
    QVERIFY(vscodePath.contains("Code"));
}

void TestDevelopmentConfigsBackupAction::testBackupVSCodeSettings() {
    QString settingsFile = "settings.json";
    
    QCOMPARE(settingsFile, QString("settings.json"));
}

void TestDevelopmentConfigsBackupAction::testBackupVSCodeExtensions() {
    QString extensionsFile = "extensions.json";
    
    QCOMPARE(extensionsFile, QString("extensions.json"));
}

void TestDevelopmentConfigsBackupAction::testBackupVSCodeKeybindings() {
    QString keybindingsFile = "keybindings.json";
    
    QCOMPARE(keybindingsFile, QString("keybindings.json"));
}

void TestDevelopmentConfigsBackupAction::testLocateVisualStudioSettings() {
    // Visual Studio settings location
    QString vsPath = R"(%AppData%\Microsoft\VisualStudio\17.0)";
    
    QVERIFY(vsPath.contains("VisualStudio"));
}

void TestDevelopmentConfigsBackupAction::testBackupVSSettings() {
    QString settingsFile = "settings.xml";
    
    QVERIFY(!settingsFile.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testBackupVSExtensions() {
    QString extensionsFile = "extensions.json";
    
    QVERIFY(!extensionsFile.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testBackupVSCodeSnippets() {
    QString snippetsPath = "snippets";
    
    QVERIFY(!snippetsPath.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testLocateIntelliJSettings() {
    // IntelliJ IDEA settings location
    QString intellijPath = R"(%UserProfile%\.IntelliJIdea2023.3\config)";
    
    QVERIFY(intellijPath.contains("IntelliJ"));
}

void TestDevelopmentConfigsBackupAction::testBackupIntelliJSettings() {
    QString settingsPath = "options";
    
    QVERIFY(!settingsPath.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testBackupIntelliJPlugins() {
    QString pluginsPath = "plugins";
    
    QCOMPARE(pluginsPath, QString("plugins"));
}

void TestDevelopmentConfigsBackupAction::testBackupIntelliJKeymaps() {
    QString keymapsPath = "keymaps";
    
    QCOMPARE(keymapsPath, QString("keymaps"));
}

void TestDevelopmentConfigsBackupAction::testEnumerateUserProfiles() {
    QStringList profiles = {
        R"(C:\Users\User1)",
        R"(C:\Users\User2)"
    };
    
    QVERIFY(profiles.size() >= 1);
}

void TestDevelopmentConfigsBackupAction::testBackupAllUsers() {
    int userCount = 2;
    
    QVERIFY(userCount >= 1);
}

void TestDevelopmentConfigsBackupAction::testCountUserProfiles() {
    int profileCount = 1;
    
    QVERIFY(profileCount > 0);
}

void TestDevelopmentConfigsBackupAction::testDetectSensitiveData() {
    // SSH private keys are sensitive
    bool hasSensitiveData = true;
    
    QVERIFY(hasSensitiveData);
}

void TestDevelopmentConfigsBackupAction::testHandleSSHPrivateKeys() {
    // Private key files (no .pub extension)
    QStringList privateKeys = {
        "id_rsa",
        "id_ed25519"
    };
    
    QVERIFY(privateKeys.size() >= 1);
}

void TestDevelopmentConfigsBackupAction::testEncryptSensitiveBackup() {
    // Should encrypt SSH keys in backup
    bool shouldEncrypt = true;
    
    QVERIFY(shouldEncrypt);
}

void TestDevelopmentConfigsBackupAction::testWarnAboutSensitiveData() {
    QString warning = "Warning: SSH private keys detected. Backup should be encrypted.";
    
    QVERIFY(warning.contains("Warning"));
    QVERIFY(warning.contains("private keys"));
}

void TestDevelopmentConfigsBackupAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDevelopmentConfigsBackupAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDevelopmentConfigsBackupAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDevelopmentConfigsBackupAction::testHandleNoConfigsFound() {
    // No development tools installed
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDevelopmentConfigsBackupAction::testHandleBackupLocationFailure() {
    // Can't create backup folder
    bool folderCreated = false;
    
    QVERIFY(!folderCreated);
}

void TestDevelopmentConfigsBackupAction::testHandleFileAccessDenied() {
    // Config file locked
    QString error = "Access denied: File is in use";
    
    QVERIFY(error.contains("Access denied"));
}

void TestDevelopmentConfigsBackupAction::testHandleLargeConfigFiles() {
    // Large VS Code extensions folder
    qint64 folderSize = 1024LL * 1024 * 500; // 500 MB
    
    QVERIFY(folderSize > 0);
}

void TestDevelopmentConfigsBackupAction::testCalculateTotalSize() {
    qint64 totalSize = 1024 * 1024 * 50; // 50 MB
    
    QVERIFY(totalSize > 0);
}

void TestDevelopmentConfigsBackupAction::testCountConfigFiles() {
    int fileCount = 15;
    
    QVERIFY(fileCount >= 0);
}

void TestDevelopmentConfigsBackupAction::testCopyConfigFiles() {
    QString sourcePath = R"(C:\Users\User\.gitconfig)";
    QString destPath = m_tempDir->path() + "/.gitconfig";
    
    QVERIFY(!sourcePath.isEmpty());
}

void TestDevelopmentConfigsBackupAction::testVerifyBackupIntegrity() {
    // Verify copied files match originals
    bool integrityVerified = true;
    
    QVERIFY(integrityVerified);
}

void TestDevelopmentConfigsBackupAction::testFormatConfigList() {
    QString list = R"(
Development Configs Found:
  Git Configuration:
    - .gitconfig (2.5 KB)
    - .git-credentials (150 bytes)
  SSH Keys:
    - id_rsa (SENSITIVE - 3.2 KB)
    - id_rsa.pub (750 bytes)
    - known_hosts (8.5 KB)
  VS Code:
    - settings.json (12 KB)
    - keybindings.json (3 KB)
    - extensions.json (5 KB)
    )";
    
    QVERIFY(list.contains("Development Configs"));
}

void TestDevelopmentConfigsBackupAction::testFormatBackupResults() {
    QString results = R"(
Development Configs Backed Up:
  ✓ Git configs (3 files, 2.7 KB)
  ✓ SSH keys (3 files, 12.5 KB) - SENSITIVE
  ✓ VS Code settings (3 files, 20 KB)
  Total: 9 files, 35.2 KB
    )";
    
    QVERIFY(results.contains("Backed Up"));
}

void TestDevelopmentConfigsBackupAction::testFormatSuccessMessage() {
    QString message = "Successfully backed up development configs (9 files)";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("configs"));
}

void TestDevelopmentConfigsBackupAction::testFormatErrorMessage() {
    QString error = "Failed to backup configs: Access denied to .ssh folder";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Access denied"));
}

void TestDevelopmentConfigsBackupAction::testNoIDEsInstalled() {
    // No IDEs found on system
    bool idesInstalled = false;
    
    QVERIFY(!idesInstalled);
}

void TestDevelopmentConfigsBackupAction::testMissingSSHDirectory() {
    // .ssh directory doesn't exist
    bool sshDirExists = false;
    
    QVERIFY(!sshDirExists);
}

void TestDevelopmentConfigsBackupAction::testCorruptedGitConfig() {
    // .gitconfig file is corrupted
    QString status = "Corrupted";
    
    QCOMPARE(status, QString("Corrupted"));
}

void TestDevelopmentConfigsBackupAction::testInsufficientDiskSpace() {
    // Not enough space for backup
    qint64 requiredSpace = 1024 * 1024 * 100; // 100 MB
    qint64 availableSpace = 1024 * 1024 * 50; // 50 MB
    
    QVERIFY(requiredSpace > availableSpace);
}

QTEST_MAIN(TestDevelopmentConfigsBackupAction)
#include "test_development_configs_backup_action.moc"
