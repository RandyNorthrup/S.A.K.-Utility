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
    void initTestCase();
    void construction_default();
    void construction_nonCopyable();
    void cancel_doesNotCrash();
    void networkShareInfo_defaults();
    void networkShareInfo_fieldAssignment();
    void discoverShares_emptyHostReportsFailureNotCompletion();
    void emitDiscoveryOutcome_partialNeverReportsComplete();
    void emitDiscoveryOutcome_cancelNamesTheCancel();
};

void TestNetworkShareBrowser::initTestCase() {
    qRegisterMetaType<NetworkShareInfo>();
    qRegisterMetaType<QVector<NetworkShareInfo>>();
}

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

void TestNetworkShareBrowser::discoverShares_emptyHostReportsFailureNotCompletion() {
    // A request that never reached NetShareEnum has no enumeration to report. Emitting
    // discoveryComplete({}) here told every listener "this host has zero shares" -- the exact
    // fail-open the discovery path must not have.
    NetworkShareBrowser browser;
    QSignalSpy complete(&browser, &NetworkShareBrowser::discoveryComplete);
    QSignalSpy failed(&browser, &NetworkShareBrowser::discoveryFailed);
    QSignalSpy errors(&browser, &NetworkShareBrowser::errorOccurred);

    browser.discoverShares(QString());

    QCOMPARE(complete.count(), 0);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(errors.count(), 1);
    QVERIFY(failed.at(0).at(0).value<QVector<NetworkShareInfo>>().isEmpty());
    QCOMPARE(failed.at(0).at(1).toString(), QStringLiteral("Hostname cannot be empty"));
}

void TestNetworkShareBrowser::emitDiscoveryOutcome_partialNeverReportsComplete() {
    // enumerateShares reports ok=false for a hard NetShareEnum error, a cancel that broke the
    // ERROR_MORE_DATA resume loop, or a buffer truncated mid-append. The terminal signal must
    // follow ok: the SAME share list is a discovery only when the read was authoritative.
    NetworkShareBrowser browser;
    QSignalSpy complete(&browser, &NetworkShareBrowser::discoveryComplete);
    QSignalSpy failed(&browser, &NetworkShareBrowser::discoveryFailed);

    NetworkShareInfo entry;
    entry.hostName = QStringLiteral("SERVER01");
    entry.shareName = QStringLiteral("Public");
    const QVector<NetworkShareInfo> shares{entry};

    browser.emitDiscoveryOutcome(shares, /*ok=*/false, QStringLiteral("SERVER01"));
    QCOMPARE(complete.count(), 0);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(0).value<QVector<NetworkShareInfo>>().size(), 1);
    QCOMPARE(failed.at(0).at(1).toString(),
             QStringLiteral("Share discovery on SERVER01 did not complete; the partial list is "
                            "not a complete enumeration"));

    browser.emitDiscoveryOutcome(shares, /*ok=*/true, QStringLiteral("SERVER01"));
    QCOMPARE(failed.count(), 1);  // unchanged: no second failure
    QCOMPARE(complete.count(), 1);
    QCOMPARE(complete.at(0).at(0).value<QVector<NetworkShareInfo>>().size(), 1);
}

void TestNetworkShareBrowser::emitDiscoveryOutcome_cancelNamesTheCancel() {
    // A cancel emits no errorOccurred of its own, so without a distinct terminal signal it
    // looked like a clean discovery that simply found fewer shares. The reason must name it.
    NetworkShareBrowser browser;
    QSignalSpy complete(&browser, &NetworkShareBrowser::discoveryComplete);
    QSignalSpy failed(&browser, &NetworkShareBrowser::discoveryFailed);

    browser.cancel();
    browser.emitDiscoveryOutcome({}, /*ok=*/false, QStringLiteral("SERVER01"));

    QCOMPARE(complete.count(), 0);
    QCOMPARE(failed.count(), 1);
    QCOMPARE(failed.at(0).at(1).toString(),
             QStringLiteral("Share discovery on SERVER01 was cancelled; the partial list is not "
                            "a complete enumeration"));
}

QTEST_MAIN(TestNetworkShareBrowser)
#include "test_network_share_browser.moc"
