// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_share_browser.cpp
/// @brief Unit tests for NetworkShareBrowser

#include "sak/network_diagnostic_types.h"
#include "sak/network_share_browser.h"

#include <QtTest/QtTest>

using namespace sak;

class TestNetworkShareBrowser : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_nonCopyable();
    void cancel_doesNotCrash();
    void networkShareInfo_defaults();
    void networkShareInfo_fieldAssignment();
};

void TestNetworkShareBrowser::construction_default() {
    NetworkShareBrowser browser;
    QVERIFY(dynamic_cast<QObject*>(&browser) != nullptr);
}

void TestNetworkShareBrowser::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<NetworkShareBrowser>);
    QVERIFY(!std::is_move_constructible_v<NetworkShareBrowser>);
}

void TestNetworkShareBrowser::cancel_doesNotCrash() {
    NetworkShareBrowser browser;
    browser.cancel();
    QVERIFY(dynamic_cast<QObject*>(&browser) != nullptr);
}

void TestNetworkShareBrowser::networkShareInfo_defaults() {
    NetworkShareInfo info;
    QVERIFY(info.hostName.isEmpty());
    QVERIFY(info.shareName.isEmpty());
    QVERIFY(info.uncPath.isEmpty());
    QCOMPARE(info.type, NetworkShareInfo::ShareType::Disk);
    QVERIFY(info.remark.isEmpty());
    QVERIFY(!info.canRead);
    QVERIFY(!info.canWrite);
    QVERIFY(!info.requiresAuth);
    QVERIFY(info.accessError.isEmpty());
}

void TestNetworkShareBrowser::networkShareInfo_fieldAssignment() {
    NetworkShareInfo info;
    info.hostName = QStringLiteral("SERVER01");
    info.shareName = QStringLiteral("Share$");
    info.uncPath = QStringLiteral("\\\\SERVER01\\Share$");
    info.type = NetworkShareInfo::ShareType::IPC;
    info.canRead = true;
    info.canWrite = false;
    info.requiresAuth = true;

    QCOMPARE(info.hostName, QStringLiteral("SERVER01"));
    QCOMPARE(info.shareName, QStringLiteral("Share$"));
    QCOMPARE(info.type, NetworkShareInfo::ShareType::IPC);
    QVERIFY(info.canRead);
    QVERIFY(!info.canWrite);
    QVERIFY(info.requiresAuth);
}

QTEST_MAIN(TestNetworkShareBrowser)
#include "test_network_share_browser.moc"
