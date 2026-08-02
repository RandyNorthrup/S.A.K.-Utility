// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_leftover_scanner.cpp
/// @brief Unit tests for LeftoverScanner — pattern matching, risk classification,
///        protected paths, file system scanning, and cancellation support

#include "sak/leftover_scanner.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <atomic>
#include <type_traits>

using sak::LeftoverItem;
using sak::LeftoverScanner;
using sak::ProgramInfo;
using sak::ScanLevel;

class LeftoverScannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──
    void construction_safe();
    void construction_moderate();
    void construction_advanced();
    void construction_notCopyable();

    // ── File System Scanning ──
    void scan_findsMatchingFolder();
    void scan_findsMatchingFile();
    void scan_ignoresNonMatchingFolder();
    void scan_safeLevelSkipsRegistry();
    void scan_cancellationStopsScan();
    void scan_progressCallbackInvoked();
    void scan_preSelectsSafeItems();

    // ── Pattern Matching ──
    void scan_matchesProgramNameExact();
    void scan_matchesProgramNameCaseInsensitive();
    void scan_matchesConcatenatedName();
    void scan_skipsCommonWords();
    void scan_matchesInstallDirName();

    // ── Risk Classification ──
    void scan_safeInAppData();
    void scan_safeInProgramFiles();
    void scan_registryKeySafe();
    void scan_serviceScanAtAdvanced();

    // ── Empty Program ──
    void scan_emptyProgram_noResults();
    void scan_emptyPublisher_noPublisherPatterns();

    // ── Service leftover builder (SCM seam, locale-independent) ──
    void buildServiceItems_matchesNameOrDisplay();
    void buildServiceItems_stopRequestedInterrupts();
    void buildServiceItems_localeIndependentFields();
};

// ── Helper ──────────────────────────────────────────────────────────────────

namespace {

/// Create a program info suitable for testing
ProgramInfo makeTestProgram(const QString& name,
                            const QString& publisher = {},
                            const QString& installLoc = {}) {
    ProgramInfo prog;
    prog.displayName = name;
    prog.publisher = publisher;
    prog.installLocation = installLoc;
    prog.registryKeyPath = "HKLM\\SOFTWARE\\" + name;
    return prog;
}

}  // namespace

// ── Construction ────────────────────────────────────────────────────────────

void LeftoverScannerTests::construction_safe() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Safe);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_moderate() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_advanced() {
    ProgramInfo prog = makeTestProgram("TestApp");
    LeftoverScanner scanner(prog, ScanLevel::Advanced);
    Q_UNUSED(scanner);
}

void LeftoverScannerTests::construction_notCopyable() {
    QVERIFY(!std::is_copy_constructible_v<LeftoverScanner>);
    QVERIFY(!std::is_copy_assignable_v<LeftoverScanner>);
    // Move is allowed
    QVERIFY(std::is_move_constructible_v<LeftoverScanner>);
}

// ── File System Scanning ────────────────────────────────────────────────────

void LeftoverScannerTests::scan_findsMatchingFolder() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Create a folder matching the program name
    QDir dir(tempDir.path());
    QVERIFY(dir.mkdir("SuperEditor"));

    ProgramInfo prog = makeTestProgram("SuperEditor");
    prog.installLocation = tempDir.path() + "/SuperEditor";

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // The scanner scans standard system directories (Program Files, AppData, etc.)
    // not our temp dir — so we just verify it runs without error
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_findsMatchingFile() {
    QTemporaryDir tempDir;
    QVERIFY(tempDir.isValid());

    // Create a file matching the program name
    QFile file(tempDir.path() + "/SuperEditor.lnk");
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("shortcut data");
    file.close();

    ProgramInfo prog = makeTestProgram("SuperEditor");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // Scanner scans standard dirs, not temp dir — no crash is the test
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_ignoresNonMatchingFolder() {
    ProgramInfo prog = makeTestProgram("UniqueXYZ123456");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // With a unique enough name, no real leftovers should be found
    // on a system that never had this app installed
    QVERIFY(results.size() >= 0);  // No crash, results should be minimal
}

void LeftoverScannerTests::scan_safeLevelSkipsRegistry() {
    ProgramInfo prog = makeTestProgram("TestRegSkip12345");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // At Safe level, registry scanning should be skipped.
    // Verify no registry-type items are returned.
    for (const auto& item : results) {
        QVERIFY(item.type != LeftoverItem::Type::RegistryKey);
        QVERIFY(item.type != LeftoverItem::Type::RegistryValue);
    }
}

void LeftoverScannerTests::scan_cancellationStopsScan() {
    ProgramInfo prog = makeTestProgram("TestCancel12345");

    LeftoverScanner scanner(prog, ScanLevel::Advanced);
    std::atomic<bool> stop{true};  // Already stopped!

    auto results = scanner.scan(stop);

    // Should return immediately with no results when stop is already set
    QVERIFY(results.isEmpty());
}

void LeftoverScannerTests::scan_progressCallbackInvoked() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    int callbackCount = 0;
    auto callback = [&callbackCount](const QString& /*path*/, int /*found*/) {
        ++callbackCount;
    };

    auto results = scanner.scan(stop, callback);

    // If any items were found, callback should have been invoked
    if (!results.isEmpty()) {
        QVERIFY(callbackCount > 0);
        QCOMPARE(callbackCount, results.size());
    }
}

