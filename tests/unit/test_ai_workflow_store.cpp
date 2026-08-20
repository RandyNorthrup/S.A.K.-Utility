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
    void rejectUnknownPhaseType();
    void earlierInvalidFileDoesNotRejectLaterValidOne();
    void rejectDuplicatePhaseIds();
    void rejectDuplicateRequiredInputIds();
    void rejectCmdPhaseCommandWithPlaceholder();
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
    QCOMPARE(workflow.promptSummary(),
             QStringLiteral("Workflow: Sample Workflow\nPurpose: Test workflow\nTask: Do the test "
                            "workflow.\nPhases:\n- plan: overseer\n  Plan the work.\n  Done when: "
                            "Plan complete.\nAcceptance criteria:\n- Workflow parses."));
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
    QCOMPARE(workflows.size(), 25);  // resources/ai.qrc bundles exactly 25 built-in workflows
    QVERIFY(store.workflowById(QStringLiteral("download_offline_installer")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("windows_update_repair")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("malware_virus_removal")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("pc_cleanup_bloatware_adware")) != nullptr);
    QVERIFY(store.workflowById(QStringLiteral("approved_bloatware_adware_removal")) != nullptr);
    QVERIFY(store.roles().contains(QStringLiteral("Software Deployment Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("Security Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("System Cleanup Technician")));
    QVERIFY(store.roles().contains(QStringLiteral("Diagnostic Technician")));
    // Exactly six built-in workflows carry the "Windows Repair Technician" role.
    QCOMPARE(store.workflowsForRole(QStringLiteral("Windows Repair Technician")).size(), 6);
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

void AiWorkflowStoreTests::rejectUnknownPhaseType() {
    // A misspelled phase type (e.g. "tool-action") has no runtime handler; it must fail
    // validation at load rather than silently loading and only erroring at run time.
    QJsonObject object = validWorkflowObject();
    QJsonObject phase = object.value(QStringLiteral("phases")).toArray().at(0).toObject();
    phase[QStringLiteral("type")] = QStringLiteral("tool-action");
    object[QStringLiteral("phases")] = QJsonArray{phase};

    QStringList errors;
    const auto workflow =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("bad_type"), &errors);
    QVERIFY(!workflow.isValid());

    QStringList detail;
    (void)workflow.isValid(&detail);
    QVERIFY(detail.join(QStringLiteral("\n")).contains(QStringLiteral("unsupported type")));
}

void AiWorkflowStoreTests::earlierInvalidFileDoesNotRejectLaterValidOne() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    // a_bad.json is valid JSON but an invalid workflow; b_good.json is fully valid.
    // Directory load is name-ordered, so the bad file is processed first and appends to
    // the shared error list. The good file must still load (isValid must be delta-based).
    QJsonObject bad = validWorkflowObject(QStringLiteral("bad_wf"));
    bad.remove(QStringLiteral("id"));
    bad.remove(QStringLiteral("title"));
    QVERIFY(writeJsonFile(QDir(temp_dir.path()).filePath(QStringLiteral("a_bad.json")), bad));

    const QJsonObject good = validWorkflowObject(QStringLiteral("good_wf"),
                                                 QStringLiteral("Good Workflow"));
    QVERIFY(writeJsonFile(QDir(temp_dir.path()).filePath(QStringLiteral("b_good.json")), good));

    sak::ai::WorkflowStore store;
    QStringList errors;
    // The load returns false because a_bad.json is invalid; the good file must still load.
    (void)store.loadDirectory(temp_dir.path(), &errors);

    QVERIFY(store.workflowById(QStringLiteral("good_wf")) != nullptr);
}

void AiWorkflowStoreTests::rejectDuplicatePhaseIds() {
    // Two phases with the same id alias each other in phase_results; reject at load.
    QJsonObject object = validWorkflowObject();
    const QJsonObject phase = object.value(QStringLiteral("phases")).toArray().at(0).toObject();
    object[QStringLiteral("phases")] = QJsonArray{phase, phase};  // same id twice

    QStringList errors;
    const auto workflow =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("dup_phase"), &errors);
    QVERIFY(!workflow.isValid());
    QVERIFY(errors.join(QStringLiteral("\n")).contains(QStringLiteral("Duplicate phase id")));
}

void AiWorkflowStoreTests::rejectDuplicateRequiredInputIds() {
    QJsonObject object = validWorkflowObject();
    QJsonObject input;
    input[QStringLiteral("id")] = QStringLiteral("app_name");
    input[QStringLiteral("label")] = QStringLiteral("App");
    object[QStringLiteral("required_inputs")] = QJsonArray{input, input};  // same id twice

    QStringList errors;
    const auto workflow =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("dup_input"), &errors);
    QVERIFY(!workflow.isValid());
    QVERIFY(errors.join(QStringLiteral("\n")).contains(QStringLiteral("Duplicate required-input")));
}

void AiWorkflowStoreTests::rejectCmdPhaseCommandWithPlaceholder() {
    // R5 p1_ai-2: a user-directory run_cmd template with a ${...} placeholder used to load
    // and then substitute the value straight into a cmd.exe command line. cmd.exe has no
    // literal-quoting construct that makes an arbitrary value inert, so the template is
    // rejected at load; a placeholder-free run_cmd command still loads.
    QJsonObject object = validWorkflowObject();
    QJsonObject phase = object.value(QStringLiteral("phases")).toArray().at(0).toObject();
    phase[QStringLiteral("type")] = QStringLiteral("tool_action");
    phase[QStringLiteral("tool")] = QStringLiteral("run_cmd");
    QJsonObject arguments;
    arguments[QStringLiteral("command")] = QStringLiteral("findstr \"${user_message}\" log.txt");
    phase[QStringLiteral("arguments")] = arguments;
    object[QStringLiteral("phases")] = QJsonArray{phase};

    QStringList errors;
    const auto workflow =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("cmd_placeholder"), &errors);
    QVERIFY(!workflow.isValid());
    QVERIFY(
        errors.join(QStringLiteral("\n")).contains(QStringLiteral("must not use placeholders")));

    arguments[QStringLiteral("command")] = QStringLiteral("ipconfig /all");
    phase[QStringLiteral("arguments")] = arguments;
    object[QStringLiteral("phases")] = QJsonArray{phase};

    QStringList clean_errors;
    const auto clean =
        sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("cmd_ok"), &clean_errors);
    QVERIFY2(clean.isValid(), qPrintable(clean_errors.join(QStringLiteral("; "))));
}

QTEST_MAIN(AiWorkflowStoreTests)
#include "test_ai_workflow_store.moc"
