// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_optimize_power_settings_action.cpp
/// @brief Unit test for OptimizePowerSettingsAction pure seam (Codex B6-18):
///        "already optimized" must be decided by GUID, so a custom plan merely
///        named like "High Performance" is not mistaken for the built-in one.

#include "sak/actions/optimize_power_settings_action.h"

#include <QtTest/QtTest>

using sak::OptimizePowerSettingsAction;

class OptimizePowerSettingsActionTests : public QObject {
    Q_OBJECT

    using Action = OptimizePowerSettingsAction;

private Q_SLOTS:
    void isHighPerformanceGuid_matchesBuiltinsByGuid();
    void discoveryPermitsActivation_failsClosedWithoutDiscovery();
    void parsePowerPlanList_isLocaleIndependent();
    void parsePowerPlanList_rejectsNonPlanText();
};


void OptimizePowerSettingsActionTests::parsePowerPlanList_isLocaleIndependent() {
    // R5-G23-4 (locale) via R5-LEDGER. The previous parser required the literal English label
    // "Power Scheme GUID:", so on a non-English Windows it matched NOTHING: the plan list came
    // back empty and the whole optimisation refused. Fail-closed, but the feature was dead --
    // the same defect class already fixed twice in this campaign (the ipconfig /displaydns
    // scrape and the netsh ethernet-config scrape).
    //
    // What does NOT localise is the scheme GUID and the trailing '*' active marker, and those
    // are what every DECISION here is made on. The parenthesised name is localised and is used
    // for display only, which these fixtures also pin.

    // English, exactly as this host prints it (recon'd with powercfg /list).
    const QString english = QStringLiteral(
        "\r\nExisting Power Schemes (* Active)\r\n"
        "-----------------------------------\r\n"
        "Power Scheme GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Balanced) *\r\n"
        "Power Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)\r\n"
        "Power Scheme GUID: a1841308-3541-4fab-bc81-f71556f20b4a  (Power saver)\r\n");

    const auto en = OptimizePowerSettingsAction::parsePowerPlanList(english);
    QCOMPARE(en.size(), qsizetype(3));
    QCOMPARE(en.at(0).guid, QStringLiteral("381b4222-f694-41f0-9685-ff5bb260df2e"));
    QCOMPARE(en.at(0).name, QStringLiteral("Balanced"));
    QVERIFY(en.at(0).isActive);
    QCOMPARE(en.at(1).guid, QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
    QVERIFY(!en.at(1).isActive);
    QVERIFY(!en.at(2).isActive);

    // German. Every word differs -- including the label the old parser keyed on -- but the GUIDs
    // and the active marker are identical. This is the case that used to yield ZERO plans.
    const QString german = QStringLiteral(
        "\r\nVorhandene Energieschemas (* Aktiv)\r\n"
        "-----------------------------------\r\n"
        "Energieschema-GUID: 381b4222-f694-41f0-9685-ff5bb260df2e  (Ausbalanciert)\r\n"
        "Energieschema-GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (Hoechstleistung) *\r\n");

    const auto de = OptimizePowerSettingsAction::parsePowerPlanList(german);
    QCOMPARE(de.size(), qsizetype(2));
    QCOMPARE(de.at(0).guid, QStringLiteral("381b4222-f694-41f0-9685-ff5bb260df2e"));
    QCOMPARE(de.at(0).name, QStringLiteral("Ausbalanciert"));
    QVERIFY(!de.at(0).isActive);
    // The ACTIVE plan is identified on a locale the parser has never seen, and it is the
    // High Performance GUID -- which is what the optimisation actually decides on.
    QCOMPARE(de.at(1).guid, QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
    QVERIFY(de.at(1).isActive);

    // French, with a differently-shaped header, to prove nothing above depends on the German
    // wording either.
    const QString french = QStringLiteral(
        "\r\nModes de gestion de l alimentation existants (* Actif)\r\n"
        "-----------------------------------\r\n"
        "GUID du mode de gestion de l alimentation: "
        "a1841308-3541-4fab-bc81-f71556f20b4a  (Economies d energie) *\r\n");
    const auto fr = OptimizePowerSettingsAction::parsePowerPlanList(french);
    QCOMPARE(fr.size(), qsizetype(1));
    QCOMPARE(fr.at(0).guid, QStringLiteral("a1841308-3541-4fab-bc81-f71556f20b4a"));
    QVERIFY(fr.at(0).isActive);
}

void OptimizePowerSettingsActionTests::parsePowerPlanList_rejectsNonPlanText() {
    // Empty and header-only output yield NO plans, which the caller treats as "cannot activate"
    // rather than falling back to a hard-coded GUID and mutating on a guess.
    QVERIFY(OptimizePowerSettingsAction::parsePowerPlanList(QString()).isEmpty());
    QVERIFY(OptimizePowerSettingsAction::parsePowerPlanList(
                QStringLiteral("Existing Power Schemes (* Active)\r\n--------\r\n"))
                .isEmpty());
    // A GUID-shaped string with no parenthesised name is not a plan row.
    QVERIFY(OptimizePowerSettingsAction::parsePowerPlanList(
                QStringLiteral("381b4222-f694-41f0-9685-ff5bb260df2e\r\n"))
                .isEmpty());
    // A truncated GUID must not match: the pattern is anchored to the full 8-4-4-4-12 shape, so
    // a mangled line cannot become a plan the action would then try to activate.
    QVERIFY(OptimizePowerSettingsAction::parsePowerPlanList(
                QStringLiteral("Power Scheme GUID: 381b4222-f694-41f0-9685  (Balanced)\r\n"))
                .isEmpty());
    // Uppercase GUIDs are normalised, so a comparison against the canonical constants holds.
    const auto upper = OptimizePowerSettingsAction::parsePowerPlanList(
        QStringLiteral("Power Scheme GUID: 8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C  (High)\r\n"));
    QCOMPARE(upper.size(), qsizetype(1));
    QCOMPARE(upper.at(0).guid, QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
}

void OptimizePowerSettingsActionTests::isHighPerformanceGuid_matchesBuiltinsByGuid() {
    // Built-in High Performance (SCHEME_MIN) and Ultimate Performance, any case,
    // with surrounding whitespace.
    QVERIFY(Action::isHighPerformanceGuid(QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c")));
    QVERIFY(
        Action::isHighPerformanceGuid(QStringLiteral("  8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C  ")));
    QVERIFY(Action::isHighPerformanceGuid(QStringLiteral("e9a42b02-d5df-448d-aa00-03f14749eb61")));
    // Ultimate Performance in the SAME normalized shape the case above proves for SCHEME_MIN:
    // without it, the trim/lower normalization could be applied to one built-in only.
    QVERIFY(
        Action::isHighPerformanceGuid(QStringLiteral("  E9A42B02-D5DF-448D-AA00-03F14749EB61  ")));

    // A custom plan's GUID (even if the plan is NAMED "High Performance") is not
    // a built-in high-performance scheme.
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("11111111-2222-3333-4444-555555555555")));
    // The OTHER canonical built-in schemes are NOT high-performance. Balanced is the active plan
    // on a stock machine, so refusing it is what makes execute() actually run powercfg -SETACTIVE
    // instead of reporting "already optimized" and changing nothing.
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("381b4222-f694-41f0-9685-ff5bb260df2e")));
    QVERIFY(
        !Action::isHighPerformanceGuid(QStringLiteral("  381B4222-F694-41F0-9685-FF5BB260DF2E  ")));
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("a1841308-3541-4fab-bc81-f71556f20b4a")));
    QVERIFY(!Action::isHighPerformanceGuid(QString()));
    QVERIFY(!Action::isHighPerformanceGuid(QStringLiteral("High Performance")));
}

void OptimizePowerSettingsActionTests::discoveryPermitsActivation_failsClosedWithoutDiscovery() {
    // CODEX_REVIEW_4 M-B3-22: a plan may be activated ONLY when powercfg discovery succeeded and
    // returned at least one plan. A failed discovery (or an empty list) must fail closed so the
    // optimizer never mutates the active plan on a hard-coded/guessed GUID.
    QVERIFY(!Action::discoveryPermitsActivation(false, 5));  // discovery failed -> no mutation
    QVERIFY(!Action::discoveryPermitsActivation(false, 0));
    QVERIFY(!Action::discoveryPermitsActivation(true, 0));   // ran but found nothing -> no mutation
    QVERIFY(Action::discoveryPermitsActivation(true, 1));    // ran and found plans -> may activate
    QVERIFY(Action::discoveryPermitsActivation(true, 42));
}

QTEST_GUILESS_MAIN(OptimizePowerSettingsActionTests)
#include "test_optimize_power_settings_action.moc"
