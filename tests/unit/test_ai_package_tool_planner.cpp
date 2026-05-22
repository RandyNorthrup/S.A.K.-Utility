// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_tool_planner.h"

#include <QtTest/QtTest>

class AiPackageToolPlannerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void normalizesInstallPlan();
    void rejectsUnknownOperation();
    void clampsTimeout();
};

void AiPackageToolPlannerTests::normalizesInstallPlan() {
    const QJsonObject args{
        {QStringLiteral("operation"), QStringLiteral(" INSTALL ")},
        {QStringLiteral("package_id"), QStringLiteral(" superantispyware; rm -rf ")},
        {QStringLiteral("version"), QStringLiteral("10.0.1288")},
        {QStringLiteral("timeout_seconds"), 90},
    };

    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(args);

    QVERIFY(plan.ok());
    QCOMPARE(plan.operation, QStringLiteral("install"));
    QCOMPARE(plan.package_id, QStringLiteral("superantispywarerm-rf"));
    QCOMPARE(plan.version, QStringLiteral("10.0.1288"));
    QCOMPARE(plan.timeout_seconds, 90);
    QVERIFY(plan.change_operation);
    QVERIFY(!plan.read_operation);
}

void AiPackageToolPlannerTests::rejectsUnknownOperation() {
    const sak::ai::AiPackageToolPlan plan = sak::ai::AiPackageToolPlanner::buildPlan(
        QJsonObject{{QStringLiteral("operation"), QStringLiteral("scan")}});

    QVERIFY(!plan.ok());
    QVERIFY(plan.error_message.contains(QStringLiteral("Unsupported")));
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
