// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_cleanup_worker.cpp
/// @brief Unit tests for CleanupWorker

#include "sak/advanced_uninstall_types.h"
#include "sak/cleanup_worker.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
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

    // Recycle-fallback surfacing (B10-23)
    void permanentMode_deletesAndEmitsNoRecycleFallback();
    void permanentMode_deletesNestedFolderTree();
    void requireRecoverable_neverPermanentlyDeletes();

    // Cancellation is not success (B10-30)
    void cancelledBeforeStart_emitsCancelledNotComplete();
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

void TestCleanupWorker::permanentMode_deletesAndEmitsNoRecycleFallback() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("leftover.txt");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    }

    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = path;
    item.selected = true;
    items.append(item);

    CleanupWorker worker(items, /*useRecycleBin=*/false);
    QSignalSpy completeSpy(&worker, &CleanupWorker::cleanupComplete);
    QSignalSpy fallbackSpy(&worker, &CleanupWorker::recycleFallbackItems);

    worker.start();
    QVERIFY(completeSpy.wait(5000));

    QVERIFY(!QFile::exists(path));     // permanently deleted
    QCOMPARE(fallbackSpy.count(), 0);  // recycle-fallback signal only fires in recycle mode
    const auto args = completeSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 1);   // one succeeded
}

void TestCleanupWorker::permanentMode_deletesNestedFolderTree() {
    // Happy-path coverage for the handle-verified recursive folder delete: a nested tree with files
    // in subdirectories is fully removed (files deleted by handle, subdirs recursed, dirs removed).
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString root = QDir(tmp.path()).filePath("AcmeLeftover");
    QDir().mkpath(QDir(root).filePath("sub/deeper"));
    const QStringList files = {QDir(root).filePath("a.txt"),
                               QDir(root).filePath("sub/b.txt"),
                               QDir(root).filePath("sub/deeper/c.txt")};
    for (const QString& f : files) {
        QFile fh(f);
        QVERIFY(fh.open(QIODevice::WriteOnly));
        fh.write("x");
        fh.close();
    }
    QVERIFY(QDir(root).exists());

    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::Folder;
    item.path = root;
    item.selected = true;
    items.append(item);

    CleanupWorker worker(items, /*useRecycleBin=*/false);
    QSignalSpy completeSpy(&worker, &CleanupWorker::cleanupComplete);
    worker.start();
    QVERIFY(completeSpy.wait(5000));

    QVERIFY(!QDir(root).exists());    // whole tree gone
    const auto args = completeSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 1);  // one folder item succeeded
    QCOMPARE(args.at(1).toInt(), 0);  // zero failed
}

void TestCleanupWorker::requireRecoverable_neverPermanentlyDeletes() {
    // Recycle-only (auto-clean) mode must NEVER permanently delete: with recycle unavailable the
    // item is left in place and reported failed, rather than destroyed. (Uses useRecycleBin=false
    // so the outcome is deterministic without depending on a real Recycle Bin in the test host.)
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("keep_me.txt");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    }

    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = path;
    item.selected = true;
    items.append(item);

    CleanupWorker worker(items, /*useRecycleBin=*/false);
    worker.setRequireRecoverable(true);
    QSignalSpy completeSpy(&worker, &CleanupWorker::cleanupComplete);
    worker.start();
    QVERIFY(completeSpy.wait(5000));

    QVERIFY(QFile::exists(path));     // NOT permanently deleted -- recoverable-only refused
    const auto args = completeSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 0);  // zero succeeded
    QCOMPARE(args.at(1).toInt(), 1);  // one failed (left for review)
}

void TestCleanupWorker::cancelledBeforeStart_emitsCancelledNotComplete() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath("keep.txt");
    {
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();
    }

    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = path;
    item.selected = true;
    items.append(item);

    CleanupWorker worker(items, /*useRecycleBin=*/false);
    QSignalSpy completeSpy(&worker, &CleanupWorker::cleanupComplete);
    QSignalSpy cancelledSpy(&worker, &WorkerBase::cancelled);

    // Stop requested before the loop processes any item -- WorkerBase preserves a
    // pre-start stop request, so execute() bails on the first checkStop().
    worker.requestStop();
    worker.start();
    QVERIFY(cancelledSpy.wait(3000));

    // A cancelled run must NOT emit the success-shaped cleanupComplete...
    QCOMPARE(completeSpy.count(), 0);
    // ...and nothing was deleted.
    QVERIFY(QFile::exists(path));
}

QTEST_MAIN(TestCleanupWorker)
#include "test_cleanup_worker.moc"
