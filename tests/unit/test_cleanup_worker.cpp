// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_cleanup_worker.cpp
/// @brief Unit tests for CleanupWorker

#include "sak/advanced_uninstall_types.h"
#include "sak/cleanup_worker.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
    // R5 p8_appaction-3: recycle mode implies recoverable-only unless opted out
    void recycleMode_defaultsToRecoverableOnly();
    // R5-G23-7 invariant 3 "recycle means recycle" as an exhaustive property test
    void decideRecycle_recoverableModeNeverPermanentlyDeletes();

    // Cancellation is not success (B10-30)
    void cancelledBeforeStart_emitsCancelledNotComplete();

    // Anti-hijack: destructive CLI tools resolved by absolute System32 path (Codex-3)
    void systemToolPath_absoluteUnderSystemRootOrFailClosed();
    void systemTools_resolveThroughTheOsNotTheEnvironment();

    // Defense-in-depth: worker re-screens each item against the cleanup denylist (Codex-3)
    void deniedTargets_refusedWithoutDeletion();
    void itemCleanupRefusal_screensEachType();

private:
    // One row of the exhaustive recycle-decision property test. Nested inside the test class
    // because RecycleOutcome is private to CleanupWorker and only this class is its friend.
    struct RecycleDecisionRow {
        bool useRecycle;
        bool requireRecov;
        bool recoverAllowed;
        bool redirectSafe;
        bool recycleSucceeds;
        CleanupWorker::RecycleOutcome outcome;
        bool fallback;
        int recoverCalls;
        int redirectCalls;
        int doRecycleCalls;
        QByteArray tag;
    };
    // Which of the three probes decideRecycle is expected to have run for this row, and whether
    // the run escalated a failed recycle to a permanent delete.
    struct RecycleProbes {
        bool recover = false;
        bool redirect = false;
        bool recycle = false;
        bool escalated = false;
    };
    static RecycleProbes recycleProbesFor(const RecycleDecisionRow& row);
    static void verifyRecycleDecisionRow(const RecycleDecisionRow& row);
};

void TestCleanupWorker::construction_emptyItems() {
    QVector<LeftoverItem> empty_items;
    CleanupWorker worker(empty_items, false);
    // The dynamic_cast to a compile-time base was a tautology; pin the observable construction
    // state: useRecycleBin=false means recoverable-only is OFF.
    QCOMPARE(worker.requireRecoverable(), false);
}

void TestCleanupWorker::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<CleanupWorker>);
    QVERIFY(!std::is_move_constructible_v<CleanupWorker>);
}

void TestCleanupWorker::construction_isWorkerBase() {
    QVector<LeftoverItem> empty_items;
    CleanupWorker worker(empty_items);
    // Single-arg ctor defaults useRecycleBin=false -> recoverable-only OFF. The cast was a
    // tautology.
    QCOMPARE(worker.requireRecoverable(), false);
}

void TestCleanupWorker::construction_withRecycleBin() {
    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = QStringLiteral("C:\\test\\file.txt");
    items.append(item);

    CleanupWorker worker(items, true);
    // useRecycleBin=true turns recoverable-only ON; the cast asserted nothing about that path.
    QVERIFY(worker.requireRecoverable());
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
    // Use the canonical (long-form) temp path: the worker's defense-in-depth denylist screen
    // refuses 8.3 short-name components (e.g. a CI "RUNNER~1" temp dir), and real scanned leftovers
    // are always long-form. Canonicalizing keeps the test representative of the production input.
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(!base.isEmpty());
    const QString path = QDir(base).filePath("leftover.txt");
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
    // QTRY, not completeSpy.wait(): the worker runs on its own QThread and QSignalSpy records
    // via DirectConnection, so a fast run can finish before the main thread reaches the wait --
    // wait() would then block for a second emission that never comes and fail a correct worker.
    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);

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
    // Canonical (long-form) base -- the worker's denylist screen refuses 8.3 short-name components.
    const QString tmpBase = QFileInfo(tmp.path()).canonicalFilePath();
    QVERIFY(!tmpBase.isEmpty());
    const QString root = QDir(tmpBase).filePath("AcmeLeftover");
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
    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);

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
    // Canonical (long-form) base so the denylist screen allows it and the requireRecoverable path
    // -- not a short-name refusal -- is what leaves the file in place.
    const QString base = QFileInfo(dir.path()).canonicalFilePath();
    QVERIFY(!base.isEmpty());
    const QString path = QDir(base).filePath("keep_me.txt");
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
    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);

    QVERIFY(QFile::exists(path));     // NOT permanently deleted -- recoverable-only refused
    const auto args = completeSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 0);  // zero succeeded
    QCOMPARE(args.at(1).toInt(), 1);  // one failed (left for review)
}

