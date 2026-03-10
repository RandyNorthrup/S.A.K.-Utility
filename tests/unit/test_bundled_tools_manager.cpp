// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bundled_tools_manager.cpp
/// @brief Unit tests for BundledToolsManager

#include "sak/bundled_tools_manager.h"

#include <QtTest/QtTest>

using namespace sak;

class TestBundledToolsManager : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void singleton_instance();
    void toolsPath_nonEmpty();
    void scriptsPath_nonEmpty();
    void psModulePath_nonEmpty();
    void scriptPath_nonEmpty();
    void toolPath_nonEmpty();
    void toolExists_nonExistent();
    void scriptExists_nonExistent();
    void moduleExists_nonExistent();
    void getModuleImportCommand_format();
};

void TestBundledToolsManager::singleton_instance() {
    auto& instance1 = BundledToolsManager::instance();
    auto& instance2 = BundledToolsManager::instance();
    QCOMPARE(&instance1, &instance2);
}

void TestBundledToolsManager::toolsPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.toolsPath();
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::scriptsPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.scriptsPath();
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::psModulePath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.psModulePath(QStringLiteral("SomeModule"));
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::scriptPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.scriptPath(QStringLiteral("test_script.ps1"));
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::toolPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.toolPath(QStringLiteral("sysinternals"),
                                          QStringLiteral("PsExec.exe"));
    QVERIFY(!path.isEmpty());
}

void TestBundledToolsManager::toolExists_nonExistent() {
    const auto& manager = BundledToolsManager::instance();
    QVERIFY(!manager.toolExists(QStringLiteral("nonexistent_category"),
                                QStringLiteral("nonexistent_tool.exe")));
}

void TestBundledToolsManager::scriptExists_nonExistent() {
    const auto& manager = BundledToolsManager::instance();
    QVERIFY(!manager.scriptExists(QStringLiteral("nonexistent_script.ps1")));
}

void TestBundledToolsManager::moduleExists_nonExistent() {
    const auto& manager = BundledToolsManager::instance();
    QVERIFY(!manager.moduleExists(QStringLiteral("NonExistentModule")));
}

void TestBundledToolsManager::getModuleImportCommand_format() {
    const auto& manager = BundledToolsManager::instance();
    const QString command = manager.getModuleImportCommand(QStringLiteral("PSWindowsUpdate"));
    QVERIFY(!command.isEmpty());
    QVERIFY(command.contains(QStringLiteral("PSWindowsUpdate")));
}

QTEST_MAIN(TestBundledToolsManager)
#include "test_bundled_tools_manager.moc"
