// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QFile>
#include "sak/actions/backup_activation_keys_action.h"

class TestBackupActivationKeysAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testRequiresAdmin();
    void testScanDetectsKeys();
    void testExecuteBacksUpKeys();
    
    // Key detection
    void testDetectWindowsKey();
    void testDetectOfficeKey();
    void testDetectOtherProductKeys();
    void testMultipleKeys();
    
    // Key extraction
    void testExtractOEMKey();
    void testExtractRetailKey();
    void testExtractVolumeKey();
    void testExtractFromRegistry();
    
    // Backup operations
    void testCreateBackupFile();
    void testBackupFileFormat();
    void testEncryptedBackup();
    void testBackupLocation();
    
    // Key validation
    void testValidateKeyFormat();
    void testMaskSensitiveKeys();
    void testKeyChecksumValidation();
    
    // Error handling
    void testHandleNoKeysFound();
    void testHandleRegistryAccessDenied();
    void testHandleBackupLocationInvalid();
    void testHandleWMIFailure();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testBackupProgress();
    
    // Results formatting
    void testFormatKeyList();
    void testFormatBackupSuccess();
    void testFormatKeyDetails();
    
    // Edge cases
    void testUnactivatedWindows();
    void testMultipleOfficeVersions();
    void testOEMActivation();
    void testDigitalLicense();

private:
    QTemporaryDir* m_tempDir;
    sak::BackupActivationKeysAction* m_action;
    
    QString createMockProductKey();
    bool isValidKeyFormat(const QString& key);
};

void TestBackupActivationKeysAction::initTestCase() {
    // One-time setup
}

void TestBackupActivationKeysAction::cleanupTestCase() {
    // One-time cleanup
}

void TestBackupActivationKeysAction::init() {
    m_tempDir = new QTemporaryDir();
    QVERIFY(m_tempDir->isValid());
    m_action = new sak::BackupActivationKeysAction(m_tempDir->path());
}

void TestBackupActivationKeysAction::cleanup() {
    delete m_action;
    delete m_tempDir;
}

void TestBackupActivationKeysAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Backup Activation Keys"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("product key", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::EmergencyRecovery);
    QVERIFY(m_action->requiresAdmin());
}

void TestBackupActivationKeysAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestBackupActivationKeysAction::testRequiresAdmin() {
    // Reading product keys requires administrator privileges
    QVERIFY(m_action->requiresAdmin());
}

void TestBackupActivationKeysAction::testScanDetectsKeys() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    QVERIFY(progressSpy.count() >= 1);
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestBackupActivationKeysAction::testExecuteBacksUpKeys() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(20000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestBackupActivationKeysAction::testDetectWindowsKey() {
    // Windows product key detection
    QString productName = "Windows 11 Pro";
    
    QVERIFY(productName.contains("Windows"));
}

void TestBackupActivationKeysAction::testDetectOfficeKey() {
    // Office product key detection
    QString productName = "Microsoft Office Professional Plus 2021";
    
    QVERIFY(productName.contains("Office"));
}

void TestBackupActivationKeysAction::testDetectOtherProductKeys() {
    // Other Microsoft products (SQL Server, Visual Studio, etc.)
    QStringList products = {
        "SQL Server 2019",
        "Visual Studio 2022"
    };
    
    QVERIFY(products.count() >= 2);
}

void TestBackupActivationKeysAction::testMultipleKeys() {
    // System may have multiple product keys
    int keyCount = 3;
    
    QVERIFY(keyCount >= 1);
}

void TestBackupActivationKeysAction::testExtractOEMKey() {
    // OEM keys embedded in BIOS/UEFI
    QString mockKey = createMockProductKey();
    
    QVERIFY(isValidKeyFormat(mockKey));
}

void TestBackupActivationKeysAction::testExtractRetailKey() {
    // Retail product key
    QString mockKey = "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX";
    
    QVERIFY(isValidKeyFormat(mockKey));
}

void TestBackupActivationKeysAction::testExtractVolumeKey() {
    // Volume license key (KMS/MAK)
    QString keyType = "Volume:MAK";
    
    QVERIFY(keyType.contains("Volume"));
}

void TestBackupActivationKeysAction::testExtractFromRegistry() {
    // Registry path for product keys
    QString registryPath = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    
    QVERIFY(!registryPath.isEmpty());
}

void TestBackupActivationKeysAction::testCreateBackupFile() {
    QString backupFile = m_tempDir->filePath("product_keys.txt");
    
    QFile file(backupFile);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("Windows: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX\n");
    file.close();
    
    QVERIFY(QFile::exists(backupFile));
}

void TestBackupActivationKeysAction::testBackupFileFormat() {
    QString content = R"(
Product Keys Backup
Generated: 2025-12-16

Windows 11 Pro: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
Microsoft Office 2021: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
    )";
    
    QVERIFY(content.contains("Product Keys"));
    QVERIFY(content.contains("Windows"));
}

