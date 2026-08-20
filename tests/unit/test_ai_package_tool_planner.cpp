// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_tool_planner.h"

#include <QtTest/QtTest>

class AiPackageToolPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalizesInstallPlan();
    void rejectsInjectionPackageId();
    void rejectsLeadingDashPackageId();       // R5-G10-9
    void rejectsOptionInjectedSearchQuery();  // R5-G10-9
    void rejectsNonStringVersion();           // R5-G10-9
    void rejectsUnknownOperation();
    void clampsTimeout();
};

void AiPackageToolPlannerTests::normalizesInstallPlan() {
    const QJsonObject args{
        {QStringLiteral("operation"), QStringLiteral(" INSTALL ")},
        {QStringLiteral("package_id"), QStringLiteral(" superantispyware ")},
        {QStringLiteral("version"), QStringLiteral("10.0.1288")},
        {QStringLiteral("timeout_seconds"), 90},
    };

    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(args);

    QVERIFY(plan.ok());
    QCOMPARE(plan.operation, QStringLiteral("install"));
    // Trimmed but otherwise preserved -- a valid id is never mangled.
    QCOMPARE(plan.package_id, QStringLiteral("superantispyware"));
    QCOMPARE(plan.version, QStringLiteral("10.0.1288"));
    QCOMPARE(plan.timeout_seconds, 90);
    QVERIFY(plan.change_operation);
    QVERIFY(!plan.read_operation);
}

void AiPackageToolPlannerTests::rejectsInjectionPackageId() {
    // An id with disallowed characters must FAIL CLOSED, not be silently rewritten
    // into a different valid token (which could install/uninstall the wrong pkg).
    const QJsonObject args{
        {QStringLiteral("operation"), QStringLiteral("install")},
        {QStringLiteral("package_id"), QStringLiteral("superantispyware; rm -rf")},
    };

    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(args);

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message,
             QStringLiteral("Invalid package identifier: superantispyware; rm -rf"));
    // The dangerous input is NOT rewritten into a usable substitute token.
    QVERIFY(plan.package_id.isEmpty());
}

void AiPackageToolPlannerTests::rejectsLeadingDashPackageId() {
    // '-' is an ALLOWED package character (isAllowedPackageChar), so an id built only of allowed
    // chars but with a LEADING dash -- e.g. "-y" or "--force" -- passes the all-of char check and
    // is caught ONLY by the startsWith('-') branch. Chocolatey would otherwise parse it as an
    // OPTION rather than a package name (argument injection). This must use a
    // dash-with-only-allowed-chars id: an id carrying a disallowed char (like
    // "--source=https://...") would be caught by the char-check branch instead (that is
    // rejectsInjectionPackageId's path), so it would NOT isolate this guard.
    const sak::ai::AiPackageToolPlan injected = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install")},
                    {QStringLiteral("package_id"), QStringLiteral("-y")}});
    QVERIFY(!injected.ok());
    QCOMPARE(injected.error_message, QStringLiteral("Invalid package identifier: -y"));
    QVERIFY(injected.package_id.isEmpty());  // never forwarded to choco as an option

    // Guard-isolation control: an INTERIOR dash (firefox-esr) is a valid package name, proving the
    // guard is scoped to a LEADING dash rather than rejecting every dash.
    const sak::ai::AiPackageToolPlan ok = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install")},
                    {QStringLiteral("package_id"), QStringLiteral("firefox-esr")}});
    QVERIFY(ok.ok());
    QCOMPARE(ok.package_id, QStringLiteral("firefox-esr"));
}

void AiPackageToolPlannerTests::rejectsOptionInjectedSearchQuery() {
    // A search query beginning with '-' would be parsed by Chocolatey as an OPTION
    // (e.g. --source=https://attacker) rather than a search term -- model-supplied argument
    // injection. It must FAIL CLOSED, not be forwarded to the package manager.
    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(QJsonObject{
        {QStringLiteral("operation"), QStringLiteral("search")},
        {QStringLiteral("query"), QStringLiteral("--source=https://attacker.example/feed")}});
    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message,
             QStringLiteral("Invalid search query: --source=https://attacker.example/feed"));

    // Non-vacuity: an ordinary search term is accepted, so the guard is the leading '-',
    // not a blanket rejection of every search.
    const sak::ai::AiPackageToolPlan ok = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("search")},
                    {QStringLiteral("query"), QStringLiteral("firefox")}});
    QVERIFY(ok.ok());
    QVERIFY(ok.query_operation);
    QCOMPARE(ok.query, QStringLiteral("firefox"));
}

void AiPackageToolPlannerTests::rejectsNonStringVersion() {
    // A version field that is PRESENT but not a JSON string must be rejected rather than
    // silently collapsing to "" (== install "latest"), which could install an unintended
    // build. Only an absent/null version legitimately means "latest".
    const sak::ai::AiPackageToolPlan numberVersion = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install")},
                    {QStringLiteral("package_id"), QStringLiteral("firefox")},
                    {QStringLiteral("version"), 99'999}});
    QVERIFY(!numberVersion.ok());
    QCOMPARE(numberVersion.error_message, QStringLiteral("Invalid version: expected a string"));

    // Non-vacuity: an ABSENT version is allowed and means latest (empty), so the reject is
    // specific to a present non-string value, not "install always needs a version".
    const sak::ai::AiPackageToolPlan latest = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("install")},
                    {QStringLiteral("package_id"), QStringLiteral("firefox")}});
    QVERIFY(latest.ok());
    QVERIFY(latest.version.isEmpty());
    QVERIFY(latest.change_operation);
}

void AiPackageToolPlannerTests::rejectsUnknownOperation() {
    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("scan")}});

    QVERIFY(!plan.ok());
    QCOMPARE(plan.error_message, QStringLiteral("Unsupported package manager operation: scan"));
}

void AiPackageToolPlannerTests::clampsTimeout() {
    auto low = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("search")},
                    {QStringLiteral("timeout_seconds"), -1}});
    auto high = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("search")},
                    {QStringLiteral("timeout_seconds"), 999'999}});

    QCOMPARE(low.timeout_seconds, 5);
    QCOMPARE(high.timeout_seconds, 7200);
}

QTEST_GUILESS_MAIN(AiPackageToolPlannerTests)
#include "test_ai_package_tool_planner.moc"
