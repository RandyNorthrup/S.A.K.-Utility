// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_installation_busy.cpp
/// @brief Unit tests for the App Installation panel's single in-flight authority.
/// R5 p11_gui-9: the online install path and the offline deployment path each
/// checked only their OWN worker, so an offline bundle install could start on top
/// of a running online Chocolatey install (both mutate the same machine
/// locations). One predicate now answers for both.

#include "sak/app_installation_busy.h"

#include <QtTest/QtTest>

class AppInstallationBusyTests : public QObject {
    Q_OBJECT

private Q_SLOTS:

    // Nothing in flight is the only idle state.
    void idleWhenNoPathIsClaimedOrRunning() {
        QVERIFY(!sak::packageOperationInFlight(sak::PackageOperationActivity{}));
    }

    // Each individual source makes the panel busy on its own.
    void everySourceMakesThePanelBusy() {
        sak::PackageOperationActivity activity;
        activity.online_claimed = true;
        QVERIFY(sak::packageOperationInFlight(activity));

        activity = {};
        activity.online_worker_running = true;
        QVERIFY(sak::packageOperationInFlight(activity));

        activity = {};
        activity.offline_claimed = true;
        QVERIFY(sak::packageOperationInFlight(activity));

        activity = {};
        activity.offline_worker_running = true;
        QVERIFY(sak::packageOperationInFlight(activity));
    }

    // The finding itself: an ONLINE install in flight must make the panel busy for the
    // OFFLINE entry points, and vice versa. A per-path predicate would answer "idle" for
    // the other path and let two concurrent choco runs start.
    void oneInFlightPathBlocksTheOther() {
        sak::PackageOperationActivity online_running;
        online_running.online_claimed = true;
        online_running.online_worker_running = true;
        QVERIFY(sak::packageOperationInFlight(online_running));
        // Offline entry points read the same predicate, so they see the online work.
        QVERIFY(!online_running.offline_claimed);
        QVERIFY(!online_running.offline_worker_running);

        sak::PackageOperationActivity offline_running;
        offline_running.offline_claimed = true;
        offline_running.offline_worker_running = true;
        QVERIFY(sak::packageOperationInFlight(offline_running));
        QVERIFY(!offline_running.online_claimed);
        QVERIFY(!offline_running.online_worker_running);
    }

    // The claim flag covers the dispatch window before the worker thread reports running,
    // so the gate never has a hole between "install dispatched" and "worker started".
    void claimCoversDispatchWindowBeforeWorkerReportsRunning() {
        sak::PackageOperationActivity dispatching;
        dispatching.online_claimed = true;
        dispatching.online_worker_running = false;
        QVERIFY(sak::packageOperationInFlight(dispatching));

        sak::PackageOperationActivity offline_dispatching;
        offline_dispatching.offline_claimed = true;
        offline_dispatching.offline_worker_running = false;
        QVERIFY(sak::packageOperationInFlight(offline_dispatching));
    }
};

QTEST_GUILESS_MAIN(AppInstallationBusyTests)
#include "test_app_installation_busy.moc"