void TestCleanupWorker::recycleMode_defaultsToRecoverableOnly() {
    // R5 p8_appaction-3: software.clean_leftovers builds the worker with the 2-arg ctor and
    // (before this fix) never asked for recoverable-only, so a Recycle Bin failure fell through
    // to a PERMANENT delete even though the caller had chosen recycle. Choosing the Recycle Bin
    // is a choice for recoverability, so the invariant now lives in the worker itself.
    QVector<LeftoverItem> items;
    LeftoverItem item;
    item.type = LeftoverItem::Type::File;
    item.path = QStringLiteral("C:\\Vendor\\App\\leftover.txt");
    items.append(item);

    CleanupWorker recycling(items, /*useRecycleBin=*/true);
    QVERIFY2(recycling.requireRecoverable(),
             "recycle mode must never silently escalate to a permanent delete");

    // Permanent mode is unchanged: it never claimed recoverability in the first place.
    CleanupWorker permanent(items, /*useRecycleBin=*/false);
    QVERIFY(!permanent.requireRecoverable());

    // A caller whose contract allows permanent deletion (the GUI manual clean, where a human
    // reviewed each item) can still opt out explicitly.
    recycling.setRequireRecoverable(false);
    QVERIFY(!recycling.requireRecoverable());
}

// Every assertion in the caller used to be ONE-WAY (an implication behind an if), so a
// decideRecycle that simply refused EVERYTHING -- never Recycled, never FallThrough -- satisfied
// all of them, and recoverCalls/redirectCalls were counted but never read. This pins the EXACT
// outcome, the escalation flag and every probe count.
TestCleanupWorker::RecycleProbes TestCleanupWorker::recycleProbesFor(
    const RecycleDecisionRow& row) {
    RecycleProbes p;
    p.recover = row.useRecycle && row.requireRecov;
    p.redirect = row.useRecycle && (!row.requireRecov || row.recoverAllowed);
    p.recycle = p.redirect && row.redirectSafe;
    p.escalated = p.recycle && !row.recycleSucceeds && !row.requireRecov;
    return p;
}

void TestCleanupWorker::verifyRecycleDecisionRow(const RecycleDecisionRow& row) {
    using RO = CleanupWorker::RecycleOutcome;
    const RecycleProbes p = recycleProbesFor(row);
    RO expected = RO::FallThrough;
    if (p.recycle && row.recycleSucceeds) {
        expected = RO::Recycled;
    } else if (p.redirect && !row.redirectSafe) {
        expected = RO::RefusedRedirected;  // the redirect refusal, NOT the recoverable one
    } else if (row.requireRecov) {
        expected = RO::RefusedRecoverable;
    }
    QVERIFY2(row.outcome == expected, row.tag.constData());
    // recycleFallback is the ONLY thing that surfaces a recycle -> permanent escalation through
    // recycleFallbackItems; nothing anywhere asserted it is ever SET.
    QVERIFY2(row.fallback == p.escalated, row.tag.constData());
    const int recoverExpected = p.recover ? 1 : 0;
    const int redirectExpected = p.redirect ? 1 : 0;
    const int recycleExpected = p.recycle ? 1 : 0;
    QVERIFY2(row.recoverCalls == recoverExpected, row.tag.constData());
    QVERIFY2(row.redirectCalls == redirectExpected, row.tag.constData());
    QVERIFY2(row.doRecycleCalls == recycleExpected, row.tag.constData());
}

