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
    void parseActivePowerPlan_isLocaleIndependent();
    void parsePowerTimeout_readsRealQueryOutput();
    void parsePowerTimeout_isLocaleIndependent();
    void parsePowerTimeout_failsClosedRatherThanGuessing();
    void formatPowerTimeout_rendersZeroAsNever();
};

namespace {

/// A verbatim excerpt of real `powercfg -QUERY` output from a Windows 11 host, kept
/// byte-for-byte (indentation included) because the INDENTATION is what the parser anchors on.
/// Retyping this "tidily" would silently defeat the thing under test.
///
/// Two blocks, deliberately of the two different shapes powercfg emits: a range setting that
/// carries Minimum/Maximum/increment attributes BEFORE its readings, and an enumerated setting
/// that carries none. A parser that took "the last two hex values in the block" would pass the
/// second and could report a Maximum Possible Setting as a live reading in the first.
const char* const kRealQueryOutput =
    "Power Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)\r\n"
    "  GUID Alias: SCHEME_MIN\r\n"
    "  Subgroup GUID: 7516b95f-f776-4464-8c53-06167f40cc99  (Display)\r\n"
    "    GUID Alias: SUB_VIDEO\r\n"
    "    Power Setting GUID: 3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e  (Turn off display after)\r\n"
    "      GUID Alias: VIDEOIDLE\r\n"
    "      Minimum Possible Setting: 0x00000000\r\n"
    "      Maximum Possible Setting: 0xffffffff\r\n"
    "      Possible Settings increment: 0x00000001\r\n"
    "      Possible Settings units: Seconds\r\n"
    "    Current AC Power Setting Index: 0x00000000\r\n"
    "    Current DC Power Setting Index: 0x00000258\r\n"
    "\r\n"
    "  Subgroup GUID: 238c9fa8-0aad-41ed-83f4-97be242c8f20  (Sleep)\r\n"
    "    Power Setting GUID: 29f6c1db-86da-48c5-9fdb-f2b67b1f44da  (Sleep after)\r\n"
    "      GUID Alias: STANDBYIDLE\r\n"
    "      Minimum Possible Setting: 0x00000000\r\n"
    "      Maximum Possible Setting: 0xffffffff\r\n"
    "      Possible Settings increment: 0x00000001\r\n"
    "      Possible Settings units: Seconds\r\n"
    "    Current AC Power Setting Index: 0x00000e10\r\n"
    "    Current DC Power Setting Index: 0x00000708\r\n"
    "\r\n"
    "    Power Setting GUID: 9d7815a6-7ee4-497e-8888-515a05f02364  (Hibernate after)\r\n"
    "      GUID Alias: HIBERNATEIDLE\r\n"
    "      Minimum Possible Setting: 0x00000000\r\n"
    "      Maximum Possible Setting: 0xffffffff\r\n"
    "      Possible Settings increment: 0x00000001\r\n"
    "      Possible Settings units: Seconds\r\n"
    "    Current AC Power Setting Index: 0x00000000\r\n"
    "    Current DC Power Setting Index: 0x00000d05\r\n"
    "\r\n"
    "  Subgroup GUID: 02f815b5-a5cf-4c84-bf20-649d1f75d3d8  (Internet Explorer)\r\n"
    "    Power Setting GUID: 4c793e7d-a264-42e1-87d3-7a0d2f523ccd  (JavaScript Timer)\r\n"
    "      Possible Setting Index: 000\r\n"
    "      Possible Setting Friendly Name: Maximum Power Savings\r\n"
    "      Possible Setting Index: 001\r\n"
    "      Possible Setting Friendly Name: Maximum Performance\r\n"
    "    Current AC Power Setting Index: 0x00000001\r\n"
    "    Current DC Power Setting Index: 0x00000001\r\n";

constexpr auto kDisplayGuid = "3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e";
constexpr auto kSleepGuid = "29f6c1db-86da-48c5-9fdb-f2b67b1f44da";
constexpr auto kHibernateGuid = "9d7815a6-7ee4-497e-8888-515a05f02364";

}  // namespace

