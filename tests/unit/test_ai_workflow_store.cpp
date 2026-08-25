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
    QCOMPARE(workflow.role, QStringLiteral("PC Technician"));
    QCOMPARE(workflow.category, QStringLiteral("Diagnostics"));
    QCOMPARE(workflow.title, QStringLiteral("Sample Workflow"));
    QCOMPARE(workflow.description, QStringLiteral("Test workflow"));
    QCOMPARE(workflow.starter_prompt, QStringLiteral("Do the test workflow."));
    QCOMPARE(workflow.phases.size(), 1);
    QCOMPARE(workflow.phases.first().arguments.value(QStringLiteral("command")).toString(),
             QStringLiteral("Write-Output test"));
    QCOMPARE(workflow.phases.first().type, QStringLiteral("overseer"));
    QVERIFY(!workflow.phases.first().always_run);
    QCOMPARE(workflow.promptSummary(),
             QStringLiteral("Workflow: Sample Workflow\nPurpose: Test workflow\nTask: Do the test "
                            "workflow.\nPhases:\n- plan: overseer\n  Plan the work.\n  Done when: "
                            "Plan complete.\nAcceptance criteria:\n- Workflow parses."));
}

void AiWorkflowStoreTests::rejectInvalidWorkflow() {
    // validateWorkflowRequiredFields is a six-guard refuser. Prove each guard ALONE,
    // by exact message and exact error count, so deleting any single guard turns this
    // red -- e.g. dropping the `role` guard would let a role-less workflow load and
    // then vanish from the picker (WorkflowStore::roles() skips empty roles).
    struct MissingFieldCase {
        const char* field;
        const char* expected_error;
    };
    const MissingFieldCase cases[] = {
        {"schema_version", "Unsupported schema_version: 0"},
        {"id", "Missing required field: id"},
        {"title", "Missing required field: title"},
        {"role", "Missing required field: role"},
        {"starter_prompt", "Missing required field: starter_prompt"},
        {"phases", "Missing required field: phases"},
    };

    for (const auto& test_case : cases) {
        QJsonObject object = validWorkflowObject();
        object.remove(QString::fromLatin1(test_case.field));

        QStringList errors;
        const auto workflow =
            sak::ai::WorkflowTemplate::fromJson(object, QStringLiteral("invalid"), &errors);

        QVERIFY2(!workflow.isValid(), test_case.field);
        // Exactly one error: removing one required field trips that field's guard
        // and no other.
        QCOMPARE(errors.size(), 1);
        QCOMPARE(errors.at(0), QString::fromLatin1(test_case.expected_error));
    }
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
    // roles() de-duplicates and sorts case-insensitively (ai_workflow_store.cpp:106-118); the
    // 25 built-ins carry only 15 distinct roles ("Windows Repair Technician" alone appears six
    // times), so pinning the exact ordered list covers both the dedup guard and the sort.
    const QStringList expected_roles{QStringLiteral("Audio Device Technician"),
                                     QStringLiteral("Battery Health Technician"),
                                     QStringLiteral("Browser Support Technician"),
                                     QStringLiteral("Customer Report Writer"),
                                     QStringLiteral("Diagnostic Technician"),
                                     QStringLiteral("Driver and Device Technician"),
                                     QStringLiteral("PC Technician"),
                                     QStringLiteral("Performance Technician"),
                                     QStringLiteral("Printer Technician"),
                                     QStringLiteral("Research Assistant"),
                                     QStringLiteral("Security Technician"),
                                     QStringLiteral("Software Deployment Technician"),
                                     QStringLiteral("Storage Diagnostic Technician"),
                                     QStringLiteral("System Cleanup Technician"),
                                     QStringLiteral("Windows Repair Technician")};
    QCOMPARE(store.roles(), expected_roles);

    // Exactly six built-in workflows carry the "Windows Repair Technician" role -- and these are
    // the six. The second probe is load-bearing: "Windows Repair Technician" sorts LAST of the 15
    // bundled roles, so a comparator-polarity slip at ai_workflow_store.cpp:99 (== 0 -> >= 0)
    // returns those same six and is invisible here. "Audio Device Technician" sorts FIRST and owns
    // exactly one workflow, so an ordering match instead of an equality match hands back all 25.
    const auto repair = store.workflowsForRole(QStringLiteral("Windows Repair Technician"));
    QStringList repair_ids;
    repair_ids.reserve(repair.size());
    for (const auto& workflow : repair) {
        repair_ids.append(workflow.id);
    }
    repair_ids.sort();
    QCOMPARE(repair_ids.join(QStringLiteral(",")),
             QStringLiteral("bsod_investigation,network_connectivity_repair,time_sync_repair,"
                            "user_profile_login_repair,windows_search_index_repair,"
                            "windows_update_repair"));

    const auto audio = store.workflowsForRole(QStringLiteral("Audio Device Technician"));
    QCOMPARE(audio.size(), 1);
    QCOMPARE(audio.first().id, QStringLiteral("audio_device_troubleshooting"));

    // The role key is trimmed and matched case-insensitively, not literally.
    QCOMPARE(store.workflowsForRole(QStringLiteral("  windows repair technician  ")).size(), 6);
    // An empty role is the "no filter" sentinel, not a match against empty-role workflows.
    QCOMPARE(store.workflowsForRole(QString()).size(), 25);
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

    // The user template must REPLACE the built-in in place (ai_workflow_store.cpp:80-87), not be
    // appended beside it. workflowById alone cannot see a duplicate: rebuildIndex (:140-145) maps
    // an id to the LAST entry carrying it, so a shadowed built-in would still answer with the
    // override's title. Pin the catalog and the role listing, which do see it.
    const auto all_workflows = store.workflows();
    QCOMPARE(all_workflows.size(), 25);  // 25 built-ins; the override replaces one, adds no 26th
    int copies_of_overridden_id = 0;
    for (const auto& candidate : all_workflows) {
        if (candidate.id == QStringLiteral("download_offline_installer")) {
            ++copies_of_overridden_id;
        }
    }
    QCOMPARE(copies_of_overridden_id, 1);
    // The replacement declares role "PC Technician", so the built-in's old role listing must lose
    // it: four built-ins are "Software Deployment Technician", three survive the override.
    QCOMPARE(store.workflowsForRole(QStringLiteral("Software Deployment Technician")).size(), 3);
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

    // The whole point of failing at LOAD time is telling the author which phase carries which
    // typo, so pin the exact message (and that it is the only error raised).
    QStringList detail;
    QVERIFY(!workflow.isValid(&detail));
    QCOMPARE(detail.size(), 1);
    QCOMPARE(detail.at(0), QStringLiteral("Phase plan has unsupported type 'tool-action'"));
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
    // The load returns false because a_bad.json is invalid, and the reason reaches the
    // caller; the good file must still load.
    QVERIFY(!store.loadDirectory(temp_dir.path(), &errors));
    QVERIFY(
        errors.join(QStringLiteral("\n")).contains(QStringLiteral("Missing required field: id")));

    QVERIFY(store.workflowById(QStringLiteral("good_wf")) != nullptr);
    // The invalid sibling is skipped, not added.
    QCOMPARE(store.workflows().size(), 1);
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
    // Pin the WHOLE message and the error count: the duplicated phase id and the owning
    // workflow id are different strings, so a swapped .arg() pair would still satisfy a
    // prefix-only contains(), and a second spurious error could hide inside the join.
    QCOMPARE(errors,
             QStringList{QStringLiteral("Duplicate phase id 'plan' in workflow sample_workflow")});
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
    // Pin the whole message, not just its prefix: the duplicated input id and the owning
    // workflow id are different strings, so a swapped .arg() pair would still satisfy a
    // prefix-only contains().
    QVERIFY2(errors.contains(QStringLiteral(
                 "Duplicate required-input id 'app_name' in workflow sample_workflow")),
             qPrintable(errors.join(QStringLiteral("; "))));

    // validateWorkflowInputs refuses on a second count that nothing else in the suite reaches:
    // an input with no id at all. input_values is keyed by that id, so an id-less required
    // input can never be filled -- it must be rejected at load, not gated on at run time.
    QJsonObject anonymous_input;
    anonymous_input[QStringLiteral("label")] = QStringLiteral("App");
    object[QStringLiteral("required_inputs")] = QJsonArray{anonymous_input};

    QStringList anonymous_errors;
    const auto anonymous = sak::ai::WorkflowTemplate::fromJson(object,
                                                               QStringLiteral("no_id_input"),
                                                               &anonymous_errors);
    QVERIFY(!anonymous.isValid());
    QVERIFY2(anonymous_errors.contains(
                 QStringLiteral("Workflow sample_workflow has a required input with no id")),
             qPrintable(anonymous_errors.join(QStringLiteral("; "))));
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
    // Pin the WIRED message, not just the validator's tail: the "Phase <id>: <detail>" envelope
    // is what tells the author WHICH phase to fix, and the detail must echo back the placeholder
    // actually found in the command. Pin the error count too, so a second spurious error cannot
    // hide inside the join.
    QCOMPARE(errors.size(), 1);
    QCOMPARE(errors.first(),
             QStringLiteral("Phase plan: cmd.exe workflow command embeds placeholder "
                            "'${user_message}'; cmd.exe has no literal quoting that can make an "
                            "arbitrary value inert, so run_cmd command templates must not use "
                            "placeholders"));

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
