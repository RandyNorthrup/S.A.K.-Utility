// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_resource_soak.cpp
/// @brief R5-G23-10: long-session resource-leak soak.
///
/// Technicians leave this application open all day launching operations, so a
/// per-operation kernel-handle, GDI/USER object or memory leak that is invisible
/// in a single run accumulates over hours into an all-day crash. This soak drives
/// the REAL process launchers -- the dominant all-day workload and a classic
/// handle-leak vector (QProcess owns process/thread/pipe/job handles) -- many
/// times and asserts the process's own handle count, GDI/USER object counts and
/// (post-warmup) working set do not grow. A genuine per-launch leak multiplies
/// across the soak and blows the tight tolerances below; a clean teardown returns
/// every count to its baseline.

#include "sak/process_runner.h"

#include <QProcessEnvironment>
#include <QtTest/QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
// <psapi.h> must follow <windows.h>.
#include <psapi.h>
#endif

namespace {

#ifdef Q_OS_WIN
struct ResourceSnapshot {
    qint64 handles{0};
    qint64 gdiObjects{0};
    qint64 userObjects{0};
    qint64 workingSetBytes{0};
};

/// @brief Sample this process's own resource counts. Handle count comes from
///        kernel32 (leak-precise), GDI/USER object counts from user32 (flat in a
///        guiless process), working set from K32GetProcessMemoryInfo (kernel32, so
///        no psapi.lib dependency is added to the link).
ResourceSnapshot captureResources() {
    ResourceSnapshot snap;
    const HANDLE self = GetCurrentProcess();
    DWORD handleCount = 0;
    if (GetProcessHandleCount(self, &handleCount)) {
        snap.handles = handleCount;
    }
    snap.gdiObjects = GetGuiResources(self, GR_GDIOBJECTS);
    snap.userObjects = GetGuiResources(self, GR_USEROBJECTS);
    PROCESS_MEMORY_COUNTERS counters{};
    counters.cb = sizeof(counters);
    if (K32GetProcessMemoryInfo(self, &counters, sizeof(counters))) {
        snap.workingSetBytes = static_cast<qint64>(counters.WorkingSetSize);
    }
    return snap;
}

/// @brief One representative all-day launch. Alternates the plain and
///        environment-carrying launchers so BOTH real handle paths are soaked.
void doOneLaunch(int index) {
    if (index % 2 == 0) {
        const auto result = sak::runProcess("cmd.exe", {"/C", "exit /b 0"}, 10'000);
        QVERIFY(result.succeeded());
    } else {
        const auto result = sak::runProcessWithEnvironment(
            "cmd.exe", {"/C", "exit /b 0"}, 10'000, QProcessEnvironment::systemEnvironment());
        QVERIFY(result.succeeded());
    }
}
#endif

}  // namespace

class ResourceSoakTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void processLaunchers_noResourceGrowthOverLongSession();
};

void ResourceSoakTests::processLaunchers_noResourceGrowthOverLongSession() {
#ifndef Q_OS_WIN
    QSKIP("Resource-soak counters are Windows-only (GetProcessHandleCount/GetGuiResources).");
#else
    constexpr int kWarmup = 16;
    constexpr int kSoak = 128;

    // Warm up first so one-time allocator arenas, threadpool spin-up and loader
    // caching are already paid for before the baseline -- otherwise that first-run
    // fill would read as a false leak against the post-soak sample.
    for (int i = 0; i < kWarmup; ++i) {
        doOneLaunch(i);
    }
    const ResourceSnapshot before = captureResources();

    for (int i = 0; i < kSoak; ++i) {
        doOneLaunch(i);
    }
    const ResourceSnapshot after = captureResources();

    // Kernel handles: leak-precise, low-noise. One leaked handle per launch would
    // be +128; the slack only absorbs benign allocator/threadpool handle churn.
    const qint64 handleDelta = after.handles - before.handles;
    QVERIFY2(handleDelta <= 24,
             qPrintable(QStringLiteral("kernel handle growth %1 over %2 launches")
                            .arg(handleDelta)
                            .arg(kSoak)));

    // Guiless process: GDI/USER object counts must stay flat (no stray HDC/HWND).
    const qint64 gdiDelta = after.gdiObjects - before.gdiObjects;
    QVERIFY2(gdiDelta <= 8, qPrintable(QStringLiteral("GDI object growth %1").arg(gdiDelta)));
    const qint64 userDelta = after.userObjects - before.userObjects;
    QVERIFY2(userDelta <= 8, qPrintable(QStringLiteral("USER object growth %1").arg(userDelta)));

    // Working set (measured post-warmup so allocator arenas do not count): a
    // generous ceiling that still catches a gross per-launch retention accumulating
    // across the soak, while tolerating benign heap-arena noise.
    const qint64 workingSetDelta = after.workingSetBytes - before.workingSetBytes;
    constexpr qint64 kWorkingSetCeiling = 16LL * 1024 * 1024;
    QVERIFY2(workingSetDelta <= kWorkingSetCeiling,
             qPrintable(QStringLiteral("working-set growth %1 bytes").arg(workingSetDelta)));
#endif
}

QTEST_GUILESS_MAIN(ResourceSoakTests)
#include "test_resource_soak.moc"