void OptimizePowerSettingsActionTests::parsePowerTimeout_readsRealQueryOutput() {
    // The action used to run powercfg -QUERY, keep only the plan NAME and throw the settings
    // away, while its own success report told the technician to go and run the same command by
    // hand. These are the values that report now states.
    const QString output = QString::fromLatin1(kRealQueryOutput);
    const auto timeouts = Action::parsePowerTimeouts(output);

    QVERIFY(timeouts.display.found);
    QCOMPARE(timeouts.display.ac_seconds, qint64(0));    // 0x00000000
    QCOMPARE(timeouts.display.dc_seconds, qint64(600));  // 0x00000258
    QVERIFY(timeouts.sleep.found);
    QCOMPARE(timeouts.sleep.ac_seconds, qint64(3600));   // 0x00000e10
    QCOMPARE(timeouts.sleep.dc_seconds, qint64(1800));   // 0x00000708
    QVERIFY(timeouts.hibernate.found);
    QCOMPARE(timeouts.hibernate.ac_seconds, qint64(0));
    QCOMPARE(timeouts.hibernate.dc_seconds, qint64(3333));  // 0x00000d05

    // THE ATTRIBUTE LINES MUST NOT BE READ AS READINGS. Each range block above carries
    // 0xffffffff as its Maximum Possible Setting, which is both the largest value present and
    // one of the last hex values before the readings -- so a parser that keyed on "the last two
    // hex values" or "the biggest value" would surface it as a live setting. Asserting the exact
    // numbers above already excludes it, and this states the intent explicitly.
    QVERIFY(timeouts.display.ac_seconds != 0xff'ff'ff'ffLL);
    QVERIFY(timeouts.display.dc_seconds != 0xff'ff'ff'ffLL);

    // The enumerated block (no Minimum/Maximum attributes at all) parses by the same rule.
    const auto enumerated =
        Action::parsePowerTimeout(output, QStringLiteral("4c793e7d-a264-42e1-87d3-7a0d2f523ccd"));
    QVERIFY(enumerated.found);
    QCOMPARE(enumerated.ac_seconds, qint64(1));
    QCOMPARE(enumerated.dc_seconds, qint64(1));
}

void OptimizePowerSettingsActionTests::parsePowerTimeout_isLocaleIndependent() {
    // EVERY label in this output translates, "Current AC Power Setting Index" included. This
    // file has already shipped that bug twice -- the list parser and the active-plan parser both
    // matched English text and returned nothing on a translated Windows -- so the read-back
    // anchors on the setting GUID and on indentation, neither of which localises.
    //
    // German labels, identical structure and identical GUIDs.
    const QString german = QStringLiteral(
        "Energieschema-GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (Hoechstleistung)\r\n"
        "  Untergruppen-GUID: 7516b95f-f776-4464-8c53-06167f40cc99  (Anzeige)\r\n"
        "    Energieeinstellungs-GUID: 3c0bc021-c8a8-4e07-a973-6b14cbcb2b7e  (Anzeige aus)\r\n"
        "      Moegliche Mindesteinstellung: 0x00000000\r\n"
        "      Moegliche Hoechsteinstellung: 0xffffffff\r\n"
        "    Aktueller Wechselstrom-Energieeinstellungsindex: 0x0000012c\r\n"
        "    Aktueller Gleichstrom-Energieeinstellungsindex: 0x0000003c\r\n");

    const auto de = Action::parsePowerTimeout(german, QString::fromLatin1(kDisplayGuid));
    QVERIFY2(de.found, "a translated Windows must still yield the reading");
    QCOMPARE(de.ac_seconds, qint64(300));  // 0x12c
    QCOMPARE(de.dc_seconds, qint64(60));   // 0x3c

    // An uppercase GUID in the output is matched too -- powercfg has printed both.
    const auto upper = Action::parsePowerTimeout(german.toUpper().replace(QStringLiteral("0X"),
                                                                          QStringLiteral("0x")),
                                                 QString::fromLatin1(kDisplayGuid));
    QVERIFY(upper.found);
    QCOMPARE(upper.ac_seconds, qint64(300));
}

void OptimizePowerSettingsActionTests::parsePowerTimeout_failsClosedRatherThanGuessing() {
    const QString output = QString::fromLatin1(kRealQueryOutput);

    // A setting this scheme does not list is reported as NOT READ. This is the whole point of
    // the read-back: the report it feeds previously asserted settings nothing had queried, so a
    // substituted default here would recreate exactly the defect being fixed. Note that
    // found == false is what carries that, NOT the zeroed seconds -- 0 is a legitimate value
    // meaning "Never", which is why the flag exists at all.
    const auto absent =
        Action::parsePowerTimeout(output, QStringLiteral("00000000-1111-2222-3333-444444444444"));
    QVERIFY(!absent.found);

    // Empty output, and output with the GUID but no readings at all.
    QVERIFY(!Action::parsePowerTimeout(QString(), QString::fromLatin1(kSleepGuid)).found);
    const QString no_readings = QStringLiteral(
        "    Power Setting GUID: 29f6c1db-86da-48c5-9fdb-f2b67b1f44da  (Sleep after)\r\n"
        "      Minimum Possible Setting: 0x00000000\r\n"
        "      Maximum Possible Setting: 0xffffffff\r\n"
        "    Subgroup GUID: 238c9fa8-0aad-41ed-83f4-97be242c8f20  (Next)\r\n");
    const auto none = Action::parsePowerTimeout(no_readings, QString::fromLatin1(kSleepGuid));
    QVERIFY2(!none.found,
             "min/max attributes must never be reported as the current AC/DC readings");

    // Exactly ONE reading is also not a valid block: half an answer must not be presented as a
    // whole one, and there is no defensible way to decide whether it was the AC or the DC value.
    const QString one_reading = QStringLiteral(
        "    Power Setting GUID: 29f6c1db-86da-48c5-9fdb-f2b67b1f44da  (Sleep after)\r\n"
        "    Current AC Power Setting Index: 0x0000001e\r\n");
    QVERIFY(!Action::parsePowerTimeout(one_reading, QString::fromLatin1(kSleepGuid)).found);

    // A reading belonging to the NEXT setting must not be borrowed to complete this one: the
    // hibernate block below is a separate setting, and the sleep block above it has only one
    // reading of its own.
    const QString bleeding = QStringLiteral(
        "    Power Setting GUID: 29f6c1db-86da-48c5-9fdb-f2b67b1f44da  (Sleep after)\r\n"
        "    Current AC Power Setting Index: 0x0000001e\r\n"
        "    Power Setting GUID: 9d7815a6-7ee4-497e-8888-515a05f02364  (Hibernate after)\r\n"
        "    Current AC Power Setting Index: 0x0000003c\r\n"
        "    Current DC Power Setting Index: 0x0000003c\r\n");
    QVERIFY2(!Action::parsePowerTimeout(bleeding, QString::fromLatin1(kSleepGuid)).found,
             "a block must stop at the next GUID line, not run into the following setting");
    // ...while the following setting itself still reads correctly, so the terminator is not
    // simply swallowing everything.
    const auto next = Action::parsePowerTimeout(bleeding, QString::fromLatin1(kHibernateGuid));
    QVERIFY(next.found);
    QCOMPARE(next.ac_seconds, qint64(60));
}

void OptimizePowerSettingsActionTests::formatPowerTimeout_rendersZeroAsNever() {
    // powercfg encodes "never time out" as 0. Rendered as a bare "0" it reads as an IMMEDIATE
    // timeout -- the opposite of the setting's meaning -- to the technician this report is for.
    QCOMPARE(Action::formatPowerTimeout(0), QStringLiteral("Never"));
    QCOMPARE(Action::formatPowerTimeout(30), QStringLiteral("30 sec"));
    QCOMPARE(Action::formatPowerTimeout(60), QStringLiteral("1 min"));
    QCOMPARE(Action::formatPowerTimeout(600), QStringLiteral("10 min"));
    QCOMPARE(Action::formatPowerTimeout(90), QStringLiteral("1 min 30 sec"));
    QCOMPARE(Action::formatPowerTimeout(3600), QStringLiteral("1 hr"));
    QCOMPARE(Action::formatPowerTimeout(5400), QStringLiteral("1 hr 30 min"));
    QCOMPARE(Action::formatPowerTimeout(3333), QStringLiteral("55 min 33 sec"));
    // A negative value cannot come from a hex reading, but must not render as "-1 sec" if one
    // ever reached here.
    QCOMPARE(Action::formatPowerTimeout(-1), QStringLiteral("Never"));
}


void OptimizePowerSettingsActionTests::parseActivePowerPlan_isLocaleIndependent() {
    // The list parser was de-localized first and THIS one was missed, which mattered more: the
    // active plan is how "already using High Performance" is decided, so an English-only match
    // meant that check could never succeed on a translated Windows and the action would
    // re-activate the plan on every single run.
    const auto english = OptimizePowerSettingsAction::parseActivePowerPlan(QStringLiteral(
        "\r\nPower Scheme GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (High performance)\r\n"));
    QCOMPARE(english.guid, QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
    QCOMPARE(english.name, QStringLiteral("High performance"));
    QVERIFY(english.isActive);
    // ...and it is recognised as the built-in High Performance scheme, which is the decision
    // this parse actually feeds.
    QVERIFY(OptimizePowerSettingsAction::isHighPerformanceGuid(english.guid));

    // German: the label differs entirely, the GUID does not.
    const auto german = OptimizePowerSettingsAction::parseActivePowerPlan(QStringLiteral(
        "\r\nEnergieschema-GUID: 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c  (Hoechstleistung)\r\n"));
    QCOMPARE(german.guid, QStringLiteral("8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c"));
    QCOMPARE(german.name, QStringLiteral("Hoechstleistung"));
    QVERIFY(german.isActive);
    QVERIFY(OptimizePowerSettingsAction::isHighPerformanceGuid(german.guid));

    // An uppercase GUID normalises, so the built-in comparison still holds.
    const auto upper = OptimizePowerSettingsAction::parseActivePowerPlan(
        QStringLiteral("GUID: 8C5E7FDA-E8BF-4A96-9A85-A6E23A8C635C  (High)\r\n"));
    QVERIFY(OptimizePowerSettingsAction::isHighPerformanceGuid(upper.guid));

    // No scheme line at all -> no active plan, and NOT a plan with an empty guid marked active:
    // "active" with nothing identified would let the already-optimized check read a blank as a
    // match.
    for (const QString& empty : {QString(),
                                 QStringLiteral("powercfg: unknown option\r\n"),
                                 QStringLiteral("Power Scheme GUID: not-a-guid  (Broken)\r\n")}) {
        const auto none = OptimizePowerSettingsAction::parseActivePowerPlan(empty);
        QVERIFY2(none.guid.isEmpty(), qPrintable(empty));
        QVERIFY2(!none.isActive, qPrintable(empty));
    }
}

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
