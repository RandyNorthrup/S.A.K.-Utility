// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_uninstall_types.cpp
/// @brief Unit tests for Advanced Uninstall shared data types

#include <QtTest/QtTest>

#include "sak/advanced_uninstall_types.h"

#include <type_traits>

class AdvancedUninstallTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── ProgramInfo ──
    void programInfo_defaultConstruction();
    void programInfo_valueSemantics();
    void programInfo_moveSemantics();
    void programInfo_sourceEnum();

    // ── ScanLevel ──
    void scanLevel_enumValues();

    // ── LeftoverItem ──
    void leftoverItem_defaultConstruction();
    void leftoverItem_typeEnum();
    void leftoverItem_riskEnum();
    void leftoverItem_valueSemantics();

    // ── UninstallReport ──
    void uninstallReport_defaultConstruction();
    void uninstallReport_resultEnum();
    void uninstallReport_valueSemantics();

    // ── UninstallQueueItem ──
    void queueItem_defaultConstruction();
    void queueItem_statusEnum();
    void queueItem_valueSemantics();

    // ── ViewFilter ──
    void viewFilter_enumValues();

    // ── Compile-Time Invariants ──
    void staticAsserts_defaultConstructible();
    void staticAsserts_copyConstructible();
    void staticAsserts_movable();
};

// ── ProgramInfo ─────────────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::programInfo_defaultConstruction()
{
    sak::ProgramInfo info;

    QVERIFY(info.displayName.isEmpty());
    QVERIFY(info.publisher.isEmpty());
    QVERIFY(info.displayVersion.isEmpty());
    QVERIFY(info.installDate.isEmpty());
    QVERIFY(info.installLocation.isEmpty());
    QVERIFY(info.uninstallString.isEmpty());
    QVERIFY(info.quietUninstallString.isEmpty());
    QVERIFY(info.modifyPath.isEmpty());
    QVERIFY(info.displayIcon.isEmpty());
    QVERIFY(info.registryKeyPath.isEmpty());
    QCOMPARE(info.estimatedSizeKB, 0);
    QCOMPARE(info.actualSizeBytes, 0);
    QCOMPARE(info.source, sak::ProgramInfo::Source::RegistryHKLM);
    QVERIFY(info.packageFamilyName.isEmpty());
    QVERIFY(info.packageFullName.isEmpty());
    QVERIFY(!info.isSystemComponent);
    QVERIFY(!info.isOrphaned);
    QVERIFY(!info.isBloatware);
    QVERIFY(info.cachedImage.isNull());
}

void AdvancedUninstallTypesTests::programInfo_valueSemantics()
{
    sak::ProgramInfo original;
    original.displayName = "Test App";
    original.publisher = "Test Publisher";
    original.displayVersion = "1.2.3";
    original.installDate = "20250101";
    original.installLocation = "C:\\Program Files\\TestApp";
    original.uninstallString = "C:\\uninst.exe";
    original.registryKeyPath = "HKLM\\SOFTWARE\\TestApp";
    original.estimatedSizeKB = 1024;
    original.actualSizeBytes = 1048576;
    original.source = sak::ProgramInfo::Source::UWP;
    original.packageFamilyName = "TestApp_abc123";
    original.isSystemComponent = true;
    original.isOrphaned = true;
    original.isBloatware = true;

    // Copy
    sak::ProgramInfo copy = original;
    QCOMPARE(copy.displayName, original.displayName);
    QCOMPARE(copy.publisher, original.publisher);
    QCOMPARE(copy.displayVersion, original.displayVersion);
    QCOMPARE(copy.installLocation, original.installLocation);
    QCOMPARE(copy.uninstallString, original.uninstallString);
    QCOMPARE(copy.registryKeyPath, original.registryKeyPath);
    QCOMPARE(copy.estimatedSizeKB, original.estimatedSizeKB);
    QCOMPARE(copy.actualSizeBytes, original.actualSizeBytes);
    QCOMPARE(copy.source, original.source);
    QCOMPARE(copy.packageFamilyName, original.packageFamilyName);
    QCOMPARE(copy.isSystemComponent, original.isSystemComponent);
    QCOMPARE(copy.isOrphaned, original.isOrphaned);
    QCOMPARE(copy.isBloatware, original.isBloatware);

    // Assignment
    sak::ProgramInfo assigned;
    assigned = original;
    QCOMPARE(assigned.displayName, original.displayName);
    QCOMPARE(assigned.source, sak::ProgramInfo::Source::UWP);
}

void AdvancedUninstallTypesTests::programInfo_moveSemantics()
{
    sak::ProgramInfo original;
    original.displayName = "MovableApp";
    original.estimatedSizeKB = 512;

    sak::ProgramInfo moved = std::move(original);
    QCOMPARE(moved.displayName, "MovableApp");
    QCOMPARE(moved.estimatedSizeKB, 512);
}

