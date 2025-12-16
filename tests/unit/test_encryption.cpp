// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * Unit tests for Encryption
 * Tests encryption, decryption, and key management
 */

#include "sak/encryption.h"
#include <QTest>
#include <QByteArray>
#include <QString>

class TestEncryption : public QObject {
    Q_OBJECT

private slots:
    void testEncryptDecryptString() {
        QString plaintext = "Hello, World! This is a test message.";
        QString password = "SecurePassword123!";
        
        sak::Encryption crypto;
        
        // Encrypt
        QString encrypted = crypto.encryptString(plaintext, password);
        QVERIFY(!encrypted.isEmpty());
        QVERIFY(encrypted != plaintext);
        
        // Decrypt
        QString decrypted = crypto.decryptString(encrypted, password);
        QCOMPARE(decrypted, plaintext);
    }

    void testEncryptDecryptByteArray() {
        QByteArray data = "Binary data: \x00\x01\x02\xFF\xFE";
        QString password = "AnotherPassword456!";
        
        sak::Encryption crypto;
        
        QByteArray encrypted = crypto.encrypt(data, password);
        QVERIFY(!encrypted.isEmpty());
        QVERIFY(encrypted != data);
        
        QByteArray decrypted = crypto.decrypt(encrypted, password);
        QCOMPARE(decrypted, data);
    }

    void testWrongPassword() {
        QString plaintext = "Secret message";
        QString correctPassword = "Correct123!";
        QString wrongPassword = "Wrong456!";
        
        sak::Encryption crypto;
        
        QString encrypted = crypto.encryptString(plaintext, correctPassword);
        QString decrypted = crypto.decryptString(encrypted, wrongPassword);
        
        QVERIFY(decrypted.isEmpty() || decrypted != plaintext);
    }

    void testEmptyData() {
        QString password = "Password123!";
        sak::Encryption crypto;
        
        QString encrypted = crypto.encryptString("", password);
        QString decrypted = crypto.decryptString(encrypted, password);
        
        QCOMPARE(decrypted, QString(""));
    }

    void testLargeData() {
        QString password = "LargeData123!";
        QString plaintext(10000, 'x'); // 10KB of 'x' characters
        
        sak::Encryption crypto;
        
        QString encrypted = crypto.encryptString(plaintext, password);
        QString decrypted = crypto.decryptString(encrypted, password);
        
        QCOMPARE(decrypted, plaintext);
    }

    void testUnicodeData() {
        QString plaintext = "Unicode: 日本語 中文 한국어 العربية עברית";
        QString password = "Unicode123!";
        
        sak::Encryption crypto;
        
        QString encrypted = crypto.encryptString(plaintext, password);
        QString decrypted = crypto.decryptString(encrypted, password);
        
        QCOMPARE(decrypted, plaintext);
    }

    void testSpecialCharacters() {
        QString plaintext = "Special: !@#$%^&*()_+-=[]{}|;':\",./<>?";
        QString password = "Special123!";
        
        sak::Encryption crypto;
        
        QString encrypted = crypto.encryptString(plaintext, password);
        QString decrypted = crypto.decryptString(encrypted, password);
        
        QCOMPARE(decrypted, plaintext);
    }

    void testGenerateKey() {
        sak::Encryption crypto;
        
        QByteArray key1 = crypto.generateKey();
        QByteArray key2 = crypto.generateKey();
        
        QVERIFY(!key1.isEmpty());
        QVERIFY(!key2.isEmpty());
        QVERIFY(key1 != key2); // Keys should be random
        QCOMPARE(key1.size(), 32); // 256-bit key
    }

    void testGenerateSalt() {
        sak::Encryption crypto;
        
        QByteArray salt1 = crypto.generateSalt();
        QByteArray salt2 = crypto.generateSalt();
        
        QVERIFY(!salt1.isEmpty());
        QVERIFY(!salt2.isEmpty());
        QVERIFY(salt1 != salt2); // Salts should be random
    }

    void testHashPassword() {
        sak::Encryption crypto;
        
        QString password = "MyPassword123!";
        QByteArray hash1 = crypto.hashPassword(password);
        QByteArray hash2 = crypto.hashPassword(password);
        
        QVERIFY(!hash1.isEmpty());
        QCOMPARE(hash1, hash2); // Same password should produce same hash
    }

    void testVerifyPassword() {
        sak::Encryption crypto;
        
        QString password = "VerifyMe123!";
        QByteArray hash = crypto.hashPassword(password);
        
        QVERIFY(crypto.verifyPassword(password, hash));
        QVERIFY(!crypto.verifyPassword("WrongPassword", hash));
    }

    void testEncryptFile() {
        QTemporaryDir tempDir;
        QVERIFY(tempDir.isValid());
        
        QString plainFile = tempDir.path() + "/plain.txt";
        QString encryptedFile = tempDir.path() + "/encrypted.bin";
        QString decryptedFile = tempDir.path() + "/decrypted.txt";
        QString password = "FilePassword123!";
        
        // Create test file
        QFile file(plainFile);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("File content to encrypt");
        file.close();
        
        sak::Encryption crypto;
        
        // Encrypt file
        QVERIFY(crypto.encryptFile(plainFile, encryptedFile, password));
        QVERIFY(QFile::exists(encryptedFile));
        
        // Decrypt file
        QVERIFY(crypto.decryptFile(encryptedFile, decryptedFile, password));
        QVERIFY(QFile::exists(decryptedFile));
        
        // Verify content
        QFile decrypted(decryptedFile);
        QVERIFY(decrypted.open(QIODevice::ReadOnly));
        QString content = QString::fromUtf8(decrypted.readAll());
        decrypted.close();
        
        QCOMPARE(content, QString("File content to encrypt"));
    }

    void testEncryptionStrength() {
        QString plaintext = "Test message";
        QString password = "Password123!";
        
        sak::Encryption crypto;
        
        // Encrypt same plaintext multiple times
        QString enc1 = crypto.encryptString(plaintext, password);
        QString enc2 = crypto.encryptString(plaintext, password);
        
        // Should produce different ciphertext (due to random IV)
        QVERIFY(enc1 != enc2);
        
        // But both should decrypt to same plaintext
        QCOMPARE(crypto.decryptString(enc1, password), plaintext);
        QCOMPARE(crypto.decryptString(enc2, password), plaintext);
    }
};

QTEST_MAIN(TestEncryption)
#include "test_encryption.moc"