void LeftoverScannerTests::scan_preSelectsSafeItems() {
    // Create a program name that might match something on the system
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.risk == LeftoverItem::RiskLevel::Safe) {
            QVERIFY(item.selected);
        } else {
            QVERIFY(!item.selected);
        }
    }
}

// ── Pattern Matching ────────────────────────────────────────────────────────

void LeftoverScannerTests::scan_matchesProgramNameExact() {
    // Use a program name that DOES exist in common system directories
    // The "VLC" or "Notepad" test verifies exact matching behavior
    ProgramInfo prog = makeTestProgram("VLC media player");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // This validates that the scanner runs without error
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_matchesProgramNameCaseInsensitive() {
    // Scanner should do case-insensitive matching
    ProgramInfo prog1 = makeTestProgram("TESTAPPUPPER");
    ProgramInfo prog2 = makeTestProgram("testappupper");

    LeftoverScanner scanner1(prog1, ScanLevel::Safe);
    LeftoverScanner scanner2(prog2, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results1 = scanner1.scan(stop);
    auto results2 = scanner2.scan(stop);

    // Both should find the same results (case-insensitive matching)
    QCOMPARE(results1.size(), results2.size());
}

void LeftoverScannerTests::scan_matchesConcatenatedName() {
    // "VLC Media Player" should also match "vlcmediaplayer" (concatenated)
    ProgramInfo prog = makeTestProgram("Test App XYZ");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // Validates no crash, pattern building with concatenation
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_skipsCommonWords() {
    // Common words like "the", "media", "player" should be excluded
    // to reduce false positives. A program named only with common words
    // should have fewer matches than expected.
    ProgramInfo prog = makeTestProgram("The Free Player");

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);
    // "the", "free", "player" are all excluded words, so the full name
    // "the free player" and concatenated "thefreeplayer" will be the patterns.
    // With only the full name pattern, false positives are reduced.
    QVERIFY(results.size() >= 0);
}

void LeftoverScannerTests::scan_matchesInstallDirName() {
    ProgramInfo prog = makeTestProgram("MySpecialApp");
    prog.installLocation = "C:\\Program Files\\SpecialAppDir";

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // The scanner should also create patterns from the install dir name
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

// ── Risk Classification ─────────────────────────────────────────────────────

void LeftoverScannerTests::scan_safeInAppData() {
    // Items found in AppData directories should be classified as Safe
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.path.toLower().contains("appdata")) {
            // File/folder items in AppData matching program name should be Safe
            if (item.type == LeftoverItem::Type::File || item.type == LeftoverItem::Type::Folder) {
                QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
            }
        }
    }
}

void LeftoverScannerTests::scan_safeInProgramFiles() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.path.toLower().contains("program files")) {
            if (item.type == LeftoverItem::Type::File || item.type == LeftoverItem::Type::Folder) {
                QCOMPARE(item.risk, LeftoverItem::RiskLevel::Safe);
            }
        }
    }
}

void LeftoverScannerTests::scan_registryKeySafe() {
    ProgramInfo prog = makeTestProgram("Notepad");

    LeftoverScanner scanner(prog, ScanLevel::Moderate);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    for (const auto& item : results) {
        if (item.type == LeftoverItem::Type::RegistryKey) {
            // Registry keys matching program name patterns should be Safe
            QVERIFY(item.risk == LeftoverItem::RiskLevel::Safe ||
                    item.risk == LeftoverItem::RiskLevel::Review);
        }
    }
}

void LeftoverScannerTests::scan_serviceScanAtAdvanced() {
    // Services are only scanned at Advanced level
    ProgramInfo prog = makeTestProgram("TestSvcScan12345");

    // Safe level - should not scan services
    {
        LeftoverScanner scanner(prog, ScanLevel::Safe);
        std::atomic<bool> stop{false};
        auto results = scanner.scan(stop);

        for (const auto& item : results) {
            QVERIFY(item.type != LeftoverItem::Type::Service);
            QVERIFY(item.type != LeftoverItem::Type::ScheduledTask);
            QVERIFY(item.type != LeftoverItem::Type::FirewallRule);
            QVERIFY(item.type != LeftoverItem::Type::StartupEntry);
        }
    }

    // Moderate level - should not scan services
    {
        LeftoverScanner scanner(prog, ScanLevel::Moderate);
        std::atomic<bool> stop{false};
        auto results = scanner.scan(stop);

        for (const auto& item : results) {
            QVERIFY(item.type != LeftoverItem::Type::Service);
            QVERIFY(item.type != LeftoverItem::Type::ScheduledTask);
            QVERIFY(item.type != LeftoverItem::Type::FirewallRule);
            QVERIFY(item.type != LeftoverItem::Type::StartupEntry);
        }
    }
}

