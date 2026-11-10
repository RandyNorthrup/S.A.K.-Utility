// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_conversation_store.h"
#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_mcp_session_pool.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_run_state.h"
#include "sak/ai/ai_subagent_tool_executor.h"
#include "sak/ai/ai_tool_call_context.h"
#include "sak/ai/ai_tool_call_router.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_loop_detector.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/ai_tool_turn.h"
#include "sak/ai/ai_trace_store.h"
#include "sak/ai/openai_responses_client.h"
#include "sak/app_action_registry.h"
#include "sak/offline_deployment_worker.h"
#include "sak/widget_helpers.h"
#include "sak/win32mcp/browser_extension_installer.h"

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <type_traits>

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
class QUrl;
class QVBoxLayout;

namespace sak {

class AppActionService;
class QuickActionController;
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
class SkillStore;
class AiRunStateStore;
class AiToolDispatcher;
class AiToolHealthLedger;
class AiLeaseManager;
class AiAsyncToolRunner;
struct AiPromptAssembly;
struct AiProviderGatewayToolCallbacks;
enum class AiAccessMode;
}  // namespace ai

#ifdef SAK_AI_TEST_HOOKS
// Test-only seam (compiled only into tests that define SAK_AI_TEST_HOOKS): when installed,
// replaces the modal approval / restore-point dialog so a test can script human decisions
// deterministically. The callback receives the prompt window title and command text and
// returns the chosen ApprovalPromptChoice as an int.
using AiApprovalPromptTestHook = std::function<int(const QString&, const QString&)>;
[[nodiscard]] AiApprovalPromptTestHook& aiApprovalPromptTestHook();
// Test-only seam: when set, delegate_subagent uses this instead of the real OpenAI model client
// factory, so the tool can be certified offline. A static, not a member: guarding a data member
// would change AiAssistantPanel's layout between hooked and product objects.
[[nodiscard]] ai::AiModelClientFactory& aiDelegateSubagentModelFactoryTestHook();
#endif  // SAK_AI_TEST_HOOKS

/// @brief UI shell for the SAK AI Assistant panel.
///
/// Provides the AI Assistant chat panel, workflow runner, local tool gateway,
/// artifact/report surface, and session trace integration.
class AiAssistantPanel : public QWidget {
    Q_OBJECT

    // Certification test for the off-GUI-thread built-in tool dispatch (P10-04):
    // drives beginToolTurn/dispatch and inspects the private async state.
    friend class AiAssistantPanelToolDispatchTest;

public:
    /// @param quick_actions the application's quick-action controller, whose built-in actions the
    ///        assistant lists and runs (shared, since this panel can outlive the main window
    ///        while its workers drain); null offers none of them
    explicit AiAssistantPanel(QWidget* parent = nullptr,
                              std::shared_ptr<QuickActionController> quick_actions = {});
    ~AiAssistantPanel() override;

    /// @brief Tears down a panel its owner no longer needs without crashing on live work.
    ///
    /// Cancels the run and every local operation, then gives the async tool worker and any
    /// workflow run a bounded drain. Drained, the panel is destroyed here. Still running past
    /// the bound, the overrun is reported and the panel is kept alive -- hidden and unparented,
    /// because those workers still reach it -- and deletes itself once they stop (owner decision
    /// 2026-10-24, "report + contain"; this was a qFatal). The owner hands a panel here instead
    /// of deleting it.
    static void release(std::unique_ptr<AiAssistantPanel> panel);

    /// @brief release() with an owner-chosen drain deadline instead of the standard 30 s.
    static void release(std::unique_ptr<AiAssistantPanel> panel, int drain_deadline_ms);