void AdvancedUninstallTypesTests::programInfo_sourceEnum()
{
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKLM), 0);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKLM_WOW64), 1);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::RegistryHKCU), 2);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::UWP), 3);
    QCOMPARE(static_cast<int>(sak::ProgramInfo::Source::Provisioned), 4);
}

// ── ScanLevel ───────────────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::scanLevel_enumValues()
{
    QCOMPARE(static_cast<int>(sak::ScanLevel::Safe), 0);
    QCOMPARE(static_cast<int>(sak::ScanLevel::Moderate), 1);
    QCOMPARE(static_cast<int>(sak::ScanLevel::Advanced), 2);
}

// ── LeftoverItem ────────────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::leftoverItem_defaultConstruction()
{
    sak::LeftoverItem item;

    QCOMPARE(item.type, sak::LeftoverItem::Type::File);
    QCOMPARE(item.risk, sak::LeftoverItem::RiskLevel::Safe);
    QVERIFY(item.path.isEmpty());
    QVERIFY(item.description.isEmpty());
    QCOMPARE(item.sizeBytes, 0);
    QVERIFY(!item.selected);
    QVERIFY(item.registryValueName.isEmpty());
    QVERIFY(item.registryValueData.isEmpty());
}

void AdvancedUninstallTypesTests::leftoverItem_typeEnum()
{
    // Verify all type values are distinct
    QSet<int> values;
    values.insert(static_cast<int>(sak::LeftoverItem::Type::File));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::Folder));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::RegistryKey));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::RegistryValue));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::Service));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::ScheduledTask));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::FirewallRule));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::StartupEntry));
    values.insert(static_cast<int>(sak::LeftoverItem::Type::ShellExtension));
    QCOMPARE(values.size(), 9);
}

void AdvancedUninstallTypesTests::leftoverItem_riskEnum()
{
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Safe), 0);
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Review), 1);
    QCOMPARE(static_cast<int>(sak::LeftoverItem::RiskLevel::Risky), 2);
}

void AdvancedUninstallTypesTests::leftoverItem_valueSemantics()
{
    sak::LeftoverItem original;
    original.type = sak::LeftoverItem::Type::RegistryKey;
    original.risk = sak::LeftoverItem::RiskLevel::Risky;
    original.path = "HKLM\\SOFTWARE\\TestApp";
    original.description = "Leftover registry key";
    original.sizeBytes = 4096;
    original.selected = true;
    original.registryValueName = "ValueName";
    original.registryValueData = "ValueData";

    // Copy
    sak::LeftoverItem copy = original;
    QCOMPARE(copy.type, original.type);
    QCOMPARE(copy.risk, original.risk);
    QCOMPARE(copy.path, original.path);
    QCOMPARE(copy.description, original.description);
    QCOMPARE(copy.sizeBytes, original.sizeBytes);
    QCOMPARE(copy.selected, original.selected);
    QCOMPARE(copy.registryValueName, original.registryValueName);
    QCOMPARE(copy.registryValueData, original.registryValueData);

    // Move
    sak::LeftoverItem moved = std::move(copy);
    QCOMPARE(moved.type, sak::LeftoverItem::Type::RegistryKey);
    QCOMPARE(moved.path, original.path);
}

// ── UninstallReport ─────────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::uninstallReport_defaultConstruction()
{
    sak::UninstallReport report;

    QVERIFY(report.programName.isEmpty());
    QVERIFY(report.programVersion.isEmpty());
    QVERIFY(report.programPublisher.isEmpty());
    QVERIFY(!report.restorePointCreated);
    QVERIFY(report.restorePointName.isEmpty());
    QCOMPARE(report.uninstallResult, sak::UninstallReport::UninstallResult::Success);
    QCOMPARE(report.nativeExitCode, 0);
    QCOMPARE(report.scanLevel, sak::ScanLevel::Moderate);
    QVERIFY(report.foundLeftovers.isEmpty());
    QCOMPARE(report.filesDeleted, 0);
    QCOMPARE(report.foldersDeleted, 0);
    QCOMPARE(report.registryKeysDeleted, 0);
    QCOMPARE(report.registryValuesDeleted, 0);
    QCOMPARE(report.servicesRemoved, 0);
    QCOMPARE(report.tasksRemoved, 0);
    QCOMPARE(report.firewallRulesRemoved, 0);
    QCOMPARE(report.startupEntriesRemoved, 0);
    QCOMPARE(report.failedDeletions, 0);
    QCOMPARE(report.totalSpaceRecovered, 0);
    QVERIFY(report.errorLog.isEmpty());
}

void AdvancedUninstallTypesTests::uninstallReport_resultEnum()
{
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Success), 0);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Failed), 1);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Cancelled), 2);
    QCOMPARE(static_cast<int>(sak::UninstallReport::UninstallResult::Skipped), 3);
}

