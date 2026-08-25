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
    void tierToString_unknownValueIsLabelled();

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
    QCOMPARE(sak::kFeatureCount, size_t(46));  // static_assert already guards > 0; pin table size
}

void ElevationTierTests::featureTable_noDuplicateIds() {
    std::set<uint16_t> seen;
    uint16_t previous_id = 0;
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        auto id_value = static_cast<uint16_t>(sak::kFeatureElevationTable[i].id);
        QVERIFY2(seen.find(id_value) == seen.end(),
                 qPrintable(QString("Duplicate FeatureId: %1").arg(id_value)));
        seen.insert(id_value);
        // The header states an ORDERING invariant -- "The table is sorted by FeatureId for
        // binary-search lookup" -- and nothing checked it. A std::set is order-blind by
        // construction, so this loop proved only that the ids are distinct. The current lookup is
        // a linear scan, so a violation is invisible at runtime today; it becomes a silent
        // correctness hole the moment anyone acts on the header's own binary-search invitation.
        if (i > 0) {
            QVERIFY2(id_value > previous_id,
                     qPrintable(QString("FeatureId table is not sorted: %1 follows %2 at index %3")
                                    .arg(id_value)
                                    .arg(previous_id)
                                    .arg(i)));
        }
        previous_id = id_value;
    }
}

void ElevationTierTests::featureTable_allHaveNames() {
    // !empty() is the weakest possible statement about a display NAME, and it cannot see the most
    // likely edit to a 46-row table written by copy-pasting the row above: a row that kept its
    // neighbour's name. All 46 names are distinct today, so uniqueness is knowable and currently
    // true -- and no test anywhere pins an exact name, so this column of a user-facing catalog was
    // guarded by nothing but "not the empty string".
    std::set<std::string_view> names;
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        const auto& entry = sak::kFeatureElevationTable[i];
        QVERIFY2(!entry.name.empty(),
                 qPrintable(QString("Feature at index %1 has empty name").arg(i)));
        QVERIFY2(names.find(entry.name) == names.end(),
                 qPrintable(QString("Duplicate feature name at index %1: %2")
                                .arg(i)
                                .arg(QString::fromUtf8(entry.name.data(),
                                                       static_cast<int>(entry.name.size())))));
        names.insert(entry.name);
    }
}

void ElevationTierTests::featureTable_elevatedHaveReasons() {
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        const auto& entry = sak::kFeatureElevationTable[i];
        // Mixed counts too. This file had two integrity loops -- one demanding a reason when the
        // tier is Elevated, one demanding an EMPTY reason when it is Standard -- and Mixed is
        // filtered out by both, so all three Mixed rows had their reason column unconstrained
        // here. featureNeedsElevation returns true for Mixed, i.e. a Mixed feature DOES raise the
        // UAC prompt and so DOES need a justification string to show with it.
        if (entry.tier == sak::ElevationTier::Elevated || entry.tier == sak::ElevationTier::Mixed) {
            QVERIFY2(!entry.reason.empty(),
                     qPrintable(QString("Elevation-bearing feature '%1' missing reason")
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
    // FeatureElevation has FOUR members and this read two. `name` is compared to an expected
    // value nowhere in the repository -- the other loops only ask !empty(), and the hardening
    // suite reads it solely to build failure messages -- so the string the elevation gate puts in
    // front of the user when it asks for administrator rights was unpinned across all 46 rows.
    QCOMPARE(entry->name, std::string_view("Advanced Search"));
    QVERIFY2(entry->reason.empty(), "a Standard feature carries no elevation reason");
}

void ElevationTierTests::findFeature_unknownId() {
    // Use a value not in the table
    const auto* entry = sak::findFeatureElevation(static_cast<sak::FeatureId>(9999));
    QVERIFY(entry == nullptr);

    // 9999 was the only non-table id probed anywhere in the repository, and it sits far PAST the
    // last row -- so it exercises the refusal only in the "ran off the end of the table"
    // direction. The INTERIOR gaps are what a neighbour-matching lookup would get wrong, and the
    // header actively invites that rewrite ("sorted by FeatureId for binary-search lookup"): a
    // std::lower_bound conversion that forgets the `it->id == id` re-check still returns nullptr
    // for 9999 and stays green. featureNeedsElevation is the security-relevant wrapper, and its
    // unknown-id default is FAIL-OPEN (unknown -> "no admin needed"), so exact matching is the
    // only thing standing between an unrecognised id and a wrong answer in either direction.
    for (uint16_t probe : {uint16_t(0),
                           uint16_t(99),
                           uint16_t(199),
                           uint16_t(403),
                           uint16_t(456),
                           uint16_t(903),
                           uint16_t(9999)}) {
        const auto id = static_cast<sak::FeatureId>(probe);
        QVERIFY2(sak::findFeatureElevation(id) == nullptr,
                 qPrintable(QString("id %1 matched a table row").arg(probe)));
        QVERIFY2(!sak::featureNeedsElevation(id),
                 qPrintable(QString("id %1 inherited a neighbour's elevation tier").arg(probe)));
    }
    // Control: every id that IS in the table still resolves, so the probes above are refusals
    // rather than a lookup that stopped working.
    for (size_t i = 0; i < sak::kFeatureCount; ++i) {
        const auto& row = sak::kFeatureElevationTable[i];
        const auto* found = sak::findFeatureElevation(row.id);
        QVERIFY2(found != nullptr, qPrintable(QString("table row %1 no longer resolves").arg(i)));
        QCOMPARE(found->id, row.id);
    }
}

void ElevationTierTests::tierToString_unknownValueIsLabelled() {
    // to_string has four exits: three case labels and a trailing "Unknown". The three tierToString
    // tests claim the case labels; the fallthrough was claimed by nothing. ElevationTier has a
    // fixed uint8_t underlying type, so an out-of-range value is well defined (not UB) and lands
    // on that line -- and since the switch has no default label, a fourth enumerator would be
    // caught by -Wswitch, making an out-of-range value the fallthrough's only observable.
    QCOMPARE(sak::to_string(static_cast<sak::ElevationTier>(99)), std::string_view("Unknown"));
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