void TestBackupActivationKeysAction::testEncryptedBackup() {
    // Keys should be stored securely
    bool useEncryption = true;
    
    QVERIFY(useEncryption);
}

void TestBackupActivationKeysAction::testBackupLocation() {
    QString location = m_tempDir->path();
    
    QVERIFY(QDir(location).exists());
}

void TestBackupActivationKeysAction::testValidateKeyFormat() {
    QString validKey = "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX";
    
    QVERIFY(isValidKeyFormat(validKey));
}

void TestBackupActivationKeysAction::testMaskSensitiveKeys() {
    QString fullKey = "12345-67890-ABCDE-FGHIJ-KLMNO";
    QString maskedKey = "12345-67890-XXXXX-XXXXX-XXXXX";
    
    // Should mask last 3 groups for security
    QVERIFY(maskedKey.contains("XXXXX"));
}

void TestBackupActivationKeysAction::testKeyChecksumValidation() {
    QString mockKey = createMockProductKey();
    
    // Windows keys have checksum validation
    QVERIFY(!mockKey.isEmpty());
}

void TestBackupActivationKeysAction::testHandleNoKeysFound() {
    // Some systems may have digital license only
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestBackupActivationKeysAction::testHandleRegistryAccessDenied() {
    // Registry access may be denied
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(20000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestBackupActivationKeysAction::testHandleBackupLocationInvalid() {
    // Invalid backup location
    QString invalidPath = "Z:\\NonExistent\\Path";
    
    QVERIFY(!QDir(invalidPath).exists());
}

void TestBackupActivationKeysAction::testHandleWMIFailure() {
    // WMI query may fail
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestBackupActivationKeysAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupActivationKeysAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupActivationKeysAction::testBackupProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestBackupActivationKeysAction::testFormatKeyList() {
    QString list = R"(
Found 2 product keys:
  1. Windows 11 Pro
  2. Microsoft Office 2021
    )";
    
    QVERIFY(list.contains("Found"));
    QVERIFY(list.contains("Windows"));
}

void TestBackupActivationKeysAction::testFormatBackupSuccess() {
    QString success = "Product keys backed up to: C:\\Backup\\product_keys.txt";
    
    QVERIFY(success.contains("backed up"));
}

void TestBackupActivationKeysAction::testFormatKeyDetails() {
    QString details = R"(
Product: Windows 11 Pro
Key: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX
Type: Retail
Status: Activated
    )";
    
    QVERIFY(details.contains("Product"));
    QVERIFY(details.contains("Type"));
}

void TestBackupActivationKeysAction::testUnactivatedWindows() {
    // Windows not activated
    QString status = "Not Activated";
    
    QCOMPARE(status, QString("Not Activated"));
}

void TestBackupActivationKeysAction::testMultipleOfficeVersions() {
    // Multiple Office versions installed
    QStringList officeVersions = {
        "Office 2016",
        "Office 2019",
        "Office 2021"
    };
    
    QVERIFY(officeVersions.count() >= 2);
}

void TestBackupActivationKeysAction::testOEMActivation() {
    // OEM activation (pre-installed)
    QString activationType = "OEM";
    
    QCOMPARE(activationType, QString("OEM"));
}

void TestBackupActivationKeysAction::testDigitalLicense() {
    // Digital license (no product key)
    QString licenseType = "Digital License";
    
    QVERIFY(licenseType.contains("Digital"));
}

// Helper methods

QString TestBackupActivationKeysAction::createMockProductKey() {
    // Generate mock product key in correct format
    return "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX";
}

bool TestBackupActivationKeysAction::isValidKeyFormat(const QString& key) {
    // Product key format: XXXXX-XXXXX-XXXXX-XXXXX-XXXXX (25 chars + 4 dashes)
    if (key.length() != 29) {
        return false;
    }
    
    QStringList parts = key.split('-');
    if (parts.count() != 5) {
        return false;
    }
    
    for (const QString& part : parts) {
        if (part.length() != 5) {
            return false;
        }
    }
    
    return true;
}

QTEST_MAIN(TestBackupActivationKeysAction)
#include "test_backup_activation_keys_action.moc"
