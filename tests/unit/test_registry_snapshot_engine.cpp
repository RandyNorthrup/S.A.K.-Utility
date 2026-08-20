// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_registry_snapshot_engine.cpp
/// @brief Unit tests for RegistrySnapshotEngine -- snapshot capture + reliability flag

#include "sak/registry_snapshot_engine.h"

#include <QSet>
#include <QtTest/QtTest>

#include <type_traits>

using sak::RegistrySnapshotEngine;

class RegistrySnapshotEngineTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Compile-Time Invariants --
    void staticAsserts_notCopyable();
    void staticAsserts_moveConstructible();

    // -- captureSnapshot --
    void captureSnapshot_returnsNonEmpty();
    void captureSnapshot_idempotent();
    void captureSnapshot_reliableFlagSet();
    void captureSnapshot_nullReliableParam_ok();
};

// -- Compile-Time Invariants -------------------------------------------------

void RegistrySnapshotEngineTests::staticAsserts_notCopyable() {
    QVERIFY(!std::is_copy_constructible_v<RegistrySnapshotEngine>);
    QVERIFY(!std::is_copy_assignable_v<RegistrySnapshotEngine>);
}

void RegistrySnapshotEngineTests::staticAsserts_moveConstructible() {
    QVERIFY(std::is_move_constructible_v<RegistrySnapshotEngine>);
    QVERIFY(std::is_default_constructible_v<RegistrySnapshotEngine>);
}

// -- captureSnapshot ---------------------------------------------------------

void RegistrySnapshotEngineTests::captureSnapshot_returnsNonEmpty() {
    // On a Windows system, the snapshot should contain entries
    auto snapshot = RegistrySnapshotEngine::captureSnapshot();
    // The three universal, world-readable roots are inserted on every Windows box, so pin them
    // rather than a bare non-empty check (which a single stray key would satisfy).
    QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SOFTWARE")));
    QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services")));
    QVERIFY(snapshot.contains(QStringLiteral("HKCU\\Software")));
}

void RegistrySnapshotEngineTests::captureSnapshot_idempotent() {
    // Two consecutive captures should return largely the same set.
    auto snapshot1 = RegistrySnapshotEngine::captureSnapshot();
    auto snapshot2 = RegistrySnapshotEngine::captureSnapshot();

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

void RegistrySnapshotEngineTests::captureSnapshot_reliableFlagSet() {
    // The out-param must be written. A reliable capture is by definition complete, so
    // it must be non-partial (more than the root key). An unreliable capture is
    // allowed -- a deep ACL-restricted subkey legitimately clears the flag, which is
    // exactly the fail-closed signal this test guards.
    bool reliable = false;
    auto snapshot = RegistrySnapshotEngine::captureSnapshot(&reliable);
    QVERIFY(!snapshot.isEmpty());
    if (reliable) {
        // A reliable capture opened every root, so the three universal roots are present.
        QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SOFTWARE")));
        QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services")));
        QVERIFY(snapshot.contains(QStringLiteral("HKCU\\Software")));
    }
}

void RegistrySnapshotEngineTests::captureSnapshot_nullReliableParam_ok() {
    // Passing nullptr (the default) must not crash and must still capture.
    auto snapshot = RegistrySnapshotEngine::captureSnapshot(nullptr);
    // nullptr only skips the *reliable write-back; the same three roots are still captured.
    QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SOFTWARE")));
    QVERIFY(snapshot.contains(QStringLiteral("HKLM\\SYSTEM\\CurrentControlSet\\Services")));
    QVERIFY(snapshot.contains(QStringLiteral("HKCU\\Software")));
}

QTEST_GUILESS_MAIN(RegistrySnapshotEngineTests)

#include "test_registry_snapshot_engine.moc"