void TestCleanupWorker::decideRecycle_recoverableModeNeverPermanentlyDeletes() {
    // R5-G23-7 invariant 3 "recycle means recycle" as a PROPERTY test: exhaustively drive the pure
    // recycle decision over ALL 32 combinations of its five boolean inputs and assert the invariant
    // on every one. The probes count their own calls so laziness (the shell recycle is attempted at
    // most once and never after an earlier refusal) is asserted too -- that laziness is what makes
    // the callback refactor behavior-identical to the inline code decideRecycle replaced.
    // Non-vacuous (G18-4): removing either requireRecoverable guard before a FallThrough lets a
    // recoverable-mode recycle failure escalate to permanent, turning the first assertion red.
    using RO = CleanupWorker::RecycleOutcome;
    for (int bits = 0; bits < 32; ++bits) {
        const bool useRecycle = (bits & 1) != 0;
        const bool requireRecov = (bits & 2) != 0;
        const bool recoverAllowed = (bits & 4) != 0;
        const bool redirectSafe = (bits & 8) != 0;
        const bool recycleSucceeds = (bits & 16) != 0;
        int recoverCalls = 0;
        int redirectCalls = 0;
        int doRecycleCalls = 0;
        bool fallback = false;
        CleanupWorker::RecycleProbes probes{[&] {
                                                ++recoverCalls;
                                                return recoverAllowed;
                                            },
                                            [&] {
                                                ++redirectCalls;
                                                return redirectSafe;
                                            },
                                            [&] {
                                                ++doRecycleCalls;
                                                return recycleSucceeds;
                                            }};
        const RO outcome = CleanupWorker::decideRecycle(
            QStringLiteral("C:/probe/item.tmp"), useRecycle, requireRecov, probes, fallback);
        const QByteArray tag = QStringLiteral("bits=%1").arg(bits).toUtf8();

        if (requireRecov) {
            QVERIFY2(outcome != RO::FallThrough, tag.constData());  // recoverable NEVER permanent
            QVERIFY2(!fallback, tag.constData());
        }
        verifyRecycleDecisionRow({useRecycle,
                                  requireRecov,
                                  recoverAllowed,
                                  redirectSafe,
                                  recycleSucceeds,
                                  outcome,
                                  fallback,
                                  recoverCalls,
                                  redirectCalls,
                                  doRecycleCalls,
                                  tag});
        if (outcome == RO::Recycled) {
            QVERIFY2(recycleSucceeds, tag.constData());  // Recycled claimed only on real success
        }
        if (outcome == RO::FallThrough) {
            QVERIFY2(!requireRecov, tag.constData());    // permanent only when NOT recoverable-only
        }
        QVERIFY2(doRecycleCalls <= 1, tag.constData());  // shell recycle attempted at most once
        if (!useRecycle) {
            QVERIFY2(doRecycleCalls == 0, tag.constData());  // no recycle probe when disabled
        }
    }
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
    QTRY_COMPARE_WITH_TIMEOUT(cancelledSpy.count(), 1, 3000);

    // A cancelled run must NOT emit the success-shaped cleanupComplete...
    QCOMPARE(completeSpy.count(), 0);
    // ...and nothing was deleted.
    QVERIFY(QFile::exists(path));
}

void TestCleanupWorker::systemToolPath_absoluteUnderSystemRootOrFailClosed() {
    // Empty SystemRoot -> fail closed (never launch a bare sc.exe/schtasks.exe/netsh.exe that a
    // planted binary in the app dir/CWD/PATH could satisfy).
    QVERIFY(CleanupWorker::systemToolPath(QString(), QStringLiteral("sc.exe")).isEmpty());

    // A known root -> an absolute, rooted System32 path, not a bare executable name.
    const QString sc = CleanupWorker::systemToolPath(QStringLiteral("C:\\Windows"),
                                                     QStringLiteral("sc.exe"));
    QVERIFY(!sc.isEmpty());
    QVERIFY(sc.endsWith(QStringLiteral("System32/sc.exe")));
    QVERIFY(QDir::isAbsolutePath(sc));
    QCOMPARE(sc,
             QStringLiteral("C:/Windows/System32/sc.exe"));  // exact rooted path, not a bare name

    const QString netsh = CleanupWorker::systemToolPath(QStringLiteral("C:/Windows"),
                                                        QStringLiteral("netsh.exe"));
    QVERIFY(netsh.endsWith(QStringLiteral("System32/netsh.exe")));

    // R5 CRITICAL: the three production callers used to pass qEnvironmentVariable("SystemRoot"),
    // which a same-user attacker sets via HKCU\Environment as the classic UAC-bypass primitive.
    // The elevated worker then ran the attacker's sc.exe / schtasks.exe / netsh.exe. They now use
    // sak::system32Path() (GetSystemDirectoryW), and this seam refuses a root that could not be
    // the local Windows directory even if some future caller hands it one.
    QVERIFY2(CleanupWorker::systemToolPath(QStringLiteral("\\\\attacker\\share"),
                                           QStringLiteral("sc.exe"))
                 .isEmpty(),
             "a UNC root must never compose into a system tool path");
    QVERIFY2(CleanupWorker::systemToolPath(QStringLiteral("//attacker/share"),
                                           QStringLiteral("sc.exe"))
                 .isEmpty(),
             "a forward-slash UNC root must be refused too");
    QVERIFY2(CleanupWorker::systemToolPath(QStringLiteral("windows"), QStringLiteral("sc.exe"))
                 .isEmpty(),
             "a relative root resolves against the CWD and must be refused");
    QVERIFY2(CleanupWorker::systemToolPath(QStringLiteral("../../tmp/evil"),
                                           QStringLiteral("sc.exe"))
                 .isEmpty(),
             "a traversal root must be refused");
}

