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
    void binaryChecksumMatches_verifiesNamedAndInferredAlgorithms();
    void binaryChecksumMatches_rejectsMismatchAndUnresolvable();
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

void TestPackageInternalizationEngine::binaryChecksumMatches_verifiesNamedAndInferredAlgorithms() {
    const QByteArray data = QByteArrayLiteral("hello");
    // Known digests of "hello".
    const QString md5 = QStringLiteral("5d41402abc4b2a76b9719d911017c592");
    const QString sha1 = QStringLiteral("aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
    const QString sha256 =
        QStringLiteral("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

    // Explicit type hint (case-insensitive on both sides).
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QStringLiteral("md5")));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, sha256.toUpper(), QStringLiteral("sha256")));
    QVERIFY(
        PackageInternalizationEngine::binaryChecksumMatches(data, sha1, QStringLiteral("SHA1")));

    // No type hint -> algorithm inferred from the checksum's hex length.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, sha256, QString()));

    // Empty expected checksum: nothing declared, so nothing to fail on.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, QString(), QString()));
}

void TestPackageInternalizationEngine::binaryChecksumMatches_rejectsMismatchAndUnresolvable() {
    const QByteArray data = QByteArrayLiteral("hello");
    // Wrong digest for the data.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("00000000000000000000000000000000"), QStringLiteral("md5")));
    // Declared but unresolvable: unknown algorithm name.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("deadbeef"), QStringLiteral("crc32")));
    // Declared but unresolvable: no hint and a nonstandard hex length.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("deadbeef"), QString()));
}

QTEST_APPLESS_MAIN(TestPackageInternalizationEngine)
#include "test_package_internalization_engine.moc"
