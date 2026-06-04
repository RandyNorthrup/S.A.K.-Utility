#include "sak/ai/ai_prompt_assembler.h"

#include <QtTest/QtTest>

class AiPromptAssemblerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void assembleIncludesRequiredGuardrails();
    void assembleIncludesDynamicSections();
    void assembleIncludesInjectionAndMutationGuardrails();
    void assembleIncludesWorkflowOrchestrationGuardrails();
};

void AiPromptAssemblerTests::assembleIncludesRequiredGuardrails() {
    sak::ai::AiPromptAssemblyInput input;
    input.access_mode_label = QStringLiteral("Unattended Full Access");
    input.agent_profile = QStringLiteral("PC Technician");
    input.local_execution_enabled = true;
    input.unattended_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Session role: PC Technician")));
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
    input.agent_profile = QStringLiteral("Tester");
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
    input.agent_profile = QStringLiteral("Windows Repair Technician");
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
    input.agent_profile = QStringLiteral("PC Technician");
    input.local_execution_enabled = true;
    input.assisted_full_access = true;

    const QString prompt = sak::ai::AiPromptAssembler::assemble(input);
    QVERIFY(prompt.contains(QStringLiteral("Workflow orchestration")));
    QVERIFY(prompt.contains(QStringLiteral("declared SAK workflow catalog")));
    QVERIFY(prompt.contains(QStringLiteral("Read-only subagents may run in parallel")));
    QVERIFY(prompt.contains(QStringLiteral("Subagent conflict rule")));
    QVERIFY(prompt.contains(QStringLiteral("critic/verification step")));
}

QTEST_GUILESS_MAIN(AiPromptAssemblerTests)
#include "test_ai_prompt_assembler.moc"