void AdvancedUninstallTypesTests::uninstallReport_valueSemantics()
{
    sak::UninstallReport original;
    original.programName = "TestProg";
    original.programVersion = "2.0";
    original.restorePointCreated = true;
    original.restorePointName = "Before TestProg";
    original.uninstallResult = sak::UninstallReport::UninstallResult::Skipped;
    original.nativeExitCode = 42;
    original.scanLevel = sak::ScanLevel::Advanced;
    original.filesDeleted = 10;
    original.foldersDeleted = 3;
    original.totalSpaceRecovered = 999999;
    original.errorLog.append("some error");

    sak::LeftoverItem leftover;
    leftover.path = "C:\\test";
    original.foundLeftovers.append(leftover);

    // Copy
    sak::UninstallReport copy = original;
    QCOMPARE(copy.programName, "TestProg");
    QCOMPARE(copy.restorePointCreated, true);
    QCOMPARE(copy.uninstallResult, sak::UninstallReport::UninstallResult::Skipped);
    QCOMPARE(copy.nativeExitCode, 42);
    QCOMPARE(copy.filesDeleted, 10);
    QCOMPARE(copy.totalSpaceRecovered, 999999);
    QCOMPARE(copy.foundLeftovers.size(), 1);
    QCOMPARE(copy.foundLeftovers[0].path, "C:\\test");
    QCOMPARE(copy.errorLog.size(), 1);
}

// ── UninstallQueueItem ──────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::queueItem_defaultConstruction()
{
    sak::UninstallQueueItem item;

    QVERIFY(item.program.displayName.isEmpty());
    QCOMPARE(item.scanLevel, sak::ScanLevel::Moderate);
    QVERIFY(item.autoCleanSafeLeftovers);
    QCOMPARE(item.status, sak::UninstallQueueItem::Status::Queued);
}

void AdvancedUninstallTypesTests::queueItem_statusEnum()
{
    QSet<int> values;
    values.insert(static_cast<int>(sak::UninstallQueueItem::Status::Queued));
    values.insert(static_cast<int>(sak::UninstallQueueItem::Status::InProgress));
    values.insert(static_cast<int>(sak::UninstallQueueItem::Status::Completed));
    values.insert(static_cast<int>(sak::UninstallQueueItem::Status::Failed));
    values.insert(static_cast<int>(sak::UninstallQueueItem::Status::Cancelled));
    QCOMPARE(values.size(), 5);
}

void AdvancedUninstallTypesTests::queueItem_valueSemantics()
{
    sak::UninstallQueueItem original;
    original.program.displayName = "Queued App";
    original.scanLevel = sak::ScanLevel::Advanced;
    original.autoCleanSafeLeftovers = false;
    original.status = sak::UninstallQueueItem::Status::InProgress;
    original.report.programName = "Queued App";

    sak::UninstallQueueItem copy = original;
    QCOMPARE(copy.program.displayName, "Queued App");
    QCOMPARE(copy.scanLevel, sak::ScanLevel::Advanced);
    QVERIFY(!copy.autoCleanSafeLeftovers);
    QCOMPARE(copy.status, sak::UninstallQueueItem::Status::InProgress);
    QCOMPARE(copy.report.programName, "Queued App");
}

// ── ViewFilter ──────────────────────────────────────────────────────────────

void AdvancedUninstallTypesTests::viewFilter_enumValues()
{
    QCOMPARE(static_cast<int>(sak::ViewFilter::All), 0);
    QCOMPARE(static_cast<int>(sak::ViewFilter::Win32Only), 1);
    QCOMPARE(static_cast<int>(sak::ViewFilter::UwpOnly), 2);
    QCOMPARE(static_cast<int>(sak::ViewFilter::BloatwareOnly), 3);
    QCOMPARE(static_cast<int>(sak::ViewFilter::OrphanedOnly), 4);
}

// ── Compile-Time Invariants ─────────────────────────────────────────────────

void AdvancedUninstallTypesTests::staticAsserts_defaultConstructible()
{
    QVERIFY(std::is_default_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_default_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_default_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_default_constructible_v<sak::UninstallQueueItem>);
}

void AdvancedUninstallTypesTests::staticAsserts_copyConstructible()
{
    QVERIFY(std::is_copy_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_copy_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_copy_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_copy_constructible_v<sak::UninstallQueueItem>);
}

void AdvancedUninstallTypesTests::staticAsserts_movable()
{
    QVERIFY(std::is_move_constructible_v<sak::ProgramInfo>);
    QVERIFY(std::is_move_constructible_v<sak::LeftoverItem>);
    QVERIFY(std::is_move_constructible_v<sak::UninstallReport>);
    QVERIFY(std::is_move_constructible_v<sak::UninstallQueueItem>);
}

QTEST_GUILESS_MAIN(AdvancedUninstallTypesTests)

#include "test_advanced_uninstall_types.moc"
