// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/**
 * @file test_network_transfer_controller.cpp
 * @brief TST-08 — Unit tests for NetworkTransferController.
 *
 * Tests configuration management, mode transitions, pause / resume /
 * cancel state machine behaviour, bandwidth clamping, and signal
 * emissions — all without requiring actual network connections.
 *
 * The controller's constructor creates live PeerDiscoveryService,
 * NetworkConnectionManager, etc.  These will bind sockets only when
 * startSource()/startDestination() are called.  Tests that call those
 * methods use a running event loop (QTEST_MAIN) and verify observable
 * side-effects via QSignalSpy.
 */

#include <QtTest>
#include <QSignalSpy>

#include "sak/network_transfer_controller.h"

using Mode = sak::NetworkTransferController::Mode;

class NetworkTransferControllerTests : public QObject {
    Q_OBJECT

private slots:
    // ---- Initial state ----
    void defaultModeIsIdle();
    void defaultSettingsAreEmpty();

    // ---- Configuration ----
    void configureRoundTrip();
    void configureOverwrite();

    // ---- stop() ----
    void stopFromIdleIsNoOp();
    void stopIsIdempotent();

    // ---- approveTransfer() guard ----
    void approveTransferWrongModeIsNoOp();

    // ---- pauseTransfer / resumeTransfer ----
    void pauseEmitsStatus();
    void pauseIsIdempotent();
    void resumeWhenNotPausedIsNoOp();

    // ---- cancelTransfer ----
    void cancelFromIdleIsNoOp();

    // ---- updateBandwidthLimit ----
    void bandwidthLimitClampsNegative();
    void bandwidthLimitAcceptsZero();
    void bandwidthLimitAcceptsPositive();

    // ---- mode() accessor ----
    void modeReturnsIdle();

    // ---- startDestination integration ----
    void startDestinationSetsModeAndEmitsStatus();
    void startDestinationThenStopReturnsToIdle();

    // ---- cancelTransfer in Destination mode ----
    void cancelTransferInDestinationClearsState();

    // ---- resumeTransfer round-trip ----
    void pauseThenResumeEmitsBothSignals();
};

// ===========================================================================
// Initial state
// ===========================================================================

