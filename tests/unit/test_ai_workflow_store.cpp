// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_workflow_store.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTest>

class AiWorkflowStoreTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parseValidWorkflow();
    void rejectInvalidWorkflow();
    void loadBuiltInWorkflows();
    void userDirectoryOverridesBuiltInWorkflow();
};

namespace {

QJsonObject validWorkflowObject(const QString& id = QStringLiteral("sample_workflow"),
                                const QString& title = QStringLiteral("Sample Workflow")) {
    QJsonObject root;
    root[QStringLiteral("schema_version")] = 1;
    root[QStringLiteral("id")] = id;
    root[QStringLiteral("title")] = title;
    root[QStringLiteral("role")] = QStringLiteral("PC Technician");
    root[QStringLiteral("category")] = QStringLiteral("Diagnostics");
    root[QStringLiteral("description")] = QStringLiteral("Test workflow");
    root[QStringLiteral("starter_prompt")] = QStringLiteral("Do the test workflow.");

    QJsonObject phase;
    phase[QStringLiteral("id")] = QStringLiteral("plan");
    phase[QStringLiteral("type")] = QStringLiteral("overseer");
    phase[QStringLiteral("prompt")] = QStringLiteral("Plan the work.");
    phase[QStringLiteral("completion")] = QStringLiteral("Plan complete.");
    QJsonObject arguments;
    arguments[QStringLiteral("command")] = QStringLiteral("Write-Output test");
    phase[QStringLiteral("arguments")] = arguments;
    root[QStringLiteral("phases")] = QJsonArray{phase};
    root[QStringLiteral("acceptance_criteria")] = QJsonArray{QStringLiteral("Workflow parses.")};
    QJsonObject cancel;
    cancel[QStringLiteral("cancel_children")] = true;
    cancel[QStringLiteral("cancel_tools")] = true;
    cancel[QStringLiteral("preserve_partial_artifacts")] = true;
    cancel[QStringLiteral("report_partial_state")] = true;
    root[QStringLiteral("cancel_policy")] = cancel;
    return root;
}

bool writeJsonFile(const QString& path, const QJsonObject& object) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }
    file.write(QJsonDocument(object).toJson(QJsonDocument::Indented));
    return true;
}

}  // namespace

void AiWorkflowStoreTests::parseValidWorkflow() {
    QStringList errors;
    const auto workflow = sak::ai::WorkflowTemplate::fromJson(validWorkflowObject(),
                                                              QStringLiteral("memory"),
                                                              &errors);

    QVERIFY2(errors.isEmpty(), qPrintable(errors.join(QStringLiteral("; "))));
    QVERIFY(workflow.isValid());
    QCOMPARE(workflow.id, QStringLiteral("sample_workflow"));
    QCOMPARE(workflow.phases.size(), 1);
    QCOMPARE(workflow.phases.first().arguments.value(QStringLiteral("command")).toString(),
             QStringLiteral("Write-Output test"));
    QVERIFY(workflow.promptSummary().contains(QStringLiteral("Workflow: Sample Workflow")));
}

void AiWorkflowStoreTests::rejectInvalidWorkflow() {
    QJsonObject object = validWorkflowObject();
    object.remove(QStringLiteral("title"));

    QStringList errors;
    const auto workflow =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("invalid"), &errors);

    QVERIFY(!workflow.isValid());
    QVERIFY(!errors.isEmpty());
    QVERIFY(errors.join(QStringLiteral("\n")).contains(QStringLiteral("title")));
}

void AiWorkflowStoreTests::loadBuiltInWorkflows() {
    sak::ai::WorkflowStore store;
    QStringList errors;
    QVERIFY2(store.loadBuiltIn(&errors), qPrintable(errors.join(QStringLiteral("; "))));

    const auto workflows = store.workflows();
    QVERIFY(workflows.size() >= 10);
    QVERIFY(store.workflowById(QStringLiteral("download_offline_installer")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("windows_update_repair")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("malware_virus_removal")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("pc_cleanup_bloatware_adware")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("approved_bloatware_adware_removal")) != nullptr);
    QVERIFY(store.roles().contains(QStringLiteral("Software Deployment Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("Security Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("System Cleanup Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("Diagnostic Technician")));
    QVERIFY(!store.workflowsForRole(QStringLiteral("Windows Repair Technician")).isEmpty());
}

void AiWorkflowStoreTests::userDirectoryOverridesBuiltInWorkflow() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    QJsonObject replacement = validWorkflowObject(QStringLiteral("download_offline_installer"),
                                                  QStringLiteral("Custom Offline Installer"));
    const QString path = QDir(temp_dir.path()).filePath(QStringLiteral("override.json"));
    QVERIFY(writeJsonFile(path, replacement));

    sak::ai::WorkflowStore store;
    QStringList errors;
    QVERIFY2(store.loadBuiltIn(&errors), qPrintable(errors.join(QStringLiteral("; "))));
    QVERIFY2(store.loadDirectory(temp_dir.path(), &errors),
             qPrintable(errors.join(QStringLiteral("; "))));

    const auto* workflow = store.workflowById(QStringLiteral("download_offline_installer"));
    QVERIFY(workflow != nullptr);
    QCOMPARE(workflow->title, QStringLiteral("Custom Offline Installer"));
}

QTEST_MAIN(AiWorkflowStoreTests)
#include "test_ai_workflow_store.moc"
