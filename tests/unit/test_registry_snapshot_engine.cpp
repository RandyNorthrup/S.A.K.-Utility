// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_registry_snapshot_engine.cpp
/// @brief Unit tests for RegistrySnapshotEngine — diff logic, pattern matching,
///        snapshot capture smoke test

#include <QtTest/QtTest>

#include "sak/registry_snapshot_engine.h"

#include <QSet>

#include <type_traits>

using sak::LeftoverItem;
using sak::RegistrySnapshotEngine;

class RegistrySnapshotEngineTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Compile-Time Invariants ──
    void staticAsserts_notCopyable();
    void staticAsserts_moveConstructible();

    // ── diffSnapshots — Survived Keys ──
    void diff_survivedKey_matchesPattern();
    void diff_survivedKey_noMatch();
    void diff_survivedKey_caseInsensitive();
    void diff_survivedKey_multiplePatterns();

    // ── diffSnapshots — Added Keys ──
    void diff_addedKey_matchesPattern();
    void diff_addedKey_noMatch();
    void diff_addedKey_caseInsensitive();

    // ── diffSnapshots — Removed Keys ──
    void diff_removedKeyBefore_notDetected();

    // ── diffSnapshots — Empty Inputs ──
    void diff_bothEmpty_noResults();
    void diff_emptyBefore_findsAdded();
    void diff_emptyAfter_noResults();
    void diff_emptyPatterns_noResults();

    // ── diffSnapshots — Item Properties ──
    void diff_itemType_isRegistryKey();
    void diff_itemDescription_survived();
    void diff_itemDescription_added();
    void diff_itemRisk_isReview();
    void diff_itemSelected_isFalse();

    // ── diffSnapshots — Complex Scenarios ──
    void diff_mixedSurvivedAndAdded();
    void diff_largeSnapshot();
    void diff_patternPartialMatch();

    // ── captureSnapshot ──
    void captureSnapshot_returnsNonEmpty();
    void captureSnapshot_idempotent();
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

void RegistrySnapshotEngineTests::staticAsserts_notCopyable()
{
    QVERIFY(!std::is_copy_constructible_v<RegistrySnapshotEngine>);
    QVERIFY(!std::is_copy_assignable_v<RegistrySnapshotEngine>);
}

void RegistrySnapshotEngineTests::staticAsserts_moveConstructible()
{
    QVERIFY(std::is_move_constructible_v<RegistrySnapshotEngine>);
    QVERIFY(std::is_default_constructible_v<RegistrySnapshotEngine>);
}

// ── diffSnapshots — Survived Keys ───────────────────────────────────────────

void RegistrySnapshotEngineTests::diff_survivedKey_matchesPattern()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp", "HKLM\\SOFTWARE\\Other"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp", "HKLM\\SOFTWARE\\Other"};

    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].path, "HKLM\\SOFTWARE\\TestApp");
}

void RegistrySnapshotEngineTests::diff_survivedKey_noMatch()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\UnrelatedApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\UnrelatedApp"};

    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QVERIFY(results.isEmpty());
}

void RegistrySnapshotEngineTests::diff_survivedKey_caseInsensitive()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TESTAPP"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TESTAPP"};

    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
}

void RegistrySnapshotEngineTests::diff_survivedKey_multiplePatterns()
{
    QSet<QString> before = {
        "HKLM\\SOFTWARE\\AppOne",
        "HKLM\\SOFTWARE\\AppTwo",
        "HKLM\\SOFTWARE\\Unrelated"
    };
    QSet<QString> after = {
        "HKLM\\SOFTWARE\\AppOne",
        "HKLM\\SOFTWARE\\AppTwo",
        "HKLM\\SOFTWARE\\Unrelated"
    };

    QStringList patterns = {"appone", "apptwo"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 2);
}

// ── diffSnapshots — Added Keys ──────────────────────────────────────────────

void RegistrySnapshotEngineTests::diff_addedKey_matchesPattern()
{
    QSet<QString> before;  // Empty
    QSet<QString> after = {"HKCU\\Software\\NewTestApp"};

    QStringList patterns = {"newtestapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].path, "HKCU\\Software\\NewTestApp");
    QVERIFY(results[0].description.contains("added"));
}

void RegistrySnapshotEngineTests::diff_addedKey_noMatch()
{
    QSet<QString> before;
    QSet<QString> after = {"HKCU\\Software\\SomethingElse"};

    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QVERIFY(results.isEmpty());
}

void RegistrySnapshotEngineTests::diff_addedKey_caseInsensitive()
{
    QSet<QString> before;
    QSet<QString> after = {"HKCU\\Software\\MYAPP"};

    QStringList patterns = {"MyApp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
}

// ── diffSnapshots — Removed Keys ────────────────────────────────────────────

void RegistrySnapshotEngineTests::diff_removedKeyBefore_notDetected()
{
    // Keys in 'before' but not in 'after' = properly uninstalled, not leftovers
    QSet<QString> before = {"HKLM\\SOFTWARE\\RemovedApp"};
    QSet<QString> after;

    QStringList patterns = {"removedapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    // Properly removed keys should NOT be reported as leftovers
    QVERIFY(results.isEmpty());
}

// ── diffSnapshots — Empty Inputs ────────────────────────────────────────────

void RegistrySnapshotEngineTests::diff_bothEmpty_noResults()
{
    QSet<QString> before;
    QSet<QString> after;
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QVERIFY(results.isEmpty());
}

void RegistrySnapshotEngineTests::diff_emptyBefore_findsAdded()
{
    QSet<QString> before;
    QSet<QString> after = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].description.contains("added"));
}

void RegistrySnapshotEngineTests::diff_emptyAfter_noResults()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after;
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QVERIFY(results.isEmpty());
}

