// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/network_transfer_security.h"

#include <QtTest/QtTest>

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
