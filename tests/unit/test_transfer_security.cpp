#include <QtTest/QtTest>

#include "sak/network_transfer_security.h"

using namespace sak;

class TransferSecurityTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void aesGcmRoundtrip();
};

void TransferSecurityTests::aesGcmRoundtrip() {
    const QString passphrase = "test-passphrase";
    const QByteArray salt = TransferSecurityManager::generateRandomBytes(16);
    QVERIFY(!salt.isEmpty());

    auto keyResult = TransferSecurityManager::deriveKey(passphrase, salt);
    QVERIFY(keyResult.has_value());

    QByteArray plaintext = "Hello transfer";
    auto encrypted = TransferSecurityManager::encryptAesGcm(plaintext, *keyResult, "aad");
    QVERIFY(encrypted.has_value());

    auto decrypted = TransferSecurityManager::decryptAesGcm(*encrypted, *keyResult, "aad");
    QVERIFY(decrypted.has_value());
    QCOMPARE(*decrypted, plaintext);
}

QTEST_MAIN(TransferSecurityTests)
#include "test_transfer_security.moc"