void TestCleanupWorker::systemTools_resolveThroughTheOsNotTheEnvironment() {
    // The real defence is that the callers ask the OS. Proving it here without launching anything:
    // poison %SystemRoot% the way the attack does, and confirm sak::system32Path -- the resolver
    // those callers now use -- is unmoved by it.
    const QString saved = qEnvironmentVariable("SystemRoot");
    qputenv("SystemRoot", QByteArrayLiteral("C:\\Users\\Public\\attacker"));

    // launchableSystemTool is the single resolver the three destructive callers use, so asserting
    // on it pins the CALLERS' choice of root -- not merely that some correct resolver exists
    // elsewhere in the tree. Reverting any of them to %SystemRoot% fails here.
    for (const QString& exe :
         {QStringLiteral("sc.exe"), QStringLiteral("schtasks.exe"), QStringLiteral("netsh.exe")}) {
        const QString resolved = CleanupWorker::launchableSystemTool(exe);
        QVERIFY2(!resolved.isEmpty(), "the OS-derived system directory must still resolve");
        QVERIFY2(!resolved.contains(QStringLiteral("attacker"), Qt::CaseInsensitive),
                 "the poisoned environment variable must not reach the resolved tool path");
        // endsWith(exe) alone is satisfied by ANY absolute path ending in the tool's name --
        // including QDir::currentPath() + "/netsh.exe", i.e. exactly the CWD hijack this test
        // exists to refuse. Require the System32 directory component; case-insensitive because
        // the OS returns its system directory in whatever case it stores it.
        QVERIFY2(resolved.endsWith(QStringLiteral("/System32/") + exe, Qt::CaseInsensitive),
                 "the destructive tools must resolve under the real System32 directory");
        QVERIFY(QDir::isAbsolutePath(resolved));
    }

    if (saved.isEmpty()) {
        qunsetenv("SystemRoot");
    } else {
        qputenv("SystemRoot", saved.toLocal8Bit());
    }
}

void TestCleanupWorker::deniedTargets_refusedWithoutDeletion() {
    // A caller that bypasses AdvancedUninstallController and hands the worker OS-critical targets
    // must be refused by the worker's own denylist re-screen -- BEFORE any destructive syscall runs
    // (no sc.exe launch, no filesystem touch). Both items are protected, so both fail with nothing
    // deleted.
    QVector<LeftoverItem> items;
    LeftoverItem svc;
    svc.type = LeftoverItem::Type::Service;
    svc.path = QStringLiteral("RpcSs");  // critical Windows service
    svc.selected = true;
    items.append(svc);

    LeftoverItem sys;
    sys.type = LeftoverItem::Type::File;
    sys.path = QStringLiteral("C:\\Windows");  // Windows system directory
    sys.selected = true;
    items.append(sys);

    CleanupWorker worker(items, /*useRecycleBin=*/false);
    QSignalSpy completeSpy(&worker, &CleanupWorker::cleanupComplete);
    QSignalSpy itemSpy(&worker, &CleanupWorker::itemCleaned);
    worker.start();
    QTRY_COMPARE_WITH_TIMEOUT(completeSpy.count(), 1, 5000);

    const auto args = completeSpy.takeFirst();
    QCOMPARE(args.at(0).toInt(), 0);  // zero succeeded -- both refused by the guard
    QCOMPARE(args.at(1).toInt(), 2);  // both failed
    // Pin WHICH item each report belongs to, and in what order: a loop over sig.at(1) alone stays
    // green if the worker reports the same item twice, swaps the two, or emits a rewritten path.
    // The path argument is what the panel shows beside each failure.
    QCOMPARE(itemSpy.count(), 2);
    QCOMPARE(itemSpy.at(0).at(0).toString(), QStringLiteral("RpcSs"));
    QCOMPARE(itemSpy.at(0).at(1).toBool(), false);
    QCOMPARE(itemSpy.at(1).at(0).toString(), QStringLiteral("C:\\Windows"));
    QCOMPARE(itemSpy.at(1).at(1).toBool(), false);
}

