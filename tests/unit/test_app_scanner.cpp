// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_app_scanner.cpp
/// @brief Unit tests for AppScanner

#include "sak/app_scanner.h"

#include <QtTest/QtTest>

using namespace sak;

class TestAppScanner : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_default();
    void appInfo_defaults();
    void appInfo_confidence_defaults();
    void appInfo_source_defaults();
    void appInfo_versionLocking_defaults();
    void appInfo_userData_defaults();
    void appInfo_fieldAssignment();
    void scanRegistry_returnsInstalledPrograms();
    void scanAll_returnsNonEmpty();
};

void TestAppScanner::construction_default() {
    AppScanner scanner;
    QVERIFY(std::is_default_constructible_v<AppScanner>);
    QVERIFY(std::is_destructible_v<AppScanner>);
}

void TestAppScanner::appInfo_defaults() {
    AppScanner::AppInfo info;
    QVERIFY(info.name.isEmpty());
    QVERIFY(info.version.isEmpty());
    QVERIFY(info.publisher.isEmpty());
    QVERIFY(info.install_date.isEmpty());
    QVERIFY(info.install_location.isEmpty());
    QVERIFY(info.uninstall_string.isEmpty());
    QVERIFY(info.registry_key.isEmpty());
    QVERIFY(info.choco_package.isEmpty());
    QVERIFY(!info.choco_available);
}

void TestAppScanner::appInfo_confidence_defaults() {
    AppScanner::AppInfo info;
    QCOMPARE(info.match_confidence, AppScanner::AppInfo::Confidence::Unknown);
}

void TestAppScanner::appInfo_source_defaults() {
    AppScanner::AppInfo info;
    QCOMPARE(info.source, AppScanner::AppInfo::Source::Registry);
}

void TestAppScanner::appInfo_versionLocking_defaults() {
    AppScanner::AppInfo info;
    QVERIFY(!info.version_locked);
    QVERIFY(info.locked_version.isEmpty());
}

void TestAppScanner::appInfo_userData_defaults() {
    AppScanner::AppInfo info;
    QVERIFY(!info.has_user_data);
    QCOMPARE(info.estimated_data_size, static_cast<qint64>(0));
}

void TestAppScanner::appInfo_fieldAssignment() {
    AppScanner::AppInfo info;
    info.name = QStringLiteral("Test App");
    info.version = QStringLiteral("1.2.3");
    info.publisher = QStringLiteral("Test Publisher");
    info.choco_available = true;
    info.match_confidence = AppScanner::AppInfo::Confidence::High;
    info.source = AppScanner::AppInfo::Source::AppX;
    info.version_locked = true;
    info.locked_version = QStringLiteral("1.0.0");
    info.has_user_data = true;
    info.estimated_data_size = 1024;

    QCOMPARE(info.name, QStringLiteral("Test App"));
    QCOMPARE(info.version, QStringLiteral("1.2.3"));
    QCOMPARE(info.publisher, QStringLiteral("Test Publisher"));
    QVERIFY(info.choco_available);
    QCOMPARE(info.match_confidence, AppScanner::AppInfo::Confidence::High);
    QCOMPARE(info.source, AppScanner::AppInfo::Source::AppX);
    QVERIFY(info.version_locked);
    QCOMPARE(info.locked_version, QStringLiteral("1.0.0"));
    QVERIFY(info.has_user_data);
    QCOMPARE(info.estimated_data_size, static_cast<qint64>(1024));
}

void TestAppScanner::scanRegistry_returnsInstalledPrograms() {
    AppScanner scanner;
    const auto programs = scanner.scanRegistry();

    QVERIFY2(!programs.empty(), "Every Windows system should have registry-installed programs");

    for (const auto& prog : programs) {
        QVERIFY2(!prog.name.isEmpty(), "Each scanned program must have a name");
        QCOMPARE(prog.source, AppScanner::AppInfo::Source::Registry);
    }
}

void TestAppScanner::scanAll_returnsNonEmpty() {
    AppScanner scanner;
    const auto programs = scanner.scanAll();

    QVERIFY2(!programs.empty(), "scanAll should find installed programs on any Windows system");
}

QTEST_MAIN(TestAppScanner)
#include "test_app_scanner.moc"
