// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_run_state.h"
#include "sak/ai/ai_tool_call_router.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/ai_tool_turn.h"
#include "sak/ai/ai_trace_store.h"
#include "sak/ai/openai_responses_client.h"
#include "sak/offline_deployment_worker.h"
#include "sak/widget_helpers.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <memory>

class QComboBox;
class QEvent;
class QFileInfo;
class QFrame;
class QGroupBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QSemaphore;
class QSplitter;
class QTextBrowser;
class QThread;
class QTimer;
class QVBoxLayout;

namespace sak {

class ChocolateyManager;
class ElevationBroker;
class AiTranscriptView;
class LogToggleSwitch;
class OfflineDeploymentWorker;
class PackageListManager;

namespace ai {
struct AiCommandRequest;
struct AiCommandResult;
struct AiCommandToolPlan;
struct WorkflowTemplate;
class CredentialStore;
class ConversationStore;
class ExecutionBroker;
class AiHumanGateStore;
class TokenUsageTracker;
class TraceStore;
class WorkflowStore;
class AiRunStateStore;
class AiToolDispatcher;
class AiToolHealthLedger;
class AiLeaseManager;
}  // namespace ai

/// @brief UI shell for the SAK AI Assistant panel.
///
/// Provides the AI Assistant chat panel, workflow runner, local tool gateway,
/// artifact/report surface, and session trace integration.
class AiAssistantPanel : public QWidget {
    Q_OBJECT

public:
    explicit AiAssistantPanel(QWidget* parent = nullptr);
    ~AiAssistantPanel() override;

    AiAssistantPanel(const AiAssistantPanel&) = delete;
    AiAssistantPanel& operator=(const AiAssistantPanel&) = delete;
    AiAssistantPanel(AiAssistantPanel&&) = delete;
    AiAssistantPanel& operator=(AiAssistantPanel&&) = delete;

    /// @brief Access the log toggle switch for MainWindow connection.
    [[nodiscard]] LogToggleSwitch* logToggle() const { return m_logToggle; }

    /// @brief Permanent status-bar text for the AI assistant.
    [[nodiscard]] QString statusDetails() const;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void statusDetailsChanged(const QString& message);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onAccessModeChanged(int index);
    void onPromptTemplateSelected(int index);
    void onAddWorkflowClicked();
    void onWorkflowDetailsClicked();
    void onRunDetailsClicked();
    void onResumeGateClicked();
    void onNewSessionClicked();
    void onLoadApiKeyClicked();
    void onAddContextFilesClicked();
    void onAddInstructionContextClicked();
    void onClearContextClicked();
    void onSendClicked();
    void onStopClicked();
    void onSessionSelected(int index);
    void onRenameSessionClicked();
    void onRequestStarted();
    void onRequestFinished();
    void onResponseReady(const sak::ai::OpenAIResponseResult& result);
    void onModelsReady(const QStringList& model_ids);
    void onInputTokenCountReady(const QString& request_id, qint64 input_tokens);
    void onInputTokenCountFailed(const QString& request_id, const QString& error_message);
    void onRequestFailed(const QString& error_message);
    void onBrokerStarted(const QString& command_id);
    void onBrokerStdoutChunk(const QString& command_id, const QString& chunk);
    void onBrokerStderrChunk(const QString& command_id, const QString& chunk);
    void onBrokerFinished(const QString& command_id, const sak::ai::AiCommandResult& result);

private:
    enum class AccessMode {
        ChatAndResearch,
        AssistedFullAccess,
        UnattendedFullAccess,
    };

    enum class BusyPromptAction {
        ApplySteering,
        QueueAfterRun,
        CancelAndQueue,
        Discard,
    };

    enum class ReportFormat {
        Html,
        Markdown,
        PlainText,
    };

    enum class WorkflowSendResult {
        NoWorkflow,
        Handled,
    };

    struct ContextItem {
        enum class Type {
            Workflow,
            Instruction,
            Skill,
            TextFile,
            ImageFile,
            DocumentFile,
        };

        Type type{Type::Instruction};
        QString display_name;
        QString path;
        QString mime_type;
        QString text;
        QByteArray bytes;
        qint64 original_size{0};
    };

    struct ContextChipPalette {
        QString background;
        QString border;
        QString text_color;
    };

    struct PendingToolCallContext {
        const ai::OpenAIFunctionCall* call{nullptr};
        ai::AiToolCallKind kind{ai::AiToolCallKind::Unknown};
        QJsonObject metadata;
    };

    struct OfflineToolRunResult {
        QString manifest_written;
        QString operation_error;
        QStringList log_lines;
        QJsonArray package_events;
        BatchStats final_stats;
        bool completed{false};
    };

    struct WorkflowToolDispatchPlan {
        ai::AiToolPolicy policy{ai::AiToolPolicy::NoLocalExecution};
        ai::AiToolCallRequest request;
        QJsonObject args;
        QJsonObject error;
        bool ok{false};
        bool phase_risky{false};
    };

