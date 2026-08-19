// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_chocolatey_manager.cpp
/// @brief Unit tests for ChocolateyManager

#include "sak/action_constants.h"
#include "sak/chocolatey_manager.h"

#include <QtTest/QtTest>

#include <limits>

using namespace sak;

class TestChocolateyManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void construction_notInitialized();
    void result_defaults();
    void packageInfo_defaults();
    void installConfig_defaults();
    void parseSearchResults_emptyInput();
    void parseSearchResults_invalidInput();
    void timeout_defaultAndSet();
    void autoConfirm_defaultAndSet();

    // Input validation (B10-31)
    void validatePackageName_rejectsLeadingDashAndSeparators();
    void validateVersion_acceptsSemVerRejectsInjection();

    // Supply-chain hardening (CODEX REVIEW 2)
    void isTrustedChocoSigner_wholeWordOnly();
    void approvedInstallSource_pinnedUnlessUnofficial();

    // Supply-chain hardening (CODEX REVIEW 3)
    void isAuthenticChocoBinary_failsClosedOnMissingOrUnsigned();
    void validateExtraArgs_rejectsChecksumOverrides();
    void computeTimeoutMs_noIntegerOverflow();
    void setDefaultTimeout_rejectsNegative();
    void searchPackage_negativeMaxResultsFailsClosed();
};

void TestChocolateyManager::construction_default() {
    ChocolateyManager manager;
    QVERIFY(!manager.isInitialized());
    QVERIFY(manager.getChocoPath().isEmpty());
}

void TestChocolateyManager::construction_notInitialized() {
    ChocolateyManager manager;
    QVERIFY(!manager.isInitialized());
    // Operations on uninitialized manager should return failure
    ChocolateyManager::InstallConfig config;
    config.package_name = "test-package";
    const auto result = manager.installPackage(config);
    QVERIFY(!result.success);
}

void TestChocolateyManager::result_defaults() {
    ChocolateyManager::Result result;
    QVERIFY(!result.success);
    QVERIFY(result.output.isEmpty());
    QVERIFY(result.error_message.isEmpty());
    QCOMPARE(result.exit_code, -1);
}

void TestChocolateyManager::packageInfo_defaults() {
    ChocolateyManager::PackageInfo info;
    QVERIFY(info.package_id.isEmpty());
    QVERIFY(info.version.isEmpty());
    QVERIFY(info.title.isEmpty());
    QVERIFY(info.description.isEmpty());
    QVERIFY(!info.is_approved);
    QCOMPARE(info.download_count, 0);
}

void TestChocolateyManager::installConfig_defaults() {
    ChocolateyManager::InstallConfig config;
    QVERIFY(config.package_name.isEmpty());
    QVERIFY(config.version.isEmpty());
    QVERIFY(!config.version_locked);
    QVERIFY(config.auto_confirm);
    QVERIFY(!config.force);
    QVERIFY(!config.allow_unofficial);
    QCOMPARE(config.timeout_seconds, 0);
    QVERIFY(config.extra_args.isEmpty());
}

void TestChocolateyManager::parseSearchResults_emptyInput() {
    ChocolateyManager manager;
    const auto results = manager.parseSearchResults(QString());
    QVERIFY(results.empty());
}

void TestChocolateyManager::parseSearchResults_invalidInput() {
    ChocolateyManager manager;
    const auto results =
        manager.parseSearchResults(QStringLiteral("not a valid chocolatey output"));
    QVERIFY(results.empty());
}

void TestChocolateyManager::timeout_defaultAndSet() {
    ChocolateyManager manager;
    // Pin the documented default (kChocoTimeoutDefaultSec = 300). `>= 0` was a near-tautology that
    // could not catch a regression that changed or zeroed the default choco execution timeout.
    const int original_timeout = manager.getDefaultTimeout();
    QCOMPARE(original_timeout, sak::kChocoTimeoutDefaultSec);

    manager.setDefaultTimeout(60);
    QCOMPARE(manager.getDefaultTimeout(), 60);
}

void TestChocolateyManager::autoConfirm_defaultAndSet() {
    ChocolateyManager manager;
    const bool original_confirm = manager.getAutoConfirm();
    Q_UNUSED(original_confirm);

    manager.setAutoConfirm(false);
    QVERIFY(!manager.getAutoConfirm());

    manager.setAutoConfirm(true);
    QVERIFY(manager.getAutoConfirm());
}

void TestChocolateyManager::validatePackageName_rejectsLeadingDashAndSeparators() {
    // Real ids.
    QVERIFY(ChocolateyManager::validatePackageName("googlechrome"));
    QVERIFY(ChocolateyManager::validatePackageName("7zip.install"));
    QVERIFY(ChocolateyManager::validatePackageName("chocolatey-core.extension"));
    // Leading dash would be parsed by choco as an option -> injection.
    QVERIFY(!ChocolateyManager::validatePackageName("-force"));
    QVERIFY(!ChocolateyManager::validatePackageName("--source=http://evil"));
    // Separators / spaces / empty.
    QVERIFY(!ChocolateyManager::validatePackageName("a b"));
    QVERIFY(!ChocolateyManager::validatePackageName("a/b"));
    QVERIFY(!ChocolateyManager::validatePackageName(QString()));
}