void NetworkTransferControllerTests::defaultModeIsIdle()
{
    sak::NetworkTransferController ctrl;
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

void NetworkTransferControllerTests::defaultSettingsAreEmpty()
{
    sak::NetworkTransferController ctrl;
    const auto s = ctrl.settings();
    // Default-constructed TransferSettings — verify defaults are set appropriately.
    // encryption_enabled defaults to true (secure-by-default).
    QVERIFY(s.encryption_enabled);
}

// ===========================================================================
// Configuration
// ===========================================================================

void NetworkTransferControllerTests::configureRoundTrip()
{
    sak::NetworkTransferController ctrl;

    sak::TransferSettings ts;
    ts.encryption_enabled   = true;
    ts.compression_enabled  = true;
    ts.control_port         = 12345;
    ts.data_port            = 12346;
    ts.discovery_port       = 12347;
    ts.max_bandwidth_kbps   = 5000;
    ts.auto_discovery_enabled = false;

    ctrl.configure(ts);
    const auto out = ctrl.settings();

    QCOMPARE(out.encryption_enabled,    true);
    QCOMPARE(out.compression_enabled,   true);
    QCOMPARE(out.control_port,          static_cast<quint16>(12345));
    QCOMPARE(out.data_port,             static_cast<quint16>(12346));
    QCOMPARE(out.discovery_port,        static_cast<quint16>(12347));
    QCOMPARE(out.max_bandwidth_kbps,    5000);
    QCOMPARE(out.auto_discovery_enabled, false);
}

void NetworkTransferControllerTests::configureOverwrite()
{
    sak::NetworkTransferController ctrl;

    sak::TransferSettings ts1;
    ts1.encryption_enabled = true;
    ts1.control_port = 100;
    ctrl.configure(ts1);

    sak::TransferSettings ts2;
    ts2.encryption_enabled = false;
    ts2.control_port = 200;
    ctrl.configure(ts2);

    QCOMPARE(ctrl.settings().encryption_enabled, false);
    QCOMPARE(ctrl.settings().control_port, static_cast<quint16>(200));
}

// ===========================================================================
// stop()
// ===========================================================================

void NetworkTransferControllerTests::stopFromIdleIsNoOp()
{
    sak::NetworkTransferController ctrl;
    // Should not crash or produce signals when already Idle.
    ctrl.stop();
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

void NetworkTransferControllerTests::stopIsIdempotent()
{
    sak::NetworkTransferController ctrl;
    ctrl.stop();
    ctrl.stop();
    ctrl.stop();
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

// ===========================================================================
// approveTransfer() guard
// ===========================================================================

void NetworkTransferControllerTests::approveTransferWrongModeIsNoOp()
{
    sak::NetworkTransferController ctrl;

    // No crash, no error when called in Idle mode.
    QSignalSpy errorSpy(&ctrl, &sak::NetworkTransferController::errorMessage);
    QVERIFY(errorSpy.isValid());

    ctrl.approveTransfer(true);
    ctrl.approveTransfer(false);

    // In Idle mode, approveTransfer returns immediately — no errors emitted.
    QCOMPARE(errorSpy.count(), 0);
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

// ===========================================================================
// pauseTransfer / resumeTransfer
// ===========================================================================

void NetworkTransferControllerTests::pauseEmitsStatus()
{
    sak::NetworkTransferController ctrl;
    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.pauseTransfer();

    QCOMPARE(statusSpy.count(), 1);
    QVERIFY(statusSpy.first().at(0).toString().contains("paused", Qt::CaseInsensitive));
}

void NetworkTransferControllerTests::pauseIsIdempotent()
{
    sak::NetworkTransferController ctrl;
    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.pauseTransfer();
    ctrl.pauseTransfer();  // Second call should be a no-op.

    QCOMPARE(statusSpy.count(), 1);  // Only one "paused" message.
}

void NetworkTransferControllerTests::resumeWhenNotPausedIsNoOp()
{
    sak::NetworkTransferController ctrl;
    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.resumeTransfer();  // Never paused — should be no-op.

    QCOMPARE(statusSpy.count(), 0);
}

// ===========================================================================
// cancelTransfer
// ===========================================================================

void NetworkTransferControllerTests::cancelFromIdleIsNoOp()
{
    sak::NetworkTransferController ctrl;
    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    // In Idle mode, neither Source nor Destination branches execute.
    ctrl.cancelTransfer();

    QCOMPARE(statusSpy.count(), 0);
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

// ===========================================================================
// updateBandwidthLimit
// ===========================================================================

void NetworkTransferControllerTests::bandwidthLimitClampsNegative()
{
    sak::NetworkTransferController ctrl;
    ctrl.updateBandwidthLimit(-500);
    QCOMPARE(ctrl.settings().max_bandwidth_kbps, 0);
}

void NetworkTransferControllerTests::bandwidthLimitAcceptsZero()
{
    sak::NetworkTransferController ctrl;
    ctrl.updateBandwidthLimit(0);
    QCOMPARE(ctrl.settings().max_bandwidth_kbps, 0);
}

void NetworkTransferControllerTests::bandwidthLimitAcceptsPositive()
{
    sak::NetworkTransferController ctrl;
    ctrl.updateBandwidthLimit(10000);
    QCOMPARE(ctrl.settings().max_bandwidth_kbps, 10000);
}

// ===========================================================================
// mode() accessor
// ===========================================================================

void NetworkTransferControllerTests::modeReturnsIdle()
{
    sak::NetworkTransferController ctrl;
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

// ===========================================================================
// startDestination integration
// ===========================================================================

void NetworkTransferControllerTests::startDestinationSetsModeAndEmitsStatus()
{
    sak::NetworkTransferController ctrl;

    // Use high port numbers to avoid conflicts, disable auto-discovery
    // to prevent UDP broadcasts during tests.
    sak::TransferSettings ts;
    ts.control_port           = 49200;
    ts.data_port              = 49201;
    ts.discovery_port         = 49202;
    ts.auto_discovery_enabled = false;
    ts.encryption_enabled     = false;
    ctrl.configure(ts);

    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.startDestination(QStringLiteral("testpass"), QDir::tempPath());

    QCOMPARE(ctrl.mode(), Mode::Destination);

    // Should emit "Waiting for incoming connections".
    QVERIFY(statusSpy.count() >= 1);
    bool found = false;
    for (const auto& args : statusSpy) {
        if (args.at(0).toString().contains("Waiting", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY2(found, "Expected 'Waiting for incoming connections' status message");

    ctrl.stop();
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

void NetworkTransferControllerTests::startDestinationThenStopReturnsToIdle()
{
    sak::NetworkTransferController ctrl;

    sak::TransferSettings ts;
    ts.control_port           = 49210;
    ts.data_port              = 49211;
    ts.discovery_port         = 49212;
    ts.auto_discovery_enabled = false;
    ts.encryption_enabled     = false;
    ctrl.configure(ts);

    ctrl.startDestination(QStringLiteral("pass"), QDir::tempPath());
    QCOMPARE(ctrl.mode(), Mode::Destination);

    ctrl.stop();
    QCOMPARE(ctrl.mode(), Mode::Idle);
}

// ===========================================================================
// cancelTransfer in Destination mode
// ===========================================================================

void NetworkTransferControllerTests::cancelTransferInDestinationClearsState()
{
    sak::NetworkTransferController ctrl;

    sak::TransferSettings ts;
    ts.control_port           = 49220;
    ts.data_port              = 49221;
    ts.discovery_port         = 49222;
    ts.auto_discovery_enabled = false;
    ts.encryption_enabled     = false;
    ctrl.configure(ts);

    ctrl.startDestination(QStringLiteral("pass"), QDir::tempPath());
    QCOMPARE(ctrl.mode(), Mode::Destination);

    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.cancelTransfer();

    // Should still be in Destination mode (cancel doesn't call stop() for destination).
    QCOMPARE(ctrl.mode(), Mode::Destination);

    // Should emit "Transfer canceled".
    bool found = false;
    for (const auto& args : statusSpy) {
        if (args.at(0).toString().contains("canceled", Qt::CaseInsensitive)) {
            found = true;
            break;
        }
    }
    QVERIFY2(found, "Expected 'Transfer canceled' status message");

    ctrl.stop();
}

// ===========================================================================
// pauseTransfer + resumeTransfer round-trip
// ===========================================================================

void NetworkTransferControllerTests::pauseThenResumeEmitsBothSignals()
{
    sak::NetworkTransferController ctrl;

    QSignalSpy statusSpy(&ctrl, &sak::NetworkTransferController::statusMessage);
    QVERIFY(statusSpy.isValid());

    ctrl.pauseTransfer();
    ctrl.resumeTransfer();

    // Should have at least 2 status messages: "paused" and "resumed".
    QVERIFY(statusSpy.count() >= 2);

    bool pauseFound = false;
    bool resumeFound = false;
    for (const auto& args : statusSpy) {
        const QString msg = args.at(0).toString();
        if (msg.contains("paused", Qt::CaseInsensitive)) pauseFound = true;
        if (msg.contains("resumed", Qt::CaseInsensitive)) resumeFound = true;
    }
    QVERIFY2(pauseFound, "Expected 'paused' status message");
    QVERIFY2(resumeFound, "Expected 'resumed' status message");
}

// ===========================================================================

QTEST_MAIN(NetworkTransferControllerTests)
#include "test_network_transfer_controller.moc"