    /// @brief True while the async tool worker or a workflow run is still executing.
    [[nodiscard]] bool hasLiveBackgroundWork() const;

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
    /// @brief Ask before Unattended Full Access goes into force. True once the person agrees.
    [[nodiscard]] bool confirmUnattendedAccess();
    void onPromptTemplateSelected(int index);
    void onAddWorkflowClicked();
    void onWorkflowDetailsClicked();
    void onRunDetailsClicked();
    void onResumeGateClicked();
    void onNewSessionClicked();
    void onLoadApiKeyClicked();
    void onBrowserExtensionButtonClicked();
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
    void onRequestFailed(const QString& error_message);
    // The async built-in tool runner's pool task left the pool (attached OR detached).
    // A job detached by Stop emits nothing else, so this is what lets a cancelled-but-
    // still-executing mutation finish the stop instead of parking the run in Cancelling.
    void onAsyncToolDrained();
    /// Settles a run parked in Cancelling to Cancelled once nothing is still running.
    void settlePendingStop();
    void onBrokerStarted(const QString& command_id);
    void onBrokerStdoutChunk(const QString& command_id, const QString& chunk);
    void onBrokerStderrChunk(const QString& command_id, const QString& chunk);
    void onBrokerFinished(const QString& command_id, const sak::ai::AiCommandResult& result);

private:
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
    void createContextPane(QSplitter* splitter);
    QWidget* createConversationPane();
    void createStatusStrip(QVBoxLayout* root_layout);
    void configureExecutionBrokers();
    [[nodiscard]] bool loadWorkflowDefaults(QStringList* workflow_errors);
    // Collects (never swallows) the skill-catalog load errors so the caller can show
    // them once the panel UI exists.
    void loadSkillDefaults(QStringList* skill_errors);
    void initializeStandardPanel(bool workflows_loaded,
                                 const QStringList& workflow_errors,
                                 const QStringList& skill_errors);
    void initializePackageManager();
    void setupContextPaneSessionSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneAgentSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneWorkflowPicker(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneWorkflowDetails(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneAccessSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneCredentialSection(QVBoxLayout* layout, QWidget* pane);
    void setupContextPaneBrowserExtensionSection(QVBoxLayout* layout, QWidget* pane);
    void refreshBrowserExtensionStatus();
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
    void setupComposerContextActions(QHBoxLayout* action_row, QWidget* composer);
    void setupComposerSendActions(QHBoxLayout* action_row, QWidget* composer);
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
    void resetContextTokenCount(const QString& status);
    /// Shows the input tokens the last response consumed (no request of its own).
    void applyLastResponseContextUsage();
    [[nodiscard]] qint64 exactContextUsageTokens() const;
    [[nodiscard]] qint64 currentContextWindowTokens() const;
    [[nodiscard]] bool currentContextWindowIsDocumented() const;
    [[nodiscard]] QString contextWindowStatusText() const;
    // 0.0 when the window is undocumented or the exact count is unavailable; else
    // used/limit. Shared by the usage label and the pressure notifier so both agree.
    [[nodiscard]] double currentContextUsageRatio() const;
    // Emit a one-time transcript notice when exact context usage crosses into the
    // warning (>=80%) or critical (>=95%) band, so the user is told to compact before
    // local full-context refusal. Server-side truncation stays disabled.
    void notifyContextPressureIfCrossed();
    void setApiKeyStatus(const QString& text,
                         const char* color,
                         const QString& marker,
                         const char* marker_color);
    void updateTokenLabels();
    void refreshPromptTemplates();
    void resetPromptTemplatePicker();
    [[nodiscard]] QString workflowTemplateComboLabel(const ai::WorkflowTemplate& workflow) const;
    [[nodiscard]] QString workflowTemplateTooltip(const ai::WorkflowTemplate& workflow,
                                                  const QString& label) const;
    void populateWorkflowTemplatePicker(const QVector<ai::WorkflowTemplate>& workflows);
    void refreshContextList();
    [[nodiscard]] int contextChipMaxWidth() const;
    [[nodiscard]] int contextChipWidth(const QString& chip_text, int max_chip_width) const;
    void addContextListItem(int index, int max_chip_width);
    [[nodiscard]] ContextChipPalette contextChipPalette(ContextItem::Type type) const;
    /// Lists the sessions OFF the GUI thread (manifests only) and fills the combo when the
    /// listing lands. A request while one is in flight runs once more after it.
    void reloadSessionPicker();
    struct SessionListing {
        QVector<ai::AiSessionInfo> sessions;
        QString error;
    };
    void applySessionListing();
    void populateSessionPicker(const SessionListing& listing);
    void startNewPersistentSession(const QString& title);
    [[nodiscard]] bool ensurePersistentSession(const QString& title);
    [[nodiscard]] QString workflowTitleForChatRename(const QString& workflow_id) const;
    void autoRenameDefaultChatFromFirstPrompt(const QString& message, const QString& workflow_id);
    void loadSessionTranscript(const QString& session_id);
    // Samples every source of in-flight AI work at one instant. isAiBusy() and the Stop
    // finalization both read this single authority, so they can never disagree about
    // whether work is still running (notably the detached-but-executing async tool).
    [[nodiscard]] ai::AiPanelActivity currentAiActivity() const;
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
    [[nodiscard]] bool filterMessageEditKeyPress(QEvent* event);
    [[nodiscard]] bool handleComposerKeyAction(int action);
    [[nodiscard]] bool handleComposerHistoryKeyAction(int action);
    [[nodiscard]] ai::AiPromptAssembly buildPromptAssembly(ai::AiAccessMode mode) const;
    void applyPromptAssembly(ai::OpenAIResponseRequest& request, ai::AiAccessMode mode) const;
    [[nodiscard]] ai::OpenAIResponseRequest buildToolContinuationRequest(
        const ai::OpenAIResponseResult& result, QVector<ai::OpenAIFunctionOutput> outputs) const;
    [[nodiscard]] QString contextInstructions() const;
    [[nodiscard]] QVector<ai::OpenAIInputAttachment> buildContextAttachments() const;
    void beginToolTurn(const ai::OpenAIResponseResult& response);
    void dispatchNextToolCall();
    [[nodiscard]] bool preparePendingToolCall(const ai::OpenAIFunctionCall& call,
                                              PendingToolCallContext* context,
                                              ai::OpenAIFunctionOutput* output);
    [[nodiscard]] ai::CancellationToken makePendingCallToken(const ai::OpenAIFunctionCall& call);
    [[nodiscard]] bool dispatchBuiltInToolCall(const PendingToolCallContext& context,
                                               const QJsonObject& args,
                                               ai::OpenAIFunctionOutput* output);
    // Built-in tools whose handlers block for minutes (download/package/offline/
    // provider gateway) run their dispatch on a worker thread so the GUI thread
    // stays live; the quick tools (screenshot/session search) stay synchronous.
    [[nodiscard]] static bool isAsyncBuiltInKind(ai::AiToolCallKind kind);
    [[nodiscard]] bool startAsyncBuiltInToolCall(const PendingToolCallContext& context,
                                                 const ai::AiToolCallRequest& policy_request,
                                                 const QJsonObject& args);
    void finishAsyncBuiltInToolCall(const QJsonObject& bundle);
    /// What a tool call dispatched now runs with: the open chat (and a store bound to it), the
    /// access mode, the run's cancel route and the user's request. GUI thread; the call owns
    /// the copy, so nothing the panel does afterwards changes it.
    [[nodiscard]] ai::AiToolCallContext captureToolCallContext() const;
    /// Whether the chat a tool call was dispatched in is still the open one. GUI thread.
    [[nodiscard]] bool toolCallChatIsOpen(const QString& call_session_id) const;
    /// Ends the tool turn of a result whose chat is no longer the open one. Continuing it would
    /// send the model's next answer, and its records, into the chat now open.
    void dropToolResultForClosedSession(const QString& tool_name);
    // Teardown helpers (destructor and release()): stop any running local work, then wait out
    // the async tool worker and the workflow run (both may deref the panel from a worker
    // thread) before members are destroyed.
    void cancelRunningWorkOnDestroy();
    void stopBackgroundWorkForTeardown();
    [[nodiscard]] bool pumpUntilBackgroundWorkStops(int deadline_ms) const;
    void deleteOnceBackgroundWorkStops();
    // Pure result-shaping shared by the sync and async built-in paths (safe to
    // call off the GUI thread: no widget or shared-state access).
    [[nodiscard]] static QJsonObject buildBuiltInToolMetadata(
        QJsonObject base, const ai::AiToolDispatcher::DispatchOutcome& outcome);
    [[nodiscard]] static QString builtInToolStatus(
        const ai::AiToolDispatcher::DispatchOutcome& outcome);
    /// @brief Shapes a failed-tool-call bundle for a handler that threw. The async path
    ///        reads its result through QFutureWatcher::result(), which rethrows inside a
    ///        slot and would otherwise terminate the process. Also off-GUI-thread safe.
    [[nodiscard]] static QJsonObject buildThrownToolBundle(QJsonObject bundle,
                                                           const QJsonObject& base_metadata,
                                                           const QString& what);
    // Marshals @p func onto the GUI thread and returns its result. A no-op
    // detour when already on the GUI thread; otherwise a blocking queued call
    // (used by tool handlers that run on a worker thread but must touch widgets
    // or the approval dialogs).
    [[nodiscard]] bool isOnGuiThread() const;
    void invokeOnGuiThreadBlocking(const std::function<void()>& func) const;
    template <typename Func>
    auto runOnGuiThread(Func&& func) const {
        using Result = std::invoke_result_t<Func>;
        if (isOnGuiThread()) {
            return func();
        }
        if constexpr (std::is_void_v<Result>) {
            invokeOnGuiThreadBlocking([&]() { func(); });
        } else {
            Result result{};
            invokeOnGuiThreadBlocking([&]() { result = func(); });
            return result;
        }
    }
    [[nodiscard]] bool authorizeCommandToolCall(const PendingToolCallContext& context,
                                                const ai::AiCommandToolPlan& plan,
                                                ai::OpenAIFunctionOutput* output);
    [[nodiscard]] bool authorizeCommandForAccessMode(const PendingToolCallContext& context,
                                                     const ai::AiCommandToolPlan& plan,
                                                     ai::OpenAIFunctionOutput* output);
    [[nodiscard]] bool rejectCommandBeforeRun(const PendingToolCallContext& context,
                                              ai::OpenAIFunctionOutput* output,
                                              const QString& output_json,
                                              const QString& trace_outcome,
                                              const QString& local_event);
    [[nodiscard]] bool acquireCommandToolLease(const PendingToolCallContext& context,
                                               const ai::AiCommandToolPlan& plan,
                                               ai::OpenAIFunctionOutput* output);
    // Circuit-breaker gate for the raw command path: if the executor for this
    // command tool has been failing (hangs / launch failures), deny with a
    // health-suppressed result before running, mirroring AiToolDispatcher.
    [[nodiscard]] bool applyCommandHealthGate(const PendingToolCallContext& context,
                                              const ai::AiCommandToolPlan& plan,
                                              ai::OpenAIFunctionOutput* output);
    // Feeds the executor outcome back to the health ledger so the breaker learns.
    // Only executor health counts: a timeout or launch failure is a failure; a
    // command that ran to any exit code is a success; a user cancel is ignored.
    void recordCommandHealth(const ai::AiCommandResult& result);
    // Forget the circuit breaker's records when a chat session starts or is opened.
    void resetToolHealthForSession();
    // A new chat starts with no tool counts, loop history or tripped breakers.
    void resetToolCountersForNewChat();
    [[nodiscard]] static QString commandHealthKey(const QString& tool_name);
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
                                              bool risky_change,
                                              bool catastrophic = false);
    // The human gates as a tool call sees them: a call whose chat is no longer the open one is
    // declined, never shown a prompt or given a gate record in the chat now open.
    [[nodiscard]] bool confirmToolCallWithUser(const ai::AiToolCallContext& context,
                                               const QString& shell,
                                               const QString& preview,
                                               bool risky_change,
                                               bool catastrophic = false);
    [[nodiscard]] bool offerRestorePointForToolCall(const ai::AiToolCallContext& context,
                                                    const QString& preview,
                                                    bool risky_change,
                                                    bool catastrophic = false);
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
    [[nodiscard]] bool offerRestorePointIfNeeded(const QString& preview,
                                                 bool risky_change,
                                                 bool catastrophic = false);
    [[nodiscard]] bool consumeResumedRestoreDecision(const QString& preview, bool risky_change);
    [[nodiscard]] QJsonObject restorePointOfferMetadata(const QString& preview,
                                                        bool risky_change) const;
    [[nodiscard]] bool handleRestorePointOfferChoice(int choice,
                                                     const QString& gate_id,
                                                     QJsonObject metadata,
                                                     ai::AiRunStatus previous_status,
                                                     const QString& preview);
    [[nodiscard]] bool handleRestorePointFailure(QJsonObject metadata,
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
    [[nodiscard]] QJsonObject runScreenshotTool(const ai::AiToolCallContext& context,
                                                const QString& reason);
    // Validate a download request and resolve the file name it will be written under, returning
    // an empty name with *error_message set to why it cannot proceed.
    [[nodiscard]] static QString resolveDownloadFileName(const ai::AiToolCallContext& context,
                                                         const QUrl& url,
                                                         const QString& url_string,
                                                         const QString& filename,
                                                         QString* error_message);
    [[nodiscard]] QJsonObject runDownloadTool(const ai::AiToolCallContext& context,
                                              const QString& url_string,
                                              const QString& filename);
    [[nodiscard]] QJsonObject runWorkflowPowerShellTool(const ai::AiToolCallContext& context,
                                                        const QJsonObject& args,
                                                        const QString& command_preview);
    // Records of a call's command and its session memory write the manifest, so they go through
    // the panel's store on the GUI thread, and only while the call's chat is the open one.
    void recordToolCallCommand(const ai::AiToolCallContext& context,
                               const QString& preview,
                               const QJsonObject& result_json);
    void appendToolCallMemory(const ai::AiToolCallContext& context,
                              const QString& kind,
                              const QString& title,
                              const QString& text);
    [[nodiscard]] bool toolCallMayWriteSessionRecords(const ai::AiToolCallContext& context,
                                                      const QString& what);
    [[nodiscard]] ai::AiCommandResult executeWorkflowPowerShellRequest(
        const ai::AiToolCallContext& context,
        const ai::AiCommandRequest& request,
        const QString& command_id);
    [[nodiscard]] ai::AiCommandResult executeElevatedWorkflowPowerShellRequest(
        const ai::AiCommandRequest& request);
    [[nodiscard]] ai::AiCommandResult executeStandardWorkflowPowerShellRequest(
        const ai::AiToolCallContext& context,
        const ai::AiCommandRequest& request,
        const QString& command_id);
    void connectWorkflowPowerShellLogging(ai::ExecutionBroker* broker, QObject* worker);
    static void connectWorkflowPowerShellCancelPolling(QTimer* cancel_timer,
                                                       ai::ExecutionBroker* broker,
                                                       QObject* worker,
                                                       const std::function<bool()>& should_cancel);
    [[nodiscard]] QJsonObject runPackageManagerTool(const ai::AiToolCallContext& context,
                                                    const QJsonObject& args);
    [[nodiscard]] QJsonObject runProviderGatewayTool(const ai::AiToolCallContext& context,
                                                     const QJsonObject& args);
    /// (Re)arms the single-shot idle reap after a provider-gateway call, so a pooled MCP server
    /// (the full-access win32 MCP child included) closes once unused for kAiMcpSessionIdleReap
    /// instead of living until the app exits. GUI thread.
    void armMcpIdleReap();
    /// Runs the pool's idle reap off the GUI thread (closing can take the pool's cleanup
    /// deadline) and re-arms while sessions remain open.
    void reapIdleMcpSessions();
    /// The human gates a provider-gateway Win32 MCP call uses (confirmation, restore point);
    /// split out of runProviderGatewayTool.
    [[nodiscard]] ai::AiProviderGatewayToolCallbacks providerGatewayCallbacks(
        const ai::AiToolCallContext& context);
    [[nodiscard]] static QJsonObject runSessionSearchTool(const ai::AiToolCallContext& context,
                                                          const QJsonObject& args);
    // sak_skill handler: operation "list" returns the skill catalog, "load"
    // returns the full guidance body for a skill_id. Read-only resource lookup.
    [[nodiscard]] QJsonObject runSkillTool(const QJsonObject& args) const;
    // sak_app_action handler: "list" returns the app-action catalog (read-only);
    // "run" executes one action by id headless, applying a human gate + restore
    // point for mutating/destructive/admin actions before invoking.
    [[nodiscard]] QJsonObject runAppActionTool(const ai::AiToolCallContext& context,
                                               const QJsonObject& args);
    [[nodiscard]] QJsonObject runAppActionRun(const ai::AiToolCallContext& context,
                                              const QString& action_id,
                                              const QJsonObject& action_args);
    // Runs the action with a staging folder of its own, publishes what it wrote into the call's
    // chat and removes the folder. The folder belongs to this call, not to the panel.
    [[nodiscard]] AppActionResult invokeAppActionWithStaging(const ai::AiToolCallContext& context,
                                                             const QString& action_id,
                                                             const QJsonObject& action_args,
                                                             QString* error_message);
    static void cleanupAppActionStaging(const QString& staging_root,
                                        AppActionResult* run,
                                        QString* error_message);
    // Access-mode gate for a mutating/destructive/admin app action: returns an error
    // object to abort (blocked/declined), or std::nullopt to proceed. Read-only
    // actions and non-execution sessions are handled by the caller/policy. @p detail
    // is a short, model-supplied argument summary (e.g. the target disk) appended to
    // the confirm/restore preview so the human sees WHAT is being run, not just the
    // action title -- critical for a catastrophic op whose target matters.
    [[nodiscard]] std::optional<QJsonObject> appActionRunGate(const ai::AiToolCallContext& context,
                                                              const AppActionDescriptor& descriptor,
                                                              const QString& detail = QString());
    // Build the app-scoped AppActionService (on the GUI thread) and seed its actions
    // into m_appActionRegistry exactly once. Idempotent.
    void ensureAppActionService();
    [[nodiscard]] QJsonObject packageManagerQueryOperation(const QJsonObject& args,
                                                           const QString& operation,
                                                           const QString& query);
    [[nodiscard]] QJsonObject packageManagerPackageOperation(const ai::AiToolCallContext& context,
                                                             const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version,
                                                             int timeout_seconds);
    [[nodiscard]] QJsonObject packageManagerPackagePreflight(const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version);
    [[nodiscard]] QJsonObject authorizePackageManagerChange(const ai::AiToolCallContext& context,
                                                            const QString& preview);
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
    [[nodiscard]] QJsonObject runOfflineDownloaderTool(const ai::AiToolCallContext& context,
                                                       const QJsonObject& args);
    [[nodiscard]] QJsonObject offlineRunOperation(const ai::AiToolCallContext& context,
                                                  const QJsonObject& args,
                                                  const QString& operation);
    [[nodiscard]] QJsonObject offlinePresetsResult() const;
    [[nodiscard]] QJsonObject offlineSearchResult(const QJsonObject& args, const QString& query);
    [[nodiscard]] static QString offlineOutputDirectory(const ai::AiToolCallContext& context,
                                                        const QString& operation,
                                                        const QJsonObject& args,
                                                        QString* error_message);
    static void publishManagedOfflineArtifacts(const ai::AiToolCallContext& context,
                                               bool managed_output,
                                               const QString& worker_output_dir,
                                               const QString& output_dir,
                                               OfflineToolRunResult* run_result);
    static void cleanupManagedOfflineStaging(bool managed_output,
                                             const QString& worker_output_dir,
                                             OfflineToolRunResult* run_result);
    [[nodiscard]] bool authorizeOfflineOperation(const ai::AiToolCallContext& context,
                                                 const QString& operation,
                                                 const QJsonObject& args);
    [[nodiscard]] OfflineToolRunResult executeOfflineOperation(
        const ai::AiToolCallContext& context,
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
    void appendOfflineArtifacts(const QString& output_dir,
                                const QString& manifest_path,
                                QJsonObject* result);
    void refreshArtifactsAfterWrite(const QString& path);
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

    [[nodiscard]] ai::AiAccessMode currentAccessMode() const;
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
    // False when the resource did not reach the context (missing, unreadable, empty),
    // so the caller can refuse to announce a half-attached workflow as added.
    [[nodiscard]] bool addWorkflowResourceContext(const QString& resource_path,
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
    /// Writes one event to the session's activity log -- the AI audit stream the panel reads
    /// back. (trace.jsonl and turn_replay.jsonl were written beside it and read by nothing.)
    void appendTraceActivityRecord(const QString& run_id,
                                   const QString& kind,
                                   const QString& name,
                                   const QString& status,
                                   const QJsonObject& metadata);
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
                                         const QJsonObject& metadata = {},
                                         const QString& preferred_gate_id = {});
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
    void appendHumanGateRecord(ai::AiHumanGate gate);
    void clearPendingGateIfResolved(const ai::AiHumanGate& gate);
    void registerToolDispatcherHandlers();
    [[nodiscard]] bool ensureToolDispatcherReady();
    void registerToolAvailabilityCheckers();
    void registerDownloadFileAvailabilityChecker();
    void registerPackageManagerAvailabilityChecker();
    void registerOfflineDownloaderAvailabilityChecker();
    void registerProviderGatewayAvailabilityChecker();
    void registerSessionSearchAvailabilityChecker();
    void registerAppActionAvailabilityChecker();
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
    // Reports a read failure through @p error_message so an unreadable activity log is
    // surfaced instead of rendering as an empty one.
    [[nodiscard]] QVector<ai::AiActivityEvent> filteredRunDetailsActivities(
        QString* error_message = nullptr) const;
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
    [[nodiscard]] static ai::AiToolPolicy accessToolPolicy(ai::AiAccessMode mode);
    [[nodiscard]] const ai::WorkflowTemplate* attachedWorkflow() const;
    [[nodiscard]] bool ensureWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                            const QString& user_message,
                                            QJsonObject* input_values,
                                            const QJsonObject& initial_values = {},
                                            bool preserve_run_id = false);
    [[nodiscard]] bool collectRequiredWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                     QJsonObject* input_values);
    [[nodiscard]] bool collectClarifyingWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                       const QString& user_message,
                                                       QJsonObject* input_values);
    void resumeWorkflowInputGate(const ai::AiHumanGate& gate);
    void resumeApprovalGate(const ai::AiHumanGate& gate);
    [[nodiscard]] bool resumeWorkflowRecoveryGate(const ai::AiHumanGate& gate);
    [[nodiscard]] bool consumeAuthenticatedWorkflowResume(const ai::WorkflowTemplate& workflow,
                                                          const ai::AiHumanGate& gate,
                                                          const QJsonObject& resume_state);
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
    [[nodiscard]] std::expected<QJsonObject, QString> buildProtectedWorkflowResumeState(
        const ai::AiPhaseExecution& execution, const QString& gate_id, int next_phase_index);
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
    // @p tool_context is the one the workflow captured at launch, with this phase's token.
    [[nodiscard]] QJsonObject dispatchWorkflowToolPhase(const ai::WorkflowPhase& phase,
                                                        ai::AiToolPolicy policy,
                                                        const ai::AiWorkflowPhaseContext& context,
                                                        const ai::AiToolCallContext& tool_context);
    // Runs one acting-subagent tool call through the same policy/lease/health
    // dispatcher as workflow tool phases. Invoked on the workflow worker thread
    // via the injected AiSubagentToolExecutor (which already gated it by allowlist
    // + parsed its arguments).
    [[nodiscard]] QJsonObject dispatchSubagentToolCall(const ai::AiToolCallContext& tool_context,
                                                       ai::AiToolPolicy policy,
                                                       const ai::AiToolCallRequest& request,
                                                       const QJsonObject& arguments,
                                                       const QString& agent_id);
    // Handles the model's delegate_subagent tool: spawns one scoped acting
    // sub-agent (policy clamped to the session ceiling), runs it off the GUI
    // thread, and returns its structured result to the overseer chat model.
    [[nodiscard]] QJsonObject runDelegateSubagentTool(const ai::AiToolCallContext& context,
                                                      const QJsonObject& args);
    // Handles the model's run_workflow tool: validates the workflow id + required
    // inputs, then runs the named catalog workflow as a blocking orchestration
    // (per-phase policy/lease/human gates still apply) and returns its result. The
    // workflow-run watcher/progress UI is left untouched; per-phase human gates do
    // still share the run-status/human-gate machinery.
    [[nodiscard]] QJsonObject runRunWorkflowTool(const ai::AiToolCallContext& context,
                                                 const QJsonObject& args);
    struct WorkflowRunToolContext {
        ai::WorkflowTemplate workflow;
        bool found{false};
        QStringList missing_inputs;
        QString api_key;
        QString model;
        QString reasoning;
        ai::CancellationToken token;
        // The user's own request for this turn: the only source of the workflow's user_message.
        QString user_message;
    };
    // Takes the run token and user request from the call's @p context, and reads the panel's
    // model settings and resolves + validates the requested workflow on the GUI thread (called
    // from the run_workflow async handler).
    [[nodiscard]] WorkflowRunToolContext resolveWorkflowRunContext(
        const ai::AiToolCallContext& context,
        const QString& workflow_id,
        const QJsonObject& input_values);
    // Builds the RawDispatch that routes a sub-agent tool call back through the
    // guarded panel to the real policy/lease/health dispatcher. Shared by workflow
    // delegate phases and the delegate_subagent tool so both gate identically. Every call
    // runs with @p tool_context, the context of the workflow or tool call that spawned it.
    [[nodiscard]] static ai::AiSubagentToolExecutor::RawDispatch makeSubagentToolDispatch(
        QPointer<AiAssistantPanel> guard, ai::AiToolCallContext tool_context);
    [[nodiscard]] WorkflowToolDispatchPlan workflowToolDispatchPlan(
        const ai::AiToolCallContext& tool_context,
        const ai::WorkflowPhase& phase,
        ai::AiToolPolicy policy,
        const ai::AiWorkflowPhaseContext& context);
    void initializeWorkflowToolPlan(const ai::AiToolCallContext& tool_context,
                                    const ai::WorkflowPhase& phase,
                                    ai::AiToolPolicy policy,
                                    const ai::AiWorkflowPhaseContext& context,
                                    WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowPowerShellTool(const ai::WorkflowPhase& phase,
                                                     WorkflowToolDispatchPlan* plan);
    // cmd.exe has no literal-quoting construct, so a run_cmd phase whose command template
    // embeds a ${...} placeholder is refused here before the command is built.
    [[nodiscard]] bool prepareWorkflowCmdTool(const ai::WorkflowPhase& phase,
                                              WorkflowToolDispatchPlan* plan);
    void finalizeWorkflowToolPlan(const ai::WorkflowPhase& phase, WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowPackageTool(const ai::AiToolCallContext& tool_context,
                                                  const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan);
    [[nodiscard]] bool prepareWorkflowOfflineTool(const ai::AiToolCallContext& tool_context,
                                                  const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan);
    // Takes the plan by non-const reference so it can record the human's explicit
    // confirmation (plan.request.human_confirmed) for a catastrophic phase, which the
    // machine gate in evaluateToolPolicy then requires when dispatch re-evaluates.
    [[nodiscard]] bool authorizeWorkflowToolPhase(const ai::AiToolCallContext& tool_context,
                                                  const ai::WorkflowPhase& phase,
                                                  WorkflowToolDispatchPlan& plan);

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
        // The session's tool-policy ceiling, captured on the GUI thread at launch and passed to
        // the orchestrator so every workflow agent is clamped to it (B12-01). Defaults to the
        // most restrictive policy so an unset ceiling fails closed.
        ai::AiToolPolicy session_policy_ceiling{ai::AiToolPolicy::NoLocalExecution};
        // What every tool the workflow runs (its tool phases, its sub-agents' tools) runs with,
        // captured when the workflow was launched.
        ai::AiToolCallContext tool_context;
        PanelToolExecutor* executor{nullptr};
        // When false (the model's run_workflow tool path) the orchestrator runs
        // WITHOUT wiring the panel's workflow-run UI callbacks, so a tool-invoked
        // workflow does not disturb the user-facing workflow run-state machine.
        bool wire_callbacks{true};
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
    static void configureWorkflowOrchestrator(ai::AiOrchestrator* orchestrator,
                                              ai::AiSubagentRunner* runner,
                                              const WorkflowRunLaunch& launch);
    static void configureWorkflowRunner(ai::AiSubagentRunner* runner);
    [[nodiscard]] static ai::AiOrchestrationOptions workflowOrchestrationOptions(
        const WorkflowRunLaunch& launch);
    [[nodiscard]] static ai::SoftwareAvailabilityResolution workflowRequirementAvailable(
        const ai::WorkflowRequirement& requirement);
    [[nodiscard]] static QJsonObject runWorkflowOverseerPhase(
        const ai::AiSubagentRunner* runner,
        const WorkflowRunLaunch& launch,
        const ai::WorkflowPhase& phase,
        const ai::AiWorkflowPhaseContext& context,
        const ai::CancellationToken& token);
    [[nodiscard]] static std::expected<QJsonObject, QString> protectWorkflowResumeState(
        const QJsonObject& resume_state);
    [[nodiscard]] static std::expected<QJsonObject, QString> unprotectWorkflowResumeState(
        const QJsonObject& protected_state);
#ifdef SAK_AI_TEST_HOOKS
    // Test-only (SAK_AI_TEST_HOOKS): observe the resume-state plaintext after it is wiped.
    static void setWorkflowResumePlaintextWipeObserverForTesting(
        std::function<void(const QByteArray&)> observer);
#endif  // SAK_AI_TEST_HOOKS
    [[nodiscard]] static std::expected<QString, QString> workflowRevision(
        const ai::WorkflowTemplate& workflow);
    [[nodiscard]] static QString workflowResumeBindingError(const ai::WorkflowTemplate& workflow,
                                                            const ai::AiHumanGate& gate,
                                                            const QJsonObject& resume_state);
    [[nodiscard]] static bool consumeWorkflowResumeGeneration(const QString& session_directory,
                                                              const QString& run_id,
                                                              const QString& gate_id,
                                                              qint64 generation,
                                                              QString* error_message);
    [[nodiscard]] static std::expected<qint64, QString> nextWorkflowResumeGeneration(
        const QString& session_directory, const QString& run_id, qint64 current_generation);
    static void applyWorkflowResumeState(ai::AiOrchestrationOptions* options,
                                         const QJsonObject& resume_state);
    static void connectWorkflowOrchestratorCallbacks(ai::AiOrchestrator* orchestrator,
                                                     QPointer<AiAssistantPanel> panel_guard);

    [[nodiscard]] bool handleBusySendPrompt(const QString& message);
    /// @brief What to tell the person before a workflow starts whose local-tool steps this access
    ///        mode will refuse. Empty when nothing will be refused.
    [[nodiscard]] static QString blockedWorkflowStepsNotice(const ai::WorkflowTemplate& workflow,
                                                            ai::AiAccessMode mode);
    [[nodiscard]] WorkflowSendResult startAttachedWorkflowFromPrompt(const QString& message);
    void appendUserTurn(const QString& message,
                        const QString& workflow_id = {},
                        const QJsonObject& workflow_inputs = {});
    [[nodiscard]] ai::OpenAIResponseRequest buildChatRequest(const QString& message) const;
    // Disable server-side context truncation and set the stable per-session prompt-cache key.
    // Over-limit requests fail explicitly instead of silently dropping prior evidence.
    void applyContextWindowPolicy(ai::OpenAIResponseRequest& request) const;
    void startChatRequest(const QString& message);
    void cancelActiveRunToken();
    void cancelLocalAiWork();
    void finalizeStopRequest();
    void releaseCurrentCommandLease();
    [[nodiscard]] QJsonObject brokerResultJson(const QString& command_id,
                                               const ai::AiCommandResult& result) const;
    void recordBrokerResult(const QString& command_id, const QJsonObject& result_json);
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
    /// The mode the person last settled on, which a declined switch to Unattended goes back to.
    int m_confirmedAccessModeIndex{0};
    /// True while they are being asked about Unattended, which is not in force until they agree.
    bool m_accessModeConfirmPending{false};
    QPushButton* m_browserExtensionButton{nullptr};
    QLabel* m_browserExtensionStatusLabel{nullptr};
    // Empty in production (installer uses real Chrome-policy paths); the panel test seam
    // points it at a throwaway HKCU key + temp dir so button clicks never touch real Chrome.
    sak::win32mcp::ExtensionInstallConfig m_browserExtensionConfig;
    // Setup state the extension button was last rendered from. A click that finds a
    // different state re-renders instead of acting on a stale label.
    sak::win32mcp::ExtensionInstallState m_browserExtensionRenderedState{
        sak::win32mcp::ExtensionInstallState::Error};

    std::unique_ptr<ai::OpenAIResponsesClient> m_client;
    std::unique_ptr<ai::CredentialStore> m_credentialStore;
    // Shared so a session listing running on the thread pool keeps the store alive.
    std::shared_ptr<ai::ConversationStore> m_conversationStore;
    std::unique_ptr<ElevationBroker> m_elevationBroker;
    std::unique_ptr<ai::ExecutionBroker> m_executionBroker;
    std::unique_ptr<ai::AiHumanGateStore> m_humanGateStore;
    std::unique_ptr<ai::TokenUsageTracker> m_tokenTracker;
    std::unique_ptr<ai::TraceStore> m_traceStore;
    std::unique_ptr<ai::AiRunStateStore> m_runStateStore;
    std::unique_ptr<ai::AiLeaseManager> m_leaseManager;
    std::unique_ptr<ai::AiToolHealthLedger> m_toolHealthLedger;
    // Shared, not owned outright: the async tool work holds its own reference while it runs,
    // so a dispatch in flight never outlives the dispatcher it is executing in.
    std::shared_ptr<ai::AiToolDispatcher> m_toolDispatcher;
    std::unique_ptr<ai::AiAsyncToolRunner> m_asyncToolRunner;
    // Persistent MCP stdio sessions, reused across provider-gateway win32 MCP tool calls. An
    // in-flight gateway call holds its own reference, so destroying the panel can never destroy
    // the pool underneath that call; the last owner closes it (worker threads joined).
    std::shared_ptr<ai::AiMcpSessionPool> m_mcpSessionPool;
    QTimer* m_mcpIdleReapTimer{nullptr};
    QFutureWatcher<int>* m_mcpIdleReapWatcher{nullptr};
    // NOTE: there is deliberately no separate "async tool in flight" flag. The runner
    // itself is the single authority (isRunning()); a mirror flag went stale the moment
    // Stop detached the job and made the panel report idle while the work still ran.
    // The panel's handle on the running async built-in tool's cancel route. The call's context
    // holds the same token (shared state) and the worker reads only that; this copy is what Stop
    // and teardown cancel, even if m_runToken has moved on. GUI thread.
    ai::CancellationToken m_activeToolRunToken;
    std::unique_ptr<ai::WorkflowStore> m_workflowStore;
    std::unique_ptr<ai::SkillStore> m_skillStore;
    // Headless command of the app's own technician actions: the registry is the
    // enumerable/invokable surface the assistant's sak_app_action tool drives; the
    // service runs the seeded built-in actions on the application's one quick-action
    // controller, shared with the panels that offer the same actions.
    std::shared_ptr<QuickActionController> m_quickActions;
    AppActionRegistry m_appActionRegistry;
    std::unique_ptr<AppActionService> m_appActionService;
    std::unique_ptr<ChocolateyManager> m_chocoManager;
    std::unique_ptr<PackageListManager> m_packageListManager;
    // True while executeOfflineOperation runs its operation (on its own local worker). This
    // replaced a member worker that was constructed and never started: every busy check, the
    // tool_busy availability check and both Stop paths watched that idle worker, so they never
    // saw a live offline operation. Stop reaches the live one through m_activeToolRunToken.
    std::atomic_bool m_offline_operation_running{false};
    QVector<ContextItem> m_contextItems;
    QString m_apiKey;
    QString m_taskStatus;
    QString m_previousResponseId;
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
    qint64 m_workflowResumeGeneration{0};
    ai::CancellationToken m_runToken;
    ai::AiRunState m_runState;
    bool m_loadingSessionPicker{false};
    QFutureWatcher<SessionListing>* m_sessionListWatcher{nullptr};
    bool m_sessionListReloadQueued{false};
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
    // Set at the very start of teardown (before the background-work drain pumps the event
    // loop). A workflow worker that is blocked marshaling a per-phase approval
    // modal onto the GUI thread would otherwise have that modal shown by the drain
    // pump and block on exec() forever -- there is no user to answer during
    // teardown. When set, the modal gate methods decline immediately instead.
    // Atomic: worker threads read it before marshaling blocking work onto the GUI
    // thread, so a teardown that started on the GUI thread is visible to them.
    std::atomic<bool> m_shuttingDown{false};
    QTimer* m_activityTimer{nullptr};
    QString m_contextTokenStatus;
    qint64 m_contextInputTokens{-1};
    // Last context-pressure band reported to the transcript: 0 normal, 1 warning
    // (>=80%), 2 critical (>=95%). Prevents re-warning every telemetry refresh.
    int m_contextPressureLevel{0};
    QString m_currentCommandId;
    QString m_currentCommandLeaseId;
    QString m_currentCommandPreview;
    // Health-ledger key for the in-flight command tool, so its completion outcome
    // is recorded against the same key the pre-run health gate checked.
    QString m_currentCommandHealthKey;
    QString m_currentStdoutBuffer;
    QString m_currentStderrBuffer;
    int m_nextCommandSequence{1};
    int m_toolTurnIterations{0};
    int m_toolCallsThisSession{0};
    QHash<QString, int> m_toolNamesThisMessage;
    QHash<QString, int> m_toolFailureClassesThisMessage;
    // Breaks a stuck loop of identical (name + arguments) tool calls before the
    // flat per-message turn cap burns the whole budget. Reset per user message.
    ai::AiToolLoopDetector m_toolLoopDetector;

    static constexpr int kMaxToolTurnsPerUserMessage = 12;
    // Hard ceiling on the tool calls accepted from ONE model response: the batch is
    // model-controlled, and each synchronous call nests one dispatch frame.
    static constexpr int kMaxToolCallsPerResponse = 32;
};

}  // namespace sak