void TestChocolateyManager::validateVersion_acceptsSemVerRejectsInjection() {
    // Valid NuGet/SemVer -- previously the dotted prerelease / build were rejected.
    QVERIFY(ChocolateyManager::validateVersion("1"));
    QVERIFY(ChocolateyManager::validateVersion("1.2.3"));
    QVERIFY(ChocolateyManager::validateVersion("1.2.3.4"));
    QVERIFY(ChocolateyManager::validateVersion("1.2.3-beta.1"));
    QVERIFY(ChocolateyManager::validateVersion("2024.01.0+build.5"));
    QVERIFY(ChocolateyManager::validateVersion("1.0.0-rc.1+exp.sha.5114f85"));
    // Injection / garbage.
    QVERIFY(!ChocolateyManager::validateVersion("-1.0"));
    QVERIFY(!ChocolateyManager::validateVersion("1.0 --force"));
    QVERIFY(!ChocolateyManager::validateVersion("latest"));
    QVERIFY(!ChocolateyManager::validateVersion(QString()));
}

void TestChocolateyManager::isTrustedChocoSigner_wholeWordOnly() {
    // The genuine Authenticode subject.
    QVERIFY(ChocolateyManager::isTrustedChocoSigner("Chocolatey Software, Inc."));
    QVERIFY(ChocolateyManager::isTrustedChocoSigner("chocolatey software, inc."));
    QVERIFY(ChocolateyManager::isTrustedChocoSigner("Chocolatey"));
    // A lookalike that merely embeds the token must be rejected (no word boundary).
    QVERIFY(!ChocolateyManager::isTrustedChocoSigner("NotChocolateyEvil Ltd"));
    QVERIFY(!ChocolateyManager::isTrustedChocoSigner("Chocolateyy Inc"));
    // Unsigned / unknown signer -> fail closed.
    QVERIFY(!ChocolateyManager::isTrustedChocoSigner(QString()));
    QVERIFY(!ChocolateyManager::isTrustedChocoSigner("Contoso Corp"));
}

void TestChocolateyManager::approvedInstallSource_pinnedUnlessUnofficial() {
    // Official install pins the approved community feed.
    const QString official = ChocolateyManager::approvedInstallSource(false);
    QVERIFY(!official.isEmpty());
    QVERIFY(official.startsWith(QStringLiteral("https://")));
    QVERIFY(official.contains(QStringLiteral("chocolatey.org")));
    // Opting into unofficial sources means no pin (caller accepts configured feeds).
    QVERIFY(ChocolateyManager::approvedInstallSource(true).isEmpty());
}

void TestChocolateyManager::isAuthenticChocoBinary_failsClosedOnMissingOrUnsigned() {
    // Empty path and a non-existent/unsigned file must never be treated as genuine
    // -- the execution-time authenticity gate fails closed.
    QVERIFY(!ChocolateyManager::isAuthenticChocoBinary(QString()));
    QVERIFY(!ChocolateyManager::isAuthenticChocoBinary(
        QStringLiteral("C:/nonexistent/definitely/not/here/choco.exe")));
}

void TestChocolateyManager::validateExtraArgs_rejectsChecksumOverrides() {
    // Benign extra args are allowed.
    QVERIFY(ChocolateyManager::validateExtraArgs(QStringList{}));
    QVERIFY(ChocolateyManager::validateExtraArgs(
        QStringList{QStringLiteral("--params"), QStringLiteral("/quiet")}));
    // Checksum/integrity overrides are refused (case-insensitive, with or without =).
    QVERIFY(
        !ChocolateyManager::validateExtraArgs(QStringList{QStringLiteral("--ignore-checksums")}));
    QVERIFY(!ChocolateyManager::validateExtraArgs(
        QStringList{QStringLiteral("--allow-empty-checksums")}));
    QVERIFY(!ChocolateyManager::validateExtraArgs(
        QStringList{QStringLiteral("--Allow-Empty-Checksums-Secure")}));
    QVERIFY(!ChocolateyManager::validateExtraArgs(
        QStringList{QStringLiteral("--params"), QStringLiteral("--ignore-checksum=true")}));
}

void TestChocolateyManager::computeTimeoutMs_noIntegerOverflow() {
    // Normal conversion.
    QCOMPARE(ChocolateyManager::computeTimeoutMs(5, 300), 5000);
    // 0 (or negative) uses the default.
    QCOMPARE(ChocolateyManager::computeTimeoutMs(0, 300), 300'000);
    // A huge seconds value would overflow int*1000; must clamp to INT_MAX, not wrap
    // to a negative/garbage value (UB).
    const int huge = ChocolateyManager::computeTimeoutMs(2'000'000'000, 300);
    QVERIFY(huge > 0);
    QCOMPARE(huge, std::numeric_limits<int>::max());
}

void TestChocolateyManager::setDefaultTimeout_rejectsNegative() {
    ChocolateyManager manager;
    manager.setDefaultTimeout(120);
    QCOMPARE(manager.getDefaultTimeout(), 120);
    // A negative default is ignored (fail closed), keeping the prior valid value.
    manager.setDefaultTimeout(-5);
    QCOMPARE(manager.getDefaultTimeout(), 120);
}

void TestChocolateyManager::searchPackage_negativeMaxResultsFailsClosed() {
    ChocolateyManager manager;
    // A negative limit must surface an error, not silently become an unbounded
    // (page-size-less) search. Checked before the initialized gate.
    const auto result = manager.searchPackage(QStringLiteral("git"), -1);
    QVERIFY(!result.success);
    QVERIFY(result.error_message.contains(QStringLiteral("negative")));
}

QTEST_MAIN(TestChocolateyManager)
#include "test_chocolatey_manager.moc"
