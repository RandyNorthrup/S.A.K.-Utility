#include "sak/ai/ai_prompt_assembler.h"

#include <QtTest/QtTest>

class AiPromptAssemblerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void assembleIncludesRequiredGuardrails();
    void assembleIncludesDynamicSections();
    void assembleIncludesInjectionAndMutationGuardrails();
    void assembleIncludesWorkflowOrchestrationGuardrails();
    void assembleIncludesAutomationSurfaceGuardrails();
    void assembleIncludesAppActionGuardrails();
};

void AiPromptAssemblerTests::assembleIncludesRequiredGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Unattended Full Access");
    input.local_execution_enabled = true;
    input.unattended_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Scan workflow")));
    QVERIFY(prompt.contains(
        QStringLiteral("first check whether that product/tool is already installed")));
    QVERIFY(prompt.contains(QStringLiteral("checksum mismatch")));
    QVERIFY(prompt.contains(QStringLiteral("Tool health")));
    QVERIFY(prompt.contains(QStringLiteral("requires_admin=true")));
}

void AiPromptAssemblerTests::assembleIncludesDynamicSections() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Chat");
    input.workflow_catalog = QStringLiteral("Workflow catalog body");
    input.context_notes = QStringLiteral("Context body");
    input.session_memory = QStringLiteral("Memory body");
    input.pending_steering_messages = {QStringLiteral("Stop retrying")};

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Workflow catalog body")));
    QVERIFY(prompt.contains(QStringLiteral("Context body")));
    QVERIFY(prompt.contains(QStringLiteral("Memory body")));
    QVERIFY(prompt.contains(QStringLiteral("- Stop retrying")));
    QVERIFY(prompt.contains(QStringLiteral("Local execution is disabled")));
}

void AiPromptAssemblerTests::assembleIncludesInjectionAndMutationGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Assisted Full Access");
    input.local_execution_enabled = true;
    input.assisted_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Prompt injection defense")));
    QVERIFY(prompt.contains(QStringLiteral("web pages, command output, logs")));
    QVERIFY(prompt.contains(QStringLiteral("Ambiguous mutation rule")));
    QVERIFY(prompt.contains(QStringLiteral("exact target, rollback/restore-point option")));
    QVERIFY(prompt.contains(QStringLiteral("Destructive boundary")));
    QVERIFY(prompt.contains(QStringLiteral("security controls")));
}

void AiPromptAssemblerTests::assembleIncludesWorkflowOrchestrationGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Assisted Full Access");
    input.local_execution_enabled = true;
    input.assisted_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Orchestration:")));
    // The model is told it can ACT on orchestration via the two agency tools, and
    // when to use each (inline vs delegate vs workflow).
    QVERIFY(prompt.contains(QStringLiteral("delegate_subagent")));
    QVERIFY(prompt.contains(QStringLiteral("run_workflow")));
    QVERIFY(prompt.contains(QStringLiteral("handle simple, single-step requests inline")));
    QVERIFY(prompt.contains(QStringLiteral("Read-only subagents may run in parallel")));
    QVERIFY(prompt.contains(QStringLiteral("Subagent conflict rule")));
    QVERIFY(prompt.contains(QStringLiteral("critic/verification step")));
}

void AiPromptAssemblerTests::assembleIncludesAutomationSurfaceGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Assisted Full Access");
    input.local_execution_enabled = true;
    input.assisted_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    // The model is told the two UI-control surfaces exist, when each applies, that a headless
    // path is preferred, and that it should proactively suggest them from the user's request.
    QVERIFY(prompt.contains(QStringLiteral("Automation surfaces")));
    QVERIFY(prompt.contains(QStringLiteral("Browser control")));
    QVERIFY(prompt.contains(QStringLiteral("Desktop control")));
    QVERIFY(prompt.contains(QStringLiteral("Use browser control when the task lives in a web")));
    QVERIFY(prompt.contains(QStringLiteral("Headless first")));
    QVERIFY(prompt.contains(QStringLiteral("Proactively suggest these surfaces")));
}

void AiPromptAssemblerTests::assembleIncludesAppActionGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Unattended Full Access");
    input.local_execution_enabled = true;
    input.unattended_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    // The model is told the app's own headless features live behind sak_app_action, to list
    // before running, to prefer it over raw shell, to suggest it proactively, and how it gates.
    QVERIFY(prompt.contains(QStringLiteral("sak_app_action")));
    QVERIFY(prompt.contains(QStringLiteral("operation=list FIRST")));
    QVERIFY(prompt.contains(QStringLiteral("Prefer sak_app_action over raw run_powershell")));
    QVERIFY(prompt.contains(QStringLiteral("Suggest sak_app_action proactively")));
    QVERIFY(prompt.contains(QStringLiteral("catastrophic actions")));
}

QTEST_GUILESS_MAIN(AiPromptAssemblerTests)
#include "test_ai_prompt_assembler.moc"
