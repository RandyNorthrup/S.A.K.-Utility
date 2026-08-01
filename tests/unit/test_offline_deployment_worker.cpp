// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_offline_deployment_worker.cpp
/// @brief Unit tests for OfflineDeploymentWorker pure path-safety seams
///        (B10-13 work-dir ownership, B10-14 installer-filename confinement).

#include "sak/offline_deployment_worker.h"

#include <QtTest/QtTest>

using namespace sak;
using WorkDirDisposition = OfflineDeploymentWorker::WorkDirDisposition;

class TestOfflineDeploymentWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void classifyWorkDir_freshWhenMissingOrEmpty();
    void classifyWorkDir_reusesOwnedMarkedDir();
    void classifyWorkDir_refusesForeignNonEmptyDir();
    void safeInstallerFilename_keepsPlainNames();
    void safeInstallerFilename_confinesTraversalNames();
};

void TestOfflineDeploymentWorker::classifyWorkDir_freshWhenMissingOrEmpty() {
    // Absent dir: create it.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(false, false, false),
             WorkDirDisposition::CreateFresh);
    // Pre-existing EMPTY dir with no marker is safe to adopt.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, false, true),
             WorkDirDisposition::CreateFresh);
}

void TestOfflineDeploymentWorker::classifyWorkDir_reusesOwnedMarkedDir() {
    // A dir bearing our ownership marker is a leftover we may reuse and delete,
    // whether or not it currently holds files.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, true, false),
             WorkDirDisposition::ReuseOwned);
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, true, true),
             WorkDirDisposition::ReuseOwned);
}

void TestOfflineDeploymentWorker::classifyWorkDir_refusesForeignNonEmptyDir() {
    // A non-empty dir we never stamped must never be created-into or wiped.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, false, false),
             WorkDirDisposition::RefuseForeign);
}

void TestOfflineDeploymentWorker::safeInstallerFilename_keepsPlainNames() {
    const QString fb = QStringLiteral("fallback.bin");
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("setup.exe"), fb),
             QStringLiteral("setup.exe"));
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("Chrome_x64.msi"), fb),
             QStringLiteral("Chrome_x64.msi"));
    // A leading directory is stripped to its safe basename.
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("a/b/evil.exe"), fb),
             QStringLiteral("evil.exe"));
}

void TestOfflineDeploymentWorker::safeInstallerFilename_confinesTraversalNames() {
    const QString fb = QStringLiteral("pkg_installer_1");
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QString(), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("."), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral(".."), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("../../.."), fb), fb);
    // Whatever the platform makes of a backslash, the result never carries a
    // separator that could redirect the write outside the output dir.
    const QString bs =
        OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("bad\\path.exe"), fb);
    QVERIFY(!bs.contains(QLatin1Char('/')));
    QVERIFY(!bs.contains(QLatin1Char('\\')));
}

QTEST_APPLESS_MAIN(TestOfflineDeploymentWorker)
#include "test_offline_deployment_worker.moc"
