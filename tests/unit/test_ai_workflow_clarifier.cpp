// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_clarifier.h"

#include <QJsonArray>
#include <QtTest/QtTest>

class AiWorkflowClarifierTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void flagsMissingRequiredInput();
    void flagsGenericAppValue();
    void acceptsSpecificAppValue();
};

namespace {

sak::ai::WorkflowTemplate workflowWithAppInput() {
    sak::ai::WorkflowTemplate workflow;
    workflow.id = QStringLiteral("install_app_now");
    workflow.title = QStringLiteral("Install App");
    sak::ai::WorkflowRequiredInput input;
    input.id = QStringLiteral("app_name");
    input.label = QStringLiteral("App name");
    input.type = QStringLiteral("string");
    input.required = true;
    workflow.required_inputs << input;
    return workflow;
}

}  // namespace

void AiWorkflowClarifierTests::flagsMissingRequiredInput() {
    const auto result = sak::ai::AiWorkflowClarifier::analyze(workflowWithAppInput(),
                                                              QStringLiteral("install"),
                                                              {});
    QVERIFY(result.needsClarification());
    QCOMPARE(result.questions.size(), 1);
    QCOMPARE(result.questions.first().input_id, QStringLiteral("app_name"));
    QCOMPARE(result.questions.first().reason, QStringLiteral("required input is missing"));
    QCOMPARE(result.questions.first().question, QStringLiteral("What should I use for App name?"));
}

void AiWorkflowClarifierTests::flagsGenericAppValue() {
    QJsonObject inputs;
    inputs[QStringLiteral("app_name")] = QStringLiteral("app");
    const auto result = sak::ai::AiWorkflowClarifier::analyze(workflowWithAppInput(),
                                                              QStringLiteral("install the app"),
                                                              inputs);
    QCOMPARE(result.questions.size(), 1);  // exactly one ambiguous-input question, not >=1
    QCOMPARE(result.questions.first().reason, QStringLiteral("app/package value is ambiguous"));
    QCOMPARE(result.questions.first().question,
             QStringLiteral("Which exact app, package, or product should I use for App name?"));
}

void AiWorkflowClarifierTests::acceptsSpecificAppValue() {
    QJsonObject inputs;
    inputs[QStringLiteral("app_name")] = QStringLiteral("Firefox");
    const auto result = sak::ai::AiWorkflowClarifier::analyze(workflowWithAppInput(),
                                                              QStringLiteral("install Firefox"),
                                                              inputs);
    QVERIFY(!result.needsClarification());
}

QTEST_GUILESS_MAIN(AiWorkflowClarifierTests)
#include "test_ai_workflow_clarifier.moc"