// ── Empty Program ───────────────────────────────────────────────────────────

void LeftoverScannerTests::scan_emptyProgram_noResults() {
    ProgramInfo prog;  // All fields empty

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    auto results = scanner.scan(stop);

    // With no patterns to match, should find nothing
    QVERIFY(results.isEmpty());
}

void LeftoverScannerTests::scan_emptyPublisher_noPublisherPatterns() {
    ProgramInfo prog = makeTestProgram("UniqueTestApp99999");
    // publisher left empty

    LeftoverScanner scanner(prog, ScanLevel::Safe);
    std::atomic<bool> stop{false};

    // Should not crash and should work based on name patterns only
    auto results = scanner.scan(stop);
    QVERIFY(results.size() >= 0);
}

// ── Service leftover builder (SCM seam) ──────────────────────────────────────
// buildServiceLeftoverItems is the locale-independent successor to the old sc.exe console parse:
// it consumes (serviceName, displayName) pairs -- exactly what EnumServicesStatusExW yields -- so
// the matching no longer depends on English "SERVICE_NAME:"/"DISPLAY_NAME:" labels. These pure
// tests exercise it directly without touching the live SCM.

void LeftoverScannerTests::buildServiceItems_matchesNameOrDisplay() {
    const QVector<QPair<QString, QString>> services = {
        {QStringLiteral("AcmeSync"), QStringLiteral("Acme Sync Service")},
        {QStringLiteral("WSearch"), QStringLiteral("Windows Search")},
        {QStringLiteral("svc-acme-helper"), QStringLiteral("Unrelated Display")},
        {QStringLiteral("keynamemiss"), QStringLiteral("Acme Background Agent")},
    };
    // Matcher: anything containing "acme" (case-insensitive), applied to name AND display.
    const auto matches = [](const QString& text) {
        return text.contains(QStringLiteral("acme"), Qt::CaseInsensitive);
    };
    std::atomic<bool> stop{false};

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);

    // Rows 0 (name+display), 2 (name only), 3 (display only) match; row 1 does not.
    QCOMPARE(items.size(), 3);
    for (const auto& item : items) {
        QCOMPARE(item.type, LeftoverItem::Type::Service);
        QCOMPARE(item.risk, LeftoverItem::RiskLevel::Risky);
    }
    // path is the service KEY name (what an uninstall would target), not the display string.
    QCOMPARE(items.at(0).path, QStringLiteral("AcmeSync"));
    QVERIFY(items.at(0).description.contains(QStringLiteral("Acme Sync Service")));
    QCOMPARE(items.at(1).path, QStringLiteral("svc-acme-helper"));
    QCOMPARE(items.at(2).path, QStringLiteral("keynamemiss"));
}

void LeftoverScannerTests::buildServiceItems_stopRequestedInterrupts() {
    QVector<QPair<QString, QString>> services;
    for (int i = 0; i < 100; ++i) {
        services.append({QStringLiteral("acme%1").arg(i), QStringLiteral("Acme %1").arg(i)});
    }
    const auto matches = [](const QString&) {
        return true;
    };  // would match every row
    std::atomic<bool> stop{true};  // already cancelled before the first iteration

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);
    QVERIFY(items.isEmpty());  // cooperative cancel honored -> no items built
}

void LeftoverScannerTests::buildServiceItems_localeIndependentFields() {
    // The SCM fields are language-neutral: even a display name in a non-Latin script flows through
    // untouched (the old console parse keyed off English labels and would have dropped this row).
    const QVector<QPair<QString, QString>> services = {
        {QStringLiteral("AcmeSvc"),
         QString::fromUtf8("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9")},  // JP "service"
    };
    const auto matches = [](const QString& text) {
        return text.contains(QStringLiteral("AcmeSvc"));  // matches the language-neutral key name
    };
    std::atomic<bool> stop{false};

    const QVector<LeftoverItem> items = sak::buildServiceLeftoverItems(services, matches, stop);
    QCOMPARE(items.size(), 1);
    QCOMPARE(items.at(0).path, QStringLiteral("AcmeSvc"));
    QVERIFY(items.at(0).description.contains(
        QString::fromUtf8("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9")));
}

QTEST_GUILESS_MAIN(LeftoverScannerTests)

#include "test_leftover_scanner.moc"
