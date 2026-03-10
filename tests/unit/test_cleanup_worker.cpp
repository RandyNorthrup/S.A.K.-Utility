// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_cleanup_worker.cpp
/// @brief Unit tests for CleanupWorker

#include "sak/advanced_uninstall_types.h"
#include "sak/cleanup_worker.h"

#include <QtTest/QtTest>

using namespace sak;

class TestCleanupWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void construction_emptyItems();
    void construction_nonCopyable();
    void construction_isWorkerBase();
    void construction_withRecycleBin();
    void leftoverItem_type_values();
    void leftoverItem_riskLevel_values();
};

void TestCleanupWorker::construction_emptyItems() {
    QVector<LeftoverItem> empty_items;
    CleanupWorker worker(empty_items, false);
    QVERIFY(dynamic_cast<QObject*>(&worker) != nullptr);
}

void TestCleanupWorker::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<CleanupWorker>);
    QVERIFY(!std::is_move_constructible_v<CleanupWorker>);
}

void TestCleanupWorker::construction_isWorkerBase() {
    QVector<LeftoverItem> empty_items;
    CleanupWorker worker(empty_items);
    QVERIFY(dynamic_cast<WorkerBase*>(&worker) != nullptr);
}

void TestCleanupWorker::construction_withRecycleBin() {
    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = QStringLiteral("C:\\test\\file.txt");
    items.append(item);

    CleanupWorker worker(items, true);
    QVERIFY(dynamic_cast<WorkerBase*>(&worker) != nullptr);
}

void TestCleanupWorker::leftoverItem_type_values() {
    QCOMPARE(static_cast<int>(LeftoverItem::Type::File), 0);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::Folder), 1);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::RegistryKey), 2);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::RegistryValue), 3);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::Service), 4);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::ScheduledTask), 5);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::FirewallRule), 6);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::StartupEntry), 7);
    QCOMPARE(static_cast<int>(LeftoverItem::Type::ShellExtension), 8);
}

void TestCleanupWorker::leftoverItem_riskLevel_values() {
    QCOMPARE(static_cast<int>(LeftoverItem::RiskLevel::Safe), 0);
    QCOMPARE(static_cast<int>(LeftoverItem::RiskLevel::Review), 1);
    QCOMPARE(static_cast<int>(LeftoverItem::RiskLevel::Risky), 2);
}

QTEST_MAIN(TestCleanupWorker)
#include "test_cleanup_worker.moc"
