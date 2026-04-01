// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_elevation_tier.cpp
/// @brief Unit tests for ElevationTier classification and feature lookup

#include "sak/elevation_tier.h"

#include <QtTest/QtTest>

#include <set>

class ElevationTierTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ElevationTier enum
    void tierToString_standard();
    void tierToString_elevated();
    void tierToString_mixed();

    // Feature table completeness
    void featureTable_isNotEmpty();
    void featureTable_noDuplicateIds();
    void featureTable_allHaveNames();
    void featureTable_elevatedHaveReasons();
    void featureTable_standardHaveEmptyReasons();

    // Feature lookup
    void findFeature_existingId();
    void findFeature_unknownId();

    // featureNeedsElevation
    void needsElevation_standardFeature();
    void needsElevation_elevatedFeature();
    void needsElevation_mixedFeature();
    void needsElevation_unknownFeature();

    // Specific tier classifications
    void tier_advancedSearchIsStandard();
    void tier_flashUsbIsElevated();
    void tier_backupCurrentUserIsStandard();
    void tier_backupCrossUserIsElevated();
    void tier_networkDiagnosticsIsStandard();
    void tier_bitlockerIsElevated();
};

// ============================================================================
// ElevationTier enum
// ============================================================================

void ElevationTierTests::tierToString_standard() {
    QCOMPARE(sak::to_string(sak::ElevationTier::Standard), std::string_view("Standard"));
}

void ElevationTierTests::tierToString_elevated() {
    QCOMPARE(sak::to_string(sak::ElevationTier::Elevated), std::string_view("Elevated"));
}

void ElevationTierTests::tierToString_mixed() {
    QCOMPARE(sak::to_string(sak::ElevationTier::Mixed), std::string_view("Mixed"));
}

// ============================================================================
// Feature table integrity
// ============================================================================

void ElevationTierTests::featureTable_isNotEmpty() {
    QVERIFY(sak::kFeatureCount > 0);
}

void ElevationTierTests::featureTable_noDuplicateIds() {
    std::set<uint16_t> seen;
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        auto id_value = static_cast<uint16_t>(sak::kFeatureElevationTable[i].id);
        QVERIFY2(seen.find(id_value) == seen.end(),
                 qPrintable(QString("Duplicate FeatureId: %1").arg(id_value)));
        seen.insert(id_value);
    }
}

void ElevationTierTests::featureTable_allHaveNames() {
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        QVERIFY2(!sak::kFeatureElevationTable[i].name.empty(),
                 qPrintable(QString("Feature at index %1 has empty name").arg(i)));
    }
}

void ElevationTierTests::featureTable_elevatedHaveReasons() {
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        const auto& entry = sak::kFeatureElevationTable[i];
        if (entry.tier == sak::ElevationTier::Elevated) {
            QVERIFY2(!entry.reason.empty(),
                     qPrintable(QString("Elevated feature '%1' missing reason")
                                    .arg(QString::fromUtf8(entry.name.data(),
                                                           static_cast<int>(entry.name.size())))));
        }
    }
}

void ElevationTierTests::featureTable_standardHaveEmptyReasons() {
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        const auto& entry = sak::kFeatureElevationTable[i];
        if (entry.tier == sak::ElevationTier::Standard) {
            QVERIFY2(entry.reason.empty(),
                     qPrintable(QString("Standard feature '%1' should not have a reason")
                                    .arg(QString::fromUtf8(entry.name.data(),
                                                           static_cast<int>(entry.name.size())))));
        }
    }
}

// ============================================================================
// Feature lookup
// ============================================================================

void ElevationTierTests::findFeature_existingId() {
    const auto* entry = sak::findFeatureElevation(sak::FeatureId::AdvancedSearch);
    QVERIFY(entry != nullptr);
    QCOMPARE(entry->id, sak::FeatureId::AdvancedSearch);
    QCOMPARE(entry->tier, sak::ElevationTier::Standard);
}

void ElevationTierTests::findFeature_unknownId() {
    // Use a value not in the table
    const auto* entry = sak::findFeatureElevation(static_cast<sak::FeatureId>(9999));
    QVERIFY(entry == nullptr);
}

// ============================================================================
// featureNeedsElevation
// ============================================================================

void ElevationTierTests::needsElevation_standardFeature() {
    QVERIFY(!sak::featureNeedsElevation(sak::FeatureId::AdvancedSearch));
}

void ElevationTierTests::needsElevation_elevatedFeature() {
    QVERIFY(sak::featureNeedsElevation(sak::FeatureId::FlashUsbDrive));
}

void ElevationTierTests::needsElevation_mixedFeature() {
    QVERIFY(sak::featureNeedsElevation(sak::FeatureId::PermissionStripInherit));
}

void ElevationTierTests::needsElevation_unknownFeature() {
    QVERIFY(!sak::featureNeedsElevation(static_cast<sak::FeatureId>(9999)));
}

// ============================================================================
// Specific tier classifications (regression tests)
// ============================================================================

void ElevationTierTests::tier_advancedSearchIsStandard() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::AdvancedSearch);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Standard);
}

void ElevationTierTests::tier_flashUsbIsElevated() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::FlashUsbDrive);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Elevated);
}

void ElevationTierTests::tier_backupCurrentUserIsStandard() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::BackupCurrentUser);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Standard);
}

void ElevationTierTests::tier_backupCrossUserIsElevated() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::BackupCrossUser);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Elevated);
}

void ElevationTierTests::tier_networkDiagnosticsIsStandard() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::NetworkDiagnostics);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Standard);
}

void ElevationTierTests::tier_bitlockerIsElevated() {
    const auto* e = sak::findFeatureElevation(sak::FeatureId::BackupBitlockerKeys);
    QVERIFY(e);
    QCOMPARE(e->tier, sak::ElevationTier::Elevated);
}

QTEST_GUILESS_MAIN(ElevationTierTests)
#include "test_elevation_tier.moc"