void TestCleanupWorker::itemCleanupRefusal_screensEachType() {
    // The per-type screen refuses a protected target for every leftover type and allows a benign
    // vendor target -- so the defense-in-depth layer covers the whole dispatch surface.
    const auto refusal = [](LeftoverItem::Type t, const QString& path, const QString& value = {}) {
        LeftoverItem item;
        item.type = t;
        item.path = path;
        item.registryValueName = value;
        return CleanupWorker::itemCleanupRefusal(item);
    };
    QVERIFY(!refusal(LeftoverItem::Type::RegistryKey, QStringLiteral("HKLM\\SYSTEM")).isEmpty());
    QVERIFY(!refusal(LeftoverItem::Type::Service, QStringLiteral("DcomLaunch")).isEmpty());
    // ScheduledTask had no positive control, so ANY non-empty refusal satisfied it -- including
    // one produced by a completely different screen. Pin the reason and the allow case.
    QCOMPARE(refusal(LeftoverItem::Type::ScheduledTask,
                     QStringLiteral("\\Microsoft\\Windows\\Foo")),
             QStringLiteral("refusing to delete a Windows/Microsoft scheduled task"));
    QVERIFY(refusal(LeftoverItem::Type::ScheduledTask, QStringLiteral("AcmeUpdateTask")).isEmpty());
    QVERIFY(!refusal(LeftoverItem::Type::FirewallRule, QStringLiteral("all")).isEmpty());
    // ShellExtension had NO positive control either, so any non-empty refusal from any screen
    // satisfied this -- e.g. splitting it onto registryValueDeletionRefusal yields "empty registry
    // value name" here and ships green, while a shell_extension item that DOES carry a
    // registry_value_name then reaches the VALUE screen, which has no shared-root check:
    // RegDeleteTreeW over the whole HKCR\CLSID tree. Pin the registry-KEY screen's own reason, and
    // allow a specific vendor CLSID subkey.
    QCOMPARE(refusal(LeftoverItem::Type::ShellExtension, QStringLiteral("HKCR\\CLSID")),
             QStringLiteral("refusing to delete a shared registry root key; delete a specific "
                            "subkey instead"));
    QVERIFY(refusal(LeftoverItem::Type::ShellExtension,
                    QStringLiteral("HKCR\\CLSID\\{11111111-2222-3333-4444-555555555555}"))
                .isEmpty());
    // A specific vendor leftover of each kind is allowed (empty refusal).
    QVERIFY(refusal(LeftoverItem::Type::RegistryKey, QStringLiteral("HKLM\\SOFTWARE\\Acme\\App"))
                .isEmpty());
    QVERIFY(refusal(LeftoverItem::Type::Service, QStringLiteral("AcmeUpdater")).isEmpty());
    QVERIFY(refusal(LeftoverItem::Type::FirewallRule, QStringLiteral("Acme App Rule")).isEmpty());
    // RegistryValue and StartupEntry were absent from a catalog that claims to cover every
    // leftover type, and StartupEntry is a TWO-ARM dispatch (registry value when named, else the
    // on-disk file) whose arms nothing reached -- so `case StartupEntry: return {};` ships green.
    // Pin both arms by their exact reason: with the arms inverted the value case would report the
    // file screen's message and the file case the value screen's "empty registry value name".
    QCOMPARE(refusal(LeftoverItem::Type::RegistryValue,
                     QStringLiteral("HKLM\\SYSTEM"),
                     QStringLiteral("Start")),
             QStringLiteral("refusing to modify a protected system registry subtree (system)"));
    QVERIFY(refusal(LeftoverItem::Type::RegistryValue,
                    QStringLiteral("HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Run"),
                    QStringLiteral("AcmeUpdater"))
                .isEmpty());
    QCOMPARE(refusal(LeftoverItem::Type::StartupEntry,
                     QStringLiteral("HKLM\\SYSTEM"),
                     QStringLiteral("Run")),
             QStringLiteral("refusing to modify a protected system registry subtree (system)"));
    QCOMPARE(refusal(LeftoverItem::Type::StartupEntry,
                     QStringLiteral("\\\\server\\share\\Acme\\run.exe")),
             QStringLiteral("path must be an absolute drive-letter path"));
}

QTEST_MAIN(TestCleanupWorker)
#include "test_cleanup_worker.moc"
