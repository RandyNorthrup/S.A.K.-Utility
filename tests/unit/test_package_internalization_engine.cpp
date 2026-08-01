// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_package_internalization_engine.cpp
/// @brief Unit tests for PackageInternalizationEngine pure seams (B10-11).

#include "sak/package_internalization_engine.h"

#include <QtTest/QtTest>

using namespace sak;

class TestPackageInternalizationEngine : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void isSafePackageComponent_acceptsNormalIdsAndVersions();
    void isSafePackageComponent_rejectsTraversalAndSeparators();
};

void TestPackageInternalizationEngine::isSafePackageComponent_acceptsNormalIdsAndVersions() {
    // Real NuGet ids (dotted) and versions (semver / prerelease / build metadata).
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("googlechrome")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("Microsoft.NET")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("7zip.install")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("1.2.3")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("1.2.3-beta.1")));
    QVERIFY(
        PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("2024.01.0+build5")));
}

void TestPackageInternalizationEngine::isSafePackageComponent_rejectsTraversalAndSeparators() {
    // A crafted id/version must not escape the work dir when concatenated into a path.
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QString()));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral(".")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("..")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("../evil")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a/b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a\\b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("C:evil")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a\nb")));
}

QTEST_APPLESS_MAIN(TestPackageInternalizationEngine)
#include "test_package_internalization_engine.moc"
