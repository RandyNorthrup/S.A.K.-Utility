// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_bundled_tools_manager.cpp
/// @brief Unit tests for BundledToolsManager

#include "sak/bundled_tools_manager.h"

#include "../support/junction_fixture.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
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
    void isTrustedToolFile_refusesAJunctionLeadingOut();
    void isTrustedToolFile_acceptsARootReachedThroughAJunction();
    void isTrustedToolFile_refusesWhatIsNotARegularFileInside();
};

void TestBundledToolsManager::singleton_instance() {
    auto& instance1 = BundledToolsManager::instance();
    auto& instance2 = BundledToolsManager::instance();
    QCOMPARE(&instance1, &instance2);
}

void TestBundledToolsManager::toolsPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.toolsPath();
    QCOMPARE(path, QCoreApplication::applicationDirPath() + QStringLiteral("/tools"));
}

void TestBundledToolsManager::scriptsPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.scriptsPath();
    QCOMPARE(path, QCoreApplication::applicationDirPath() + QStringLiteral("/scripts"));
}

void TestBundledToolsManager::psModulePath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.psModulePath(QStringLiteral("SomeModule"));
    QCOMPARE(path,
             QCoreApplication::applicationDirPath() +
                 QStringLiteral("/tools/ps_modules/SomeModule"));
}

void TestBundledToolsManager::scriptPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.scriptPath(QStringLiteral("test_script.ps1"));
    QCOMPARE(path,
             QCoreApplication::applicationDirPath() + QStringLiteral("/scripts/test_script.ps1"));
}

void TestBundledToolsManager::toolPath_nonEmpty() {
    const auto& manager = BundledToolsManager::instance();
    const QString path = manager.toolPath(QStringLiteral("sysinternals"),
                                          QStringLiteral("PsExec.exe"));
    QCOMPARE(path,
             QCoreApplication::applicationDirPath() +
                 QStringLiteral("/tools/sysinternals/PsExec.exe"));
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
    QCOMPARE(command,
             QStringLiteral("Import-Module '") + QCoreApplication::applicationDirPath() +
                 QStringLiteral("/tools/ps_modules/PSWindowsUpdate' -Force"));
}

namespace {

bool writeTool(const QString& path) {
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        return false;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write("MZ") == 2;
}

}  // namespace

// Lane SEC2 (2026-11-17): choco.exe, aria2c, UUPMediaConverter and 7-Zip are trusted by this one
// check. It compared QFileInfo::canonicalFilePath, which in Qt 6 does not resolve a junction, so a
// junction in a folder under the tools root, pointing outside, let an outside executable pass.
void TestBundledToolsManager::isTrustedToolFile_refusesAJunctionLeadingOut() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("tools"));
    const QString outside = temp.filePath(QStringLiteral("outside"));
    QVERIFY(QDir().mkpath(root));
    QVERIFY(writeTool(QDir(outside).filePath(QStringLiteral("choco.exe"))));
    QVERIFY(writeTool(QDir(root).filePath(QStringLiteral("real/tool.exe"))));
    QVERIFY(sak::test::createJunction(QDir(root).filePath(QStringLiteral("chocolatey")), outside));

    QVERIFY(BundledToolsManager::isTrustedToolFile(
        QDir(root).filePath(QStringLiteral("real/tool.exe")), root));
    QVERIFY2(!BundledToolsManager::isTrustedToolFile(
                 QDir(root).filePath(QStringLiteral("chocolatey/choco.exe")), root),
             "an executable reached through a junction that leads out of the tools root passed");
}

void TestBundledToolsManager::isTrustedToolFile_acceptsARootReachedThroughAJunction() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString real = temp.filePath(QStringLiteral("app/tools"));
    QVERIFY(writeTool(QDir(real).filePath(QStringLiteral("uup/aria2c.exe"))));
    const QString alias = temp.filePath(QStringLiteral("alias"));
    QVERIFY(sak::test::createJunction(alias, temp.filePath(QStringLiteral("app"))));
    const QString alias_root = QDir(alias).filePath(QStringLiteral("tools"));
    QVERIFY2(BundledToolsManager::isTrustedToolFile(
                 QDir(alias_root).filePath(QStringLiteral("uup/aria2c.exe")), alias_root),
             "a real tool in an application folder reached through a junction was refused");
}

void TestBundledToolsManager::isTrustedToolFile_refusesWhatIsNotARegularFileInside() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString root = temp.filePath(QStringLiteral("tools"));
    QVERIFY(writeTool(QDir(root).filePath(QStringLiteral("a/tool.exe"))));
    QVERIFY(writeTool(temp.filePath(QStringLiteral("toolsX/tool.exe"))));
    // A folder, a missing name, a sibling whose name starts with the root's, a junction as the
    // name itself.
    QVERIFY(
        !BundledToolsManager::isTrustedToolFile(QDir(root).filePath(QStringLiteral("a")), root));
    QVERIFY(!BundledToolsManager::isTrustedToolFile(QDir(root).filePath(QStringLiteral("nope.exe")),
                                                    root));
    QVERIFY(!BundledToolsManager::isTrustedToolFile(
        temp.filePath(QStringLiteral("toolsX/tool.exe")), root));
    QVERIFY(sak::test::createJunction(QDir(root).filePath(QStringLiteral("j")),
                                      QDir(root).filePath(QStringLiteral("a"))));
    QVERIFY(
        !BundledToolsManager::isTrustedToolFile(QDir(root).filePath(QStringLiteral("j")), root));
}

QTEST_MAIN(TestBundledToolsManager)
#include "test_bundled_tools_manager.moc"