    void setupUi();
    QWidget* createChatWorkspace();
    QWidget* createContextPane();
    QWidget* createConversationPane();
    void createStatusStrip(QVBoxLayout* rootLayout);
    void configureExecutionBrokers();
    [[nodiscard]] bool initializeAccessibilityAuditUi();
    [[nodiscard]] bool loadWorkflowDefaults(QStringList* workflow_errors);
    [[nodiscard]] QString initializeToolHealthLedger();
    void initializeStandardPanel(bool workflows_loaded,
                                 const QStringList& workflow_errors,
                                 const QString& health_ledger_error);
    void initializePackageManager();
    void setupContextPaneSessionSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneAgentSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneWorkflowPicker(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneWorkflowDetails(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneAccessSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneCredentialSection(QVBoxLayout* layout, QWidget* pane);
    void onWorkflowTemplatePickerChanged(int index);
    void clearWorkflowSelectionPreview();
    void previewWorkflowTemplateSelection(const ai::WorkflowTemplate& workflow);
    void showUnstructuredWorkflowTemplateSelection(int index);
    void setupConversationHeader(QVBoxLayout* layout, QWidget* pane);
    void setupConversationStatusRow(QVBoxLayout* layout, QWidget* header);
    void setupConversationHeaderActions(QVBoxLayout* layout, QWidget* header);
    void setupConversationTranscript(QVBoxLayout* layout, QWidget* pane);
    void setupConversationComposer(QVBoxLayout* layout, QWidget* pane);
    void setupComposerInput(QVBoxLayout* layout, QWidget* composer);
    void setupComposerContextList(QVBoxLayout* layout, QWidget* composer);
    void setupComposerActions(QVBoxLayout* layout, QWidget* composer);
    void connectAiClient();
    void connectOpenAiClientSignals();
    void connectExecutionBrokerSignals();
    void connectElevationBrokerSignals();
    void handleElevationBrokerProgress(int percent, const QString& status);
    void ensureActivityTimer();
    void loadRememberedApiKey();
    void updateCredentialControls();
    void updateLoadKeyButton(bool has_key, bool busy);
    void updateReportButton(bool busy);
    void updateResumeGateButton(bool has_key, bool busy);
    void updatePrimaryActionButton();
    void updateRunTelemetryLabels();
    void updateAgentActivityLabel();
    void updateContextWindowUsageLabel();
    void ensureContextTokenTimer();
    void scheduleContextTokenRefresh();
    void refreshContextTokenCount();
    void resetContextTokenCount(const QString& status);
    [[nodiscard]] qint64 exactContextUsageTokens() const;
    [[nodiscard]] qint64 currentContextWindowTokens() const;
    [[nodiscard]] bool currentContextWindowIsDocumented() const;
    [[nodiscard]] QString contextWindowStatusText() const;
    void setApiKeyStatus(const QString& text,
                         const char* color,
                         const QString& marker,
                         const char* marker_color);
    void updateTokenLabels();
    void refreshPromptTemplates();
    void resetPromptTemplatePicker();
    void syncSessionRoleForWorkflow(const ai::WorkflowTemplate* workflow);
    void updateSessionRoleDisplay();
    void resetSessionRole();
    void restoreSessionRoleForSession(const QString& session_id);
    void updateSessionRoleForPrompt(const QString& message);
    void setSessionRole(const QString& role, const QString& source, bool persist);
    void persistSessionRoleChoice();
    [[nodiscard]] QString currentWorkflowRole() const;
    [[nodiscard]] const ai::WorkflowTemplate* selectedWorkflowTemplate() const;
    [[nodiscard]] QString workflowTemplateComboLabel(const ai::WorkflowTemplate& workflow) const;
    [[nodiscard]] QString workflowTemplateTooltip(const ai::WorkflowTemplate& workflow,
                                                  const QString& label) const;
    void populateWorkflowTemplatePicker(const QVector<ai::WorkflowTemplate>& workflows);
    void refreshContextList();
    [[nodiscard]] int contextChipMaxWidth() const;
    [[nodiscard]] int contextChipWidth(const QString& chip_text, int max_chip_width) const;
    void addContextListItem(int index, int max_chip_width);
    [[nodiscard]] ContextChipPalette contextChipPalette(ContextItem::Type type) const;
    void reloadSessionPicker();
    void startNewPersistentSession(const QString& title);
    [[nodiscard]] bool ensurePersistentSession(const QString& title);
    [[nodiscard]] QString workflowTitleForChatRename(const QString& workflow_id) const;
    void autoRenameDefaultChatFromFirstPrompt(const QString& message, const QString& workflow_id);
    void loadSessionTranscript(const QString& session_id);
    [[nodiscard]] bool isAiBusy() const;
    void setUiBusy(bool busy);
    void setActivityIndicator(const QString& text, bool active);
    void updateActivityIndicatorFrame();
    void appendTranscriptMessage(const QString& role, const QString& text, bool expanded = false);
    void appendLoadedTranscriptLine(const QString& line);
    void scrollTranscriptToBottomLater(bool force = false);
    void restoreTranscriptScrollPositionLater(int value);
    [[nodiscard]] bool isTranscriptScrolledToBottom() const;
    void setTranscriptAutoScroll(bool enabled);
    void updateJumpToNewestButton();
    void jumpTranscriptToNewest();
    void renderTranscriptMessages(bool scroll_to_bottom = true);
    void recordPromptHistory(const QString& prompt);
    [[nodiscard]] bool cyclePromptHistory(int direction);
    [[nodiscard]] BusyPromptAction askBusyPromptAction(const QString& message);
    void applySteeringMessage(const QString& message);
    void queuePromptForLater(const QString& message, const QString& reason);
    void dispatchQueuedPromptIfIdle();
    [[nodiscard]] QString apiKey() const;
    [[nodiscard]] QString messageText() const;
    [[nodiscard]] bool filterWheelEvent(QObject* watched, QEvent* event);
    [[nodiscard]] bool filterMessageEditKeyPress(QEvent* event);
    [[nodiscard]] bool handleComposerKeyAction(int action);
    [[nodiscard]] bool handleComposerHistoryKeyAction(int action);
    [[nodiscard]] QString buildInstructions() const;
    [[nodiscard]] QString contextInstructions() const;
    [[nodiscard]] QVector<ai::OpenAIInputAttachment> buildContextAttachments() const;
    void beginToolTurn(const ai::OpenAIResponseResult& response);
    void dispatchNextToolCall();
    [[nodiscard]] bool preparePendingToolCall(const ai::OpenAIFunctionCall& call,
                                              PendingToolCallContext* context,
                                              ai::OpenAIFunctionOutput* output);
    [[nodiscard]] bool dispatchBuiltInToolCall(const PendingToolCallContext& context,
                                               const QJsonObject& args,
                                               ai::OpenAIFunctionOutput* output);
    [[nodiscard]] bool authorizeCommandToolCall(const PendingToolCallContext& context,
                                                const ai::AiCommandToolPlan& plan,
                                                ai::OpenAIFunctionOutput* output);
    [[nodiscard]] bool acquireCommandToolLease(const PendingToolCallContext& context,
                                               const ai::AiCommandToolPlan& plan,
                                               ai::OpenAIFunctionOutput* output);
    void startCommandToolCall(const PendingToolCallContext& context,
                              const ai::AiCommandToolPlan& plan);
    [[nodiscard]] bool dispatchCommandToolCall(const PendingToolCallContext& context,
                                               const QJsonObject& args,
                                               ai::OpenAIFunctionOutput* output);
    void appendToolOutputAndContinue(ai::OpenAIFunctionOutput output);
    void finishToolTurnAndContinue();
    void resetPendingToolTurn();
    [[nodiscard]] QString currentPendingToolCallId() const;
    [[nodiscard]] QJsonObject pendingToolTurnState() const;
    [[nodiscard]] bool restorePendingToolTurnState(const QJsonObject& state);
    void restorePendingRunIdentity(const QString& run_id, const QString& response_id);
    [[nodiscard]] bool completeResumedToolGateWithOutput(const ai::AiHumanGate& gate,
                                                         const QJsonObject& output_json);
    [[nodiscard]] bool confirmCommandWithUser(const QString& shell,
                                              const QString& preview,
                                              bool risky_change);
    [[nodiscard]] bool consumeResumedCommandApproval(const QString& shell,
                                                     const QString& preview,
                                                     bool risky_change,
                                                     const QString& pending_call_id,
                                                     bool* approval_result);
    [[nodiscard]] QJsonObject commandApprovalMetadata(const QString& shell,
                                                      const QString& preview,
                                                      bool risky_change) const;
    [[nodiscard]] QString beginCommandApprovalGate(QJsonObject* metadata);
    [[nodiscard]] bool requestCommandApprovalFromUser(const QString& shell, const QString& preview);
    [[nodiscard]] bool rejectCommandApproval(const QString& gate_id,
                                             QJsonObject metadata,
                                             const QString& shell,
                                             ai::AiRunStatus previous_status);
    void acceptCommandApproval(const QString& gate_id,
                               QJsonObject metadata,
                               const QString& shell,
                               ai::AiRunStatus previous_status);
    [[nodiscard]] bool offerRestorePointIfNeeded(const QString& preview, bool risky_change);
    [[nodiscard]] bool consumeResumedRestoreDecision(const QString& preview, bool risky_change);
    [[nodiscard]] QJsonObject restorePointOfferMetadata(const QString& preview,
                                                        bool risky_change) const;
    [[nodiscard]] bool handleRestorePointOfferChoice(int choice,
                                                     const QString& gate_id,
                                                     QJsonObject metadata,
                                                     ai::AiRunStatus previous_status,
                                                     const QString& preview);
    [[nodiscard]] bool handleRestorePointFailure(const QString& gate_id,
                                                 QJsonObject metadata,
                                                 ai::AiRunStatus previous_status,
                                                 const QString& preview);
    void restoreRunStatusAfterHumanDecision(ai::AiRunStatus previous_status,
                                            const QString& message);
    [[nodiscard]] bool createRestorePoint();
    [[nodiscard]] bool handleRestorePointBrokerUnavailable();
    [[nodiscard]] QString restorePointDescription() const;
    [[nodiscard]] QString restorePointScript(const QString& description) const;
    [[nodiscard]] QJsonObject restorePointPayload(const QString& script) const;
    void traceRestorePointStart(QJsonObject* trace_metadata, const QString& description);
    [[nodiscard]] bool handleRestorePointExecutionError(const QString& error_message,
                                                        QJsonObject trace_metadata);
    [[nodiscard]] bool handleRestorePointResponse(const QJsonObject& response_data,
                                                  QJsonObject trace_metadata);
    [[nodiscard]] QString allocateCommandId();
    void appendCitationsToList(const QVector<ai::OpenAIUrlCitation>& citations);
    [[nodiscard]] QJsonObject runScreenshotTool(const QString& reason);
    [[nodiscard]] QJsonObject runDownloadTool(const QString& url, const QString& filename);
    [[nodiscard]] QJsonObject runWorkflowPowerShellTool(const QJsonObject& args,
                                                        const QString& command_preview);
    [[nodiscard]] ai::AiCommandResult executeWorkflowPowerShellRequest(
        const ai::AiCommandRequest& request, const QString& command_id);
    [[nodiscard]] ai::AiCommandResult executeElevatedWorkflowPowerShellRequest(
        const ai::AiCommandRequest& request);
    [[nodiscard]] ai::AiCommandResult executeStandardWorkflowPowerShellRequest(
        const ai::AiCommandRequest& request, const QString& command_id);
    void connectWorkflowPowerShellLogging(ai::ExecutionBroker* broker, QObject* worker);
    void connectWorkflowPowerShellCancelPolling(QTimer* cancel_timer,
                                                ai::ExecutionBroker* broker,
                                                QObject* worker);
    [[nodiscard]] QJsonObject runPackageManagerTool(const QJsonObject& args);
    [[nodiscard]] QJsonObject runProviderGatewayTool(const QJsonObject& args);
    [[nodiscard]] QJsonObject runSessionSearchTool(const QJsonObject& args) const;
    [[nodiscard]] QJsonObject packageManagerQueryOperation(const QJsonObject& args,
                                                           const QString& operation,
                                                           const QString& query);
    [[nodiscard]] QJsonObject packageManagerPackageOperation(const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version,
                                                             int timeout_seconds);
    [[nodiscard]] QJsonObject packageManagerPackagePreflight(const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version);
    [[nodiscard]] QJsonObject authorizePackageManagerChange(const QString& preview);
    [[nodiscard]] QJsonObject executePackageManagerPackageChange(const QString& operation,
                                                                 const QString& package_id,
                                                                 const QString& version,
                                                                 int timeout_seconds,
                                                                 const QString& preview);
    [[nodiscard]] QJsonObject packageAlreadyInstalledResult(const QString& operation,
                                                            const QString& package_id,
                                                            const QString& version,
                                                            const QString& installed_version);
    [[nodiscard]] QJsonObject packageManagerVersionResult() const;
    [[nodiscard]] QJsonObject packageManagerSearchResult(const QJsonObject& args,
                                                         const QString& query);
    [[nodiscard]] QJsonObject packageManagerReadResult(const QString& operation,
                                                       const QString& package_id) const;
    [[nodiscard]] QString packageManagerChangePreview(const QString& operation,
                                                      const QString& package_id,
                                                      const QString& version) const;
    [[nodiscard]] QJsonObject runPackageManagerChange(const QString& operation,
                                                      const QString& package_id,
                                                      const QString& version,
                                                      int timeout_seconds);
    [[nodiscard]] QJsonObject runOfflineDownloaderTool(const QJsonObject& args);
    [[nodiscard]] QJsonObject offlineRunOperation(const QJsonObject& args,
                                                  const QString& operation);
    [[nodiscard]] QJsonObject offlinePresetsResult() const;
    [[nodiscard]] QJsonObject offlineSearchResult(const QJsonObject& args, const QString& query);
    [[nodiscard]] QString offlineOutputDirectory(const QString& operation,
                                                 const QJsonObject& args,
                                                 QString* error_message) const;
    [[nodiscard]] bool authorizeOfflineOperation(const QString& operation, const QJsonObject& args);
    [[nodiscard]] OfflineToolRunResult executeOfflineOperation(
        const QString& operation,
        const QVector<QPair<QString, QString>>& packages,
        const QString& output_dir,
        const QJsonObject& args);
    [[nodiscard]] bool validateOfflineOperation(const QString& operation,
                                                OfflineToolRunResult* run) const;
    void appendOfflineOperationStartedEvent(const QString& operation,
                                            const QString& output_dir,
                                            const QJsonObject& args);
    void connectOfflineWorkerSignals(OfflineDeploymentWorker* worker,
                                     QObject* context,
                                     QThread* offline_thread,
                                     QSemaphore* done,
                                     OfflineToolRunResult* run);
    void startOfflineWorkerOperation(OfflineDeploymentWorker* worker,
                                     const QString& operation,
                                     const QVector<QPair<QString, QString>>& packages,
                                     const QString& output_dir,
                                     const QJsonObject& args);
    [[nodiscard]] QJsonObject offlineOperationResultJson(const QString& operation,
                                                         const QString& output_dir,
                                                         const OfflineToolRunResult& run_result);
    void appendOfflineArtifacts(const QString& operation,
                                const QString& output_dir,
                                const QString& manifest_path,
                                QJsonObject* result);
    void appendArtifactRow(const QString& path, const QString& kind);
    void refreshArtifactList();
    void onArtifactsClicked();
    void onGenerateReportClicked();
    [[nodiscard]] QString generateReport(QString* error_message,
                                         const QString& output_path = {},
                                         ReportFormat format = ReportFormat::Html) const;
    [[nodiscard]] bool hasReportableResults() const;
    [[nodiscard]] bool hasReportableCounters() const;
    [[nodiscard]] bool hasReportableWorkflowPhases() const;
    [[nodiscard]] bool hasReportableTraceActivities() const;
    [[nodiscard]] bool hasReportableArtifacts() const;
    [[nodiscard]] ai::AiCommandResult runElevatedPowerShell(const ai::AiCommandRequest& request);
    void continueAfterToolCalls(const ai::OpenAIResponseResult& result,
                                QVector<ai::OpenAIFunctionOutput> outputs);

    [[nodiscard]] AccessMode currentAccessMode() const;
    [[nodiscard]] QString currentAccessModeLabel() const;
    [[nodiscard]] static QString contextItemLabel(const ContextItem& item);
    [[nodiscard]] static QString contextItemKindLabel(ContextItem::Type type);
    void updateAccessStatus();
    void addContextFile(const QString& path);
    [[nodiscard]] ContextItem contextItemFromFile(const QFileInfo& info,
                                                  QByteArray bytes,
                                                  const QString& mime_type) const;
    void persistContextItem(const ContextItem& item);
    void addInstructionFile(const QString& path);
    void removeContextItem(int index);
    void appendLocalEvent(const QString& message);
    void appendSessionMemory(const QString& kind, const QString& title, const QString& text);
    void applyPromptTemplate(const QString& title, const QString& prompt);
    void applyWorkflowTemplate(const ai::WorkflowTemplate& workflow);
    void addWorkflowContextChip(const ai::WorkflowTemplate& workflow);
    void addWorkflowResourceContext(const QString& resource_path,
                                    const QString& label_prefix,
                                    ContextItem::Type type);
    void refreshTraceStoreForSession();
    void startAiRunTrace(const QString& message,
                         const QString& model,
                         const QString& preferred_run_id = {});
    void traceAiEvent(const QString& kind,
                      const QString& name,
                      const QString& status,
                      const QJsonObject& metadata = {});
    [[nodiscard]] QString currentTraceRunId() const;
    void appendTraceEventRecord(const QString& run_id,
                                const QString& kind,
                                const QString& name,
                                const QString& status,
                                const QJsonObject& metadata);
    void appendTraceActivityRecord(const ai::AiTraceEvent& event);
    void finishAiRunTrace(const QString& status, const QJsonObject& metadata = {});
    void emitStatusDetails();
    void saveRunStateSnapshot();
    void loadRunStateSnapshotForSession();
    void mergePendingHumanGate(ai::AiRunState* loaded);
    void markStaleRunCancelled(ai::AiRunState* loaded);
    void applyLoadedRunState(const ai::AiRunState& loaded);
    [[nodiscard]] QString beginHumanGate(const QString& kind,
                                         const QString& name,
                                         const QString& question,
                                         const QJsonObject& metadata = {});
    void resolveHumanGate(const QString& gate_id,
                          const QString& status,
                          const QString& decision,
                          const QString& response_summary,
                          const QJsonObject& metadata = {});
    [[nodiscard]] ai::AiHumanGate resolvedGateFromState(const QString& gate_id,
                                                        const QString& status,
                                                        const QString& decision,
                                                        const QString& response_summary,
                                                        const QJsonObject& metadata) const;
    void appendHumanGateRecord(const ai::AiHumanGate& gate);
    void clearPendingGateIfResolved(const ai::AiHumanGate& gate);
    void registerToolDispatcherHandlers();
    [[nodiscard]] bool ensureToolDispatcherReady();
    void registerToolAvailabilityCheckers();
    void registerDownloadFileAvailabilityChecker();
    void registerPackageManagerAvailabilityChecker();
    void registerOfflineDownloaderAvailabilityChecker();
    void registerProviderGatewayAvailabilityChecker();
    void registerSessionSearchAvailabilityChecker();
    void registerToolHandlers();
    void rebuildWorkflowDetailsView();
    [[nodiscard]] QString runDetailsText() const;
    [[nodiscard]] QStringList runDetailsSummaryLines() const;
    [[nodiscard]] QStringList runDetailsPhaseLines() const;
    [[nodiscard]] QStringList runDetailsActivityLines() const;
    [[nodiscard]] QStringList runDetailsHealthLines() const;
    [[nodiscard]] QStringList runDetailsToolHealthLines() const;
    [[nodiscard]] QStringList runDetailsProviderHealthLines() const;
    [[nodiscard]] QString runDetailsHealthRecordLine(const QJsonObject& record) const;
    [[nodiscard]] QVector<ai::AiActivityEvent> filteredRunDetailsActivities() const;
    [[nodiscard]] QString runDetailsActivityLine(const ai::AiActivityEvent& activity) const;
    [[nodiscard]] QString runDetailsHtml() const;
    void showRunDetails();
    void showWorkflowDetails(const ai::WorkflowTemplate& workflow);
    void hideWorkflowDetails();
    void clearWorkflowProgressUi();
    void beginWorkflowProgressUi(const ai::WorkflowTemplate& workflow,
                                 const QJsonObject& resume_state);
    void finishWorkflowProgressUi(const ai::AiOrchestratorResult& result);
    void updateWorkflowProgressUi();
    [[nodiscard]] QString workflowProgressFormat(int completed,
                                                 int total,
                                                 bool has_active_phase,
                                                 const QString& current_phase) const;
    [[nodiscard]] int workflowProgressValue(int completed, int total, bool has_active_phase) const;
    [[nodiscard]] int workflowCompletedPhaseCount() const;
    [[nodiscard]] ai::AiToolPolicy currentAccessToolPolicy() const;
    [[nodiscard]] const ai::WorkflowTemplate* attachedWorkflow() const;
    [[nodiscard]] bool ensureWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                            const QString& user_message,
                                            QJsonObject* input_values,
                                            const QJsonObject& initial_values = {},
                                            bool preserve_run_id = false);
    [[nodiscard]] bool collectRequiredWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                     const QString& user_message,
                                                     QJsonObject* input_values);
    [[nodiscard]] bool collectClarifyingWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                       const QString& user_message,
                                                       QJsonObject* input_values);
    [[nodiscard]] bool resumeWorkflowInputGate(const ai::AiHumanGate& gate);
    [[nodiscard]] bool resumeApprovalGate(const ai::AiHumanGate& gate);
    [[nodiscard]] bool resumeWorkflowRecoveryGate(const ai::AiHumanGate& gate);
    [[nodiscard]] const ai::WorkflowTemplate* recoveryWorkflowForGate(const ai::AiHumanGate& gate,
                                                                      QJsonObject* metadata,
                                                                      QJsonObject* resume_state);
    [[nodiscard]] bool resolveWorkflowRecoveryChoice(const ai::AiHumanGate& gate,
                                                     QJsonObject* metadata);
    void runWorkflowAsync(const ai::WorkflowTemplate& workflow,
                          const QString& user_message,
                          const QJsonObject& input_values,
                          const QString& preferred_run_id = {},
                          const QJsonObject& resume_state = {});
    void appendPhaseStartedToTranscript(const ai::WorkflowPhase& phase);
    void appendPhaseToTranscript(const ai::AiPhaseExecution& execution);
    void updatePhaseRunCounters(const ai::AiPhaseExecution& execution);
    [[nodiscard]] QJsonObject phaseTranscriptMetadata(const ai::AiPhaseExecution& execution,
                                                      QStringList* transcript_details);
    void appendRecoveryMetadata(const ai::AiPhaseExecution& execution, QJsonObject* metadata);
    void appendSubagentTranscriptDetails(const QJsonObject& subagent_result,
                                         QJsonObject* metadata,
                                         QStringList* transcript_details) const;
    [[nodiscard]] QString phaseTranscriptLine(const ai::AiPhaseExecution& execution,
                                              const QStringList& transcript_details) const;
    void recordPhaseTranscript(const ai::AiPhaseExecution& execution,
                               const QString& phase_chat,
                               const QJsonObject& metadata,
                               const QStringList& transcript_details);
    void onWorkflowRunFinished();
    [[nodiscard]] QString workflowResultText(const ai::AiOrchestratorResult& result) const;
    void recordWorkflowResult(const ai::AiOrchestratorResult& result,
                              const QString& workflow_result_text);
    void finishWorkflowRunState(const ai::AiOrchestratorResult& result);
    [[nodiscard]] QJsonObject dispatchWorkflowToolPhase(const ai::WorkflowPhase& phase,
                                                        ai::AiToolPolicy policy,
                                                        const ai::AiWorkflowPhaseContext& context);
    [[nodiscard]] WorkflowToolDispatchPlan workflowToolDispatchPlan(
        const ai::WorkflowPhase& phase,
        ai::AiToolPolicy policy,
        const ai::AiWorkflowPhaseContext& context);
    void initializeWorkflowToolPlan(const ai::WorkflowPhase& phase,
                                    ai::AiToolPolicy policy,
                                    const ai::AiWorkflowPhaseContext& context,
                                    WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowPowerShellTool(const ai::WorkflowPhase& phase,
                                                     WorkflowToolDispatchPlan* plan);
    void finalizeWorkflowToolPlan(const ai::WorkflowPhase& phase, WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowPackageTool(const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowOfflineTool(const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool authorizeWorkflowToolPhase(const ai::WorkflowPhase& phase,
                                                  const WorkflowToolDispatchPlan& plan);

    class PanelToolExecutor;

    struct WorkflowRunLaunch {
        QPointer<AiAssistantPanel> panel_guard;
        ai::WorkflowTemplate workflow;
        QString run_id;
        ai::CancellationToken token;
        QString api_key;
        QString model;
        QString reasoning;
        QJsonObject input_values;
        QJsonObject resume_state;
        QString user_message;
        PanelToolExecutor* executor{nullptr};
    };

    void beginWorkflowRunUiState(const ai::WorkflowTemplate& workflow,
                                 const QString& user_message,
                                 const QJsonObject& input_values,
                                 const QString& preferred_run_id,
                                 const QJsonObject& resume_state);
    void resetWorkflowRunWatcher();
    void startWorkflowRunFuture(const ai::WorkflowTemplate& workflow,
                                const QString& user_message,
                                const QJsonObject& input_values,
                                const QJsonObject& resume_state);
    [[nodiscard]] static ai::AiOrchestratorResult executeWorkflowRun(
        const WorkflowRunLaunch& launch);
    static void configureWorkflowRunner(ai::AiSubagentRunner* runner);
    [[nodiscard]] static ai::AiOrchestrationOptions workflowOrchestrationOptions(
        const WorkflowRunLaunch& launch);
    static void applyWorkflowResumeState(ai::AiOrchestrationOptions* options,
                                         const QJsonObject& resume_state);
    static void connectWorkflowOrchestratorCallbacks(ai::AiOrchestrator* orchestrator,
                                                     QPointer<AiAssistantPanel> panel_guard);

    [[nodiscard]] bool handleBusySendPrompt(const QString& message);
    [[nodiscard]] WorkflowSendResult startAttachedWorkflowFromPrompt(const QString& message);
    void appendUserTurn(const QString& message,
                        const QString& workflow_id = {},
                        const QJsonObject& workflow_inputs = {});
    [[nodiscard]] ai::OpenAIResponseRequest buildChatRequest(const QString& message) const;
    void startChatRequest(const QString& message);
    void cancelActiveRunToken();
    void cancelLocalAiWork();
    void finalizeStopRequest();
    void releaseCurrentCommandLease();
    [[nodiscard]] QJsonObject brokerResultJson(const QString& command_id,
                                               const ai::AiCommandResult& result) const;
    void recordBrokerResult(const QString& command_id,
                            const ai::AiCommandResult& result,
                            const QJsonObject& result_json);
    void appendBrokerResultToTranscript(const QJsonObject& result_json,
                                        const QJsonObject& trace_metadata);
    void completeBrokerToolTurn(const QJsonObject& result_json);
    void handleResponseToolCalls(const ai::OpenAIResponseResult& result,
                                 const QJsonObject& response_metadata);
    void handleAssistantResponse(const ai::OpenAIResponseResult& result,
                                 const QJsonObject& response_metadata);
    [[nodiscard]] QString assistantTextWithCitations(const ai::OpenAIResponseResult& result) const;
    [[nodiscard]] QJsonObject assistantResponseMetadata(
        const ai::OpenAIResponseResult& result) const;
    void persistAssistantResponse(const ai::OpenAIResponseResult& result,
                                  const QJsonObject& assistant_metadata);
    void recordToolLoopObservation(const QString& tool_name, const QJsonObject& result = {});
    [[nodiscard]] QString toolLoopCapSummary() const;

    PanelHeaderWidgets m_headerWidgets{};
    LogToggleSwitch* m_logToggle{nullptr};

    QPushButton* m_loadKeyButton{nullptr};
    QComboBox* m_modelCombo{nullptr};
    QLabel* m_sessionRoleValueLabel{nullptr};
    QComboBox* m_promptTemplateCombo{nullptr};
    QComboBox* m_reasoningEffortCombo{nullptr};
    QComboBox* m_sessionCombo{nullptr};
    QComboBox* m_accessModeCombo{nullptr};

    QLabel* m_connectionStatusLabel{nullptr};
    QLabel* m_connectionStatusIconLabel{nullptr};
    QLabel* m_accessStatusLabel{nullptr};

    AiTranscriptView* m_transcriptView{nullptr};
    QProgressBar* m_workflowProgressBar{nullptr};
    QLabel* m_agentActivityLabel{nullptr};
    QLabel* m_contextWindowLabel{nullptr};
    QListWidget* m_contextList{nullptr};
    QPushButton* m_artifactsButton{nullptr};
    QPlainTextEdit* m_messageEdit{nullptr};
    QPushButton* m_newSessionButton{nullptr};
    QPushButton* m_renameSessionButton{nullptr};
    QPushButton* m_addWorkflowButton{nullptr};
    QPushButton* m_workflowDetailsButton{nullptr};
    QPushButton* m_runDetailsButton{nullptr};
    QPushButton* m_resumeGateButton{nullptr};
    QPushButton* m_addContextFilesButton{nullptr};
    QPushButton* m_addInstructionButton{nullptr};
    QPushButton* m_clearContextButton{nullptr};
    QPushButton* m_sendButton{nullptr};
    QPushButton* m_generateReportButton{nullptr};

    std::unique_ptr<ai::OpenAIResponsesClient> m_client;
    std::unique_ptr<ai::CredentialStore> m_credentialStore;
    std::unique_ptr<ai::ConversationStore> m_conversationStore;
    std::unique_ptr<ElevationBroker> m_elevationBroker;
    std::unique_ptr<ai::ExecutionBroker> m_executionBroker;
    std::unique_ptr<ai::AiHumanGateStore> m_humanGateStore;
    std::unique_ptr<ai::TokenUsageTracker> m_tokenTracker;
    std::unique_ptr<ai::TraceStore> m_traceStore;
    std::unique_ptr<ai::AiRunStateStore> m_runStateStore;
    std::unique_ptr<ai::AiLeaseManager> m_leaseManager;
    std::unique_ptr<ai::AiToolHealthLedger> m_toolHealthLedger;
    std::unique_ptr<ai::AiToolDispatcher> m_toolDispatcher;
    std::unique_ptr<ai::WorkflowStore> m_workflowStore;
    std::unique_ptr<ChocolateyManager> m_chocoManager;
    std::unique_ptr<PackageListManager> m_packageListManager;
    std::unique_ptr<OfflineDeploymentWorker> m_offlineWorker;
    QVector<ContextItem> m_contextItems;
    QString m_apiKey;
    QString m_taskStatus;
    QString m_previousResponseId;
    QString m_sessionRole;
    QString m_sessionRoleSource;
    QString m_currentRunId;
    QString m_pendingWorkflowRunId;
    QString m_pendingSessionTitle;
    QString m_activeUserMessage;
    QString m_activeWorkflowUserMessage;
    QJsonObject m_activeWorkflowInputValues;
    QVector<ai::AiPhaseExecution> m_workflowPhaseHistory;
    QStringList m_activeWorkflowPhaseOrder;
    QString m_activeWorkflowTitle;
    QString m_activeWorkflowCurrentPhase;
    QHash<QString, int> m_activeWorkflowPhaseStartCounts;
    int m_activeWorkflowCompletedPhaseCount{0};
    ai::CancellationToken m_runToken;
    ai::AiRunState m_runState;
    bool m_loadingSessionPicker{false};
    bool m_restorePointOfferedThisSession{false};
    QStringList m_promptHistory;
    int m_promptHistoryIndex{-1};
    QString m_promptHistoryDraft;
    QStringList m_pendingSteeringMessages;
    QStringList m_queuedUserMessages;
    QStringList m_resumedApprovedToolCallIds;
    QStringList m_resumedRestoreToolCallIds;
    QVector<ai::OpenAIUrlCitation> m_citations;
    bool m_dispatchingQueuedPrompt{false};

    ai::AiToolTurn m_toolTurn;
    ai::CancellationToken m_pendingTurnToken;
    ai::CancellationToken m_pendingCallToken;
    QFrame* m_workflowDetailsPanel{nullptr};
    QLabel* m_workflowDetailsTitle{nullptr};
    QTextBrowser* m_workflowDetailsBody{nullptr};
    QPushButton* m_workflowDetailsCloseButton{nullptr};
    QString m_workflowDetailsCurrentId;
    QFutureWatcher<ai::AiOrchestratorResult>* m_workflowRunWatcher{nullptr};
    bool m_workflowRunActive{false};
    QTimer* m_activityTimer{nullptr};
    QTimer* m_contextTokenTimer{nullptr};
    QString m_contextTokenRequestId;
    QString m_contextTokenStatus;
    qint64 m_contextInputTokens{-1};
    int m_nextContextTokenRequestSequence{1};
    QString m_currentCommandId;
    QString m_currentCommandLeaseId;
    QString m_currentCommandPreview;
    QString m_currentStdoutBuffer;
    QString m_currentStderrBuffer;
    int m_nextCommandSequence{1};
    int m_toolTurnIterations{0};
    int m_toolCallsThisSession{0};
    QHash<QString, int> m_toolNamesThisMessage;
    QHash<QString, int> m_toolFailureClassesThisMessage;

    static constexpr int kMaxToolTurnsPerUserMessage = 12;
};

}  // namespace sak
