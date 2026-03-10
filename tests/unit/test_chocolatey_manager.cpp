// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_chocolatey_manager.cpp
/// @brief Unit tests for ChocolateyManager

#include "sak/chocolatey_manager.h"

#include <QtTest/QtTest>

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
    const int original_timeout = manager.getDefaultTimeout();
    QVERIFY(original_timeout >= 0);

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

QTEST_MAIN(TestChocolateyManager)
#include "test_chocolatey_manager.moc"