void RegistrySnapshotEngineTests::diff_emptyPatterns_noResults()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns;  // Empty

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QVERIFY(results.isEmpty());
}

// ── diffSnapshots — Item Properties ─────────────────────────────────────────

void RegistrySnapshotEngineTests::diff_itemType_isRegistryKey()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].type, LeftoverItem::Type::RegistryKey);
}

void RegistrySnapshotEngineTests::diff_itemDescription_survived()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].description.contains("survived"));
}

void RegistrySnapshotEngineTests::diff_itemDescription_added()
{
    QSet<QString> before;
    QSet<QString> after = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QVERIFY(results[0].description.contains("added"));
}

void RegistrySnapshotEngineTests::diff_itemRisk_isReview()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].risk, LeftoverItem::RiskLevel::Review);
}

void RegistrySnapshotEngineTests::diff_itemSelected_isFalse()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\TestApp"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\TestApp"};
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QVERIFY(!results[0].selected);
}

// ── diffSnapshots — Complex Scenarios ───────────────────────────────────────

void RegistrySnapshotEngineTests::diff_mixedSurvivedAndAdded()
{
    QSet<QString> before = {
        "HKLM\\SOFTWARE\\TestApp\\Settings",
        "HKLM\\SOFTWARE\\Other"
    };
    QSet<QString> after = {
        "HKLM\\SOFTWARE\\TestApp\\Settings",    // Survived
        "HKLM\\SOFTWARE\\TestApp\\Cache",        // Added
        "HKLM\\SOFTWARE\\Other"                  // Survived but doesn't match
    };

    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    // Should find survived TestApp\Settings + added TestApp\Cache = 2
    QCOMPARE(results.size(), 2);

    // Verify one survived and one added
    bool found_survived = false;
    bool found_added = false;
    for (const auto& item : results) {
        if (item.description.contains("survived")) found_survived = true;
        if (item.description.contains("added")) found_added = true;
    }
    QVERIFY(found_survived);
    QVERIFY(found_added);
}

void RegistrySnapshotEngineTests::diff_largeSnapshot()
{
    QSet<QString> before;
    QSet<QString> after;
    QStringList patterns = {"target"};

    // Populate with 1000 keys, only a few matching
    for (int i = 0; i < 1000; ++i) {
        QString key = QString("HKLM\\SOFTWARE\\App%1").arg(i);
        before.insert(key);
        after.insert(key);
    }
    // Add targeted entries
    before.insert("HKLM\\SOFTWARE\\TargetApp");
    after.insert("HKLM\\SOFTWARE\\TargetApp");

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    QCOMPARE(results.size(), 1);
    QCOMPARE(results[0].path, "HKLM\\SOFTWARE\\TargetApp");
}

void RegistrySnapshotEngineTests::diff_patternPartialMatch()
{
    QSet<QString> before = {"HKLM\\SOFTWARE\\MyTestAppPro"};
    QSet<QString> after  = {"HKLM\\SOFTWARE\\MyTestAppPro"};

    // Pattern is a substring of the key name
    QStringList patterns = {"testapp"};

    auto results = RegistrySnapshotEngine::diffSnapshots(before, after, patterns);

    // Should match because "testapp" is contained in "MyTestAppPro"
    QCOMPARE(results.size(), 1);
}

// ── captureSnapshot ─────────────────────────────────────────────────────────

void RegistrySnapshotEngineTests::captureSnapshot_returnsNonEmpty()
{
    // On a Windows system, the snapshot should contain entries
    auto snapshot = RegistrySnapshotEngine::captureSnapshot();
    QVERIFY(!snapshot.isEmpty());
}

void RegistrySnapshotEngineTests::captureSnapshot_idempotent()
{
    // Two consecutive captures should return the same set
    // (assuming no other processes are modifying registry)
    auto snapshot1 = RegistrySnapshotEngine::captureSnapshot();
    auto snapshot2 = RegistrySnapshotEngine::captureSnapshot();

    // Allow for minor differences (race with system activity)
    // but they should be largely the same
    int common = 0;
    for (const auto& key : snapshot1) {
        if (snapshot2.contains(key)) {
            ++common;
        }
    }

    // At least 95% overlap expected
    double overlap = static_cast<double>(common) / snapshot1.size();
    QVERIFY2(overlap > 0.95,
             qPrintable(QString("Snapshot overlap: %1%").arg(overlap * 100, 0, 'f', 1)));
}

QTEST_GUILESS_MAIN(RegistrySnapshotEngineTests)

#include "test_registry_snapshot_engine.moc"
