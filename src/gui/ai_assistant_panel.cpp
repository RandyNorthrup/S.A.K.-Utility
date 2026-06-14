// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ai_assistant_panel.cpp
/// @brief UI shell for the SAK AI Assistant panel.

#include "sak/ai_assistant_panel.h"

#include "sak/ai/ai_chat_title.h"
#include "sak/ai/ai_command_guard.h"
#include "sak/ai/ai_command_tool_planner.h"
#include "sak/ai/ai_conversation_store.h"
#include "sak/ai/ai_credential_store.h"
#include "sak/ai/ai_execution_broker.h"
#include "sak/ai/ai_human_gate_store.h"
#include "sak/ai/ai_lease_manager.h"
#include "sak/ai/ai_offline_downloader_tool_runner.h"
#include "sak/ai/ai_openai_model_client.h"
#include "sak/ai/ai_orchestrator.h"
#include "sak/ai/ai_package_selection.h"
#include "sak/ai/ai_package_tool_planner.h"
#include "sak/ai/ai_prompt_assembler.h"
#include "sak/ai/ai_provider_gateway.h"
#include "sak/ai/ai_provider_gateway_tool_runner.h"
#include "sak/ai/ai_run_state_store.h"
#include "sak/ai/ai_subagent_runner.h"
#include "sak/ai/ai_token_usage_tracker.h"
#include "sak/ai/ai_tool_dispatcher.h"
#include "sak/ai/ai_tool_health_ledger.h"
#include "sak/ai/ai_tool_policy.h"
#include "sak/ai/ai_tool_result_recorder.h"
#include "sak/ai/ai_trace_store.h"
#include "sak/ai/ai_workflow_clarifier.h"
#include "sak/ai/ai_workflow_powershell_tool_runner.h"
#include "sak/ai/ai_workflow_store.h"
#include "sak/ai/openai_responses_client.h"
#include "sak/ai_transcript_view.h"
#include "sak/app_paths.h"
#include "sak/chocolatey_manager.h"
#include "sak/detachable_log_window.h"
#include "sak/elevation_broker.h"
#include "sak/error_codes.h"
#include "sak/format_utils.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/offline_deployment_constants.h"
#include "sak/offline_deployment_worker.h"
#include "sak/package_list_manager.h"
#include "sak/report_style_constants.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontMetrics>
#include <QFrame>
#include <QFuture>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QIcon>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QLocale>
#include <QMessageBox>
#include <QMimeDatabase>
#include <QMimeType>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSemaphore>
#include <QSet>
#include <QSignalBlocker>
#include <QSize>
#include <QSizePolicy>
#include <QSplitter>
#include <QtConcurrent/QtConcurrentRun>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVariantMap>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <atomic>
#include <functional>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace sak {

namespace {

constexpr int kDefaultCommandTimeoutSec = 120;
constexpr int kDefaultOutputCapKb = 256;
constexpr qint64 kContextMaxFileBytes = 50LL * sak::kBytesPerMB;
constexpr qint64 kContextTextPreviewBytes = 512LL * sak::kBytesPerKB;
constexpr const char* kWheelRequiresFocusProperty = "sakWheelRequiresFocus";
constexpr int kApprovalDialogWidth = 760;
constexpr int kApprovalDialogHeight = 520;
constexpr int kApprovalDialogMinWidth = 620;
constexpr int kApprovalDialogMinHeight = 420;
constexpr int kApprovalCommandMinHeight = 170;
constexpr int kApprovalCommandCollapsedMaxHeight = 260;
constexpr int kApprovalCommandExpandedMaxHeight = 520;
constexpr int kApprovalExpandedDialogMinWidth = 860;
constexpr int kApprovalExpandedDialogMinHeight = 680;
constexpr int kReadableComboMinHeight = sak::ui::kUiButtonSizeInline;
constexpr int kContextSplitterConversationWidth = 860;
constexpr int kContextSplitterRailWidth = 280;
constexpr int kContextPaneMinWidth = 320;
constexpr int kContextPaneMaxWidth = 440;
constexpr int kWorkflowDetailsMinHeight = 140;
constexpr int kWorkflowDetailsMaxHeight = 320;
constexpr int kConnectionStatusIconWidth = sak::ui::kUiIconSmall;
constexpr int kContextScrollMinWidth = 340;
constexpr int kContextScrollMaxWidth = 460;
constexpr int kWorkflowProgressMinWidth = 160;
constexpr int kWorkflowProgressMaxWidth = 520;
constexpr int kAgentActivityLabelMinWidth = 96;
constexpr int kContextWindowLabelMinWidth = 154;
constexpr int kMessageEditMinHeight = 92;
constexpr int kMessageEditMaxHeight = 160;
constexpr int kContextListMinHeight = 42;
constexpr int kContextListMaxHeight = 122;
constexpr int kContextChipViewportReserve = sak::ui::kUiButtonSizeMicro;
constexpr int kContextChipMinAvailableWidth = 220;
constexpr int kContextChipMinWidth = 180;
constexpr int kContextChipTargetMinWidth = 260;
constexpr int kContextChipTargetMaxWidth = 460;
constexpr int kContextChipTextReserve = 44;
constexpr int kContextChipMetricPadding = 48;
constexpr int kContextChipHeight = 26;
constexpr int kContextChipRowHeight = sak::ui::kUiButtonSizeInline;
constexpr int kContextChipTextMinWidth = 80;
constexpr int kRunDetailsDialogWidth = 880;
constexpr int kRunDetailsDialogHeight = 700;
constexpr int kFontWeightBold = 700;
constexpr int kFontWeightExtraBold = 800;
constexpr int kDefaultCleanReportMaxChars = 700;
constexpr int kReportNormalizedKeyMaxChars = 160;
constexpr int kReportFindingSeverityMaxChars = 80;
constexpr int kReportFindingDetailMaxChars = 500;
constexpr int kReportChatNameMaxChars = 240;
constexpr int kReportPathMaxChars = 500;
constexpr int kReportSummaryMaxChars = 420;
constexpr int kReportWarningMaxChars = 500;
constexpr int kReportTranscriptExcerptLimit = 25;
constexpr int kReportTranscriptExcerptMaxChars = 900;
constexpr int kReportHtmlSessionMaxChars = 80;
constexpr int kReportHtmlSeverityMaxChars = 80;
constexpr int kReportHtmlFindingTitleMaxChars = 300;
constexpr int kReportHtmlFindingDetailMaxChars = 700;
constexpr int kReportHtmlArtifactLabelMaxChars = 260;
constexpr int kReportHtmlPhaseIdMaxChars = 120;
constexpr int kReportHtmlStatusMaxChars = 80;
constexpr int kReportHtmlDurationMaxChars = 40;
constexpr int kReportHtmlPhaseSummaryMaxChars = 500;
constexpr int kReportHtmlTimestampMaxChars = 80;
constexpr int kReportHtmlActivityKindMaxChars = 80;
constexpr int kReportHtmlActivitySummaryMaxChars = 600;
constexpr int kReportHtmlPathMaxChars = 700;
constexpr int kReportHtmlSummaryItemLimit = 3;
constexpr int kReportHtmlEvidenceItemLimit = 18;
constexpr int kReportHtmlNextStepItemLimit = 10;
constexpr int kReportHtmlRiskItemLimit = 10;
constexpr int kReportHtmlActionItemLimit = 10;
constexpr int kReportHtmlEvidenceNoteItemLimit = 8;
constexpr int kReportHtmlWarningItemLimit = 3;
constexpr int kReportHtmlFindingLimit = 12;
constexpr int kReportHtmlTimelineActivityLimit = 18;
constexpr int kReadableDurationMillisecondsPerSecond = 1000;
constexpr double kReadableDurationMillisecondsPerSecondF = 1000.0;
constexpr int kReadableComboDefaultContentsLength = 24;
constexpr int kReadableComboDefaultPopupMinWidth = 420;
constexpr int kReadableComboMaxVisibleItems = 18;
constexpr int kToolHealthLedgerRetentionHours = 24;
constexpr int kSessionComboContentsLength = 26;
constexpr int kSessionComboPopupMinWidth = 440;
constexpr int kReasoningComboContentsLength = 14;
constexpr int kReasoningComboPopupMinWidth = 260;
constexpr int kWorkflowComboContentsLength = 32;
constexpr int kWorkflowComboPopupMinWidth = 620;
constexpr ushort kStatusMarkerSuccess = 0x2714;
constexpr ushort kStatusMarkerError = 0x2718;
constexpr int kActivityTimerIntervalMs = 450;
constexpr int kCommandIdWidth = 3;
constexpr int kCommandIdBase = 10;
constexpr int kWorkflowInferenceWordMinChars = 4;
constexpr qsizetype kSafetyIdentifierDigestChars = 32;
constexpr int kFieldLabelFontWeight = 400;
constexpr qint64 kTokenCompactMillion = 1'000'000;
constexpr qint64 kTokenCompactThousand = 1000;
constexpr double kContextUsageErrorRatio = 0.95;
constexpr double kContextUsageWarningRatio = 0.80;
constexpr int kArtifactCountDisplayLimit = 999;
constexpr int kContextChipColumns = 2;
constexpr int kContextItemTooltipMaxChars = 400;
constexpr int kWorkflowWorkerCancelPollMs = 150;
constexpr int kWorkflowRunnerMaxRetries = 1;
constexpr int kWorkflowWallClockTimeoutMs = 300'000;
constexpr int kWorkflowMaxParallelSubagents = 3;
constexpr int kContextTokenDebounceMs = 700;
constexpr qint64 kDefaultContextWindowTokens = 128'000;
constexpr qint64 kGptFiveFrontierContextWindowTokens = 1'050'000;
constexpr qint64 kGptFourOneContextWindowTokens = 1'047'576;
constexpr qint64 kGptFiveContextWindowTokens = 400'000;
constexpr qint64 kReasoningContextWindowTokens = 200'000;
constexpr qint64 kLegacyGptFourContextWindowTokens = 8192;
constexpr int kRestorePointTimeoutSec = 180;
constexpr int kRestorePointMaxOutputBytes = 32 * sak::kBytesPerKB;
constexpr int kRestorePointSuccessExitCode = 0;
constexpr int kRestorePointErrorPreviewChars = 1000;
constexpr int kNoisyLogLineMaxChars = 6000;
constexpr ushort kUnicodeReplacementCharacter = 0xfffd;
constexpr ushort kAsciiControlCharacterLimit = 0x20;
constexpr int kNoisyLogMinCheckedChars = 40;
constexpr int kNoisyLogMinSuspiciousChars = 8;
constexpr int kNoisyLogSuspiciousDivisor = 12;
constexpr int kDefaultPreviewMaxChars = 500;
constexpr int kActivitySummaryMaxChars = 500;
constexpr int kWorkflowCatalogReserveBase = 4;
constexpr int kWorkflowCatalogReservePerWorkflow = 3;
constexpr int kReportRiskLineLimit = 12;
constexpr int kReportActionLineLimit = 14;
constexpr int kReportEvidenceSnapshotLineLimit = 24;
constexpr int kReportArtifactMarkdownLabelMaxChars = 220;
constexpr int kReportDefaultHtmlListItemLimit = 12;
constexpr int kInlineToolListLimit = 5;
constexpr int kSessionMemoryMaxChars = 12'000;
constexpr int kTraceTokenIdChars = 12;
constexpr int kShortTraceTokenIdChars = 8;
constexpr int kMetadataPreviewMaxChars = 1000;
constexpr int kPackageResultOutputMaxChars = 8000;
constexpr int kPackageFileResultLimit = 200;
constexpr int kPackageArtifactDisplayLimit = 20;
constexpr int kPackageLogLineResultLimit = 80;
constexpr int kToolOutputMaxBytes = 4 * static_cast<int>(sak::kBytesPerMB);
constexpr int kAccessModeUnattendedComboIndex = 2;
constexpr int kWorkflowPhaseStatusMessageMs = 2500;
constexpr int kRunStateMessageMaxChars = 200;
constexpr int kHumanApprovalInfoMaxChars = 600;
constexpr int kCommandPreviewMaxChars = 500;
constexpr int kCommandResultPreviewMaxChars = 1200;
constexpr int kOpenAiResponsePreviewMaxChars = 3000;
constexpr int kSubagentTranscriptFindingLimit = 4;
constexpr int kSubagentTranscriptActionLimit = 4;
constexpr int kSubagentTranscriptNextStepLimit = 3;
constexpr int kSubagentPhaseFindingLimit = 6;
constexpr int kSubagentSummarySectionLimit = 8;
constexpr int kProgressCompletedStepWeight = 2;

QString statusLabelStyle(const char* color) {
    return sak::ui::fontWeightAndColorStyle(sak::ui::kFontWeightSemibold, color);
}

bool containsAny(const QString& value, std::initializer_list<const char*> needles) {
    for (const char* needle : needles) {
        if (value.contains(QString::fromLatin1(needle))) {
            return true;
        }
    }
    return false;
}

void appendUniqueProfile(QStringList* profiles, const QString& profile) {
    const QString clean = profile.trimmed();
    if (clean.isEmpty()) {
        return;
    }
    for (const auto& existing : std::as_const(*profiles)) {
        if (existing.compare(clean, Qt::CaseInsensitive) == 0) {
            return;
        }
    }
    profiles->append(clean);
}

constexpr const char* kDefaultSessionRole = "PC Technician";
constexpr const char* kSessionRoleSourcePending = "pending";
constexpr const char* kSessionRoleSourceDefault = "default";
constexpr const char* kSessionRoleSourcePrompt = "prompt";
constexpr const char* kSessionRoleSourceWorkflow = "workflow";
constexpr const char* kSessionRoleSourceWorkflowSelection = "workflow_selection";
constexpr const char* kSessionRoleSourceUser = "user";

QStringList defaultAgentProfiles() {
    return {QStringLiteral("PC Technician")};
}

QStringList agentProfilesForWorkflowStore(const ai::WorkflowStore* store) {
    QStringList profiles;
    if (!store) {
        return defaultAgentProfiles();
    }
    const QStringList roles = store->roles();
    for (const auto& role : roles) {
        appendUniqueProfile(&profiles, role);
    }
    if (profiles.isEmpty()) {
        profiles = defaultAgentProfiles();
    }
    return profiles;
}

QString normalizedRolePromptText(QString text) {
    text = text.toLower();
    text.replace(QRegularExpression(QStringLiteral("[^a-z0-9]+")), QStringLiteral(" "));
    return text.simplified();
}

bool roleDirectivePresent(const QString& normalized) {
    return containsAny(normalized,
                       {"act as",
                        "assume",
                        "switch to",
                        "be a",
                        "be an",
                        "become",
                        "serve as",
                        "work as",
                        "from now on",
                        "role",
                        "profile",
                        "persona"});
}

void appendRoleAlias(QVector<QPair<QString, QString>>* aliases,
                     const QStringList& available_roles,
                     const QString& role,
                     const QString& alias) {
    if (!aliases || alias.trimmed().isEmpty()) {
        return;
    }
    const auto role_it = std::find_if(available_roles.cbegin(),
                                      available_roles.cend(),
                                      [&](const QString& available) {
                                          return available.compare(role, Qt::CaseInsensitive) == 0;
                                      });
    if (role_it == available_roles.cend()) {
        return;
    }
    aliases->append({*role_it, normalizedRolePromptText(alias)});
}

QVector<QPair<QString, QStringList>> roleAliasGroups() {
    return {
        {QStringLiteral("PC Technician"),
         {QStringLiteral("it technician"),
          QStringLiteral("desktop support"),
          QStringLiteral("help desk")}},
        {QStringLiteral("Diagnostic Technician"),
         {QStringLiteral("health check technician"), QStringLiteral("system diagnostic")}},
        {QStringLiteral("Storage Diagnostic Technician"),
         {QStringLiteral("drive technician"),
          QStringLiteral("storage technician"),
          QStringLiteral("disk health")}},
        {QStringLiteral("Driver and Device Technician"),
         {QStringLiteral("hardware technician"),
          QStringLiteral("driver technician"),
          QStringLiteral("device technician")}},
        {QStringLiteral("Audio Device Technician"), {QStringLiteral("audio technician")}},
        {QStringLiteral("Printer Technician"), {QStringLiteral("printer technician")}},
        {QStringLiteral("Battery Health Technician"), {QStringLiteral("battery technician")}},
        {QStringLiteral("Browser Support Technician"), {QStringLiteral("browser technician")}},
        {QStringLiteral("Performance Technician"), {QStringLiteral("performance technician")}},
        {QStringLiteral("Windows Repair Technician"),
         {QStringLiteral("windows technician"),
          QStringLiteral("windows repair"),
          QStringLiteral("windows repair technician"),
          QStringLiteral("network technician"),
          QStringLiteral("repair technician")}},
        {QStringLiteral("System Cleanup Technician"),
         {QStringLiteral("cleanup technician"), QStringLiteral("system optimizer")}},
        {QStringLiteral("Software Deployment Technician"),
         {QStringLiteral("software deployment"), QStringLiteral("installer")}},
        {QStringLiteral("Security Technician"),
         {QStringLiteral("security analyst"),
          QStringLiteral("malware analyst"),
          QStringLiteral("incident responder")}},
        {QStringLiteral("Research Assistant"),
         {QStringLiteral("researcher"), QStringLiteral("web researcher")}},
        {QStringLiteral("Customer Report Writer"),
         {QStringLiteral("service writer"),
          QStringLiteral("documentation specialist"),
          QStringLiteral("handoff writer"),
          QStringLiteral("report writer")}},
    };
}

QVector<QPair<QString, QString>> roleAliases(const ai::WorkflowStore* store) {
    const QStringList roles = agentProfilesForWorkflowStore(store);
    QVector<QPair<QString, QString>> aliases;
    for (const auto& role : roles) {
        appendRoleAlias(&aliases, roles, role, role);
    }
    for (const auto& group : roleAliasGroups()) {
        for (const auto& alias : group.second) {
            appendRoleAlias(&aliases, roles, group.first, alias);
        }
    }
    std::sort(aliases.begin(), aliases.end(), [](const auto& left, const auto& right) {
        return left.second.size() > right.second.size();
    });
    return aliases;
}

QString explicitRoleFromPrompt(const QString& message, const ai::WorkflowStore* store) {
    const QString normalized = normalizedRolePromptText(message);
    if (!roleDirectivePresent(normalized)) {
        return {};
    }
    for (const auto& alias : roleAliases(store)) {
        if (normalized.contains(alias.second)) {
            return alias.first;
        }
    }
    return {};
}

int promptKeywordScore(const QString& normalized, const QStringList& needles) {
    int score = 0;
    for (const auto& needle : needles) {
        const QString term = normalizedRolePromptText(needle);
        if (normalized.contains(term)) {
            score += std::max(
                1, static_cast<int>(term.split(QLatin1Char(' '), Qt::SkipEmptyParts).size()));
        }
    }
    return score;
}

QVector<QPair<QString, QStringList>> roleKeywordGroups() {
    return {
        {QStringLiteral("Customer Report Writer"),
         {QStringLiteral("report"),
          QStringLiteral("handoff"),
          QStringLiteral("write up"),
          QStringLiteral("customer ready")}},
        {QStringLiteral("Research Assistant"),
         {QStringLiteral("research"),
          QStringLiteral("look up"),
          QStringLiteral("latest"),
          QStringLiteral("documentation"),
          QStringLiteral("compare"),
          QStringLiteral("advisory")}},
        {QStringLiteral("Security Technician"),
         {QStringLiteral("malware"),
          QStringLiteral("virus"),
          QStringLiteral("ransomware"),
          QStringLiteral("infected"),
          QStringLiteral("suspicious"),
          QStringLiteral("defender"),
          QStringLiteral("antivirus"),
          QStringLiteral("threat"),
          QStringLiteral("quarantine"),
          QStringLiteral("vulnerability")}},
        {QStringLiteral("Software Deployment Technician"),
         {QStringLiteral("install"),
          QStringLiteral("uninstall"),
          QStringLiteral("upgrade"),
          QStringLiteral("package"),
          QStringLiteral("offline installer"),
          QStringLiteral("deployment bundle"),
          QStringLiteral("chocolatey"),
          QStringLiteral("winget")}},
        {QStringLiteral("System Cleanup Technician"),
         {QStringLiteral("cleanup"),
          QStringLiteral("clean up"),
          QStringLiteral("optimize"),
          QStringLiteral("disk space"),
          QStringLiteral("storage full"),
          QStringLiteral("bloatware"),
          QStringLiteral("adware"),
          QStringLiteral("temporary files"),
          QStringLiteral("startup clutter")}},
        {QStringLiteral("Windows Repair Technician"),
         {QStringLiteral("windows update"),
          QStringLiteral("blue screen"),
          QStringLiteral("bsod"),
          QStringLiteral("network"),
          QStringLiteral("wifi"),
          QStringLiteral("dns"),
          QStringLiteral("time sync"),
          QStringLiteral("search index"),
          QStringLiteral("profile"),
          QStringLiteral("login"),
          QStringLiteral("sfc"),
          QStringLiteral("dism"),
          QStringLiteral("service"),
          QStringLiteral("registry"),
          QStringLiteral("repair windows")}},
        {QStringLiteral("Diagnostic Technician"),
         {QStringLiteral("health check"),
          QStringLiteral("diagnose pc"),
          QStringLiteral("full diagnostic")}},
        {QStringLiteral("Storage Diagnostic Technician"),
         {QStringLiteral("hard drive"),
          QStringLiteral("disk health"),
          QStringLiteral("smart"),
          QStringLiteral("ssd"),
          QStringLiteral("storage")}},
        {QStringLiteral("Driver and Device Technician"),
         {QStringLiteral("driver"),
          QStringLiteral("device"),
          QStringLiteral("hardware"),
          QStringLiteral("pnp"),
          QStringLiteral("usb")}},
        {QStringLiteral("Audio Device Technician"),
         {QStringLiteral("audio"),
          QStringLiteral("sound"),
          QStringLiteral("microphone"),
          QStringLiteral("speaker")}},
        {QStringLiteral("Printer Technician"),
         {QStringLiteral("printer"), QStringLiteral("print spooler"), QStringLiteral("printing")}},
        {QStringLiteral("Battery Health Technician"),
         {QStringLiteral("battery"), QStringLiteral("laptop battery"), QStringLiteral("powercfg")}},
        {QStringLiteral("Browser Support Technician"),
         {QStringLiteral("browser"),
          QStringLiteral("chrome"),
          QStringLiteral("edge"),
          QStringLiteral("firefox"),
          QStringLiteral("proxy")}},
        {QStringLiteral("Performance Technician"),
         {QStringLiteral("performance"),
          QStringLiteral("startup"),
          QStringLiteral("slow boot"),
          QStringLiteral("slow pc")}},
        {QStringLiteral("PC Technician"),
         {QStringLiteral("general pc"),
          QStringLiteral("pc technician"),
          QStringLiteral("technician task")}},
    };
}

QString bestRoleFromScores(const QHash<QString, int>& scores) {
    QString best_role;
    int best_score = 0;
    for (auto it = scores.cbegin(); it != scores.cend(); ++it) {
        if (it.value() > best_score) {
            best_score = it.value();
            best_role = it.key();
        }
    }
    return best_score > 0 ? best_role : QString{};
}

QString inferredRoleFromPrompt(const QString& message, const ai::WorkflowStore* store) {
    const QString normalized = normalizedRolePromptText(message);
    if (normalized.isEmpty()) {
        return QString::fromLatin1(kDefaultSessionRole);
    }

    QHash<QString, int> scores;
    for (const auto& group : roleKeywordGroups()) {
        scores[group.first] += promptKeywordScore(normalized, group.second);
    }

    if (store) {
        for (const auto& workflow : store->workflows()) {
            const QString role = workflow.role.trimmed();
            if (role.isEmpty()) {
                continue;
            }
            const QString corpus = normalizedRolePromptText(
                QStringLiteral("%1 %2 %3 %4")
                    .arg(workflow.id, workflow.title, workflow.category, workflow.description));
            int workflow_score = 0;
            for (const auto& word : corpus.split(QLatin1Char(' '), Qt::SkipEmptyParts)) {
                if (word.size() >= kWorkflowInferenceWordMinChars && normalized.contains(word)) {
                    ++workflow_score;
                }
            }
            scores[role] += workflow_score;
        }
    }

    const QString scored = bestRoleFromScores(scores);
    return scored.isEmpty() ? QString::fromLatin1(kDefaultSessionRole) : scored;
}

QString safetyIdentifierFromSeed(const QString& seed) {
    const QString clean = seed.trimmed();
    if (clean.isEmpty()) {
        return {};
    }
    const QByteArray digest =
        QCryptographicHash::hash(clean.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("sak-session-%1")
        .arg(QString::fromLatin1(digest.left(kSafetyIdentifierDigestChars)));
}

struct ModelContextWindow {
    qint64 tokens{kDefaultContextWindowTokens};
    bool documented{false};
};

struct ModelContextRule {
    QStringList starts_with;
    QStringList contains_all;
    QStringList starts_with_excluded;
    qint64 tokens{kDefaultContextWindowTokens};
};

bool modelStartsWithAny(const QString& model, const QStringList& prefixes) {
    return std::any_of(prefixes.cbegin(), prefixes.cend(), [&](const QString& prefix) {
        return model.startsWith(prefix);
    });
}

bool modelContainsAll(const QString& model, const QStringList& needles) {
    return std::all_of(needles.cbegin(), needles.cend(), [&](const QString& needle) {
        return model.contains(needle);
    });
}

bool modelMatchesRule(const QString& model, const ModelContextRule& rule) {
    const bool starts_ok = rule.starts_with.isEmpty() ||
                           modelStartsWithAny(model, rule.starts_with);
    return starts_ok && modelContainsAll(model, rule.contains_all) &&
           !modelStartsWithAny(model, rule.starts_with_excluded);
}

ModelContextWindow modelContextWindowInfo(const QString& model_id) {
    const QString model = model_id.trimmed().toLower();
    if (model.isEmpty()) {
        return {};
    }
    const QVector<ModelContextRule> rules{
        {{QStringLiteral("gpt-5.5"), QStringLiteral("gpt-5.4")},
         {},
         {QStringLiteral("gpt-5.4-mini"), QStringLiteral("gpt-5.4-nano")},
         kGptFiveFrontierContextWindowTokens},
        {{}, {QStringLiteral("gpt-5"), QStringLiteral("chat")}, {}, kGptFiveContextWindowTokens},
        {{QStringLiteral("gpt-5")}, {}, {}, kGptFiveContextWindowTokens},
        {{QStringLiteral("gpt-4.1")}, {}, {}, kGptFourOneContextWindowTokens},
        {{QStringLiteral("o3"), QStringLiteral("o4")}, {}, {}, kReasoningContextWindowTokens},
        {{}, {QStringLiteral("deep-research")}, {}, kReasoningContextWindowTokens},
        {{QStringLiteral("gpt-4o"), QStringLiteral("gpt-4-turbo")},
         {},
         {},
         kDefaultContextWindowTokens},
        {{QStringLiteral("gpt-4")}, {}, {}, kLegacyGptFourContextWindowTokens},
    };
    for (const auto& rule : rules) {
        if (modelMatchesRule(model, rule)) {
            return {rule.tokens, true};
        }
    }
    return {};
}

qint64 modelContextWindowTokens(const QString& model_id) {
    return modelContextWindowInfo(model_id).tokens;
}

bool modelContextWindowIsDocumented(const QString& model_id) {
    return modelContextWindowInfo(model_id).documented;
}

enum class ComposerKeyAction {
    None,
    InsertNewline,
    Send,
    PreviousHistory,
    NextHistory
};

ComposerKeyAction composerKeyAction(const QKeyEvent& event) {
    const bool is_return = event.key() == Qt::Key_Return || event.key() == Qt::Key_Enter;
    if (is_return && event.modifiers().testFlag(Qt::ControlModifier)) {
        return ComposerKeyAction::InsertNewline;
    }
    if (is_return && event.modifiers() == Qt::NoModifier) {
        return ComposerKeyAction::Send;
    }
    if (event.modifiers() == Qt::NoModifier && event.key() == Qt::Key_Up) {
        return ComposerKeyAction::PreviousHistory;
    }
    if (event.modifiers() == Qt::NoModifier && event.key() == Qt::Key_Down) {
        return ComposerKeyAction::NextHistory;
    }
    return ComposerKeyAction::None;
}

QLabel* makeFieldLabel(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(sak::ui::transparentTextStyle(
        sak::ui::kFontSizeSmall, kFieldLabelFontWeight, sak::ui::kColorTextMuted));
    label->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    return label;
}

QString compactTokenCount(qint64 value) {
    const QLocale locale;
    if (value >= kTokenCompactMillion) {
        return QStringLiteral("%1M").arg(
            locale.toString(static_cast<double>(value) / kTokenCompactMillion, 'f', 1));
    }
    if (value >= kTokenCompactThousand) {
        return QStringLiteral("%1k").arg(
            locale.toString(static_cast<double>(value) / kTokenCompactThousand, 'f', 0));
    }
    return locale.toString(value);
}

const char* contextUsageStatusColor(double ratio) {
    if (ratio >= kContextUsageErrorRatio) {
        return sak::ui::kStatusColorError;
    }
    if (ratio >= kContextUsageWarningRatio) {
        return sak::ui::kStatusColorWarning;
    }
    return sak::ui::kColorTextMuted;
}

bool looksLikeNoisyBinaryLogLine(const QString& line) {
    if (line.size() > kNoisyLogLineMaxChars) {
        return true;
    }
    int suspicious = 0;
    int checked = 0;
    const qsizetype limit = std::min(line.size(), qsizetype{800});
    for (qsizetype i = 0; i < limit; ++i) {
        const QChar ch = line.at(i);
        const ushort code = ch.unicode();
        if (code == kUnicodeReplacementCharacter ||
            (code < kAsciiControlCharacterLimit && ch != QLatin1Char('\t'))) {
            ++suspicious;
        }
        ++checked;
    }
    return checked >= kNoisyLogMinCheckedChars &&
           suspicious >= qMax(kNoisyLogMinSuspiciousChars, checked / kNoisyLogSuspiciousDivisor);
}

QString trimmedLogLine(QString line) {
    line = line.trimmed();
    constexpr int kMaxLiveLogLineChars = 1400;
    if (line.size() > kMaxLiveLogLineChars) {
        line = line.left(kMaxLiveLogLineChars).trimmed() + QStringLiteral(" ...[line truncated]");
    }
    return line;
}

QString activityStateFromStatus(const QString& status) {
    const QString normalized_status = status.trimmed().toLower();
    if (containsAny(normalized_status, {"cancel", "reject", "declin"})) {
        return QStringLiteral("cancelled");
    }
    if (containsAny(normalized_status, {"waiting_for_human", "human"})) {
        return QStringLiteral("waiting_human");
    }
    if (containsAny(normalized_status, {"fail", "error", "denied", "missing"})) {
        return QStringLiteral("failed");
    }
    if (containsAny(normalized_status,
                    {"complete", "success", "succeed", "approv", "skip", "accept"})) {
        return QStringLiteral("complete");
    }
    if (normalized_status.contains(QStringLiteral("queue"))) {
        return QStringLiteral("queued");
    }
    return {};
}

QString activityStateFromKind(const QString& kind) {
    const QString normalized_kind = kind.trimmed().toLower();
    if (normalized_kind.contains(QStringLiteral("approval"))) {
        return QStringLiteral("waiting_human");
    }
    if (normalized_kind.contains(QStringLiteral("tool"))) {
        return QStringLiteral("running_tool");
    }
    if (normalized_kind.contains(QStringLiteral("model"))) {
        return QStringLiteral("running_model");
    }
    if (normalized_kind.contains(QStringLiteral("report"))) {
        return QStringLiteral("reporting");
    }
    if (normalized_kind.contains(QStringLiteral("cleanup"))) {
        return QStringLiteral("cleaning");
    }
    return QStringLiteral("briefing");
}

QString activityStateFromTrace(const QString& kind, const QString& status) {
    const QString state = activityStateFromStatus(status);
    return state.isEmpty() ? activityStateFromKind(kind) : state;
}

QString activitySummaryFromTrace(const QString& name,
                                 const QString& status,
                                 const QJsonObject& metadata) {
    const QString explicit_summary = metadata.value(QStringLiteral("summary")).toString().trimmed();
    if (!explicit_summary.isEmpty()) {
        return explicit_summary.left(kActivitySummaryMaxChars);
    }
    const QString title = metadata.value(QStringLiteral("title")).toString().trimmed();
    if (!title.isEmpty()) {
        return QStringLiteral("%1: %2").arg(status, title).left(kActivitySummaryMaxChars);
    }
    return QStringLiteral("%1: %2").arg(status, name).left(kActivitySummaryMaxChars);
}

QString runTimelineStateColor(const QString& state) {
    const QString value = state.trimmed().toLower();
    if (containsAny(value, {"cancel", "fail", "error", "denied"})) {
        return QString::fromLatin1(sak::ui::kStatusColorError);
    }
    if (containsAny(value, {"wait", "queue", "recover"})) {
        return QString::fromLatin1(sak::ui::kStatusColorWarning);
    }
    if (containsAny(value, {"complete", "success", "approved"})) {
        return QString::fromLatin1(sak::ui::kStatusColorSuccess);
    }
    if (containsAny(value, {"running", "brief", "think", "tool", "model"})) {
        return QString::fromLatin1(sak::ui::kStatusColorRunning);
    }
    return QString::fromLatin1(sak::ui::kStatusColorIdle);
}

bool isReportableActivityState(const QString& state) {
    const QString normalized = state.trimmed().toLower();
    return normalized.contains(QStringLiteral("complete")) ||
           normalized.contains(QStringLiteral("success"));
}

bool activityHasReportablePayload(const ai::AiActivityEvent& activity) {
    const QJsonObject metadata = activity.metadata;
    if (activity.kind == QLatin1String("tool_call")) {
        return metadata.contains(QStringLiteral("exit_code"));
    }
    if (activity.kind != QLatin1String("workflow_phase")) {
        return false;
    }
    return metadata.contains(QStringLiteral("tool_result")) ||
           metadata.contains(QStringLiteral("subagent_result")) ||
           metadata.contains(QStringLiteral("result"));
}

bool isWorkflowDelegatePhaseType(const QString& phase_type) {
    return phase_type.compare(QStringLiteral("delegate"), Qt::CaseInsensitive) == 0;
}

bool isWorkflowToolPhaseType(const QString& phase_type) {
    return phase_type.compare(QStringLiteral("tool_action"), Qt::CaseInsensitive) == 0 ||
           phase_type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0;
}

QString workflowPhaseOwnerLabel(const ai::WorkflowPhase& phase) {
    if (!phase.agent.trimmed().isEmpty()) {
        return phase.agent.trimmed();
    }
    if (!phase.tool.trimmed().isEmpty()) {
        return phase.tool.trimmed();
    }
    return phase.type.trimmed();
}

QString runTimelineBadge(const QString& text, const QString& color) {
    if (text.trimmed().isEmpty()) {
        return {};
    }
    return QString::fromLatin1(sak::ui::kHtmlTimelineBadge)
        .arg(sak::ui::kHtmlTimelineBadgeBorderPx)
        .arg(color)
        .arg(sak::ui::htmlColor(sak::ui::kColorBgSurface))
        .arg(sak::ui::kHtmlTimelineBadgeRadiusPx)
        .arg(sak::ui::kHtmlTimelineBadgePaddingVerticalPx)
        .arg(sak::ui::kHtmlTimelineBadgePaddingHorizontalPx)
        .arg(sak::ui::kFontSizeSmall)
        .arg(sak::ui::kFontWeightSemibold)
        .arg(text.toHtmlEscaped());
}

QString runTimelineMeta(const QStringList& tags) {
    QStringList clean;
    for (const auto& tag : tags) {
        const QString value = tag.trimmed();
        if (!value.isEmpty()) {
            clean << value.toHtmlEscaped();
        }
    }
    if (clean.isEmpty()) {
        return {};
    }
    return QString::fromLatin1(sak::ui::kHtmlTimelineMeta)
        .arg(sak::ui::htmlColor(sak::ui::kColorTextMuted))
        .arg(sak::ui::kFontSizeSmall)
        .arg(sak::ui::kHtmlTimelineMetaMarginTopPx)
        .arg(clean.join(QStringLiteral(" | ")));
}

enum class ApprovalPromptChoice {
    Accept,
    Secondary,
    Reject,
    Cancel,
};

enum class ApprovalPromptButtonStyle {
    Primary,
    Secondary,
    Danger,
};

struct ApprovalPromptButton {
    QString text;
    ApprovalPromptChoice choice{ApprovalPromptChoice::Cancel};
    ApprovalPromptButtonStyle style{ApprovalPromptButtonStyle::Secondary};
    bool default_button{false};
};

struct ApprovalPromptSpec {
    QWidget* parent{nullptr};
    QString window_title;
    QString heading;
    QString body;
    QString command_text;
    QVector<ApprovalPromptButton> buttons;
};

QString approvalButtonStyle(ApprovalPromptButtonStyle style) {
    switch (style) {
    case ApprovalPromptButtonStyle::Primary:
        return sak::ui::kPrimaryButtonStyle;
    case ApprovalPromptButtonStyle::Danger:
        return sak::ui::kDangerButtonStyle;
    case ApprovalPromptButtonStyle::Secondary:
    default:
        return sak::ui::kSecondaryButtonStyle;
    }
}

void configureApprovalDialog(QDialog* dialog, const QString& window_title) {
    dialog->setWindowTitle(window_title);
    dialog->setModal(true);
    dialog->resize(kApprovalDialogWidth, kApprovalDialogHeight);
    dialog->setMinimumSize(kApprovalDialogMinWidth, kApprovalDialogMinHeight);
    dialog->setStyleSheet(sak::ui::approvalPromptStyle());
}

void addApprovalText(QVBoxLayout* layout, QDialog* dialog, const ApprovalPromptSpec& spec) {
    auto* header = new QLabel(spec.heading, dialog);
    header->setObjectName(QStringLiteral("approvalHeading"));
    header->setWordWrap(true);
    layout->addWidget(header);

    if (!spec.body.trimmed().isEmpty()) {
        auto* body_label = new QLabel(spec.body, dialog);
        body_label->setObjectName(QStringLiteral("approvalBody"));
        body_label->setWordWrap(true);
        layout->addWidget(body_label);
    }
}

QPlainTextEdit* addApprovalCommandBox(QVBoxLayout* layout,
                                      QDialog* dialog,
                                      const QString& command_text) {
    auto* command_label = new QLabel(QObject::tr("Command preview"), dialog);
    command_label->setObjectName(QStringLiteral("approvalCommandLabel"));
    layout->addWidget(command_label);

    const QString redacted_command = ai::CredentialStore::redactSecrets(command_text);
    auto* command_box = new QPlainTextEdit(dialog);
    command_box->setObjectName(QStringLiteral("approvalCommandBox"));
    command_box->setReadOnly(true);
    command_box->setLineWrapMode(QPlainTextEdit::NoWrap);
    command_box->setPlainText(redacted_command.trimmed().isEmpty()
                                  ? QObject::tr("(No command preview available)")
                                  : redacted_command);
    command_box->setMinimumHeight(kApprovalCommandMinHeight);
    command_box->setMaximumHeight(kApprovalCommandCollapsedMaxHeight);
    command_box->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(command_box, 1);
    return command_box;
}

void addApprovalCommandControls(QVBoxLayout* layout, QDialog* dialog, QPlainTextEdit* command_box) {
    auto* command_controls = new QHBoxLayout();
    command_controls->addStretch();
    auto* expand_button = new QPushButton(QObject::tr("Expand"), dialog);
    expand_button->setCheckable(true);
    expand_button->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    expand_button->setToolTip(QObject::tr("Expand or collapse the command preview box"));
    command_controls->addWidget(expand_button);
    layout->addLayout(command_controls);

    QObject::connect(
        expand_button, &QPushButton::clicked, dialog, [dialog, command_box, expand_button]() {
            const bool expanded = expand_button->isChecked();
            command_box->setMaximumHeight(expanded ? kApprovalCommandExpandedMaxHeight
                                                   : kApprovalCommandCollapsedMaxHeight);
            expand_button->setText(expanded ? QObject::tr("Collapse") : QObject::tr("Expand"));
            if (expanded) {
                dialog->resize(qMax(dialog->width(), kApprovalExpandedDialogMinWidth),
                               qMax(dialog->height(), kApprovalExpandedDialogMinHeight));
            }
        });
}

void addApprovalButtons(QVBoxLayout* layout,
                        QDialog* dialog,
                        const QVector<ApprovalPromptButton>& buttons,
                        ApprovalPromptChoice* choice) {
    auto* button_row = new QHBoxLayout();
    button_row->addStretch();
    QPushButton* default_button = nullptr;
    for (const auto& button_spec : buttons) {
        auto* button = new QPushButton(button_spec.text, dialog);
        button->setMinimumHeight(sak::ui::kUiButtonHeightDialog);
        button->setStyleSheet(approvalButtonStyle(button_spec.style));
        button_row->addWidget(button);
        if (button_spec.default_button) {
            default_button = button;
        }
        QObject::connect(button, &QPushButton::clicked, dialog, [dialog, choice, button_spec]() {
            *choice = button_spec.choice;
            dialog->accept();
        });
    }
    layout->addLayout(button_row);
    if (default_button) {
        default_button->setDefault(true);
        default_button->setFocus();
    }
}

ApprovalPromptChoice showApprovalPrompt(const ApprovalPromptSpec& spec) {
    QDialog dialog(spec.parent);
    configureApprovalDialog(&dialog, spec.window_title);
    dialog.setModal(true);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(
        sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge, sak::ui::kMarginLarge);
    layout->setSpacing(sak::ui::kSpacingMedium);

    ApprovalPromptChoice choice = ApprovalPromptChoice::Cancel;
    addApprovalText(layout, &dialog, spec);
    addApprovalCommandControls(layout,
                               &dialog,
                               addApprovalCommandBox(layout, &dialog, spec.command_text));
    addApprovalButtons(layout, &dialog, spec.buttons, &choice);

    return dialog.exec() == QDialog::Accepted ? choice : ApprovalPromptChoice::Cancel;
}

QString redactedHtml(const QString& text, int max_chars = 0) {
    QString value = ai::CredentialStore::redactSecrets(text).simplified();
    if (max_chars > 0 && value.size() > max_chars) {
        value = value.left(max_chars - 1).trimmed() + QStringLiteral("...");
    }
    return value.toHtmlEscaped();
}

QString fromStringView(std::string_view value) {
    return QString::fromUtf8(value.data(), static_cast<qsizetype>(value.size()));
}

QString compactByteCount(qint64 bytes) {
    if (bytes >= sak::kBytesPerMB) {
        return QStringLiteral("%1 MB").arg(bytes / sak::kBytesPerMB);
    }
    if (bytes >= sak::kBytesPerKB) {
        return QStringLiteral("%1 KB").arg(bytes / sak::kBytesPerKB);
    }
    return QStringLiteral("%1 B").arg(bytes);
}

void emitPrefixedLogLines(const std::function<void(const QString&)>& emit_line,
                          const QString& prefix,
                          const QString& chunk) {
    if (!emit_line) {
        return;
    }
    const QString text = chunk.trimmed();
    if (text.isEmpty()) {
        emit_line(prefix);
        return;
    }
    const auto lines = text.split(QRegularExpression(QStringLiteral(R"(\r?\n)")),
                                  Qt::SkipEmptyParts);
    bool suppressed_noisy_output = false;
    for (const auto& line : lines) {
        if (looksLikeNoisyBinaryLogLine(line)) {
            if (!suppressed_noisy_output) {
                emit_line(QStringLiteral("%1 [binary or very large output suppressed in live log]")
                              .arg(prefix));
                suppressed_noisy_output = true;
            }
            continue;
        }
        emit_line(QStringLiteral("%1 %2").arg(prefix, trimmedLogLine(line)));
    }
}

QStringList jsonStringList(const QJsonValue& value) {
    QStringList values;
    if (value.isArray()) {
        const auto array = value.toArray();
        for (const auto& entry : array) {
            const QString text = entry.toString().trimmed();
            if (!text.isEmpty()) {
                values << text;
            }
        }
    } else {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            values << text;
        }
    }
    return values;
}

QJsonArray stringListToJsonArray(const QStringList& values) {
    QJsonArray array;
    for (const auto& value : values) {
        if (!value.trimmed().isEmpty()) {
            array.append(value);
        }
    }
    return array;
}

QStringList findingSummaries(const QJsonValue& value, int max_items) {
    QStringList summaries;
    const auto array = value.toArray();
    for (const auto& entry : array) {
        if (summaries.size() >= max_items || !entry.isObject()) {
            continue;
        }
        const QJsonObject finding = entry.toObject();
        const QString severity = finding.value(QStringLiteral("severity")).toString().trimmed();
        const QString title = finding.value(QStringLiteral("title")).toString().trimmed();
        const QString recommendation =
            finding.value(QStringLiteral("recommendation")).toString().trimmed();
        QString line;
        if (!severity.isEmpty() && !title.isEmpty()) {
            line = QStringLiteral("%1: %2").arg(severity, title);
        } else if (!title.isEmpty()) {
            line = title;
        }
        if (!recommendation.isEmpty()) {
            line += line.isEmpty() ? recommendation : QStringLiteral(" -> %1").arg(recommendation);
        }
        if (!line.trimmed().isEmpty()) {
            summaries << line.simplified().left(kDefaultPreviewMaxChars);
        }
    }
    return summaries;
}

QStringList findingEvidenceRefs(const QJsonValue& value) {
    QStringList refs;
    const auto array = value.toArray();
    for (const auto& entry : array) {
        if (!entry.isObject()) {
            continue;
        }
        refs << jsonStringList(entry.toObject().value(QStringLiteral("evidence_refs")));
    }
    refs.removeDuplicates();
    return refs;
}

void appendWorkflowRequiredInputs(QStringList* lines, const ai::WorkflowTemplate& workflow) {
    if (workflow.required_inputs.isEmpty()) {
        return;
    }
    *lines << QString() << QStringLiteral("Required inputs:");
    for (const auto& input : workflow.required_inputs) {
        *lines << QStringLiteral("- %1 (%2)%3")
                      .arg(input.label.isEmpty() ? input.id : input.label,
                           input.type.isEmpty() ? QStringLiteral("text") : input.type,
                           input.required ? QStringLiteral(", required") : QString());
    }
}

void appendWorkflowRequiredSoftware(QStringList* lines, const ai::WorkflowTemplate& workflow) {
    if (workflow.required_software.isEmpty()) {
        return;
    }
    *lines << QString() << QStringLiteral("Required tools/software:");
    for (const auto& requirement : workflow.required_software) {
        *lines << QStringLiteral("- %1 [%2, %3]")
                      .arg(requirement.id, requirement.kind, requirement.install_policy);
    }
}

void appendWorkflowAgents(QStringList* lines, const ai::WorkflowTemplate& workflow) {
    if (workflow.agents.isEmpty()) {
        return;
    }
    *lines << QString() << QStringLiteral("Agents:");
    for (const auto& agent : workflow.agents) {
        *lines << QStringLiteral("- %1: %2, budget %3")
                      .arg(agent.id, agent.tool_policy, QString::number(agent.token_budget));
    }
}

void appendWorkflowPhases(QStringList* lines, const ai::WorkflowTemplate& workflow) {
    if (workflow.phases.isEmpty()) {
        return;
    }
    *lines << QString() << QStringLiteral("Phases:");
    for (const auto& phase : workflow.phases) {
        const QString owner = phase.agent.isEmpty() ? phase.type : phase.agent;
        *lines << QStringLiteral("- %1 [%2]").arg(phase.id, owner);
        if (!phase.prompt.isEmpty()) {
            *lines << QStringLiteral("  %1").arg(phase.prompt);
        }
        if (!phase.completion.isEmpty()) {
            *lines << QStringLiteral("  Done when: %1").arg(phase.completion);
        }
    }
}

void appendWorkflowStringList(QStringList* lines,
                              const QString& heading,
                              const QStringList& values) {
    if (values.isEmpty()) {
        return;
    }
    *lines << QString() << heading;
    for (const auto& value : values) {
        *lines << QStringLiteral("- %1").arg(value);
    }
}

QString workflowDetailsText(const ai::WorkflowTemplate& workflow) {
    QStringList lines;
    lines << QStringLiteral("Role: %1").arg(workflow.role);
    if (!workflow.category.isEmpty()) {
        lines << QStringLiteral("Category: %1").arg(workflow.category);
    }
    if (!workflow.description.isEmpty()) {
        lines << QStringLiteral("Purpose: %1").arg(workflow.description);
    }

    appendWorkflowRequiredInputs(&lines, workflow);
    appendWorkflowRequiredSoftware(&lines, workflow);
    appendWorkflowAgents(&lines, workflow);
    appendWorkflowPhases(&lines, workflow);
    appendWorkflowStringList(&lines,
                             QStringLiteral("Acceptance criteria:"),
                             workflow.acceptance_criteria);
    appendWorkflowStringList(&lines, QStringLiteral("Instruction files:"), workflow.instructions);
    appendWorkflowStringList(&lines, QStringLiteral("Skills:"), workflow.skills);

    return lines.join(QLatin1Char('\n'));
}

QString workflowCatalogInputs(const ai::WorkflowTemplate& workflow) {
    QStringList input_labels;
    for (const auto& input : workflow.required_inputs) {
        const QString label = input.label.trimmed().isEmpty() ? input.id.trimmed()
                                                              : input.label.trimmed();
        if (!label.isEmpty()) {
            input_labels << label;
        }
    }
    return input_labels.isEmpty() ? QStringLiteral("none")
                                  : input_labels.join(QStringLiteral(", "));
}

QString workflowCatalogTools(const ai::WorkflowTemplate& workflow) {
    QStringList tools;
    for (const auto& requirement : workflow.required_software) {
        if (!requirement.id.trimmed().isEmpty()) {
            tools << requirement.id.trimmed();
        }
    }
    return tools.isEmpty() ? QStringLiteral("built-in guidance") : tools.join(QStringLiteral(", "));
}

QString workflowCatalogRisks(const ai::WorkflowTemplate& workflow) {
    QStringList risks;
    for (const auto& phase : workflow.phases) {
        const QString risk = phase.risk.trimmed();
        if (!risk.isEmpty()) {
            risks << risk;
        }
    }
    risks.removeDuplicates();
    return risks.isEmpty() ? QStringLiteral("none declared") : risks.join(QStringLiteral(", "));
}

QString workflowCatalogEntry(const ai::WorkflowTemplate& workflow) {
    QStringList parts;
    parts << QStringLiteral("%1 [%2]").arg(workflow.title, workflow.id);
    if (!workflow.category.trimmed().isEmpty()) {
        parts << QStringLiteral("category=%1").arg(workflow.category.trimmed());
    }
    if (!workflow.description.trimmed().isEmpty()) {
        parts << QStringLiteral("best_for=%1").arg(workflow.description.trimmed());
    }
    parts << QStringLiteral("phases=%1").arg(workflow.phases.size());
    parts << QStringLiteral("inputs=%1").arg(workflowCatalogInputs(workflow));
    parts << QStringLiteral("tools=%1").arg(workflowCatalogTools(workflow));
    parts << QStringLiteral("risk=%1").arg(workflowCatalogRisks(workflow));
    return QStringLiteral("- %1").arg(parts.join(QStringLiteral("; ")));
}

void appendWorkflowCatalogPreamble(QStringList* lines) {
    *lines << QStringLiteral(
        "Built-in SAK workflow and role catalog follows. You must know this catalog when the user "
        "asks what you can do, asks which workflow fits a job, or asks about roles.");
    *lines << QStringLiteral(
        "Workflow selection behavior: recommend the closest built-in workflow before free-form "
        "tool use; explain what it does, when to use it, required inputs, risk level, "
        "verification, reporting, and cleanup. If none fits, say so and propose a bounded custom "
        "plan.");
    *lines << QStringLiteral(
        "For app install, uninstall, upgrade, offline installer, and deployment requests, prefer "
        "SAK package/offline workflows before raw web or shell commands. For malware, adware, "
        "bloatware, cleanup, health checks, update repair, network repair, BSOD, printer, and "
        "tool-assisted tasks, recommend the matching workflow below.");
}

QString workflowCatalogInstructions(const ai::WorkflowStore* store) {
    if (store == nullptr) {
        return {};
    }

    const auto workflows = store->workflows();
    if (workflows.isEmpty()) {
        return {};
    }

    QStringList lines;
    lines.reserve(kWorkflowCatalogReserveBase +
                  (workflows.size() * kWorkflowCatalogReservePerWorkflow));
    appendWorkflowCatalogPreamble(&lines);
    QString current_role;
    for (const auto& workflow : workflows) {
        const QString role = workflow.role.trimmed().isEmpty() ? QStringLiteral("General")
                                                               : workflow.role.trimmed();
        if (role.compare(current_role, Qt::CaseInsensitive) != 0) {
            current_role = role;
            lines << QString();
            lines << QStringLiteral("Role: %1").arg(current_role);
        }

        lines << workflowCatalogEntry(workflow);
    }

    lines << QString();
    lines << QStringLiteral(
        "When describing workflows, be plain-language and technician-oriented. Include what "
        "evidence gets collected, what changes may occur, what approval is needed, what success "
        "means, and what report/artifacts will exist.");
    return lines.join(QLatin1Char('\n'));
}

QJsonObject toolError(const QString& message) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error_message")] = message;
    return result;
}

QJsonObject phaseExecutionToPanelJson(const ai::AiPhaseExecution& execution) {
    QJsonObject obj;
    obj[QStringLiteral("phase_id")] = execution.phase_id;
    obj[QStringLiteral("phase_type")] = execution.phase_type;
    obj[QStringLiteral("agent_id")] = execution.agent_id;
    obj[QStringLiteral("subagent_status")] = ai::subagentStatusToString(execution.subagent_status);
    obj[QStringLiteral("ran")] = execution.ran;
    obj[QStringLiteral("skipped")] = execution.skipped;
    obj[QStringLiteral("success")] = execution.success;
    obj[QStringLiteral("skip_reason")] = execution.skip_reason;
    obj[QStringLiteral("error_message")] = execution.error_message;
    obj[QStringLiteral("duration_ms")] = static_cast<double>(execution.duration_ms);
    if (!execution.tool_result.isEmpty()) {
        obj[QStringLiteral("tool_result")] = execution.tool_result;
    }
    if (!execution.metadata.isEmpty()) {
        obj[QStringLiteral("metadata")] = execution.metadata;
    }
    return obj;
}

ai::AiPhaseExecution phaseExecutionFromPanelJson(const QJsonObject& obj) {
    ai::AiPhaseExecution execution;
    execution.phase_id = obj.value(QStringLiteral("phase_id")).toString();
    execution.phase_type = obj.value(QStringLiteral("phase_type")).toString();
    execution.agent_id = obj.value(QStringLiteral("agent_id")).toString();
    execution.subagent_status =
        ai::subagentStatusFromString(obj.value(QStringLiteral("subagent_status")).toString());
    execution.ran = obj.value(QStringLiteral("ran")).toBool(false);
    execution.skipped = obj.value(QStringLiteral("skipped")).toBool(false);
    execution.success = obj.value(QStringLiteral("success")).toBool(false);
    execution.skip_reason = obj.value(QStringLiteral("skip_reason")).toString();
    execution.error_message = obj.value(QStringLiteral("error_message")).toString();
    execution.duration_ms =
        static_cast<qint64>(obj.value(QStringLiteral("duration_ms")).toDouble(0.0));
    execution.tool_result = obj.value(QStringLiteral("tool_result")).toObject();
    execution.metadata = obj.value(QStringLiteral("metadata")).toObject();
    return execution;
}

QJsonArray phaseHistoryToJson(const QVector<ai::AiPhaseExecution>& phases) {
    QJsonArray array;
    for (const auto& phase : phases) {
        array.append(phaseExecutionToPanelJson(phase));
    }
    return array;
}

QVector<ai::AiPhaseExecution> phaseHistoryFromJson(const QJsonArray& array) {
    QVector<ai::AiPhaseExecution> phases;
    phases.reserve(array.size());
    for (const auto& value : array) {
        const auto phase = phaseExecutionFromPanelJson(value.toObject());
        if (!phase.phase_id.trimmed().isEmpty()) {
            phases.append(phase);
        }
    }
    return phases;
}

QSet<QString> stringSetFromJson(const QJsonArray& array) {
    QSet<QString> values;
    for (const auto& value : array) {
        const QString text = value.toString().trimmed();
        if (!text.isEmpty()) {
            values.insert(text);
        }
    }
    return values;
}

QString safePackageToken(const QString& value) {
    QString out = value.trimmed().toLower();
    out.remove(QRegularExpression(QStringLiteral(R"([^a-z0-9_.+-])")));
    return out;
}

QString workflowSearchText(const QString& user_message) {
    QString text = user_message.trimmed();
    text.replace(QRegularExpression(QStringLiteral(R"([,;])")), QStringLiteral(" "));
    text.remove(QRegularExpression(
        QStringLiteral(
            R"(\b(please|can you|could you|use|using|the|an|a|for|to|my|this|that|now|app|application|program|software|offline|installer|installers|install|download|downloads|bundle|deployment|package|packages|create|build|get|fetch|setup)\b)"),
        QRegularExpression::CaseInsensitiveOption));
    text = text.simplified();
    return text.isEmpty() ? user_message.trimmed() : text;
}

QString workflowInputValue(const ai::AiWorkflowPhaseContext& context,
                           const QString& key,
                           const QString& fallback = {}) {
    const auto value = context.input_values.value(key);
    if (value.isString()) {
        const QString text = value.toString().trimmed();
        return text.isEmpty() ? fallback : text;
    }
    if (value.isArray()) {
        QStringList parts;
        const auto array = value.toArray();
        for (const auto& item : array) {
            if (item.isString() && !item.toString().trimmed().isEmpty()) {
                parts << item.toString().trimmed();
            }
        }
        return parts.isEmpty() ? fallback : parts.join(QStringLiteral(", "));
    }
    return fallback;
}

enum class WorkflowPlaceholderMode {
    Raw,
    PowerShellSingleQuoted,
};

QString scalarJsonValueToString(const QJsonValue& value) {
    if (value.isString()) {
        return value.toString().trimmed();
    }
    if (value.isDouble()) {
        return QString::number(value.toDouble(), 'f', 0);
    }
    if (value.isBool()) {
        return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    }
    return {};
}

QString workflowPhaseResultValue(const ai::AiWorkflowPhaseContext& context, const QString& key) {
    if (!key.startsWith(QStringLiteral("result_"))) {
        return {};
    }
    const QString tail = key.mid(7);
    const QStringList known_fields{
        QStringLiteral("error_message"),
        QStringLiteral("source_url"),
        QStringLiteral("size_bytes"),
        QStringLiteral("stdout_path"),
        QStringLiteral("stderr_path"),
        QStringLiteral("exit_code"),
        QStringLiteral("sha256"),
        QStringLiteral("path"),
        QStringLiteral("success"),
    };
    for (const auto& field : known_fields) {
        const QString suffix = QStringLiteral("_%1").arg(field);
        if (!tail.endsWith(suffix)) {
            continue;
        }
        const QString phase_id = tail.left(tail.size() - suffix.size());
        const QJsonObject phase_result = context.phase_results.value(phase_id).toObject();
        if (phase_result.isEmpty()) {
            return {};
        }
        const QString direct = scalarJsonValueToString(phase_result.value(field));
        if (!direct.isEmpty()) {
            return direct;
        }
        return scalarJsonValueToString(
            phase_result.value(QStringLiteral("tool_result")).toObject().value(field));
    }
    return {};
}

QString workflowPlaceholderValue(const ai::AiWorkflowPhaseContext& context,
                                 const QString& key,
                                 WorkflowPlaceholderMode mode) {
    QString value;
    if (key == QLatin1String("user_message")) {
        value = context.user_message.trimmed();
    } else if (key == QLatin1String("workflow_id")) {
        value = context.workflow_id.trimmed();
    } else if (key == QLatin1String("run_id")) {
        value = context.run_id.trimmed();
    } else if (key.startsWith(QStringLiteral("result_"))) {
        value = workflowPhaseResultValue(context, key);
    } else {
        value = workflowInputValue(context, key);
    }
    if (mode == WorkflowPlaceholderMode::PowerShellSingleQuoted) {
        value.replace(QLatin1Char('\''), QStringLiteral("''"));
    }
    return value;
}

QString substituteWorkflowPlaceholders(
    const QString& text,
    const ai::AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw) {
    static const QRegularExpression placeholder_pattern(QStringLiteral(R"(\$\{([A-Za-z0-9_]+)\})"));
    QString result;
    result.reserve(text.size());
    qsizetype cursor = 0;
    auto matches = placeholder_pattern.globalMatch(text);
    while (matches.hasNext()) {
        const auto match = matches.next();
        result += text.mid(cursor, match.capturedStart() - cursor);
        result += workflowPlaceholderValue(context, match.captured(1), mode);
        cursor = match.capturedEnd();
    }
    result += text.mid(cursor);
    return result;
}

QJsonValue substituteWorkflowPlaceholdersInValue(
    const QJsonValue& value,
    const ai::AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw);

QJsonObject substituteWorkflowPlaceholdersInObject(
    const QJsonObject& object,
    const ai::AiWorkflowPhaseContext& context,
    WorkflowPlaceholderMode mode = WorkflowPlaceholderMode::Raw) {
    QJsonObject substituted;
    for (auto it = object.begin(); it != object.end(); ++it) {
        substituted.insert(it.key(),
                           substituteWorkflowPlaceholdersInValue(it.value(), context, mode));
    }
    return substituted;
}

QJsonValue substituteWorkflowPlaceholdersInValue(const QJsonValue& value,
                                                 const ai::AiWorkflowPhaseContext& context,
                                                 WorkflowPlaceholderMode mode) {
    if (value.isString()) {
        return substituteWorkflowPlaceholders(value.toString(), context, mode);
    }
    if (value.isArray()) {
        QJsonArray array;
        const auto values = value.toArray();
        for (const auto& item : values) {
            array.append(substituteWorkflowPlaceholdersInValue(item, context, mode));
        }
        return array;
    }
    if (value.isObject()) {
        return substituteWorkflowPlaceholdersInObject(value.toObject(), context, mode);
    }
    return value;
}

bool workflowInputHasValue(const QJsonValue& value) {
    if (value.isString()) {
        return !value.toString().trimmed().isEmpty();
    }
    if (value.isArray()) {
        return !value.toArray().isEmpty();
    }
    return !value.isNull() && !value.isUndefined();
}

QJsonValue workflowInputValueFromAnswer(const QString& type, const QString& answer) {
    if (type.compare(QStringLiteral("list"), Qt::CaseInsensitive) == 0) {
        QJsonArray array;
        const auto parts = answer.split(
            QRegularExpression(QStringLiteral(R"(\s*(?:,|\band\b)\s*)")), Qt::SkipEmptyParts);
        for (const auto& part : parts) {
            const QString item = part.trimmed();
            if (!item.isEmpty()) {
                array.append(item);
            }
        }
        if (array.isEmpty() && !answer.trimmed().isEmpty()) {
            array.append(answer.trimmed());
        }
        return array;
    }
    return answer.trimmed();
}

QString workflowInputSummary(const QJsonValue& value) {
    if (value.isArray()) {
        QStringList parts;
        const auto array = value.toArray();
        for (const auto& item : array) {
            const QString text = item.toString().trimmed();
            if (!text.isEmpty()) {
                parts << text;
            }
        }
        return parts.join(QStringLiteral(", "));
    }
    return value.toString().trimmed();
}

std::optional<QJsonValue> promptWorkflowInputValue(QWidget* parent,
                                                   const ai::WorkflowRequiredInput& input,
                                                   const QString& workflow_title,
                                                   const QString& question) {
    const QString id = input.id.trimmed();
    const QString label = input.label.trimmed().isEmpty() ? id : input.label.trimmed();
    if (input.type.compare(QStringLiteral("path"), Qt::CaseInsensitive) == 0) {
        const QString dir = QFileDialog::getExistingDirectory(parent,
                                                              QObject::tr("Choose %1").arg(label));
        const QString trimmed = dir.trimmed();
        if (trimmed.isEmpty()) {
            return std::nullopt;
        }
        return QJsonValue(trimmed);
    }

    bool ok = false;
    const QString prompt =
        question.trimmed().isEmpty()
            ? QObject::tr("Missing required workflow input for %1:\n%2").arg(workflow_title, label)
            : question;
    const QString answer =
        QInputDialog::getText(
            parent, QObject::tr("Workflow Input"), prompt, QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || answer.isEmpty()) {
        return std::nullopt;
    }
    return workflowInputValueFromAnswer(input.type, answer);
}

QJsonObject mergeWorkflowInputValues(const QJsonObject& base, const QJsonObject& overlay) {
    QJsonObject merged = base;
    for (auto it = overlay.begin(); it != overlay.end(); ++it) {
        if (workflowInputHasValue(it.value())) {
            merged.insert(it.key(), it.value());
        }
    }
    return merged;
}

QJsonArray workflowInputListValue(const QString& guessed) {
    QJsonArray array;
    const auto parts = guessed.split(QRegularExpression(QStringLiteral(R"(\s*(?:,|\band\b)\s*)")),
                                     Qt::SkipEmptyParts);
    for (const auto& part : parts) {
        const QString item = part.trimmed();
        if (!item.isEmpty()) {
            array.append(item);
        }
    }
    if (array.isEmpty() && !guessed.isEmpty()) {
        array.append(guessed);
    }
    return array;
}

QJsonValue workflowInputGuessValue(const QString& input_id,
                                   const QString& guessed,
                                   const QString& user_message) {
    const QString lower = input_id.toLower();
    if (lower.contains(QStringLiteral("list"))) {
        return workflowInputListValue(guessed);
    }
    if (containsAny(lower, {"app", "package", "product", "name", "query"})) {
        return guessed;
    }
    if (containsAny(lower, {"output", "dir", "path"})) {
        const QRegularExpression quoted_path(QStringLiteral(R"(["']([A-Za-z]:\\[^"']+)["'])"));
        const auto match = quoted_path.match(user_message);
        return match.hasMatch() ? QJsonValue(match.captured(1)) : QJsonValue();
    }
    return {};
}

QJsonObject workflowInputValues(const ai::WorkflowTemplate& workflow, const QString& user_message) {
    QJsonObject values;
    const QString guessed = workflowSearchText(user_message);
    values[QStringLiteral("user_request")] = user_message;
    values[QStringLiteral("query")] = guessed;
    for (const auto& input : workflow.required_inputs) {
        const QString id = input.id.trimmed();
        if (id.isEmpty()) {
            continue;
        }
        const QJsonValue value = workflowInputGuessValue(id, guessed, user_message);
        if (workflowInputHasValue(value)) {
            values[id] = value;
        }
    }
    return values;
}

QString workflowPackageQuery(const ai::AiWorkflowPhaseContext& context) {
    QString query = workflowInputValue(context, QStringLiteral("app_name"));
    if (query.isEmpty()) {
        query = workflowInputValue(context, QStringLiteral("app_list"));
    }
    if (query.isEmpty()) {
        query = workflowInputValue(context, QStringLiteral("query"));
    }
    if (query.isEmpty()) {
        query = workflowSearchText(context.user_message);
    }
    return query.trimmed();
}

QStringList workflowPackageQueries(const ai::AiWorkflowPhaseContext& context) {
    QStringList queries;
    const auto list_value = context.input_values.value(QStringLiteral("app_list"));
    if (list_value.isArray()) {
        const auto array = list_value.toArray();
        for (const auto& value : array) {
            const QString query = value.toString().trimmed();
            if (!query.isEmpty()) {
                queries << query;
            }
        }
    }
    if (queries.isEmpty()) {
        const QString query = workflowPackageQuery(context);
        if (!query.isEmpty()) {
            queries << query;
        }
    }
    queries.removeDuplicates();
    return queries;
}

void mergePackageSelectionError(QJsonObject* error, const QJsonObject& selection_details) {
    if (!error || selection_details.isEmpty()) {
        return;
    }
    error->insert(QStringLiteral("package_selection"), selection_details);
    error->insert(QStringLiteral("ambiguous_package"),
                  selection_details.value(QStringLiteral("ambiguous")).toBool(false));
    error->insert(QStringLiteral("requires_human"),
                  selection_details.value(QStringLiteral("requires_human")).toBool(false));
    const auto question = selection_details.value(QStringLiteral("question_for_human")).toString();
    if (!question.isEmpty()) {
        error->insert(QStringLiteral("question_for_human"), question);
    }
    const auto candidates = selection_details.value(QStringLiteral("candidates")).toArray();
    if (!candidates.isEmpty()) {
        error->insert(QStringLiteral("package_candidates"), candidates);
    }
}

QJsonArray packagesFromToolSearch(const QJsonObject& search_result,
                                  const QString& query,
                                  QString* error_message,
                                  QJsonObject* selection_details = nullptr) {
    const auto packages = search_result.value(QStringLiteral("packages")).toArray();
    const ai::AiPackageSelectionResult selection = ai::selectPackageForWorkflow(query, packages);
    if (selection_details) {
        *selection_details = selection.toJson();
    }
    if (!selection.success) {
        if (error_message) {
            *error_message = selection.error_message;
        }
        return {};
    }

    QJsonArray resolved;
    const QString package_id = safePackageToken(selection.selected.package_id);
    if (package_id.isEmpty()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Selected package candidate did not include package_id");
        }
        return {};
    }
    QJsonObject item;
    item[QStringLiteral("package_id")] = package_id;
    const QString version = selection.selected.version.trimmed();
    if (!version.isEmpty()) {
        item[QStringLiteral("version")] = version;
    }
    item[QStringLiteral("resolved_from")] = selection.selected.toJson();
    resolved.append(item);
    return resolved;
}

QVector<QPair<QString, QString>> packagesFromJson(const QJsonArray& array, QString* error_message) {
    QVector<QPair<QString, QString>> packages;
    packages.reserve(array.size());
    for (const auto& value : array) {
        if (!value.isObject()) {
            if (error_message) {
                *error_message = QStringLiteral("Package list contains a non-object item");
            }
            return {};
        }
        const QJsonObject object = value.toObject();
        const QString package_id =
            safePackageToken(object.value(QStringLiteral("package_id")).toString());
        if (package_id.isEmpty()) {
            if (error_message) {
                *error_message = QStringLiteral("Package item missing package_id");
            }
            return {};
        }
        const QString version = object.value(QStringLiteral("version")).toString().trimmed();
        packages.append({package_id, version});
    }
    return packages;
}

QJsonArray packageInfoToJson(const std::vector<ChocolateyManager::PackageInfo>& packages) {
    QJsonArray array;
    for (const auto& package : packages) {
        QJsonObject item;
        item[QStringLiteral("package_id")] = package.package_id;
        item[QStringLiteral("version")] = package.version;
        item[QStringLiteral("title")] = package.title;
        item[QStringLiteral("description")] = package.description.left(kDefaultPreviewMaxChars);
        item[QStringLiteral("approved")] = package.is_approved;
        item[QStringLiteral("download_count")] = package.download_count;
        array.append(item);
    }
    return array;
}

QJsonArray presetToJson(const PackageList& preset) {
    QJsonArray packages;
    for (const auto& entry : preset.entries) {
        QJsonObject item;
        item[QStringLiteral("package_id")] = entry.package_id;
        item[QStringLiteral("version")] = entry.version;
        item[QStringLiteral("notes")] = entry.notes;
        item[QStringLiteral("pinned")] = entry.pinned;
        packages.append(item);
    }
    return packages;
}

QStringList filesUnderDirectory(const QString& dir_path) {
    QStringList files;
    QDirIterator iter(dir_path, QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        files.append(QDir::toNativeSeparators(iter.next()));
    }
    files.sort(Qt::CaseInsensitive);
    return files;
}

bool isImageMime(const QString& mime_type) {
    return mime_type == QLatin1String("image/png") || mime_type == QLatin1String("image/jpeg") ||
           mime_type == QLatin1String("image/webp") || mime_type == QLatin1String("image/gif");
}

bool isTextLikeFile(const QFileInfo& info, const QString& mime_type) {
    if (mime_type.startsWith(QLatin1String("text/"))) {
        return true;
    }
    const QString suffix = info.suffix().toLower();
    return QStringList{QStringLiteral("log"),
                       QStringLiteral("md"),
                       QStringLiteral("json"),
                       QStringLiteral("xml"),
                       QStringLiteral("csv"),
                       QStringLiteral("ini"),
                       QStringLiteral("ps1"),
                       QStringLiteral("bat"),
                       QStringLiteral("cmd"),
                       QStringLiteral("cpp"),
                       QStringLiteral("h"),
                       QStringLiteral("hpp"),
                       QStringLiteral("txt")}
        .contains(suffix);
}

bool isPdfFile(const QFileInfo& info, const QString& mime_type) {
    return mime_type == QLatin1String("application/pdf") ||
           info.suffix().compare(QStringLiteral("pdf"), Qt::CaseInsensitive) == 0;
}

QString dataUrlForBytes(const QString& mime_type, const QByteArray& bytes) {
    return QStringLiteral("data:%1;base64,%2")
        .arg(mime_type, QString::fromLatin1(bytes.toBase64()));
}

QString safeDownloadFileName(const QUrl& url, const QString& filename) {
    QString safe_name = filename.trimmed();
    if (safe_name.isEmpty()) {
        safe_name = QFileInfo(url.path()).fileName();
    }
    if (safe_name.isEmpty()) {
        safe_name = QStringLiteral("download_%1.bin")
                        .arg(QDateTime::currentDateTimeUtc().toString(
                            QStringLiteral("yyyyMMdd_HHmmsszzz")));
    }
    return safe_name;
}

bool downloadUrlBytes(const QUrl& url, QByteArray* bytes, QString* error_message) {
    struct DownloadState {
        QByteArray payload;
        QString error;
        bool ok{false};
        bool timed_out{false};
    };

    DownloadState state;
    QSemaphore done;
    QThread network_thread;
    QObject* worker = new QObject;
    worker->moveToThread(&network_thread);
    QObject::connect(&network_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&network_thread, &QThread::started, worker, [&, worker]() {
        auto* nam = new QNetworkAccessManager(worker);
        nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkRequest request(url);
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
        auto* reply = nam->get(request);
        auto* timeout_timer = new QTimer(worker);
        timeout_timer->setSingleShot(true);
        const auto completed = std::make_shared<std::atomic_bool>(false);
        const auto finish = [&, reply, timeout_timer, completed](bool timed_out) {
            bool expected = false;
            if (!completed->compare_exchange_strong(expected, true)) {
                return;
            }
            if (timeout_timer->isActive()) {
                timeout_timer->stop();
            }
            state.timed_out = timed_out;
            if (timed_out) {
                state.error = QStringLiteral("Download timed out");
            } else if (reply->error() != QNetworkReply::NoError) {
                state.error = QStringLiteral("Download failed: %1").arg(reply->errorString());
            } else {
                state.payload = reply->readAll();
                state.ok = true;
            }
            reply->deleteLater();
            done.release();
            network_thread.quit();
        };
        QObject::connect(reply, &QNetworkReply::finished, worker, [finish]() { finish(false); });
        QObject::connect(timeout_timer, &QTimer::timeout, worker, [reply, finish]() {
            reply->abort();
            finish(true);
        });
        constexpr int kDownloadTimeoutMs = 5 * 60 * 1000;
        timeout_timer->start(kDownloadTimeoutMs);
    });
    network_thread.start();
    done.acquire();
    network_thread.quit();
    network_thread.wait();

    if (!state.ok) {
        if (error_message) {
            *error_message = state.error.isEmpty() ? QStringLiteral("Download failed")
                                                   : state.error;
        }
        return false;
    }
    if (bytes) {
        *bytes = state.payload;
    }
    return true;
}

bool writeDownloadBytes(const QString& destination,
                        const QByteArray& bytes,
                        QString* error_message) {
    QFile file(destination);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_message) {
            *error_message =
                QStringLiteral("Could not write destination: %1").arg(file.errorString());
        }
        return false;
    }
    if (file.write(bytes) != bytes.size()) {
        if (error_message) {
            *error_message = QStringLiteral("Short write to destination: %1").arg(destination);
        }
        return false;
    }
    return true;
}

enum class PanelReportFormat {
    Html,
    Markdown,
    PlainText,
};

struct PanelReportArtifact {
    QString label;
    QString target;
    QString type;
};

struct PanelReportFinding {
    QString severity;
    QString title;
    QString detail;
};

struct PanelReportData {
    ai::AiSessionInfo session;
    QVector<PanelReportArtifact> artifacts;
    QVector<PanelReportArtifact> sources;
    QVector<ai::AiActivityEvent> activities;
    QVector<ai::AiHumanGate> gates;
    QVector<ai::AiPhaseExecution> phases;
    QStringList transcript;
    QStringList summaries;
    QStringList actions;
    QStringList next_steps;
    QStringList evidence_notes;
    QStringList evidence_snapshot;
    QStringList risks;
    QVector<PanelReportFinding> findings;
    QString memory_warning;
    QString activity_warning;
    int tool_calls{0};
    ai::TokenUsage tokens;
};

struct PanelReportInputs {
    const ai::ConversationStore* conversation{nullptr};
    const ai::TraceStore* trace{nullptr};
    const ai::AiHumanGateStore* gates{nullptr};
    const ai::TokenUsageTracker* tokens{nullptr};
    QVector<ai::AiPhaseExecution> phases;
    QVector<ai::OpenAIUrlCitation> citations;
    int tool_calls{0};
};

PanelReportFormat panelReportFormat(bool markdown, bool plain_text) {
    if (markdown) {
        return PanelReportFormat::Markdown;
    }
    if (plain_text) {
        return PanelReportFormat::PlainText;
    }
    return PanelReportFormat::Html;
}

QString reportSuffix(PanelReportFormat format) {
    switch (format) {
    case PanelReportFormat::Markdown:
        return QStringLiteral("md");
    case PanelReportFormat::PlainText:
        return QStringLiteral("txt");
    case PanelReportFormat::Html:
    default:
        return QStringLiteral("html");
    }
}

QString cleanReportText(const QString& value, int max_chars = kDefaultCleanReportMaxChars) {
    QString text = ai::CredentialStore::redactSecrets(value).simplified();
    if (max_chars > 0 && text.size() > max_chars) {
        text = text.left(max_chars - 3) + QStringLiteral("...");
    }
    return text;
}

QString normalizedReportKey(QString value) {
    value = cleanReportText(value, 0).toLower();
    value.remove(QRegularExpression(QStringLiteral(
        R"(\b(low|medium|high|critical|info|informational|unknown|review|risk|issue)\s*:)")));
    value.replace(QRegularExpression(QStringLiteral(R"([^a-z0-9]+)")), QStringLiteral(" "));
    return value.simplified().left(kReportNormalizedKeyMaxChars);
}

void appendUniqueReportLine(QStringList* lines, const QString& value, int max_items) {
    if (!lines) {
        return;
    }
    const QString text = cleanReportText(value);
    if (text.isEmpty() || lines->contains(text) || lines->size() >= max_items) {
        return;
    }
    lines->append(text);
}

bool reportLineLooksOperationalNoise(const QString& value) {
    const QString text = value.trimmed().toLower();
    return text.contains(QStringLiteral("subagent exceeded advisory token budget")) ||
           text.contains(QStringLiteral("did not run local commands")) ||
           text.contains(QStringLiteral("reviewed the available workflow context only")) ||
           text.contains(QStringLiteral("checked whether each stated diagnostic conclusion"));
}

void appendReportFinding(QVector<PanelReportFinding>* findings,
                         const QString& severity,
                         const QString& title,
                         const QString& detail) {
    if (!findings) {
        return;
    }
    const QString clean_title = cleanReportText(title, 260);
    if (clean_title.isEmpty() || reportLineLooksOperationalNoise(clean_title)) {
        return;
    }
    const QString key = normalizedReportKey(clean_title);
    for (const auto& existing : *findings) {
        if (normalizedReportKey(existing.title) == key) {
            return;
        }
    }
    findings->append({cleanReportText(severity, kReportFindingSeverityMaxChars),
                      clean_title,
                      cleanReportText(detail, kReportFindingDetailMaxChars)});
}

void appendReportRisk(PanelReportData* data, const QString& risk) {
    if (!data || reportLineLooksOperationalNoise(risk)) {
        return;
    }
    appendUniqueReportLine(&data->risks, risk, kReportRiskLineLimit);
}

QVector<PanelReportArtifact> collectReportArtifacts(const QString& artifact_root_path,
                                                    const QString& report_path) {
    QVector<PanelReportArtifact> artifacts;
    if (artifact_root_path.isEmpty()) {
        return artifacts;
    }
    const QDir artifact_root(artifact_root_path);
    QDirIterator iter(artifact_root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        const QString path = iter.next();
        const QString normalized = QFileInfo(path).absoluteFilePath();
        QString normalized_slash = normalized;
        normalized_slash.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (normalized == QFileInfo(report_path).absoluteFilePath() ||
            normalized_slash.contains(QStringLiteral("/reports/"))) {
            continue;
        }
        const QString folder = artifact_root.relativeFilePath(path).section(QLatin1Char('/'), 0, 0);
        artifacts.append({QStringLiteral("[%1] %2").arg(folder, QFileInfo(path).fileName()),
                          path,
                          QStringLiteral("file")});
    }
    return artifacts;
}

QVector<PanelReportArtifact> collectReportSources(const QVector<ai::OpenAIUrlCitation>& citations) {
    QVector<PanelReportArtifact> sources;
    QSet<QString> seen;
    for (const auto& citation : citations) {
        if (citation.url.trimmed().isEmpty() || seen.contains(citation.url)) {
            continue;
        }
        seen.insert(citation.url);
        const QString label = citation.title.isEmpty()
                                  ? citation.url
                                  : QStringLiteral("%1 - %2").arg(citation.title, citation.url);
        sources.append({label, citation.url, QStringLiteral("url")});
    }
    return sources;
}

QStringList cleanTranscriptLines(const QStringList& raw_lines) {
    QStringList transcript;
    for (const auto& line : raw_lines) {
        const QString text = cleanReportText(line, 2400);
        if (!text.isEmpty()) {
            transcript << text;
        }
    }
    return transcript;
}

void appendStructuredFinding(const QJsonObject& finding, PanelReportData* data) {
    if (!data || finding.isEmpty()) {
        return;
    }
    QString severity = finding.value(QStringLiteral("severity")).toString().trimmed();
    QString title = finding.value(QStringLiteral("title")).toString().trimmed();
    const QString recommendation =
        finding.value(QStringLiteral("recommendation")).toString().trimmed();
    const QStringList evidence_refs =
        jsonStringList(finding.value(QStringLiteral("evidence_refs")));

    if (severity.isEmpty()) {
        severity = QStringLiteral("Review");
    }
    if (title.isEmpty()) {
        title = recommendation;
    }

    QStringList detail_parts;
    if (!recommendation.isEmpty() && recommendation.compare(title, Qt::CaseInsensitive) != 0) {
        detail_parts << QStringLiteral("Recommendation: %1").arg(recommendation);
    }
    if (!evidence_refs.isEmpty()) {
        detail_parts
            << QStringLiteral("Evidence: %1").arg(evidence_refs.join(QStringLiteral(", ")));
    }
    appendReportFinding(&data->findings, severity, title, detail_parts.join(QStringLiteral(" ")));
}

void appendSubagentReportData(const QJsonObject& result, PanelReportData* data) {
    if (!data || result.isEmpty()) {
        return;
    }
    appendUniqueReportLine(&data->summaries,
                           result.value(QStringLiteral("summary")).toString(),
                           kReportHtmlSummaryItemLimit);
    const auto findings = result.value(QStringLiteral("findings")).toArray();
    for (const auto& entry : findings) {
        if (entry.isObject()) {
            appendStructuredFinding(entry.toObject(), data);
        }
    }
    for (const auto& action : jsonStringList(result.value(QStringLiteral("actions_taken")))) {
        if (!reportLineLooksOperationalNoise(action)) {
            appendUniqueReportLine(&data->actions, action, kReportActionLineLimit);
        }
    }
    for (const auto& step :
         jsonStringList(result.value(QStringLiteral("recommended_next_steps")))) {
        appendUniqueReportLine(&data->next_steps, step, kReportRiskLineLimit);
    }
    for (const auto& risk : jsonStringList(result.value(QStringLiteral("risks")))) {
        appendReportRisk(data, risk);
    }
}

bool isUsefulEvidenceLine(const QString& line) {
    const QString text = line.trimmed();
    if (text.isEmpty() || text.startsWith(QStringLiteral("=="))) {
        return false;
    }
    static const QStringList labels{
        QStringLiteral("OsName"),
        QStringLiteral("OsBuildNumber"),
        QStringLiteral("WindowsVersion"),
        QStringLiteral("CsManufacturer"),
        QStringLiteral("CsModel"),
        QStringLiteral("CsTotalPhysicalMemory"),
        QStringLiteral("LastBootUpTime"),
        QStringLiteral("LocalDateTime"),
        QStringLiteral("DriveLetter"),
        QStringLiteral("FileSystemLabel"),
        QStringLiteral("HealthStatus"),
        QStringLiteral("OperationalStatus"),
        QStringLiteral("SizeRemaining"),
        QStringLiteral("FriendlyName"),
        QStringLiteral("Model"),
        QStringLiteral("Status"),
        QStringLiteral("InterfaceType"),
        QStringLiteral("AMServiceEnabled"),
        QStringLiteral("AntivirusEnabled"),
        QStringLiteral("RealTimeProtectionEnabled"),
        QStringLiteral("AntivirusSignatureLastUpdated"),
    };
    for (const auto& label : labels) {
        if (text.startsWith(label + QStringLiteral(" "))) {
            return true;
        }
    }
    return text.startsWith(QStringLiteral("[warning]")) ||
           text.contains(QStringLiteral("Microsoft-Windows-TPM-WMI")) ||
           text.contains(QStringLiteral("Service Control Manager")) ||
           text.contains(QStringLiteral("WindowsUpdateClient")) ||
           text.contains(QStringLiteral("DistributedCOM"));
}

void appendToolEvidenceSnapshot(const QJsonObject& tool_result, PanelReportData* data) {
    if (!data || tool_result.isEmpty()) {
        return;
    }
    const QString stdout_text = tool_result.value(QStringLiteral("stdout")).toString();
    const auto lines = stdout_text.split(QRegularExpression(QStringLiteral(R"(\r?\n)")));
    for (const auto& line : lines) {
        if (data->evidence_snapshot.size() >= kReportEvidenceSnapshotLineLimit) {
            break;
        }
        if (isUsefulEvidenceLine(line)) {
            appendUniqueReportLine(&data->evidence_snapshot,
                                   line,
                                   kReportEvidenceSnapshotLineLimit);
        }
    }
    const QString stderr_text = tool_result.value(QStringLiteral("stderr")).toString().trimmed();
    if (!stderr_text.isEmpty()) {
        appendUniqueReportLine(&data->evidence_notes,
                               QStringLiteral("Command stderr: %1").arg(stderr_text),
                               kReportHtmlEvidenceItemLimit);
    }
}

void appendPhaseReportData(const ai::AiPhaseExecution& phase, PanelReportData* data) {
    if (!data) {
        return;
    }
    if (!phase.error_message.trimmed().isEmpty()) {
        appendReportFinding(
            &data->findings, QStringLiteral("Issue"), phase.phase_id, phase.error_message);
    }
    if (!phase.skip_reason.trimmed().isEmpty()) {
        appendUniqueReportLine(&data->evidence_notes,
                               phase.skip_reason,
                               kReportHtmlEvidenceItemLimit);
    }
    appendSubagentReportData(phase.metadata.value(QStringLiteral("result")).toObject(), data);
    appendSubagentReportData(phase.metadata.value(QStringLiteral("subagent_result")).toObject(),
                             data);
    appendToolEvidenceSnapshot(phase.tool_result, data);
}

void appendActivityReportData(const ai::AiActivityEvent& activity, PanelReportData* data) {
    if (!data) {
        return;
    }
    if (!activity.error.trimmed().isEmpty()) {
        appendReportFinding(
            &data->findings, QStringLiteral("Issue"), activity.kind, activity.error);
    }
    if (!activity.question_for_human.trimmed().isEmpty()) {
        appendUniqueReportLine(&data->evidence_notes,
                               activity.question_for_human,
                               kReportHtmlEvidenceItemLimit);
    }
    const QJsonObject tool_result =
        activity.metadata.value(QStringLiteral("tool_result")).toObject();
    if (tool_result.value(QStringLiteral("success")).toBool(true)) {
        return;
    }
    const QString detail = tool_result.value(QStringLiteral("error_message")).toString();
    appendReportFinding(&data->findings,
                        QStringLiteral("Issue"),
                        activity.phase_id.isEmpty() ? activity.kind : activity.phase_id,
                        detail);
}

QString overallReportStatus(const PanelReportData& data) {
    for (const auto& finding : data.findings) {
        if (finding.severity.compare(QStringLiteral("Issue"), Qt::CaseInsensitive) == 0 ||
            finding.severity.compare(QStringLiteral("Risk"), Qt::CaseInsensitive) == 0 ||
            finding.severity.compare(QStringLiteral("High"), Qt::CaseInsensitive) == 0 ||
            finding.severity.compare(QStringLiteral("Critical"), Qt::CaseInsensitive) == 0 ||
            finding.severity.compare(QStringLiteral("Medium"), Qt::CaseInsensitive) == 0) {
            return QStringLiteral("Attention recommended");
        }
    }
    if (!data.risks.isEmpty()) {
        return QStringLiteral("Evidence incomplete");
    }
    return data.activities.isEmpty() && data.phases.isEmpty()
               ? QStringLiteral("No completed work recorded")
               : QStringLiteral("No blocking issues recorded");
}

void appendListSection(QStringList* lines,
                       const QString& heading,
                       const QStringList& items,
                       const QString& empty_text) {
    lines->append(QStringLiteral("## %1").arg(heading));
    lines->append(QString());
    if (items.isEmpty()) {
        lines->append(QStringLiteral("- %1").arg(empty_text));
    } else {
        for (const auto& item : items) {
            lines->append(
                QStringLiteral("- %1").arg(cleanReportText(item, kDefaultCleanReportMaxChars)));
        }
    }
    lines->append(QString());
}

void appendFindingSection(QStringList* lines, const QVector<PanelReportFinding>& findings) {
    lines->append(QStringLiteral("## Key Findings"));
    lines->append(QString());
    if (findings.isEmpty()) {
        lines->append(QStringLiteral("- No issues were identified in the recorded evidence."));
    } else {
        int emitted = 0;
        for (const auto& finding : findings) {
            if (emitted >= kReportHtmlFindingLimit) {
                lines->append(QStringLiteral(
                    "- Additional lower-priority findings are retained in the session trace."));
                break;
            }
            lines->append(
                QStringLiteral("- **%1:** %2")
                    .arg(finding.severity.isEmpty() ? QStringLiteral("Review") : finding.severity,
                         finding.title));
            if (!finding.detail.isEmpty()) {
                lines->append(QStringLiteral("  - %1").arg(finding.detail));
            }
            ++emitted;
        }
    }
    lines->append(QString());
}

void appendArtifactSection(QStringList* lines,
                           const QString& heading,
                           const QVector<PanelReportArtifact>& artifacts) {
    lines->append(QStringLiteral("## %1").arg(heading));
    lines->append(QString());
    if (artifacts.isEmpty()) {
        lines->append(QStringLiteral("- None recorded."));
    } else {
        for (const auto& artifact : artifacts) {
            const QString target = artifact.type == QLatin1String("url")
                                       ? ai::CredentialStore::redactSecrets(artifact.target)
                                       : QUrl::fromLocalFile(artifact.target).toString();
            lines->append(
                QStringLiteral("- [%1](%2)")
                    .arg(cleanReportText(artifact.label, kReportArtifactMarkdownLabelMaxChars),
                         target));
        }
    }
    lines->append(QString());
}

void appendActivitySection(QStringList* lines, const QVector<ai::AiActivityEvent>& activities) {
    lines->append(QStringLiteral("## Workflow Timeline"));
    lines->append(QString());
    if (activities.isEmpty()) {
        lines->append(QStringLiteral("- No activity events were recorded."));
    } else {
        const int start = std::max(0, static_cast<int>(activities.size()) - 18);
        for (int i = start; i < activities.size(); ++i) {
            const auto& activity = activities.at(i);
            const QString when = activity.timestamp_utc.isValid()
                                     ? activity.timestamp_utc.toString(Qt::ISODateWithMs)
                                     : QStringLiteral("unknown-time");
            lines->append(QStringLiteral("- %1 `%2` %3: %4")
                              .arg(when,
                                   activity.state,
                                   activity.kind,
                                   cleanReportText(activity.summary, kDefaultCleanReportMaxChars)));
        }
    }
    lines->append(QString());
}

QString reportGeneratedAtUtc() {
    return QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
}

QString phaseStatusLabel(const ai::AiPhaseExecution& phase) {
    if (phase.skipped) {
        return QStringLiteral("Skipped");
    }
    return phase.success ? QStringLiteral("Completed") : QStringLiteral("Needs attention");
}

QString phaseSummary(const ai::AiPhaseExecution& phase) {
    const QJsonObject result = phase.metadata.value(QStringLiteral("result")).toObject();
    QString summary = result.value(QStringLiteral("summary")).toString().trimmed();
    if (summary.isEmpty()) {
        summary = phase.metadata.value(QStringLiteral("summary")).toString().trimmed();
    }
    if (summary.isEmpty() && !phase.error_message.trimmed().isEmpty()) {
        summary = phase.error_message.trimmed();
    }
    if (summary.isEmpty() && !phase.skip_reason.trimmed().isEmpty()) {
        summary = phase.skip_reason.trimmed();
    }
    return cleanReportText(summary.isEmpty() ? phase.phase_id : summary, kReportSummaryMaxChars);
}

QString durationText(qint64 duration_ms) {
    if (duration_ms <= 0) {
        return QStringLiteral("<1 sec");
    }
    if (duration_ms < kReadableDurationMillisecondsPerSecond) {
        return QStringLiteral("%1 ms").arg(duration_ms);
    }
    return QStringLiteral("%1 sec").arg(
        QString::number(duration_ms / kReadableDurationMillisecondsPerSecondF, 'f', 1));
}

void appendReportMetadataMarkdown(QStringList* lines,
                                  const PanelReportData& data,
                                  const QString& report_path) {
    const QString session_url = QUrl::fromLocalFile(data.session.path).toString();
    const QString report_dir = QFileInfo(report_path).absolutePath();
    *lines << QStringLiteral("# PC Health Report") << QString();
    *lines << QStringLiteral("- Status: **%1**").arg(overallReportStatus(data));
    *lines << QStringLiteral("- Generated: %1 UTC").arg(reportGeneratedAtUtc());
    *lines << QStringLiteral("- Session: `%1`").arg(data.session.id);
    *lines << QStringLiteral("- Chat name: %1")
                  .arg(cleanReportText(data.session.title, kReportChatNameMaxChars));
    *lines << QStringLiteral("- Report directory: [%1](%2)")
                  .arg(cleanReportText(QDir::toNativeSeparators(report_dir), kReportPathMaxChars),
                       QUrl::fromLocalFile(report_dir).toString());
    *lines << QStringLiteral("- Session directory: [%1](%2)")
                  .arg(cleanReportText(QDir::toNativeSeparators(data.session.path),
                                       kReportPathMaxChars),
                       session_url);
    *lines << QStringLiteral("- Tool calls: %1").arg(data.tool_calls);
    *lines << QStringLiteral("- Total tokens: %1").arg(data.tokens.total_tokens);
    *lines << QString();
}

void appendReportBodyMarkdown(QStringList* lines, const PanelReportData& data) {
    appendListSection(lines,
                      QStringLiteral("Executive Summary"),
                      data.summaries,
                      QStringLiteral(
                          "The workflow completed, but no narrative summary was recorded."));
    appendFindingSection(lines, data.findings);
    appendListSection(lines,
                      QStringLiteral("Evidence Snapshot"),
                      data.evidence_snapshot,
                      QStringLiteral("No structured evidence snapshot was available."));
    appendListSection(lines,
                      QStringLiteral("Recommended Next Steps"),
                      data.next_steps,
                      data.findings.isEmpty()
                          ? QStringLiteral("Continue normal monitoring.")
                          : QStringLiteral(
                                "Review the findings and rerun incomplete checks as needed."));
    appendListSection(lines,
                      QStringLiteral("Evidence Limits and Risks"),
                      data.risks,
                      QStringLiteral("No evidence limitations were recorded."));
    appendListSection(lines,
                      QStringLiteral("Work Performed"),
                      data.actions,
                      QStringLiteral("No change actions were recorded."));
    appendListSection(lines,
                      QStringLiteral("Evidence Notes"),
                      data.evidence_notes,
                      QStringLiteral("No extra evidence notes were recorded."));
    appendActivitySection(lines, data.activities);
    appendArtifactSection(lines, QStringLiteral("Evidence Artifacts"), data.artifacts);
    appendArtifactSection(lines, QStringLiteral("Sources"), data.sources);
}

void appendReportTranscriptMarkdown(QStringList* lines, const PanelReportData& data) {
    *lines << QStringLiteral("## Appendix: Raw Transcript Excerpts") << QString();
    if (data.transcript.isEmpty()) {
        *lines << QStringLiteral("- No transcript entries were recorded.");
    } else {
        *lines << QStringLiteral(
            "These excerpts are for audit/debugging. The sections above are the technician "
            "report.");
        *lines << QString();
        for (const auto& entry : data.transcript.mid(0, kReportTranscriptExcerptLimit)) {
            *lines << QStringLiteral("- %1").arg(
                cleanReportText(entry, kReportTranscriptExcerptMaxChars));
        }
    }
    *lines << QString();
    if (!data.activity_warning.isEmpty()) {
        *lines << QStringLiteral("- Activity warning: %1")
                      .arg(cleanReportText(data.activity_warning, kReportWarningMaxChars));
    }
    if (!data.memory_warning.isEmpty()) {
        *lines << QStringLiteral("- Memory warning: %1")
                      .arg(cleanReportText(data.memory_warning, kReportWarningMaxChars));
    }
}

QString renderReportMarkdown(const PanelReportData& data, const QString& report_path) {
    QStringList lines;
    appendReportMetadataMarkdown(&lines, data, report_path);
    appendReportBodyMarkdown(&lines, data);
    appendReportTranscriptMarkdown(&lines, data);
    return lines.join(QLatin1Char('\n')) + QLatin1Char('\n');
}

QString markdownReportToPlainText(const QString& markdown) {
    QStringList plain_lines;
    for (QString line : markdown.split(QLatin1Char('\n'))) {
        line.replace(QRegularExpression(QStringLiteral(R"(^#{1,6}\s*)")), QString());
        line.replace(QRegularExpression(QStringLiteral(R"(\[([^\]]+)\]\(([^)]+)\))")),
                     QStringLiteral("\\1 (\\2)"));
        line.replace(QStringLiteral("**"), QString());
        line.replace(QLatin1Char('`'), QString());
        plain_lines << line;
    }
    return plain_lines.join(QLatin1Char('\n'));
}

QString htmlText(const QString& value, int max_chars = 0) {
    return cleanReportText(value, max_chars).toHtmlEscaped();
}

QString htmlList(const QStringList& items,
                 const QString& empty_text,
                 int max_items = kReportDefaultHtmlListItemLimit) {
    QStringList lines;
    if (items.isEmpty()) {
        return QStringLiteral("<p class=\"empty\">%1</p>").arg(htmlText(empty_text));
    }
    const int limit = std::min(max_items, static_cast<int>(items.size()));
    lines << QStringLiteral("<ul>");
    for (int i = 0; i < limit; ++i) {
        lines << QStringLiteral("<li>%1</li>")
                     .arg(htmlText(items.at(i), kReportTranscriptExcerptMaxChars));
    }
    if (items.size() > limit) {
        lines << QStringLiteral("<li>%1 more item(s) retained in the session trace.</li>")
                     .arg(items.size() - limit);
    }
    lines << QStringLiteral("</ul>");
    return lines.join(QString());
}

QString htmlSeverityClass(const QString& severity) {
    const QString value = severity.trimmed().toLower();
    if (value.contains(QStringLiteral("critical")) || value.contains(QStringLiteral("high")) ||
        value.contains(QStringLiteral("issue"))) {
        return QStringLiteral("sev-high");
    }
    if (value.contains(QStringLiteral("medium")) || value.contains(QStringLiteral("risk"))) {
        return QStringLiteral("sev-med");
    }
    if (value.contains(QStringLiteral("low")) || value.contains(QStringLiteral("info"))) {
        return QStringLiteral("sev-low");
    }
    return QStringLiteral("sev-review");
}

QString htmlFindingCards(const QVector<PanelReportFinding>& findings) {
    if (findings.isEmpty()) {
        return QStringLiteral(
            "<p class=\"empty\">No issues were identified in the recorded evidence.</p>");
    }
    QStringList cards;
    cards << QStringLiteral("<div class=\"finding-grid\">");
    const int limit = std::min(kReportHtmlFindingLimit, static_cast<int>(findings.size()));
    for (int i = 0; i < limit; ++i) {
        const auto& finding = findings.at(i);
        const QString severity = finding.severity.isEmpty() ? QStringLiteral("Review")
                                                            : finding.severity;
        cards << QStringLiteral(
                     "<article class=\"finding %1\"><div class=\"severity\">%2</div>"
                     "<h3>%3</h3>")
                     .arg(htmlSeverityClass(severity),
                          htmlText(severity, kReportHtmlSeverityMaxChars),
                          htmlText(finding.title, kReportHtmlFindingTitleMaxChars));
        if (!finding.detail.isEmpty()) {
            cards << QStringLiteral("<p>%1</p>")
                         .arg(htmlText(finding.detail, kReportHtmlFindingDetailMaxChars));
        }
        cards << QStringLiteral("</article>");
    }
    if (findings.size() > limit) {
        cards << QStringLiteral(
                     "<p class=\"note\">%1 additional lower-priority finding(s) are retained in "
                     "the session trace.</p>")
                     .arg(findings.size() - limit);
    }
    cards << QStringLiteral("</div>");
    return cards.join(QString());
}

QString htmlArtifactList(const QVector<PanelReportArtifact>& artifacts) {
    if (artifacts.isEmpty()) {
        return QStringLiteral("<p class=\"empty\">None recorded.</p>");
    }
    QStringList lines;
    lines << QStringLiteral("<ul>");
    for (const auto& artifact : artifacts) {
        const QString target = artifact.type == QLatin1String("url")
                                   ? ai::CredentialStore::redactSecrets(artifact.target)
                                   : QUrl::fromLocalFile(artifact.target).toString();
        lines << QStringLiteral("<li><a href=\"%1\">%2</a></li>")
                     .arg(target.toHtmlEscaped(),
                          htmlText(artifact.label, kReportHtmlArtifactLabelMaxChars));
    }
    lines << QStringLiteral("</ul>");
    return lines.join(QString());
}

QString htmlWorkflowTimeline(const QVector<ai::AiPhaseExecution>& phases,
                             const QVector<ai::AiActivityEvent>& activities) {
    QStringList rows;
    if (!phases.isEmpty()) {
        rows << QStringLiteral(
            "<table><thead><tr><th>Phase</th><th>Status</th><th>Duration</th><th>Summary</th></"
            "tr></thead><tbody>");
        for (const auto& phase : phases) {
            rows << QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td></tr>")
                        .arg(htmlText(phase.phase_id, kReportHtmlPhaseIdMaxChars),
                             htmlText(phaseStatusLabel(phase), kReportHtmlStatusMaxChars),
                             htmlText(durationText(phase.duration_ms), kReportHtmlDurationMaxChars),
                             htmlText(phaseSummary(phase), kReportHtmlPhaseSummaryMaxChars));
        }
        rows << QStringLiteral("</tbody></table>");
        return rows.join(QString());
    }
    if (activities.isEmpty()) {
        return QStringLiteral("<p class=\"empty\">No workflow timeline was recorded.</p>");
    }
    rows << QStringLiteral("<ul>");
    const int start =
        std::max(0, static_cast<int>(activities.size()) - kReportHtmlTimelineActivityLimit);
    for (int i = start; i < activities.size(); ++i) {
        const auto& activity = activities.at(i);
        const QString when = activity.timestamp_utc.isValid()
                                 ? activity.timestamp_utc.toString(Qt::ISODateWithMs)
                                 : QStringLiteral("unknown-time");
        rows << QStringLiteral("<li><span class=\"mono\">%1</span> %2: %3</li>")
                    .arg(htmlText(when, kReportHtmlTimestampMaxChars),
                         htmlText(activity.kind, kReportHtmlActivityKindMaxChars),
                         htmlText(activity.summary, kReportHtmlActivitySummaryMaxChars));
    }
    rows << QStringLiteral("</ul>");
    return rows.join(QString());
}

QString htmlTranscriptAppendix(const QStringList& transcript) {
    if (transcript.isEmpty()) {
        return QStringLiteral("<p class=\"empty\">No transcript entries were recorded.</p>");
    }
    QStringList lines;
    lines << QStringLiteral(
        "<details><summary>Raw transcript excerpts for audit/debugging</summary>");
    lines << QStringLiteral(
        "<p class=\"note\">The sections above are the technician report. "
        "This appendix is intentionally abridged.</p><ol>");
    for (const auto& entry : transcript.mid(0, kReportTranscriptExcerptLimit)) {
        lines
            << QStringLiteral("<li>%1</li>").arg(htmlText(entry, kReportTranscriptExcerptMaxChars));
    }
    lines << QStringLiteral("</ol></details>");
    return lines.join(QString());
}

int completedReportPhaseCount(const PanelReportData& data) {
    return static_cast<int>(
        std::count_if(data.phases.cbegin(), data.phases.cend(), [](const auto& phase) {
            return phase.ran && phase.success && !phase.skipped;
        }));
}

void appendReportHtmlDocumentStart(QStringList* html, const PanelReportData& data) {
    *html << QStringLiteral(
                 "<!doctype html><html><head><meta charset=\"utf-8\">"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">"
                 "<title>PC Health Report</title>") +
                 QString::fromLatin1(sak::ui::kHtmlStyleTagOpen) +
                 report::aiHealthReportStyleSheet() +
                 QString::fromLatin1(sak::ui::kHtmlStyleHeadMainCloseOpen);
    *html << QStringLiteral(
                 "<div class=\"hero\"><span class=\"badge\">%1</span><h1>PC Health Report</h1>"
                 "<p>Generated %2 UTC from S.A.K. Utility AI workflow evidence.</p></div>")
                 .arg(htmlText(overallReportStatus(data)), htmlText(reportGeneratedAtUtc()));
}

void appendReportHtmlMetricGrid(QStringList* html, const PanelReportData& data) {
    *html << QStringLiteral("<div class=\"grid\">");
    *html << QStringLiteral(
                 "<div class=\"card\"><div class=\"label\">Session</div><div class=\"value "
                 "mono\">%1</div></div>")
                 .arg(htmlText(data.session.id, kReportHtmlSessionMaxChars));
    *html << QStringLiteral(
                 "<div class=\"card\"><div class=\"label\">Workflow Phases</div><div "
                 "class=\"value\">%1/%2 completed</div></div>")
                 .arg(completedReportPhaseCount(data))
                 .arg(data.phases.size());
    *html << QStringLiteral(
                 "<div class=\"card\"><div class=\"label\">Findings</div><div "
                 "class=\"value\">%1</div></div>")
                 .arg(data.findings.size());
    *html << QStringLiteral(
                 "<div class=\"card\"><div class=\"label\">Artifacts</div><div "
                 "class=\"value\">%1</div></div>")
                 .arg(data.artifacts.size());
    *html << QStringLiteral("</div>");
}

void appendReportHtmlNarrativeSections(QStringList* html, const PanelReportData& data) {
    *html << QStringLiteral("<section><h2>Executive Summary</h2>%1</section>")
                 .arg(
                     htmlList(data.summaries,
                              QStringLiteral(
                                  "The workflow completed, but no narrative summary was recorded."),
                              kReportHtmlSummaryItemLimit));
    *html << QStringLiteral("<section><h2>Key Findings</h2>%1</section>")
                 .arg(htmlFindingCards(data.findings));
    *html << QStringLiteral("<section><h2>Evidence Snapshot</h2>%1</section>")
                 .arg(htmlList(data.evidence_snapshot,
                               QStringLiteral("No structured evidence snapshot was available."),
                               kReportHtmlEvidenceItemLimit));
    *html << QStringLiteral("<section><h2>Recommended Next Steps</h2>%1</section>")
                 .arg(htmlList(data.next_steps,
                               data.findings.isEmpty()
                                   ? QStringLiteral("Continue normal monitoring.")
                                   : QStringLiteral(
                                         "Review findings and rerun incomplete checks as needed."),
                               kReportHtmlNextStepItemLimit));
}

void appendReportHtmlEvidenceSections(QStringList* html, const PanelReportData& data) {
    *html << QStringLiteral("<section><h2>Evidence Limits and Risks</h2>%1</section>")
                 .arg(htmlList(data.risks,
                               QStringLiteral("No evidence limitations were recorded."),
                               kReportHtmlRiskItemLimit));
    *html << QStringLiteral("<section><h2>Work Performed</h2>%1</section>")
                 .arg(htmlList(data.actions,
                               QStringLiteral("No change actions were recorded."),
                               kReportHtmlActionItemLimit));
    *html << QStringLiteral("<section><h2>Evidence Notes</h2>%1</section>")
                 .arg(htmlList(data.evidence_notes,
                               QStringLiteral("No extra evidence notes were recorded."),
                               kReportHtmlEvidenceNoteItemLimit));
    *html << QStringLiteral("<section><h2>Workflow Timeline</h2>%1</section>")
                 .arg(htmlWorkflowTimeline(data.phases, data.activities));
    *html << QStringLiteral("<section><h2>Evidence Artifacts</h2>%1</section>")
                 .arg(htmlArtifactList(data.artifacts));
    *html << QStringLiteral("<section><h2>Sources</h2>%1</section>")
                 .arg(htmlArtifactList(data.sources));
}

void appendReportHtmlAppendix(QStringList* html,
                              const PanelReportData& data,
                              const QString& report_path) {
    const QString report_dir = QFileInfo(report_path).absolutePath();
    *html << QStringLiteral(
                 "<section><h2>Report Paths</h2><ul>"
                 "<li>Report directory: <span class=\"mono\">%1</span></li>"
                 "<li>Session directory: <span class=\"mono\">%2</span></li></ul></section>")
                 .arg(htmlText(QDir::toNativeSeparators(report_dir), kReportHtmlPathMaxChars),
                      htmlText(QDir::toNativeSeparators(data.session.path),
                               kReportHtmlPathMaxChars));
    *html << QStringLiteral("<section><h2>Appendix</h2>%1</section>")
                 .arg(htmlTranscriptAppendix(data.transcript));
}

void appendReportHtmlWarnings(QStringList* html, const PanelReportData& data) {
    if (!data.activity_warning.isEmpty() || !data.memory_warning.isEmpty()) {
        QStringList warnings;
        appendUniqueReportLine(&warnings, data.activity_warning, kReportHtmlWarningItemLimit);
        appendUniqueReportLine(&warnings, data.memory_warning, kReportHtmlWarningItemLimit);
        *html << QStringLiteral("<section><h2>Report Generation Warnings</h2>%1</section>")
                     .arg(htmlList(warnings,
                                   QStringLiteral("No report generation warnings."),
                                   kReportHtmlWarningItemLimit));
    }
}

QString renderReportHtml(const PanelReportData& data, const QString& report_path) {
    QStringList html;
    appendReportHtmlDocumentStart(&html, data);
    appendReportHtmlMetricGrid(&html, data);
    appendReportHtmlNarrativeSections(&html, data);
    appendReportHtmlEvidenceSections(&html, data);
    appendReportHtmlAppendix(&html, data, report_path);
    appendReportHtmlWarnings(&html, data);
    html << QStringLiteral("</main></body></html>");
    return html.join(QString());
}

QString markdownReportToHtml(const QString& markdown) {
    QTextDocument document;
    document.setDefaultStyleSheet(report::markdownReportStyleSheet());
    document.setMarkdown(markdown);
    return document.toHtml();
}

bool writeReportText(const QString& path, const QString& text, QString* error_message) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error_message) {
            *error_message = file.errorString();
        }
        return false;
    }
    const QByteArray bytes = text.toUtf8();
    if (file.write(bytes) != bytes.size() || !file.commit()) {
        if (error_message) {
            *error_message = file.errorString().isEmpty() ? QStringLiteral("Could not write report")
                                                          : file.errorString();
        }
        return false;
    }
    return true;
}

bool isPackageReadOperation(const QString& operation) {
    return operation == QLatin1String("is_installed") ||
           operation == QLatin1String("installed_version");
}

bool isPackageChangeOperation(const QString& operation) {
    return operation == QLatin1String("install") || operation == QLatin1String("uninstall") ||
           operation == QLatin1String("upgrade");
}

bool offlineNeedsPackages(const QString& operation) {
    return operation == QLatin1String("direct_download") ||
           operation == QLatin1String("build_bundle");
}

bool offlineCreatesDefaultOutput(const QString& operation) {
    return offlineNeedsPackages(operation);
}

QString phaseTraceStatus(const ai::AiPhaseExecution& execution, const QJsonObject& metadata) {
    if (execution.skipped) {
        return QStringLiteral("skipped");
    }
    if (execution.success) {
        return QStringLiteral("completed");
    }
    return metadata.value(QStringLiteral("recovery_action")).toString() ==
                   QLatin1String("ask_human")
               ? QStringLiteral("waiting_for_human")
               : QStringLiteral("failed");
}

QString resolvePanelReportPath(const ai::ConversationStore* conversation,
                               const QString& output_path,
                               PanelReportFormat format,
                               QString* error_message) {
    if (!conversation) {
        if (error_message) {
            *error_message = QStringLiteral("No conversation store");
        }
        return {};
    }
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    QString error;
    QString path = output_path.trimmed();
    if (path.isEmpty()) {
        path = conversation->artifactPath(
            QStringLiteral("reports"),
            QStringLiteral("report_%1.%2").arg(stamp, reportSuffix(format)),
            &error);
    } else if (QFileInfo(path).suffix().isEmpty()) {
        path += QStringLiteral(".%1").arg(reportSuffix(format));
    }
    if (path.isEmpty() && error_message) {
        *error_message = error;
    }
    return path;
}

bool ensureReportParentDirectory(const QString& report_path, QString* error_message) {
    const QString parent_dir = QFileInfo(report_path).absolutePath();
    if (QDir().mkpath(parent_dir)) {
        return true;
    }
    if (error_message) {
        *error_message = QStringLiteral("Could not create report directory: %1").arg(parent_dir);
    }
    return false;
}

PanelReportData collectPanelReportData(const PanelReportInputs& inputs,
                                       const QString& report_path,
                                       QString* error_message) {
    PanelReportData data;
    data.session = inputs.conversation->currentSessionInfo();
    data.phases = inputs.phases;
    data.sources = collectReportSources(inputs.citations);
    data.tool_calls = inputs.tool_calls;
    if (inputs.tokens) {
        data.tokens = inputs.tokens->sessionTotal();
    }
    data.transcript = cleanTranscriptLines(
        inputs.conversation->loadTranscriptLines(data.session.id, error_message));
    QString artifact_error;
    data.artifacts = collectReportArtifacts(
        inputs.conversation->artifactRootDirectory(&artifact_error), report_path);
    appendUniqueReportLine(&data.evidence_notes, artifact_error, kReportHtmlEvidenceItemLimit);
    return data;
}

void appendTraceAndGateData(const PanelReportInputs& inputs, PanelReportData* data) {
    if (inputs.trace && !inputs.trace->sessionDirectory().isEmpty()) {
        data->activities = inputs.trace->loadActivityEvents(&data->activity_warning);
    }
    if (inputs.gates && !inputs.gates->sessionDirectory().isEmpty()) {
        data->gates = inputs.gates->loadGates();
    }
}

void evaluatePanelReportData(PanelReportData* data) {
    for (const auto& phase : data->phases) {
        appendPhaseReportData(phase, data);
    }
    for (const auto& activity : data->activities) {
        appendActivityReportData(activity, data);
    }
    for (const auto& gate : data->gates) {
        if (gate.isPending()) {
            appendUniqueReportLine(&data->evidence_notes,
                                   gate.question,
                                   kReportHtmlEvidenceItemLimit);
        }
    }
}

QString renderPanelReport(const PanelReportData& data,
                          const QString& report_path,
                          PanelReportFormat format) {
    const QString markdown = renderReportMarkdown(data, report_path);
    if (format == PanelReportFormat::PlainText) {
        return markdownReportToPlainText(markdown);
    }
    if (format == PanelReportFormat::Html) {
        return renderReportHtml(data, report_path);
    }
    return markdown;
}

void appendWorkflowTextSection(QStringList* lines,
                               const QString& heading,
                               const QStringList& values) {
    if (values.isEmpty()) {
        return;
    }
    lines->append(QStringLiteral("## %1").arg(heading));
    for (const auto& value : values) {
        lines->append(QStringLiteral("- %1").arg(value));
    }
    lines->append(QString());
}

QStringList workflowInputLines(const ai::WorkflowTemplate& workflow) {
    QStringList inputs;
    for (const auto& input : workflow.required_inputs) {
        inputs << QStringLiteral("%1 (%2%3)")
                      .arg(input.label.isEmpty() ? input.id : input.label,
                           input.type.isEmpty() ? QStringLiteral("text") : input.type,
                           input.required ? QStringLiteral(", required") : QString());
    }
    return inputs;
}

QStringList workflowToolLines(const ai::WorkflowTemplate& workflow) {
    QStringList tools;
    for (const auto& requirement : workflow.required_software) {
        tools << QStringLiteral("%1 (%2, %3)")
                     .arg(requirement.id, requirement.kind, requirement.install_policy);
    }
    return tools;
}

QStringList workflowAgentLines(const ai::WorkflowTemplate& workflow) {
    QStringList agents;
    for (const auto& agent : workflow.agents) {
        agents << QStringLiteral("%1 - policy %2, budget %3")
                      .arg(agent.id, agent.tool_policy)
                      .arg(agent.token_budget);
    }
    return agents;
}

QStringList workflowPhaseLines(const ai::WorkflowTemplate& workflow) {
    QStringList phases;
    for (const auto& phase : workflow.phases) {
        phases << QStringLiteral("%1 [%2] %3")
                      .arg(phase.id,
                           phase.agent.isEmpty() ? phase.type : phase.agent,
                           phase.prompt.simplified());
    }
    return phases;
}

QString workflowDetailsMarkdown(const ai::WorkflowTemplate& workflow) {
    QStringList lines;
    lines << QStringLiteral("# %1").arg(workflow.title) << QString();
    appendWorkflowTextSection(&lines,
                              QStringLiteral("Required Inputs"),
                              workflowInputLines(workflow));
    appendWorkflowTextSection(&lines,
                              QStringLiteral("Required Tools / Software"),
                              workflowToolLines(workflow));
    appendWorkflowTextSection(&lines, QStringLiteral("Agents"), workflowAgentLines(workflow));
    appendWorkflowTextSection(&lines, QStringLiteral("Phases"), workflowPhaseLines(workflow));
    appendWorkflowTextSection(&lines,
                              QStringLiteral("Acceptance Criteria"),
                              workflow.acceptance_criteria);
    appendWorkflowTextSection(&lines, QStringLiteral("Instruction Files"), workflow.instructions);
    appendWorkflowTextSection(&lines, QStringLiteral("Skills"), workflow.skills);
    return lines.join(QLatin1Char('\n'));
}

QString workflowDetailsHtml(const ai::WorkflowTemplate& workflow) {
    QTextDocument document;
    document.setDefaultStyleSheet(sak::ui::aiWorkflowMarkdownStyle());
    document.setMarkdown(workflowDetailsMarkdown(workflow));
    return document.toHtml();
}

QScrollArea* createScrollArea(QWidget* parent, QWidget* content) {
    auto* scroll = new QScrollArea(parent);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidget(content);
    return scroll;
}

bool widgetOwnsFocus(const QWidget* widget) {
    if (!widget) {
        return false;
    }
    const QWidget* focus_widget = QApplication::focusWidget();
    return widget->hasFocus() || (focus_widget && widget->isAncestorOf(focus_widget));
}

bool forwardWheelToAncestorScrollArea(QWidget* widget, QEvent* event) {
    if (!widget || !event || event->type() != QEvent::Wheel) {
        return false;
    }
    auto* wheel_event = static_cast<QWheelEvent*>(event);
    QWidget* parent = widget->parentWidget();
    while (parent) {
        if (auto* scroll_area = qobject_cast<QAbstractScrollArea*>(parent)) {
            QWidget* viewport = scroll_area->viewport();
            if (!viewport) {
                return false;
            }
            const QPointF viewport_pos =
                viewport->mapFromGlobal(wheel_event->globalPosition().toPoint());
            QWheelEvent forwarded(viewport_pos,
                                  wheel_event->globalPosition(),
                                  wheel_event->pixelDelta(),
                                  wheel_event->angleDelta(),
                                  wheel_event->buttons(),
                                  wheel_event->modifiers(),
                                  wheel_event->phase(),
                                  wheel_event->inverted(),
                                  wheel_event->source(),
                                  wheel_event->pointingDevice());
            QCoreApplication::sendEvent(viewport, &forwarded);
            return true;
        }
        parent = parent->parentWidget();
    }
    return false;
}

void requireFocusForWheel(QWidget* widget, QObject* owner) {
    if (!widget || !owner) {
        return;
    }
    widget->setFocusPolicy(Qt::StrongFocus);
    widget->setProperty(kWheelRequiresFocusProperty, true);
    widget->installEventFilter(owner);
}

void configureReadableCombo(QComboBox* combo,
                            int minimum_contents_length = kReadableComboDefaultContentsLength,
                            int popup_min_width = kReadableComboDefaultPopupMinWidth) {
    if (!combo) {
        return;
    }
    combo->setMinimumHeight(kReadableComboMinHeight);
    combo->setMinimumContentsLength(minimum_contents_length);
    combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    combo->setMaxVisibleItems(kReadableComboMaxVisibleItems);
    if (auto* view = combo->view()) {
        view->setMinimumWidth(popup_min_width);
        view->setTextElideMode(Qt::ElideNone);
        view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    }
}

QString workbenchListStyle() {
    return sak::ui::aiWorkbenchListStyle();
}

QString composerEditStyle() {
    return sak::ui::aiComposerEditStyle();
}

void configureCompactButton(QPushButton* button, const QString& icon_path = {}) {
    if (!button) {
        return;
    }
    button->setMinimumHeight(sak::ui::kUiButtonHeightDialog);
    button->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    if (!icon_path.isEmpty()) {
        button->setIcon(QIcon(icon_path));
        button->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
    }
}

QLabel* makeRailTitle(QWidget* parent, const QString& text) {
    auto* label = new QLabel(text, parent);
    label->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(kFontWeightBold, sak::ui::kColorTextHeading));
    return label;
}

}  // namespace

AiAssistantPanel::AiAssistantPanel(QWidget* parent)
    : QWidget(parent)
    , m_client(std::make_unique<ai::OpenAIResponsesClient>(this))
    , m_credentialStore(std::make_unique<ai::CredentialStore>())
    , m_conversationStore(std::make_unique<ai::ConversationStore>())
    , m_elevationBroker(std::make_unique<ElevationBroker>(this))
    , m_executionBroker(std::make_unique<ai::ExecutionBroker>(this))
    , m_humanGateStore(std::make_unique<ai::AiHumanGateStore>())
    , m_tokenTracker(std::make_unique<ai::TokenUsageTracker>())
    , m_traceStore(std::make_unique<ai::TraceStore>())
    , m_runStateStore(std::make_unique<ai::AiRunStateStore>())
    , m_leaseManager(std::make_unique<ai::AiLeaseManager>())
    , m_toolHealthLedger(std::make_unique<ai::AiToolHealthLedger>())
    , m_toolDispatcher(std::make_unique<ai::AiToolDispatcher>())
    , m_workflowStore(std::make_unique<ai::WorkflowStore>())
    , m_chocoManager(std::make_unique<ChocolateyManager>(this))
    , m_packageListManager(std::make_unique<PackageListManager>())
    , m_offlineWorker(std::make_unique<OfflineDeploymentWorker>(this)) {
    configureExecutionBrokers();
    m_taskStatus = tr("Idle");
    if (initializeAccessibilityAuditUi()) {
        return;
    }

    QStringList workflow_errors;
    const bool workflows_loaded = loadWorkflowDefaults(&workflow_errors);
    const QString health_ledger_error = initializeToolHealthLedger();
    registerToolDispatcherHandlers();
    initializeStandardPanel(workflows_loaded, workflow_errors, health_ledger_error);
    initializePackageManager();
    logInfo("AiAssistantPanel initialized");
}

void AiAssistantPanel::configureExecutionBrokers() {
    m_executionBroker->setElevatedRunner(
        [this](const ai::AiCommandRequest& request) { return runElevatedPowerShell(request); });
    m_executionBroker->setElevatedCancel([this]() {
        if (m_elevationBroker) {
            m_elevationBroker->cancelCurrentTask();
        }
    });
}

bool AiAssistantPanel::initializeAccessibilityAuditUi() {
    const bool accessibility_audit = qApp->property("sakAccessibilityAudit").toBool();
    if (!accessibility_audit) {
        return false;
    }
    setupUi();
    connectAiClient();
    updateCredentialControls();
    updateTokenLabels();
    updateAccessStatus();
    logInfo("AiAssistantPanel initialized");
    return true;
}

bool AiAssistantPanel::loadWorkflowDefaults(QStringList* workflow_errors) {
    return m_workflowStore && m_workflowStore->loadDefaults(workflow_errors);
}

QString AiAssistantPanel::initializeToolHealthLedger() {
    QString health_ledger_error;
    if (m_toolHealthLedger) {
        const QString path =
            QDir(sak::app_paths::dataRoot()).filePath(QStringLiteral("ai/tool_health_ledger.json"));
        m_toolHealthLedger->setPersistencePath(path, kToolHealthLedgerRetentionHours);
        (void)m_toolHealthLedger->load(&health_ledger_error);
    }
    return health_ledger_error;
}

void AiAssistantPanel::initializeStandardPanel(bool workflows_loaded,
                                               const QStringList& workflow_errors,
                                               const QString& health_ledger_error) {
    setupUi();
    connectAiClient();
    loadRememberedApiKey();
    updateCredentialControls();
    updateTokenLabels();
    refreshContextList();
    updateAccessStatus();
    m_pendingSessionTitle = tr("AI Session");
    reloadSessionPicker();
    refreshTraceStoreForSession();
    appendLocalEvent(tr("AI Assistant panel initialized"));
    if (!health_ledger_error.isEmpty()) {
        appendLocalEvent(tr("AI tool health ledger load failed: %1").arg(health_ledger_error));
    }
    if (!workflows_loaded) {
        appendLocalEvent(tr("AI workflow catalog loaded with errors"));
    }
    for (const auto& error : workflow_errors) {
        appendLocalEvent(tr("Workflow catalog: %1").arg(error));
    }
}

void AiAssistantPanel::initializePackageManager() {
    const QString choco_path =
        QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("tools/chocolatey"));
    if (m_chocoManager->initialize(choco_path)) {
        appendLocalEvent(tr("SAK package manager ready"));
    } else {
        appendLocalEvent(tr("SAK package manager unavailable at %1").arg(choco_path));
    }
}

AiAssistantPanel::~AiAssistantPanel() {
    if (m_runToken.isValid()) {
        m_runToken.cancel(QStringLiteral("panel_destroyed"));
    }
    if (m_executionBroker && m_executionBroker->isRunning()) {
        m_executionBroker->cancel();
    }
    if (m_offlineWorker && m_offlineWorker->isRunning()) {
        m_offlineWorker->cancel();
    }
    if (m_client) {
        m_client->cancel();
    }
    if (!m_currentCommandLeaseId.isEmpty() && m_leaseManager) {
        m_leaseManager->release(m_currentCommandLeaseId);
        m_currentCommandLeaseId.clear();
    }
    if (m_workflowRunWatcher) {
        m_workflowRunWatcher->disconnect(this);
        m_workflowRunWatcher->deleteLater();
        m_workflowRunWatcher = nullptr;
    }
}

QString AiAssistantPanel::statusDetails() const {
    const QString task = m_taskStatus.isEmpty() ? tr("Idle") : m_taskStatus;
    const QString key = ai::OpenAIResponsesClient::hasUsableApiKey(apiKey()) ? tr("Loaded")
                                                                             : tr("No key");
    QString access;
    switch (currentAccessMode()) {
    case AccessMode::ChatAndResearch:
        access = tr("Research");
        break;
    case AccessMode::UnattendedFullAccess:
        access = tr("Unattended");
        break;
    case AccessMode::AssistedFullAccess:
    default:
        access = tr("Assisted");
        break;
    }
    const QString turn = m_tokenTracker
                             ? ai::TokenUsageTracker::formatTurn(m_tokenTracker->lastTurn())
                             : tr("0 total");
    const QString session =
        m_tokenTracker ? ai::TokenUsageTracker::formatSession(m_tokenTracker->sessionTotal())
                       : tr("0 total");
    const QString phase = m_runState.phase_id.isEmpty() ? tr("none") : m_runState.phase_id;
    const QString context = contextWindowStatusText();
    return tr("AI: %1 | Key: %2 | Access: %3 | Phase: %4 | Tokens: %5 / %6 | Context: %7 | "
              "Tools: %8 (%9 active) | Agents: %10/%11")
        .arg(task, key, access, phase, turn, session)
        .arg(context)
        .arg(m_toolCallsThisSession)
        .arg(m_runState.active_tools)
        .arg(m_runState.active_subagents)
        .arg(m_runState.completed_subagents);
}

bool AiAssistantPanel::filterWheelEvent(QObject* watched, QEvent* event) {
    if (event->type() != QEvent::Wheel || watched == nullptr ||
        !watched->property(kWheelRequiresFocusProperty).toBool()) {
        return false;
    }
    auto* widget = qobject_cast<QWidget*>(watched);
    if (widget == nullptr || widgetOwnsFocus(widget)) {
        return false;
    }
    if (forwardWheelToAncestorScrollArea(widget, event)) {
        return true;
    }
    event->ignore();
    return true;
}

bool AiAssistantPanel::filterMessageEditKeyPress(QEvent* event) {
    if (event->type() != QEvent::KeyPress) {
        return false;
    }
    const auto* key_event = static_cast<QKeyEvent*>(event);
    return handleComposerKeyAction(static_cast<int>(composerKeyAction(*key_event)));
}

bool AiAssistantPanel::handleComposerKeyAction(int action) {
    const auto composer_action = static_cast<ComposerKeyAction>(action);
    if (composer_action == ComposerKeyAction::InsertNewline) {
        if (m_messageEdit) {
            m_messageEdit->insertPlainText(QStringLiteral("\n"));
        }
        return true;
    }
    if (composer_action == ComposerKeyAction::Send) {
        onSendClicked();
        return true;
    }
    return handleComposerHistoryKeyAction(action);
}

bool AiAssistantPanel::handleComposerHistoryKeyAction(int action) {
    const auto composer_action = static_cast<ComposerKeyAction>(action);
    if (composer_action == ComposerKeyAction::PreviousHistory && m_messageEdit &&
        m_messageEdit->textCursor().atStart()) {
        return cyclePromptHistory(-1);
    }
    if (composer_action == ComposerKeyAction::NextHistory && m_messageEdit &&
        m_messageEdit->textCursor().atEnd()) {
        return cyclePromptHistory(1);
    }
    return false;
}

bool AiAssistantPanel::eventFilter(QObject* watched, QEvent* event) {
    if (filterWheelEvent(watched, event)) {
        return true;
    }

    if (watched == m_messageEdit) {
        if (filterMessageEditKeyPress(event)) {
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void AiAssistantPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);

    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium,
                                   sak::ui::kMarginMedium);
    rootLayout->setSpacing(sak::ui::kSpacingDefault);

    m_headerWidgets = sak::createDynamicPanelHeader(
        this,
        QStringLiteral(":/icons/icons/panel_ai.svg"),
        tr("AI Assistant"),
        tr("OpenAI-powered technician assistant with controlled local execution"),
        rootLayout);

    auto* workspace = createChatWorkspace();
    setAccessible(workspace, tr("AI Assistant workspace"), tr("AI assistant chat workspace"));
    rootLayout->addWidget(workspace, 1);

    createStatusStrip(rootLayout);
}

QWidget* AiAssistantPanel::createChatWorkspace() {
    auto* content = new QWidget(this);
    content->setStyleSheet(sak::ui::bareBackgroundStyle(sak::ui::kColorBgWhite));
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(sak::ui::kSpacingNone);

    auto* splitter = new QSplitter(Qt::Horizontal, content);
    splitter->setChildrenCollapsible(false);
    splitter->addWidget(createConversationPane());
    splitter->addWidget(createContextPane());
    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 0);
    splitter->setSizes({kContextSplitterConversationWidth, kContextSplitterRailWidth});
    layout->addWidget(splitter, 1);

    return content;
}

QWidget* AiAssistantPanel::createContextPane() {
    auto* pane = new QFrame(this);
    pane->setObjectName(QStringLiteral("aiContextPane"));
    pane->setMinimumWidth(kContextPaneMinWidth);
    pane->setMaximumWidth(kContextPaneMaxWidth);
    pane->setStyleSheet(sak::ui::aiContextPaneStyle());
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium);
    layout->setSpacing(sak::ui::kSpacingSmall);

    setupContextPaneSessionSection(layout, pane);
    setupContextPaneAgentSection(layout, pane);
    setupContextPaneAccessSection(layout, pane);
    setupContextPaneCredentialSection(layout, pane);
    layout->addStretch();

    auto* scroll = createScrollArea(this, pane);
    scroll->setObjectName(QStringLiteral("aiContextScroll"));
    scroll->setMinimumWidth(kContextScrollMinWidth);
    scroll->setMaximumWidth(kContextScrollMaxWidth);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    scroll->setStyleSheet(
        sak::ui::backgroundStyle("QScrollArea#aiContextScroll", sak::ui::kColorBgSurface));
    return scroll;
}

void AiAssistantPanel::setupContextPaneSessionSection(QVBoxLayout* layout, QWidget* pane) {
    layout->addWidget(makeRailTitle(pane, tr("Session")));
    layout->addWidget(makeFieldLabel(pane, tr("Chat session")));

    m_sessionCombo = new QComboBox(pane);
    configureReadableCombo(m_sessionCombo, kSessionComboContentsLength, kSessionComboPopupMinWidth);
    requireFocusForWheel(m_sessionCombo, this);
    m_sessionCombo->setToolTip(tr("Switch between saved AI chat sessions"));
    setAccessible(m_sessionCombo, tr("AI session"), tr("Open or switch AI chat session"));
    connect(m_sessionCombo,
            &QComboBox::currentIndexChanged,
            this,
            &AiAssistantPanel::onSessionSelected);
    layout->addWidget(m_sessionCombo);

    m_newSessionButton = new QPushButton(tr("New Chat"), pane);
    configureCompactButton(m_newSessionButton, QStringLiteral(":/icons/icons/icons8-new-chat.svg"));
    m_newSessionButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_newSessionButton->setToolTip(tr("Start a new AI chat session"));
    setAccessible(m_newSessionButton, tr("Start new AI session"));
    connect(
        m_newSessionButton, &QPushButton::clicked, this, &AiAssistantPanel::onNewSessionClicked);

    m_renameSessionButton = new QPushButton(tr("Rename Chat"), pane);
    configureCompactButton(m_renameSessionButton,
                           QStringLiteral(":/icons/icons/icons8-rename.svg"));
    m_renameSessionButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_renameSessionButton->setToolTip(
        tr("Rename the current chat session and its active artifact folder"));
    setAccessible(m_renameSessionButton, tr("Rename current AI chat"));
    connect(m_renameSessionButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::onRenameSessionClicked);

    auto* sessionActionRow = new QHBoxLayout();
    sessionActionRow->setSpacing(sak::ui::kSpacingSmall);
    sessionActionRow->addWidget(m_newSessionButton, 1);
    sessionActionRow->addWidget(m_renameSessionButton, 1);
    layout->addLayout(sessionActionRow);

    m_resumeGateButton = new QPushButton(tr("Resume Waiting Run"), pane);
    configureCompactButton(m_resumeGateButton, QStringLiteral(":/icons/icons/icons8-sent.svg"));
    m_resumeGateButton->setToolTip(
        tr("Answer the pending workflow question and continue the same AI run"));
    setAccessible(m_resumeGateButton,
                  tr("Resume waiting AI run"),
                  tr("Answer pending workflow input and continue"));
    connect(
        m_resumeGateButton, &QPushButton::clicked, this, &AiAssistantPanel::onResumeGateClicked);
    m_resumeGateButton->setVisible(false);
    layout->addWidget(m_resumeGateButton);
}

void AiAssistantPanel::setupContextPaneAgentSection(QVBoxLayout* layout, QWidget* pane) {
    layout->addSpacing(sak::ui::kSpacingMedium);
    layout->addWidget(makeRailTitle(pane, tr("GPT Assistant")));

    layout->addWidget(makeFieldLabel(pane, tr("Model")));
    m_modelCombo = new QComboBox(pane);
    configureReadableCombo(m_modelCombo);
    requireFocusForWheel(m_modelCombo, this);
    m_modelCombo->setEditable(true);
    m_modelCombo->addItems(
        {QStringLiteral("gpt-5.5"), QStringLiteral("gpt-5.4"), QStringLiteral("gpt-5.4-mini")});
    m_modelCombo->setToolTip(tr("Choose or type the OpenAI model for this session"));
    setAccessible(m_modelCombo, tr("AI model"), tr("OpenAI model for the active session"));
    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this]() {
        scheduleContextTokenRefresh();
        updateRunTelemetryLabels();
    });
    layout->addWidget(m_modelCombo);

    layout->addWidget(makeFieldLabel(pane, tr("Session role")));
    m_sessionRoleValueLabel = new QLabel(pane);
    m_sessionRoleValueLabel->setWordWrap(true);
    m_sessionRoleValueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_sessionRoleValueLabel->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextBody, sak::ui::kFontSizeBody));
    setAccessible(m_sessionRoleValueLabel,
                  tr("Session role"),
                  tr("Technician role inferred from workflow or first prompt"));
    layout->addWidget(m_sessionRoleValueLabel);
    resetSessionRole();
    setupContextPaneWorkflowPicker(layout, pane);
    setupContextPaneWorkflowDetails(layout, pane);

    refreshPromptTemplates();

    layout->addWidget(makeFieldLabel(pane, tr("Reasoning effort")));
    m_reasoningEffortCombo = new QComboBox(pane);
    configureReadableCombo(m_reasoningEffortCombo,
                           kReasoningComboContentsLength,
                           kReasoningComboPopupMinWidth);
    requireFocusForWheel(m_reasoningEffortCombo, this);
    m_reasoningEffortCombo->addItems({tr("Low"), tr("Medium"), tr("High")});
    m_reasoningEffortCombo->setCurrentIndex(1);
    m_reasoningEffortCombo->setToolTip(tr("Set reasoning effort for supported OpenAI models"));
    setAccessible(m_reasoningEffortCombo,
                  tr("Reasoning effort"),
                  tr("Reasoning effort sent to supported models"));
    layout->addWidget(m_reasoningEffortCombo);
}

void AiAssistantPanel::setupContextPaneWorkflowPicker(QVBoxLayout* layout, QWidget* pane) {
    layout->addWidget(makeFieldLabel(pane, tr("Workflow")));
    m_promptTemplateCombo = new QComboBox(pane);
    configureReadableCombo(m_promptTemplateCombo,
                           kWorkflowComboContentsLength,
                           kWorkflowComboPopupMinWidth);
    requireFocusForWheel(m_promptTemplateCombo, this);
    m_promptTemplateCombo->setToolTip(
        tr("Choose a multi-step workflow. The list shows category, job name, and phase count."));
    setAccessible(m_promptTemplateCombo,
                  tr("Workflow library"),
                  tr("AI workflow templates for technician tasks"));
    connect(m_promptTemplateCombo,
            &QComboBox::currentIndexChanged,
            this,
            &AiAssistantPanel::onWorkflowTemplatePickerChanged);
    auto* workflowRow = new QHBoxLayout();
    workflowRow->setSpacing(sak::ui::kSpacingSmall);
    workflowRow->addWidget(m_promptTemplateCombo, 1);

    m_addWorkflowButton = new QPushButton(tr("Add"), pane);
    configureCompactButton(m_addWorkflowButton,
                           QStringLiteral(":/icons/icons/icons8-add-to-chat.svg"));
    m_addWorkflowButton->setToolTip(
        tr("Attach the selected workflow and its instruction and skill files to the next message"));
    setAccessible(m_addWorkflowButton, tr("Add selected workflow"));
    connect(
        m_addWorkflowButton, &QPushButton::clicked, this, &AiAssistantPanel::onAddWorkflowClicked);
    workflowRow->addWidget(m_addWorkflowButton);

    layout->addLayout(workflowRow);
}

void AiAssistantPanel::onWorkflowTemplatePickerChanged(int index) {
    const bool has_selection = index > 0;
    if (m_addWorkflowButton) {
        m_addWorkflowButton->setEnabled(has_selection);
    }
    if (!has_selection) {
        clearWorkflowSelectionPreview();
        return;
    }
    if (!m_workflowStore || !m_promptTemplateCombo) {
        return;
    }
    const QString item_data = m_promptTemplateCombo->itemData(index).toString();
    if (const auto* workflow = m_workflowStore->workflowById(item_data)) {
        previewWorkflowTemplateSelection(*workflow);
        return;
    }
    showUnstructuredWorkflowTemplateSelection(index);
}

void AiAssistantPanel::clearWorkflowSelectionPreview() {
    hideWorkflowDetails();
    if (m_sessionRoleSource == QLatin1String(kSessionRoleSourceWorkflowSelection)) {
        resetSessionRole();
        return;
    }
    updateSessionRoleDisplay();
}

void AiAssistantPanel::previewWorkflowTemplateSelection(const ai::WorkflowTemplate& workflow) {
    if (!workflow.role.trimmed().isEmpty()) {
        setSessionRole(workflow.role.trimmed(),
                       QString::fromLatin1(kSessionRoleSourceWorkflowSelection),
                       false);
    }
    showWorkflowDetails(workflow);
}

void AiAssistantPanel::showUnstructuredWorkflowTemplateSelection(int index) {
    if (!m_promptTemplateCombo || !m_workflowDetailsPanel || !m_workflowDetailsBody ||
        !m_workflowDetailsTitle) {
        return;
    }
    m_workflowDetailsTitle->setText(m_promptTemplateCombo->itemText(index));
    m_workflowDetailsBody->setPlainText(tr("No structured details available for this entry."));
    m_workflowDetailsPanel->setVisible(true);
    m_workflowDetailsCurrentId.clear();
}

void AiAssistantPanel::setupContextPaneWorkflowDetails(QVBoxLayout* layout, QWidget* pane) {
    m_workflowDetailsPanel = new QFrame(pane);
    m_workflowDetailsPanel->setObjectName(QStringLiteral("aiWorkflowDetailsPanel"));
    m_workflowDetailsPanel->setStyleSheet(sak::ui::aiWorkflowDetailsPanelStyle());
    m_workflowDetailsPanel->setVisible(false);
    auto* detailsLayout = new QVBoxLayout(m_workflowDetailsPanel);
    detailsLayout->setContentsMargins(
        sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall, sak::ui::kMarginSmall);
    detailsLayout->setSpacing(sak::ui::kSpacingTight);
    auto* detailsHeaderRow = new QHBoxLayout();
    detailsHeaderRow->setSpacing(sak::ui::kSpacingSmall);
    m_workflowDetailsTitle = new QLabel(tr("Workflow"), m_workflowDetailsPanel);
    m_workflowDetailsTitle->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(kFontWeightBold, sak::ui::kColorTextHeading));
    detailsHeaderRow->addWidget(m_workflowDetailsTitle, 1);
    m_workflowDetailsCloseButton = new QPushButton(tr("Close"), m_workflowDetailsPanel);
    configureCompactButton(m_workflowDetailsCloseButton,
                           QStringLiteral(":/icons/icons/icons8-close-window.svg"));
    m_workflowDetailsCloseButton->setToolTip(tr("Hide workflow details panel"));
    setAccessible(m_workflowDetailsCloseButton, tr("Hide workflow details"));
    connect(m_workflowDetailsCloseButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::hideWorkflowDetails);
    detailsHeaderRow->addWidget(m_workflowDetailsCloseButton);
    detailsLayout->addLayout(detailsHeaderRow);

    m_workflowDetailsBody = new QTextBrowser(m_workflowDetailsPanel);
    m_workflowDetailsBody->setReadOnly(true);
    m_workflowDetailsBody->setOpenExternalLinks(false);
    m_workflowDetailsBody->setMinimumHeight(kWorkflowDetailsMinHeight);
    m_workflowDetailsBody->setMaximumHeight(kWorkflowDetailsMaxHeight);
    m_workflowDetailsBody->setStyleSheet(sak::ui::aiWorkflowDetailsBodyStyle());
    setAccessible(m_workflowDetailsBody, tr("Workflow details preview"));
    requireFocusForWheel(m_workflowDetailsBody, this);
    detailsLayout->addWidget(m_workflowDetailsBody);
    layout->addWidget(m_workflowDetailsPanel);
}

void AiAssistantPanel::setupContextPaneAccessSection(QVBoxLayout* layout, QWidget* pane) {
    layout->addSpacing(sak::ui::kSpacingMedium);
    layout->addWidget(makeRailTitle(pane, tr("Access")));

    layout->addWidget(makeFieldLabel(pane, tr("Local tool access")));
    m_accessModeCombo = new QComboBox(pane);
    configureReadableCombo(m_accessModeCombo);
    requireFocusForWheel(m_accessModeCombo, this);
    m_accessModeCombo->addItems(
        {tr("Chat and Research"), tr("Assisted Full Access"), tr("Unattended Full Access")});
    m_accessModeCombo->setCurrentIndex(1);
    m_accessModeCombo->setToolTip(
        tr("Choose whether the assistant may use local PC execution tools"));
    setAccessible(m_accessModeCombo,
                  tr("AI access mode"),
                  tr("Controls whether the assistant can request local execution"));
    connect(m_accessModeCombo,
            &QComboBox::currentIndexChanged,
            this,
            &AiAssistantPanel::onAccessModeChanged);
    layout->addWidget(m_accessModeCombo);

    m_accessStatusLabel = new QLabel(pane);
    m_accessStatusLabel->setToolTip(tr("Current local execution mode"));
    setAccessible(m_accessStatusLabel, tr("Access mode status"));
    layout->addWidget(m_accessStatusLabel);
}

void AiAssistantPanel::setupContextPaneCredentialSection(QVBoxLayout* layout, QWidget* pane) {
    layout->addSpacing(sak::ui::kSpacingMedium);
    auto* keyRow = new QHBoxLayout();
    keyRow->setSpacing(sak::ui::kSpacingSmall);
    keyRow->addWidget(makeRailTitle(pane, tr("OpenAI API Key")));
    keyRow->addStretch();
    m_loadKeyButton = new QPushButton(tr("Load Key"), pane);
    configureCompactButton(m_loadKeyButton);
    m_loadKeyButton->setToolTip(tr("Enter an OpenAI API key without displaying it in the panel"));
    setAccessible(m_loadKeyButton, tr("Load OpenAI API key"));
    connect(m_loadKeyButton, &QPushButton::clicked, this, &AiAssistantPanel::onLoadApiKeyClicked);
    keyRow->addWidget(m_loadKeyButton);
    layout->addLayout(keyRow);

    auto* keyStatusRow = new QHBoxLayout();
    keyStatusRow->setSpacing(sak::ui::kSpacingTight);
    m_connectionStatusIconLabel = new QLabel(pane);
    m_connectionStatusIconLabel->setFixedWidth(kConnectionStatusIconWidth);
    m_connectionStatusIconLabel->setAlignment(Qt::AlignCenter);
    m_connectionStatusIconLabel->setToolTip(tr("OpenAI API key status indicator"));
    setAccessible(m_connectionStatusIconLabel, tr("OpenAI key status indicator"));
    keyStatusRow->addWidget(m_connectionStatusIconLabel);

    m_connectionStatusLabel = new QLabel(pane);
    m_connectionStatusLabel->setToolTip(tr("OpenAI API key status"));
    setAccessible(m_connectionStatusLabel, tr("AI connection status"));
    keyStatusRow->addWidget(m_connectionStatusLabel, 1);
    layout->addLayout(keyStatusRow);
    setApiKeyStatus(tr("Not loaded"),
                    sak::ui::kStatusColorError,
                    QString(QChar(kStatusMarkerError)),
                    sak::ui::kStatusColorError);
}

QWidget* AiAssistantPanel::createConversationPane() {
    auto* pane = new QFrame(this);
    pane->setObjectName(QStringLiteral("aiConversationPane"));
    pane->setStyleSheet(sak::ui::aiConversationPaneStyle());
    auto* layout = new QVBoxLayout(pane);
    layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);
    layout->setSpacing(sak::ui::kSpacingNone);

    setupConversationHeader(layout, pane);
    setupConversationTranscript(layout, pane);
    setupConversationComposer(layout, pane);
    return pane;
}

void AiAssistantPanel::setupConversationHeader(QVBoxLayout* layout, QWidget* pane) {
    auto* chatHeader = new QFrame(pane);
    chatHeader->setObjectName(QStringLiteral("aiConversationHeader"));
    chatHeader->setStyleSheet(sak::ui::aiConversationHeaderStyle());
    auto* chatHeaderLayout = new QVBoxLayout(chatHeader);
    chatHeaderLayout->setContentsMargins(sak::ui::kMarginMedium,
                                         sak::ui::kMarginTight,
                                         sak::ui::kMarginMedium,
                                         sak::ui::kMarginTight);
    chatHeaderLayout->setSpacing(sak::ui::kSpacingTight);

    setupConversationStatusRow(chatHeaderLayout, chatHeader);
    setupConversationHeaderActions(chatHeaderLayout, chatHeader);
    layout->addWidget(chatHeader);
}

void AiAssistantPanel::setupConversationStatusRow(QVBoxLayout* layout, QWidget* header) {
    auto* statusRow = new QHBoxLayout();
    statusRow->setSpacing(sak::ui::kSpacingSmall);
    statusRow->addWidget(makeRailTitle(header, tr("Chat")));

    m_workflowProgressBar = new QProgressBar(header);
    m_workflowProgressBar->setVisible(false);
    m_workflowProgressBar->setTextVisible(true);
    m_workflowProgressBar->setMinimumWidth(kWorkflowProgressMinWidth);
    m_workflowProgressBar->setMaximumWidth(kWorkflowProgressMaxWidth);
    m_workflowProgressBar->setFixedHeight(sak::ui::kUiProgressBarHeight);
    m_workflowProgressBar->setRange(0, 1);
    m_workflowProgressBar->setValue(0);
    m_workflowProgressBar->setFormat(tr("Workflow idle"));
    m_workflowProgressBar->setToolTip(
        tr("Live workflow progress. Shows the active phase and completed phase count."));
    m_workflowProgressBar->setStyleSheet(sak::ui::aiWorkflowProgressStyle());
    setAccessible(m_workflowProgressBar,
                  tr("AI workflow progress"),
                  tr("Shows live workflow phase progress while a multi-agent workflow runs"));
    statusRow->addWidget(m_workflowProgressBar, 1);

    m_agentActivityLabel = new QLabel(header);
    m_agentActivityLabel->setMinimumWidth(kAgentActivityLabelMinWidth);
    m_agentActivityLabel->setAlignment(Qt::AlignCenter);
    m_agentActivityLabel->setToolTip(tr("Background subagent activity for the current run"));
    m_agentActivityLabel->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(kFontWeightBold, sak::ui::kColorTextMuted));
    setAccessible(m_agentActivityLabel,
                  tr("AI background agents"),
                  tr("Shows active and completed background subagents"));
    statusRow->addWidget(m_agentActivityLabel);
    updateRunTelemetryLabels();
    layout->addLayout(statusRow);
}

void AiAssistantPanel::setupConversationHeaderActions(QVBoxLayout* layout, QWidget* header) {
    auto* headerActionRow = new QHBoxLayout();
    headerActionRow->setSpacing(sak::ui::kSpacingSmall);
    headerActionRow->addStretch();

    m_runDetailsButton = new QPushButton(tr("Details"), header);
    configureCompactButton(m_runDetailsButton, QStringLiteral(":/icons/icons/panel_about.svg"));
    m_runDetailsButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_runDetailsButton->setToolTip(
        tr("Open a modal with the current or most recent AI run state, phases, and activity"));
    setAccessible(m_runDetailsButton,
                  tr("AI run details"),
                  tr("Show current run state and recent AI activity"));
    connect(
        m_runDetailsButton, &QPushButton::clicked, this, &AiAssistantPanel::onRunDetailsClicked);
    headerActionRow->addWidget(m_runDetailsButton);

    m_artifactsButton = new QPushButton(tr("Artifacts"), header);
    configureCompactButton(m_artifactsButton,
                           QStringLiteral(":/icons/icons/icons8-opened-folder.svg"));
    m_artifactsButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_artifactsButton->setToolTip(
        tr("Open the current AI session artifact directory when screenshots, downloads, bundles, "
           "or reports exist"));
    setAccessible(m_artifactsButton,
                  tr("Open AI artifacts folder"),
                  tr("Open the folder containing artifacts for the current AI session"));
    connect(m_artifactsButton, &QPushButton::clicked, this, &AiAssistantPanel::onArtifactsClicked);
    headerActionRow->addWidget(m_artifactsButton);
    refreshArtifactList();

    m_generateReportButton = new QPushButton(tr("Report"), header);
    configureCompactButton(m_generateReportButton,
                           QStringLiteral(":/icons/icons/icons8-export.svg"));
    m_generateReportButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_generateReportButton->setToolTip(
        tr("Export a report after the AI session has completed local actions or workflow checks"));
    setAccessible(m_generateReportButton, tr("Generate session report"));
    connect(m_generateReportButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::onGenerateReportClicked);
    headerActionRow->addWidget(m_generateReportButton);
    layout->addLayout(headerActionRow);
}

void AiAssistantPanel::setupConversationTranscript(QVBoxLayout* layout, QWidget* pane) {
    m_transcriptView = new AiTranscriptView(pane);
    m_transcriptView->setTextRedactor(ai::CredentialStore::redactSecrets);
    layout->addWidget(m_transcriptView, 1);
}

void AiAssistantPanel::setupConversationComposer(QVBoxLayout* layout, QWidget* pane) {
    auto* composer = new QFrame(pane);
    composer->setObjectName(QStringLiteral("aiComposer"));
    composer->setStyleSheet(sak::ui::aiComposerStyle());
    auto* composerLayout = new QVBoxLayout(composer);
    composerLayout->setContentsMargins(sak::ui::kMarginMedium,
                                       sak::ui::kMarginSmall,
                                       sak::ui::kMarginMedium,
                                       sak::ui::kMarginSmall);
    composerLayout->setSpacing(sak::ui::kSpacingSmall);

    setupComposerInput(composerLayout, composer);
    setupComposerContextList(composerLayout, composer);
    setupComposerActions(composerLayout, composer);
    layout->addWidget(composer);
}

void AiAssistantPanel::setupComposerInput(QVBoxLayout* layout, QWidget* composer) {
    m_messageEdit = new QPlainTextEdit(composer);
    m_messageEdit->setPlaceholderText(tr("Ask S.A.K. AI"));
    m_messageEdit->setToolTip(
        tr("Type your request. Press Enter to send, Ctrl+Enter for a new line. Up/Down at the "
           "start/end cycles prompt history."));
    m_messageEdit->setMinimumHeight(kMessageEditMinHeight);
    m_messageEdit->setMaximumHeight(kMessageEditMaxHeight);
    m_messageEdit->setFrameShape(QFrame::NoFrame);
    m_messageEdit->setStyleSheet(composerEditStyle());
    m_messageEdit->installEventFilter(this);
    setAccessible(m_messageEdit, tr("AI message input"));
    connect(m_messageEdit, &QPlainTextEdit::textChanged, this, [this]() {
        updateCredentialControls();
        scheduleContextTokenRefresh();
        updateRunTelemetryLabels();
    });
    layout->addWidget(m_messageEdit);
}

void AiAssistantPanel::setupComposerContextList(QVBoxLayout* layout, QWidget* composer) {
    m_contextList = new QListWidget(composer);
    m_contextList->setObjectName(QStringLiteral("aiContextChips"));
    m_contextList->setViewMode(QListView::IconMode);
    m_contextList->setFlow(QListView::LeftToRight);
    m_contextList->setWrapping(true);
    m_contextList->setResizeMode(QListView::Adjust);
    m_contextList->setMovement(QListView::Static);
    m_contextList->setSelectionMode(QAbstractItemView::NoSelection);
    m_contextList->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_contextList->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_contextList->setMinimumHeight(kContextListMinHeight);
    m_contextList->setMaximumHeight(kContextListMaxHeight);
    m_contextList->setGridSize(QSize());
    m_contextList->setSpacing(sak::ui::kSpacingTight);
    m_contextList->setStyleSheet(sak::ui::aiContextChipsListStyle());
    setAccessible(m_contextList, tr("AI context attachments"));
    requireFocusForWheel(m_contextList, this);
    m_contextList->hide();
    layout->addWidget(m_contextList);
}

void AiAssistantPanel::setupComposerActions(QVBoxLayout* layout, QWidget* composer) {
    auto* actionRow = new QHBoxLayout();
    actionRow->setSpacing(sak::ui::kSpacingSmall);
    setupComposerContextActions(actionRow, composer);
    actionRow->addStretch();
    setupComposerSendActions(actionRow, composer);
    updatePrimaryActionButton();
    layout->addLayout(actionRow);
}

void AiAssistantPanel::setupComposerContextActions(QHBoxLayout* actionRow, QWidget* composer) {
    m_addContextFilesButton = new QPushButton(tr("Attach"), composer);
    configureCompactButton(m_addContextFilesButton,
                           QStringLiteral(":/icons/icons/icons8-attachment.svg"));
    m_addContextFilesButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_addContextFilesButton->setToolTip(tr(
        "Attach files, screenshots, logs, scripts, or documents as evidence for the next request"));
    setAccessible(m_addContextFilesButton, tr("Add context files"));
    connect(m_addContextFilesButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::onAddContextFilesClicked);
    actionRow->addWidget(m_addContextFilesButton);

    m_addInstructionButton = new QPushButton(tr("Instructions"), composer);
    configureCompactButton(m_addInstructionButton,
                           QStringLiteral(":/icons/icons/icons8-instructions.svg"));
    m_addInstructionButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_addInstructionButton->setToolTip(tr("Add markdown instruction files"));
    setAccessible(m_addInstructionButton, tr("Add markdown instruction files"));
    connect(m_addInstructionButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::onAddInstructionContextClicked);
    actionRow->addWidget(m_addInstructionButton);

    m_clearContextButton = new QPushButton(tr("Clear"), composer);
    configureCompactButton(m_clearContextButton, QStringLiteral(":/icons/icons/icons8-trash.svg"));
    m_clearContextButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_clearContextButton->setEnabled(false);
    m_clearContextButton->setToolTip(tr("Remove all attached context and instruction chips"));
    setAccessible(m_clearContextButton, tr("Clear context"));
    connect(m_clearContextButton,
            &QPushButton::clicked,
            this,
            &AiAssistantPanel::onClearContextClicked);
    actionRow->addWidget(m_clearContextButton);
}

void AiAssistantPanel::setupComposerSendActions(QHBoxLayout* actionRow, QWidget* composer) {
    m_contextWindowLabel = new QLabel(composer);
    m_contextWindowLabel->setMinimumWidth(kContextWindowLabelMinWidth);
    m_contextWindowLabel->setAlignment(Qt::AlignCenter);
    m_contextWindowLabel->setToolTip(tr("Exact context window usage for the selected model"));
    m_contextWindowLabel->setStyleSheet(
        sak::ui::fontWeightAndColorStyle(kFontWeightBold, sak::ui::kColorTextMuted));
    setAccessible(m_contextWindowLabel,
                  tr("AI context window usage"),
                  tr("Shows exact input tokens used against selected model context window"));
    actionRow->addWidget(m_contextWindowLabel);

    m_sendButton = new QPushButton(tr("Send"), composer);
    m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-send.svg")));
    m_sendButton->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
    m_sendButton->setMinimumHeight(sak::ui::kUiButtonHeightMini);
    m_sendButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_sendButton->setToolTip(tr("Send the message to the AI assistant"));
    setAccessible(m_sendButton, tr("Send AI message"));
    connect(m_sendButton, &QPushButton::clicked, this, [this]() {
        if (isAiBusy()) {
            onStopClicked();
            return;
        }
        onSendClicked();
    });
    actionRow->addWidget(m_sendButton);
}

void AiAssistantPanel::createStatusStrip(QVBoxLayout* rootLayout) {
    Q_ASSERT(rootLayout);

    auto* statusRow = new QHBoxLayout();
    statusRow->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kSpacingTight, sak::ui::kMarginNone, sak::ui::kMarginNone);
    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    statusRow->addWidget(m_logToggle);
    statusRow->addStretch();
    rootLayout->addLayout(statusRow);
}

void AiAssistantPanel::connectAiClient() {
    connectOpenAiClientSignals();
    connectExecutionBrokerSignals();
    connectElevationBrokerSignals();
    ensureActivityTimer();
    ensureContextTokenTimer();
}

void AiAssistantPanel::connectOpenAiClientSignals() {
    qRegisterMetaType<sak::ai::AiCommandResult>("sak::ai::AiCommandResult");
    Q_ASSERT(m_client);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::requestStarted,
            this,
            &AiAssistantPanel::onRequestStarted);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::requestFinished,
            this,
            &AiAssistantPanel::onRequestFinished);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::responseReady,
            this,
            &AiAssistantPanel::onResponseReady);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::modelsReady,
            this,
            &AiAssistantPanel::onModelsReady);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::inputTokenCountReady,
            this,
            &AiAssistantPanel::onInputTokenCountReady);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::inputTokenCountFailed,
            this,
            &AiAssistantPanel::onInputTokenCountFailed);
    connect(m_client.get(),
            &ai::OpenAIResponsesClient::requestFailed,
            this,
            &AiAssistantPanel::onRequestFailed);
}

void AiAssistantPanel::connectExecutionBrokerSignals() {
    Q_ASSERT(m_executionBroker);
    connect(m_executionBroker.get(),
            &ai::ExecutionBroker::started,
            this,
            &AiAssistantPanel::onBrokerStarted);
    connect(m_executionBroker.get(),
            &ai::ExecutionBroker::stdoutChunk,
            this,
            &AiAssistantPanel::onBrokerStdoutChunk);
    connect(m_executionBroker.get(),
            &ai::ExecutionBroker::stderrChunk,
            this,
            &AiAssistantPanel::onBrokerStderrChunk);
    connect(m_executionBroker.get(),
            &ai::ExecutionBroker::finished,
            this,
            &AiAssistantPanel::onBrokerFinished);
}

void AiAssistantPanel::connectElevationBrokerSignals() {
    Q_ASSERT(m_elevationBroker);
    connect(m_elevationBroker.get(),
            &ElevationBroker::progressUpdated,
            this,
            [this](int percent, const QString& status) {
                handleElevationBrokerProgress(percent, status);
            });
}

void AiAssistantPanel::handleElevationBrokerProgress(int percent, const QString& status) {
    if (m_currentCommandId.isEmpty()) {
        return;
    }
    if (status.startsWith(QLatin1String("STDOUT|"))) {
        const QString chunk = status.mid(7);
        m_currentStdoutBuffer.append(chunk);
        emitPrefixedLogLines(
            [this](const QString& line) {
                Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line));
            },
            QStringLiteral("[%1 *ADMIN*]").arg(m_currentCommandId),
            chunk);
        return;
    }
    if (status.startsWith(QLatin1String("STDERR|"))) {
        const QString chunk = status.mid(7);
        m_currentStderrBuffer.append(chunk);
        emitPrefixedLogLines(
            [this](const QString& line) {
                Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line));
            },
            QStringLiteral("[%1 *ADMIN* err]").arg(m_currentCommandId),
            chunk);
        return;
    }
    Q_EMIT logOutput(ai::CredentialStore::redactSecrets(
        QStringLiteral("[%1 *ADMIN* %2%%] %3").arg(m_currentCommandId).arg(percent).arg(status)));
}

void AiAssistantPanel::ensureActivityTimer() {
    if (!m_activityTimer) {
        m_activityTimer = new QTimer(this);
        m_activityTimer->setInterval(kActivityTimerIntervalMs);
        connect(m_activityTimer,
                &QTimer::timeout,
                this,
                &AiAssistantPanel::updateActivityIndicatorFrame);
    }
}

void AiAssistantPanel::loadRememberedApiKey() {
    Q_ASSERT(m_credentialStore);
    QString error;
    const QString remembered_key = m_credentialStore->loadApiKey(&error);
    if (!remembered_key.isEmpty()) {
        m_apiKey = remembered_key;
        setApiKeyStatus(tr("Remembered key loaded"),
                        sak::ui::kStatusColorSuccess,
                        QString(QChar(kStatusMarkerSuccess)),
                        sak::ui::kStatusColorSuccess);
        appendLocalEvent(tr("Remembered OpenAI key loaded from encrypted app credential file"));
        scheduleContextTokenRefresh();
    } else if (!error.isEmpty()) {
        setApiKeyStatus(tr("Credential load failed"),
                        sak::ui::kStatusColorError,
                        QString(QChar(kStatusMarkerError)),
                        sak::ui::kStatusColorError);
        appendLocalEvent(tr("Credential load failed: %1").arg(error));
    }
}

void AiAssistantPanel::setApiKeyStatus(const QString& text,
                                       const char* color,
                                       const QString& marker,
                                       const char* marker_color) {
    if (m_connectionStatusIconLabel) {
        m_connectionStatusIconLabel->setText(marker);
        m_connectionStatusIconLabel->setStyleSheet(sak::ui::fontWeightAndColorStyle(
            kFontWeightExtraBold, QString::fromLatin1(marker_color)));
    }
    if (m_connectionStatusLabel) {
        m_connectionStatusLabel->setText(text);
        m_connectionStatusLabel->setStyleSheet(statusLabelStyle(color));
    }
    emitStatusDetails();
}

void AiAssistantPanel::updateLoadKeyButton(bool has_key, bool busy) {
    if (m_loadKeyButton) {
        m_loadKeyButton->setEnabled(!busy);
        m_loadKeyButton->setText(has_key ? tr("Clear Key") : tr("Load Key"));
        m_loadKeyButton->setIcon(QIcon());
        m_loadKeyButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
        m_loadKeyButton->setToolTip(
            has_key
                ? tr("Clear the loaded OpenAI API key from this session and encrypted app storage")
                : tr("Enter an OpenAI API key without displaying it in the panel"));
        setAccessible(m_loadKeyButton,
                      has_key ? tr("Clear OpenAI API key") : tr("Load OpenAI API key"));
    }
}

void AiAssistantPanel::updateReportButton(bool busy) {
    if (!m_generateReportButton) {
        return;
    }
    const bool reportable = hasReportableResults();
    m_generateReportButton->setEnabled(!busy && reportable);
    m_generateReportButton->setToolTip(
        reportable ? tr("Export the current AI session report as HTML, markdown, or plain text")
                   : tr("Run a local action or workflow and wait for results before exporting "
                        "a report"));
}

bool canResumeWorkflowInputGate(const ai::AiHumanGate& gate) {
    return gate.kind == QLatin1String("workflow_input") &&
           gate.name == QLatin1String("required_input");
}

bool canResumeApprovalGate(const ai::AiHumanGate& gate) {
    return gate.kind == QLatin1String("approval") &&
           (gate.name == QLatin1String("command_approval") ||
            gate.name == QLatin1String("restore_point_offer") ||
            gate.name == QLatin1String("restore_point_failure_continue"));
}

bool canResumeRecoveryGate(const ai::AiHumanGate& gate) {
    return gate.kind == QLatin1String("workflow_recovery") &&
           gate.name == QLatin1String("phase_needs_human");
}

bool canResumeHumanGate(const ai::AiHumanGate& gate) {
    return gate.isPending() && (canResumeWorkflowInputGate(gate) || canResumeApprovalGate(gate) ||
                                canResumeRecoveryGate(gate));
}

void AiAssistantPanel::updateResumeGateButton(bool has_key, bool busy) {
    if (!m_resumeGateButton) {
        return;
    }
    const auto& gate = m_runState.pending_human_gate;
    const bool has_pending_gate = m_runState.has_pending_human_gate && gate.isPending();
    const bool can_resume = has_pending_gate && canResumeHumanGate(gate);
    m_resumeGateButton->setVisible(has_pending_gate);
    m_resumeGateButton->setEnabled(has_key && !busy && can_resume);
    m_resumeGateButton->setToolTip(
        can_resume ? tr("Answer the pending AI gate and continue the same run when possible")
                   : tr("Open Run Details to review the pending human gate"));
}

void AiAssistantPanel::updateCredentialControls() {
    const bool has_key = ai::OpenAIResponsesClient::hasUsableApiKey(apiKey());
    const bool busy = isAiBusy();
    updateLoadKeyButton(has_key, busy);
    if (m_newSessionButton) {
        m_newSessionButton->setEnabled(!busy);
        m_newSessionButton->setToolTip(busy
                                           ? tr("Stop the active AI run before starting a new chat")
                                           : tr("Start a new AI chat session"));
    }
    if (m_renameSessionButton) {
        const bool has_persistent_session = m_conversationStore &&
                                            !m_conversationStore->currentSessionId().isEmpty();
        m_renameSessionButton->setEnabled(!busy && has_persistent_session);
        m_renameSessionButton->setToolTip(has_persistent_session
                                              ? tr("Rename the current saved AI chat")
                                              : tr("Send a prompt before renaming this chat"));
    }
    updatePrimaryActionButton();
    updateReportButton(busy);
    updateResumeGateButton(has_key, busy);
}

void AiAssistantPanel::updatePrimaryActionButton() {
    if (!m_sendButton) {
        return;
    }

    if (isAiBusy()) {
        m_sendButton->setText(tr("Stop"));
        m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-close-window.svg")));
        m_sendButton->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
        m_sendButton->setToolTip(tr("Stop the active AI request or running local command"));
        m_sendButton->setStyleSheet(sak::ui::kDangerButtonStyle);
        m_sendButton->setEnabled(true);
        setAccessible(m_sendButton,
                      tr("Stop AI session"),
                      tr("Stop the active AI request or running local command"));
        return;
    }

    const bool has_key = ai::OpenAIResponsesClient::hasUsableApiKey(apiKey());
    m_sendButton->setText(tr("Send"));
    m_sendButton->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-send.svg")));
    m_sendButton->setIconSize(QSize(sak::ui::kUiIconSmall, sak::ui::kUiIconSmall));
    m_sendButton->setToolTip(tr("Send the message to the AI assistant"));
    m_sendButton->setStyleSheet(sak::ui::kSecondaryButtonStyle);
    m_sendButton->setEnabled(has_key && !messageText().isEmpty());
    setAccessible(m_sendButton, tr("Send AI message"));
}

qint64 AiAssistantPanel::currentContextWindowTokens() const {
    return modelContextWindowTokens(m_modelCombo ? m_modelCombo->currentText() : QString{});
}

bool AiAssistantPanel::currentContextWindowIsDocumented() const {
    return modelContextWindowIsDocumented(m_modelCombo ? m_modelCombo->currentText() : QString{});
}

void AiAssistantPanel::ensureContextTokenTimer() {
    if (m_contextTokenTimer) {
        return;
    }
    m_contextTokenTimer = new QTimer(this);
    m_contextTokenTimer->setSingleShot(true);
    m_contextTokenTimer->setInterval(kContextTokenDebounceMs);
    connect(
        m_contextTokenTimer, &QTimer::timeout, this, &AiAssistantPanel::refreshContextTokenCount);
}

void AiAssistantPanel::scheduleContextTokenRefresh() {
    if (!m_contextWindowLabel) {
        return;
    }
    ensureContextTokenTimer();
    if (!ai::OpenAIResponsesClient::hasUsableApiKey(apiKey())) {
        resetContextTokenCount(tr("key needed"));
        return;
    }
    if (!m_modelCombo || m_modelCombo->currentText().trimmed().isEmpty()) {
        resetContextTokenCount(tr("model needed"));
        return;
    }
    m_contextTokenStatus = tr("counting");
    updateRunTelemetryLabels();
    m_contextTokenTimer->start();
}

void AiAssistantPanel::refreshContextTokenCount() {
    if (!m_client || !ai::OpenAIResponsesClient::hasUsableApiKey(apiKey()) || !m_modelCombo) {
        resetContextTokenCount(tr("key needed"));
        return;
    }
    ai::OpenAIResponseRequest request = buildChatRequest(messageText());
    request.function_outputs.clear();
    m_contextTokenRequestId = QStringLiteral("ctx_%1").arg(m_nextContextTokenRequestSequence++);
    m_contextTokenStatus = tr("counting");
    updateRunTelemetryLabels();
    m_client->countInputTokens(request, m_contextTokenRequestId);
}

void AiAssistantPanel::resetContextTokenCount(const QString& status) {
    m_contextInputTokens = -1;
    m_contextTokenRequestId.clear();
    m_contextTokenStatus = status;
    if (m_contextTokenTimer) {
        m_contextTokenTimer->stop();
    }
    updateRunTelemetryLabels();
}

qint64 AiAssistantPanel::exactContextUsageTokens() const {
    return std::max<qint64>(-1, m_contextInputTokens);
}

QString AiAssistantPanel::contextWindowStatusText() const {
    const qint64 limit = currentContextWindowTokens();
    const bool documented_limit = currentContextWindowIsDocumented();
    const QString limit_text = documented_limit ? compactTokenCount(limit) : tr("unknown");
    const qint64 used = exactContextUsageTokens();
    if (used >= 0) {
        return documented_limit ? tr("%1/%2 exact").arg(compactTokenCount(used), limit_text)
                                : tr("%1/%2 input exact").arg(compactTokenCount(used), limit_text);
    }
    const QString status = m_contextTokenStatus.trimmed().isEmpty() ? tr("pending")
                                                                    : m_contextTokenStatus;
    return tr("%1/%2").arg(status, limit_text);
}

void AiAssistantPanel::updateRunTelemetryLabels() {
    updateAgentActivityLabel();
    updateContextWindowUsageLabel();
}

void AiAssistantPanel::updateAgentActivityLabel() {
    if (m_agentActivityLabel) {
        const QString text = tr("Agents: %1/%2")
                                 .arg(std::max(0, m_runState.active_subagents))
                                 .arg(std::max(0, m_runState.completed_subagents));
        m_agentActivityLabel->setText(text);
        m_agentActivityLabel->setToolTip(
            tr("%1 active background subagent(s), %2 completed in this run.")
                .arg(std::max(0, m_runState.active_subagents))
                .arg(std::max(0, m_runState.completed_subagents)));
        const char* color = m_runState.active_subagents > 0 ? sak::ui::kStatusColorRunning
                                                            : sak::ui::kColorTextMuted;
        m_agentActivityLabel->setStyleSheet(
            sak::ui::fontWeightAndColorStyle(kFontWeightBold, color));
    }
}

void AiAssistantPanel::updateContextWindowUsageLabel() {
    if (m_contextWindowLabel) {
        const qint64 used = exactContextUsageTokens();
        const qint64 limit = currentContextWindowTokens();
        const bool documented_limit = currentContextWindowIsDocumented();
        const double ratio = documented_limit && limit > 0 && used >= 0
                                 ? static_cast<double>(used) / static_cast<double>(limit)
                                 : 0.0;
        const char* color = contextUsageStatusColor(ratio);
        m_contextWindowLabel->setText(tr("Ctx: %1").arg(contextWindowStatusText()));
        const QString usage = used >= 0 ? QLocale().toString(used) : m_contextTokenStatus.trimmed();
        const QString limit_text = documented_limit ? QLocale().toString(limit) : tr("unknown");
        const QString compact_hint =
            documented_limit ? tr(" Consider compacting or starting a fresh chat near 80%.")
                             : tr(" Select a documented OpenAI model to show the exact window.");
        m_contextWindowLabel->setToolTip(
            tr("Exact input context usage: %1 of %2 tokens for %3. Count comes from OpenAI "
               "/v1/responses/input_tokens using the same model-visible Responses payload, "
               "including current draft, instructions, tools, previous response chain, and "
               "attached context.%4")
                .arg(usage.isEmpty() ? tr("pending") : usage,
                     limit_text,
                     m_modelCombo ? m_modelCombo->currentText().trimmed() : tr("selected model"),
                     compact_hint));
        m_contextWindowLabel->setStyleSheet(
            sak::ui::fontWeightAndColorStyle(kFontWeightBold, color));
    }
}

void AiAssistantPanel::updateTokenLabels() {
    Q_ASSERT(m_tokenTracker);
    scheduleContextTokenRefresh();
    updateRunTelemetryLabels();
    emitStatusDetails();
}

bool AiAssistantPanel::isAiBusy() const {
    return (m_client && m_client->isBusy()) || m_toolTurn.active() || m_workflowRunActive ||
           (m_executionBroker && m_executionBroker->isRunning()) ||
           (m_offlineWorker && m_offlineWorker->isRunning());
}

void AiAssistantPanel::setUiBusy(bool busy) {
    m_taskStatus = busy ? tr("OpenAI request running") : tr("Idle");
    emitStatusDetails();
    setActivityIndicator(busy ? tr("Thinking") : QString(), busy);
    if (m_loadKeyButton) {
        m_loadKeyButton->setEnabled(!busy);
    }
    updateCredentialControls();
}

void AiAssistantPanel::setActivityIndicator(const QString& text, bool active) {
    const QString next_text = active ? text.trimmed() : QString();
    if (m_activityTimer) {
        if (!next_text.isEmpty()) {
            if (!m_activityTimer->isActive()) {
                m_activityTimer->start();
            }
        } else {
            m_activityTimer->stop();
        }
    }
    if (m_transcriptView) {
        m_transcriptView->setActivityText(next_text);
    }
}

void AiAssistantPanel::updateActivityIndicatorFrame() {
    if (m_transcriptView) {
        m_transcriptView->updateActivityFrame();
    }
}

void AiAssistantPanel::appendTranscriptMessage(const QString& role,
                                               const QString& text,
                                               bool expanded) {
    if (m_transcriptView) {
        (void)m_transcriptView->appendMessage(role, text, expanded);
    }
}

void AiAssistantPanel::appendLoadedTranscriptLine(const QString& line) {
    if (m_transcriptView) {
        (void)m_transcriptView->appendLoadedLine(line);
    }
}

void AiAssistantPanel::scrollTranscriptToBottomLater(bool force) {
    if (m_transcriptView) {
        m_transcriptView->scrollToBottomLater(force);
    }
}

void AiAssistantPanel::restoreTranscriptScrollPositionLater(int value) {
    if (m_transcriptView) {
        m_transcriptView->restoreScrollPositionLater(value);
    }
}

bool AiAssistantPanel::isTranscriptScrolledToBottom() const {
    return !m_transcriptView || m_transcriptView->isScrolledToBottom();
}

void AiAssistantPanel::setTranscriptAutoScroll(bool enabled) {
    if (m_transcriptView) {
        m_transcriptView->setAutoScroll(enabled);
    }
}

void AiAssistantPanel::updateJumpToNewestButton() {
    if (m_transcriptView) {
        m_transcriptView->updateJumpToNewestButton();
    }
}

void AiAssistantPanel::jumpTranscriptToNewest() {
    if (m_transcriptView) {
        m_transcriptView->jumpToNewest();
    }
}

void AiAssistantPanel::renderTranscriptMessages(bool scroll_to_bottom) {
    if (m_transcriptView) {
        m_transcriptView->renderMessages(scroll_to_bottom);
    }
}

void AiAssistantPanel::recordPromptHistory(const QString& prompt) {
    const QString trimmed = prompt.trimmed();
    if (trimmed.isEmpty()) {
        return;
    }
    if (m_promptHistory.isEmpty() || m_promptHistory.last() != trimmed) {
        m_promptHistory.append(trimmed);
    }
    constexpr int kMaxPromptHistory = 50;
    while (m_promptHistory.size() > kMaxPromptHistory) {
        m_promptHistory.removeFirst();
    }
    m_promptHistoryIndex = -1;
    m_promptHistoryDraft.clear();
}

bool AiAssistantPanel::cyclePromptHistory(int direction) {
    if (!m_messageEdit || m_promptHistory.isEmpty()) {
        return false;
    }
    if (m_promptHistoryIndex < 0) {
        m_promptHistoryDraft = m_messageEdit->toPlainText();
        m_promptHistoryIndex = m_promptHistory.size();
    }
    m_promptHistoryIndex += direction;
    if (m_promptHistoryIndex < 0) {
        m_promptHistoryIndex = 0;
    }
    if (m_promptHistoryIndex > m_promptHistory.size()) {
        m_promptHistoryIndex = m_promptHistory.size();
    }
    const QString text = m_promptHistoryIndex == m_promptHistory.size()
                             ? m_promptHistoryDraft
                             : m_promptHistory.at(m_promptHistoryIndex);
    m_messageEdit->setPlainText(text);
    m_messageEdit->moveCursor(QTextCursor::End);
    return true;
}

AiAssistantPanel::BusyPromptAction AiAssistantPanel::askBusyPromptAction(const QString& message) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("AI Task In Progress"));
    box.setText(tr("The assistant is already working. What should SAK do with this new request?"));
    box.setInformativeText(message.left(kHumanApprovalInfoMaxChars));
    auto* steer = box.addButton(tr("Apply as Steering"), QMessageBox::AcceptRole);
    auto* queue = box.addButton(tr("Queue Next"), QMessageBox::ActionRole);
    auto* cancel_queue = box.addButton(tr("Stop and Queue"), QMessageBox::DestructiveRole);
    auto* discard = box.addButton(tr("Discard"), QMessageBox::RejectRole);
    box.setDefaultButton(qobject_cast<QPushButton*>(steer));
    box.exec();

    if (box.clickedButton() == queue) {
        return BusyPromptAction::QueueAfterRun;
    }
    if (box.clickedButton() == cancel_queue) {
        return BusyPromptAction::CancelAndQueue;
    }
    if (box.clickedButton() == discard) {
        return BusyPromptAction::Discard;
    }
    return BusyPromptAction::ApplySteering;
}

void AiAssistantPanel::applySteeringMessage(const QString& message) {
    m_pendingSteeringMessages.append(message);
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Steering"), message);
    }
    if (m_conversationStore) {
        QString error;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("Steering"), message, {}, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
    }
    appendSessionMemory(QStringLiteral("Steering"),
                        QStringLiteral("User request during active run"),
                        message);
    if (m_messageEdit) {
        m_messageEdit->clear();
    }
    setActivityIndicator(tr("Steering saved"), true);
    appendLocalEvent(tr("Steering saved for active AI run"));
    Q_EMIT statusMessage(tr("Steering saved for active AI run"), sak::kTimerStatusDefaultMs);
    updateCredentialControls();
}

void AiAssistantPanel::queuePromptForLater(const QString& message, const QString& reason) {
    m_queuedUserMessages.append(message);
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Queued"), message);
    }
    if (m_conversationStore) {
        QString error;
        QJsonObject metadata;
        metadata[QStringLiteral("reason")] = reason;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("Queued"), message, metadata, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
    }
    appendSessionMemory(QStringLiteral("Queued"),
                        reason.isEmpty() ? QStringLiteral("Next user request") : reason,
                        message);
    if (m_messageEdit) {
        m_messageEdit->clear();
    }
    appendLocalEvent(tr("Queued AI request for later"));
    Q_EMIT statusMessage(tr("AI request queued"), sak::kTimerStatusDefaultMs);
    updateCredentialControls();
}

void AiAssistantPanel::dispatchQueuedPromptIfIdle() {
    if (m_dispatchingQueuedPrompt || isAiBusy() || m_queuedUserMessages.isEmpty()) {
        return;
    }
    if (!m_messageEdit) {
        return;
    }
    const QString next = m_queuedUserMessages.takeFirst();
    if (next.trimmed().isEmpty()) {
        QTimer::singleShot(0, this, &AiAssistantPanel::dispatchQueuedPromptIfIdle);
        return;
    }
    m_dispatchingQueuedPrompt = true;
    m_messageEdit->setPlainText(next);
    QTimer::singleShot(0, this, [this]() {
        onSendClicked();
        m_dispatchingQueuedPrompt = false;
    });
}

QString AiAssistantPanel::apiKey() const {
    return m_apiKey.trimmed();
}

void AiAssistantPanel::emitStatusDetails() {
    updateRunTelemetryLabels();
    Q_EMIT statusDetailsChanged(statusDetails());
}

void AiAssistantPanel::resetPromptTemplatePicker() {
    m_promptTemplateCombo->clear();
    m_promptTemplateCombo->addItem(tr("Choose workflow..."));
    m_promptTemplateCombo->setItemData(0,
                                       tr("Pick the technician workflow that matches the job. "
                                          "Each workflow attaches its prompts, instructions, "
                                          "skills, tools, verification, reporting, and cleanup."),
                                       Qt::ToolTipRole);
    hideWorkflowDetails();
    if (m_workflowDetailsButton) {
        m_workflowDetailsButton->setEnabled(false);
    }
    if (m_addWorkflowButton) {
        m_addWorkflowButton->setEnabled(false);
    }
}

QString AiAssistantPanel::workflowTemplateComboLabel(const ai::WorkflowTemplate& workflow) const {
    const QString category = workflow.category.trimmed().isEmpty() ? workflow.role.trimmed()
                                                                   : workflow.category.trimmed();
    if (category.isEmpty()) {
        return tr("%1 (%2 phases)").arg(workflow.title).arg(workflow.phases.size());
    }
    return tr("%1 - %2 (%3 phases)").arg(category, workflow.title).arg(workflow.phases.size());
}

QStringList limitedWorkflowTools(const ai::WorkflowTemplate& workflow) {
    QStringList tools;
    for (const auto& requirement : workflow.required_software) {
        if (!requirement.id.trimmed().isEmpty()) {
            tools << requirement.id.trimmed();
        }
        if (tools.size() >= kInlineToolListLimit) {
            break;
        }
    }
    return tools;
}

QStringList limitedWorkflowInputs(const ai::WorkflowTemplate& workflow) {
    QStringList inputs;
    for (const auto& input : workflow.required_inputs) {
        inputs << (input.label.trimmed().isEmpty() ? input.id.trimmed() : input.label.trimmed());
        if (inputs.size() >= kInlineToolListLimit) {
            break;
        }
    }
    return inputs;
}

QString AiAssistantPanel::workflowTemplateTooltip(const ai::WorkflowTemplate& workflow,
                                                  const QString& label) const {
    const QStringList tools = limitedWorkflowTools(workflow);
    const QStringList inputs = limitedWorkflowInputs(workflow);
    const QString description =
        workflow.description.trimmed().isEmpty()
            ? tr("Structured technician work with evidence, verification, report, and cleanup.")
            : workflow.description.trimmed();
    return tr("%1\n\nBest for: %2\nRole: %3\nPhases: %4\nRequired inputs: %5\nTools: %6")
        .arg(label,
             description,
             workflow.role.trimmed().isEmpty() ? tr("Any") : workflow.role.trimmed())
        .arg(workflow.phases.size())
        .arg(inputs.isEmpty() ? tr("None") : inputs.join(QStringLiteral(", ")),
             tools.isEmpty() ? tr("Built-in or none") : tools.join(QStringLiteral(", ")));
}

void AiAssistantPanel::populateWorkflowTemplatePicker(
    const QVector<ai::WorkflowTemplate>& workflows) {
    m_promptTemplateCombo->setEnabled(true);
    for (const auto& workflow : workflows) {
        const QString label = workflowTemplateComboLabel(workflow);
        m_promptTemplateCombo->addItem(label, workflow.id);
        const int item_index = m_promptTemplateCombo->count() - 1;
        m_promptTemplateCombo->setItemData(item_index,
                                           workflowTemplateTooltip(workflow, label),
                                           Qt::ToolTipRole);
    }
}

void AiAssistantPanel::refreshPromptTemplates() {
    if (!m_promptTemplateCombo) {
        return;
    }

    QSignalBlocker blocker(m_promptTemplateCombo);
    resetPromptTemplatePicker();

    if (m_workflowStore && !m_workflowStore->workflows().isEmpty()) {
        populateWorkflowTemplatePicker(m_workflowStore->workflows());
        return;
    }

    m_promptTemplateCombo->setItemText(0, tr("No workflows available"));
    m_promptTemplateCombo->setItemData(0,
                                       tr("Workflow catalog failed to load. Fix the workflow "
                                          "resource load error before using workflow prompts."),
                                       Qt::ToolTipRole);
    m_promptTemplateCombo->setEnabled(false);
}

void AiAssistantPanel::syncSessionRoleForWorkflow(const ai::WorkflowTemplate* workflow) {
    if (workflow && !workflow->role.trimmed().isEmpty()) {
        setSessionRole(workflow->role.trimmed(),
                       QString::fromLatin1(kSessionRoleSourceWorkflow),
                       true);
        return;
    }
    if (m_sessionRoleSource == QLatin1String(kSessionRoleSourceWorkflow)) {
        resetSessionRole();
    } else {
        updateSessionRoleDisplay();
    }
}

void AiAssistantPanel::updateSessionRoleDisplay() {
    if (!m_sessionRoleValueLabel) {
        return;
    }
    const QString role = currentWorkflowRole();
    const QString source = m_sessionRoleSource.trimmed();
    QString source_label;
    if (source == QLatin1String(kSessionRoleSourceWorkflow)) {
        source_label = tr("workflow");
    } else if (source == QLatin1String(kSessionRoleSourceWorkflowSelection)) {
        source_label = tr("selected workflow");
    } else if (source == QLatin1String(kSessionRoleSourcePrompt)) {
        source_label = tr("first prompt");
    } else if (source == QLatin1String(kSessionRoleSourceUser)) {
        source_label = tr("user directed");
    } else if (source == QLatin1String(kSessionRoleSourcePending) || m_sessionRole.isEmpty()) {
        source_label = tr("pending first prompt");
    } else {
        source_label = tr("default");
    }
    m_sessionRoleValueLabel->setText(tr("%1 (%2)").arg(role, source_label));
    m_sessionRoleValueLabel->setToolTip(
        tr("Role is selected from the workflow, the first prompt, or an explicit user request to "
           "assume another role."));
}

void AiAssistantPanel::resetSessionRole() {
    m_sessionRole.clear();
    m_sessionRoleSource = QString::fromLatin1(kSessionRoleSourcePending);
    updateSessionRoleDisplay();
}

void AiAssistantPanel::restoreSessionRoleForSession(const QString& session_id) {
    if (!m_conversationStore || session_id.trimmed().isEmpty()) {
        resetSessionRole();
        return;
    }
    QString source;
    QString error;
    const QString role = m_conversationStore->latestSessionRole(session_id, &source, &error);
    if (!error.isEmpty()) {
        appendLocalEvent(tr("Could not restore AI session role: %1").arg(error));
    }
    if (role.trimmed().isEmpty()) {
        resetSessionRole();
        return;
    }
    setSessionRole(role,
                   source.trimmed().isEmpty() ? QString::fromLatin1(kSessionRoleSourcePrompt)
                                              : source.trimmed(),
                   false);
}

void AiAssistantPanel::updateSessionRoleForPrompt(const QString& message) {
    const QString explicit_role = explicitRoleFromPrompt(message, m_workflowStore.get());
    if (!explicit_role.isEmpty()) {
        setSessionRole(explicit_role, QString::fromLatin1(kSessionRoleSourceUser), true);
        return;
    }

    if (const auto* workflow = attachedWorkflow()) {
        if (!workflow->role.trimmed().isEmpty()) {
            setSessionRole(workflow->role.trimmed(),
                           QString::fromLatin1(kSessionRoleSourceWorkflow),
                           true);
            return;
        }
    }

    if (const auto* workflow = selectedWorkflowTemplate()) {
        if (!workflow->role.trimmed().isEmpty()) {
            setSessionRole(workflow->role.trimmed(),
                           QString::fromLatin1(kSessionRoleSourceWorkflowSelection),
                           true);
            return;
        }
    }

    const bool role_pending = m_sessionRole.trimmed().isEmpty() ||
                              m_sessionRoleSource == QLatin1String(kSessionRoleSourcePending);
    const bool creating_session = !m_conversationStore ||
                                  m_conversationStore->currentSessionId().isEmpty();
    if (role_pending || creating_session) {
        setSessionRole(inferredRoleFromPrompt(message, m_workflowStore.get()),
                       QString::fromLatin1(kSessionRoleSourcePrompt),
                       true);
    } else {
        updateSessionRoleDisplay();
    }
}

void AiAssistantPanel::setSessionRole(const QString& role, const QString& source, bool persist) {
    const QString clean_role = role.trimmed().isEmpty() ? QString::fromLatin1(kDefaultSessionRole)
                                                        : role.trimmed();
    const QString clean_source = source.trimmed().isEmpty()
                                     ? QString::fromLatin1(kSessionRoleSourceDefault)
                                     : source.trimmed();
    const bool changed = clean_role.compare(m_sessionRole, Qt::CaseInsensitive) != 0 ||
                         clean_source.compare(m_sessionRoleSource, Qt::CaseInsensitive) != 0;
    m_sessionRole = clean_role;
    m_sessionRoleSource = clean_source;
    updateSessionRoleDisplay();
    if (changed && persist) {
        persistSessionRoleChoice();
    }
}

void AiAssistantPanel::persistSessionRoleChoice() {
    if (!m_conversationStore || m_conversationStore->currentSessionId().isEmpty()) {
        return;
    }
    appendSessionMemory(
        QStringLiteral("Session"),
        QStringLiteral("Role"),
        tr("Active AI role: %1\nRole source: %2").arg(currentWorkflowRole(), m_sessionRoleSource));
}

QString AiAssistantPanel::currentWorkflowRole() const {
    if (!m_sessionRole.trimmed().isEmpty()) {
        return m_sessionRole.trimmed();
    }
    return QString::fromLatin1(kDefaultSessionRole);
}

const ai::WorkflowTemplate* AiAssistantPanel::selectedWorkflowTemplate() const {
    if (!m_promptTemplateCombo || !m_workflowStore) {
        return nullptr;
    }
    const int index = m_promptTemplateCombo->currentIndex();
    if (index <= 0) {
        return nullptr;
    }
    const QString workflow_id = m_promptTemplateCombo->itemData(index).toString();
    if (workflow_id.trimmed().isEmpty()) {
        return nullptr;
    }
    return m_workflowStore->workflowById(workflow_id);
}

void AiAssistantPanel::applyPromptTemplate(const QString& title, const QString& prompt) {
    if (!m_messageEdit || prompt.trimmed().isEmpty()) {
        return;
    }

    const QString block = tr("Task: %1\n\n%2").arg(title, prompt.trimmed());
    const QString current = m_messageEdit->toPlainText().trimmed();
    if (current.isEmpty()) {
        m_messageEdit->setPlainText(block);
    } else if (!current.contains(prompt.trimmed(), Qt::CaseInsensitive)) {
        m_messageEdit->setPlainText(QStringLiteral("%1\n\n%2").arg(current, block));
    }
    m_messageEdit->moveCursor(QTextCursor::End);
    m_messageEdit->setFocus();
    updateCredentialControls();
    appendLocalEvent(tr("Added workflow prompt: %1").arg(title));
}

void AiAssistantPanel::applyWorkflowTemplate(const ai::WorkflowTemplate& workflow) {
    syncSessionRoleForWorkflow(&workflow);
    applyPromptTemplate(workflow.title, workflow.promptSummary());
    addWorkflowContextChip(workflow);

    for (const auto& instruction : workflow.instructions) {
        addWorkflowResourceContext(instruction, tr("Instruction"), ContextItem::Type::Instruction);
    }
    for (const auto& skill : workflow.skills) {
        addWorkflowResourceContext(skill, tr("Skill"), ContextItem::Type::Skill);
    }

    Q_EMIT statusMessage(tr("Workflow added: %1").arg(workflow.title), sak::kTimerStatusDefaultMs);
    appendLocalEvent(tr("Added workflow: %1").arg(workflow.title));
}

void AiAssistantPanel::addWorkflowContextChip(const ai::WorkflowTemplate& workflow) {
    const QString path = QStringLiteral("workflow:%1").arg(workflow.id);
    for (const auto& existing : m_contextItems) {
        if (existing.path == path) {
            return;
        }
    }

    const QString text = workflow.promptSummary();
    ContextItem item;
    item.type = ContextItem::Type::Workflow;
    item.display_name = workflow.title;
    item.path = path;
    item.mime_type = QStringLiteral("application/vnd.sak.workflow");
    item.text = text;
    item.original_size = text.toUtf8().size();
    m_contextItems.append(item);
    refreshContextList();
    persistContextItem(item);
}

void AiAssistantPanel::addWorkflowResourceContext(const QString& resource_path,
                                                  const QString& label_prefix,
                                                  ContextItem::Type type) {
    QString path = resource_path.trimmed();
    if (path.isEmpty()) {
        return;
    }
    if (!path.startsWith(QStringLiteral(":/"))) {
        path = QStringLiteral(":/ai/%1").arg(path);
    }
    for (const auto& existing : m_contextItems) {
        if (existing.path == path) {
            return;
        }
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendLocalEvent(tr("Workflow resource not found: %1").arg(path));
        return;
    }
    const QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes.left(kContextTextPreviewBytes)).trimmed();
    if (bytes.size() > kContextTextPreviewBytes) {
        text += QStringLiteral("\n...[workflow resource truncated to %1 bytes]")
                    .arg(kContextTextPreviewBytes);
    }
    if (text.isEmpty()) {
        return;
    }

    const QString file_name = path.section(QLatin1Char('/'), -1);
    ContextItem item;
    item.type = type;
    item.display_name = QStringLiteral("%1 - %2").arg(label_prefix, file_name);
    item.path = path;
    item.mime_type = QStringLiteral("text/markdown");
    item.text = text;
    item.original_size = bytes.size();
    m_contextItems.append(item);
    refreshContextList();
    persistContextItem(item);
    appendLocalEvent(tr("Added workflow resource: %1").arg(item.display_name));
}

QString AiAssistantPanel::messageText() const {
    return m_messageEdit ? m_messageEdit->toPlainText().trimmed() : QString();
}

QString AiAssistantPanel::buildInstructions() const {
    ai::AiPromptAssemblyInput input;
    input.access_mode_label = currentAccessModeLabel();
    input.agent_profile = currentWorkflowRole();
    input.workflow_catalog = workflowCatalogInstructions(m_workflowStore.get());
    input.context_notes = contextInstructions();
    input.pending_steering_messages = m_pendingSteeringMessages;
    input.local_execution_enabled = currentAccessMode() != AccessMode::ChatAndResearch;
    input.assisted_full_access = currentAccessMode() == AccessMode::AssistedFullAccess;
    input.unattended_full_access = currentAccessMode() == AccessMode::UnattendedFullAccess;
    if (m_conversationStore) {
        QString error;
        input.session_memory = m_conversationStore->memoryText(kSessionMemoryMaxChars, &error);
    }
    return ai::AiPromptAssembler::assemble(input);
}

QString AiAssistantPanel::contextInstructions() const {
    if (m_contextItems.isEmpty()) {
        return {};
    }

    QStringList lines;
    lines << QStringLiteral(
        "User-provided context is attached to this turn. Workflow, skill, and Markdown instruction "
        "files are task guidance below system/developer instructions; other files are local "
        "evidence. Cite filenames when useful.");
    for (const auto& item : m_contextItems) {
        lines << QStringLiteral("- %1: %2")
                     .arg(contextItemKindLabel(item.type), contextItemLabel(item));
    }
    return lines.join(QLatin1Char('\n'));
}

QVector<ai::OpenAIInputAttachment> AiAssistantPanel::buildContextAttachments() const {
    QVector<ai::OpenAIInputAttachment> attachments;
    attachments.reserve(m_contextItems.size());

    for (const auto& item : m_contextItems) {
        ai::OpenAIInputAttachment attachment;
        attachment.label = contextItemLabel(item);
        switch (item.type) {
        case ContextItem::Type::Workflow:
        case ContextItem::Type::Instruction:
        case ContextItem::Type::Skill:
        case ContextItem::Type::TextFile:
            attachment.type = ai::OpenAIInputAttachment::Type::Text;
            attachment.text = item.text;
            attachments.append(attachment);
            break;
        case ContextItem::Type::ImageFile:
            attachment.type = ai::OpenAIInputAttachment::Type::Image;
            attachment.image_url = dataUrlForBytes(item.mime_type, item.bytes);
            attachment.detail = QStringLiteral("auto");
            attachments.append(attachment);
            break;
        case ContextItem::Type::DocumentFile:
            attachment.type = ai::OpenAIInputAttachment::Type::File;
            attachment.filename = QFileInfo(item.path).fileName();
            attachment.file_data = QString::fromLatin1(item.bytes.toBase64());
            attachment.detail = QStringLiteral("low");
            attachments.append(attachment);
            break;
        }
    }

    return attachments;
}

void AiAssistantPanel::beginToolTurn(const ai::OpenAIResponseResult& response) {
    QString error;
    if (!m_toolTurn.begin(response.id, response.function_calls, &error)) {
        const QString message = tr("Pending tool turn could not start: %1")
                                    .arg(error.isEmpty() ? tr("unknown error") : error);
        appendLocalEvent(message);
        appendTranscriptMessage(QStringLiteral("System"), message, true);
        traceAiEvent(QStringLiteral("tool_queue"),
                     QStringLiteral("local_tools"),
                     QStringLiteral("failed"),
                     QJsonObject{{QStringLiteral("error_message"), message}});
        m_runState.status = ai::AiRunStatus::Failed;
        m_runState.active_tools = 0;
        m_runState.message = message;
        saveRunStateSnapshot();
        emitStatusDetails();
        setUiBusy(false);
        updateCredentialControls();
        Q_EMIT statusMessage(message, sak::kTimerStatusDefaultMs);
        return;
    }
    if (m_runToken.isValid()) {
        m_pendingTurnToken = m_runToken.createChild(
            QStringLiteral("turn_%1").arg(response.id.left(kTraceTokenIdChars)));
    } else {
        m_pendingTurnToken = {};
    }
    dispatchNextToolCall();
}

void AiAssistantPanel::dispatchNextToolCall() {
    if (!m_toolTurn.active()) {
        return;
    }
    while (const auto* call_ptr = m_toolTurn.currentCall()) {
        const auto& call = *call_ptr;
        ai::OpenAIFunctionOutput output;
        PendingToolCallContext context;
        if (!preparePendingToolCall(call, &context, &output)) {
            return;
        }

        const ai::AiToolCallRouter::ParsedArguments parsed =
            ai::AiToolCallRouter::parseArguments(call);
        if (!parsed.ok) {
            appendToolOutputAndContinue(parsed.output);
            return;
        }

        if (dispatchBuiltInToolCall(context, parsed.arguments, &output)) {
            return;
        }
        if (dispatchCommandToolCall(context, parsed.arguments, &output)) {
            return;
        }
    }
    finishToolTurnAndContinue();
}

bool AiAssistantPanel::preparePendingToolCall(const ai::OpenAIFunctionCall& call,
                                              PendingToolCallContext* context,
                                              ai::OpenAIFunctionOutput* output) {
    if (!context || !output) {
        return false;
    }
    const ai::AiToolCallRouter::PreparedCall prepared =
        ai::AiToolCallRouter::prepare(call, m_toolTurn.callIndex());
    context->call = &call;
    context->kind = prepared.kind;
    context->metadata = prepared.metadata;
    *output = prepared.output;
    traceAiEvent(
        QStringLiteral("tool_call"), call.name, QStringLiteral("started"), context->metadata);
    recordToolLoopObservation(call.name);

    m_pendingCallToken = m_pendingTurnToken.isValid()
                             ? m_pendingTurnToken.createChild(QStringLiteral("call_%1").arg(
                                   call.call_id.left(kTraceTokenIdChars)))
                         : m_runToken.isValid()
                             ? m_runToken.createChild(QStringLiteral("call_%1").arg(
                                   call.call_id.left(kTraceTokenIdChars)))
                             : ai::CancellationToken{};
    if ((m_runToken.isValid() && m_runToken.isCancellationRequested()) ||
        (m_pendingCallToken.isValid() && m_pendingCallToken.isCancellationRequested())) {
        *output = ai::AiToolCallRouter::cancelledOutput(call);
        traceAiEvent(
            QStringLiteral("tool_call"), call.name, QStringLiteral("cancelled"), context->metadata);
        appendToolOutputAndContinue(std::move(*output));
        return false;
    }
    if (!prepared.recognized) {
        traceAiEvent(
            QStringLiteral("tool_call"), call.name, QStringLiteral("failed"), context->metadata);
        appendToolOutputAndContinue(std::move(*output));
        return false;
    }
    return true;
}

bool AiAssistantPanel::dispatchBuiltInToolCall(const PendingToolCallContext& context,
                                               const QJsonObject& args,
                                               ai::OpenAIFunctionOutput* output) {
    if (!output || !ai::AiToolCallRouter::isBuiltInTool(context.kind)) {
        return false;
    }
    ai::AiToolCallRequest policy_request;
    policy_request.tool_name = context.call->name;
    policy_request.operation = args.value(QStringLiteral("operation")).toString();
    policy_request.command_preview = args.value(QStringLiteral("query")).toString();
    policy_request.user_message = m_activeUserMessage;
    appendLocalEvent(tr("Using AI tool: %1").arg(context.call->name));

    if (!m_toolDispatcher) {
        const QJsonObject result =
            toolError(QStringLiteral("AI tool dispatcher is not initialized"));
        output->output = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));

        QJsonObject metadata = context.metadata;
        metadata[QStringLiteral("dispatcher_missing")] = true;
        metadata[QStringLiteral("error_message")] =
            QStringLiteral("AI tool dispatcher is not initialized");
        traceAiEvent(
            QStringLiteral("tool_call"), context.call->name, QStringLiteral("failed"), metadata);
        appendLocalEvent(tr("AI tool dispatcher unavailable for %1").arg(context.call->name));
        appendToolOutputAndContinue(std::move(*output));
        return true;
    }

    const auto outcome =
        m_toolDispatcher->dispatch(currentAccessToolPolicy(), policy_request, args);
    const QJsonObject& result = outcome.result;
    recordToolLoopObservation(context.call->name, result);
    output->output = QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact));

    QJsonObject metadata = context.metadata;
    metadata[QStringLiteral("policy_allowed")] = outcome.policy_decision.allowed;
    metadata[QStringLiteral("requires_lease")] = outcome.policy_decision.requires_lease;
    metadata[QStringLiteral("risky_change")] = outcome.policy_decision.risky_change;
    metadata[QStringLiteral("availability_denied")] = outcome.availability_denied;
    metadata[QStringLiteral("health_suppressed")] = outcome.health_suppressed;
    metadata[QStringLiteral("health_key")] = outcome.health_key;
    metadata[QStringLiteral("latency_ms")] = static_cast<double>(outcome.latency_ms);
    metadata[QStringLiteral("result_success")] =
        result.value(QStringLiteral("success")).toBool(false);
    const QString result_error = result.value(QStringLiteral("error_message")).toString();
    if (!result_error.isEmpty()) {
        metadata[QStringLiteral("result_error_message")] =
            result_error.left(kMetadataPreviewMaxChars);
    }
    const QString status = outcome.dispatched
                               ? (result.value(QStringLiteral("success")).toBool(false)
                                      ? QStringLiteral("completed")
                                      : QStringLiteral("failed"))
                           : outcome.health_suppressed   ? QStringLiteral("health_suppressed")
                           : outcome.availability_denied ? QStringLiteral("availability_denied")
                           : outcome.handler_missing     ? QStringLiteral("handler_missing")
                                                         : QStringLiteral("policy_denied");
    traceAiEvent(QStringLiteral("tool_call"), context.call->name, status, metadata);
    appendToolOutputAndContinue(std::move(*output));
    return true;
}

void AiAssistantPanel::recordToolLoopObservation(const QString& tool_name,
                                                 const QJsonObject& result) {
    const QString name = tool_name.trimmed();
    if (!name.isEmpty() && result.isEmpty()) {
        m_toolNamesThisMessage[name] += 1;
    }
    if (result.isEmpty() || result.value(QStringLiteral("success")).toBool(false)) {
        return;
    }
    const QString failure_class = ai::AiToolHealthLedger::classifyResult(result);
    if (!failure_class.isEmpty()) {
        m_toolFailureClassesThisMessage[failure_class] += 1;
    }
}

QString AiAssistantPanel::toolLoopCapSummary() const {
    auto topEntry = [](const QHash<QString, int>& counts) {
        QString key;
        int count = 0;
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) {
            if (it.value() > count) {
                key = it.key();
                count = it.value();
            }
        }
        return qMakePair(key, count);
    };
    const auto top_tool = topEntry(m_toolNamesThisMessage);
    const auto top_failure = topEntry(m_toolFailureClassesThisMessage);
    const QString repeated_tools =
        top_tool.first.isEmpty()
            ? tr("none")
            : QStringLiteral("%1 x%2").arg(top_tool.first, QString::number(top_tool.second));
    const QString failure_class =
        top_failure.first.isEmpty()
            ? tr("none")
            : QStringLiteral("%1 x%2").arg(top_failure.first, QString::number(top_failure.second));
    return tr("Repeated tools: %1. Top failure class: %2.").arg(repeated_tools, failure_class);
}

bool AiAssistantPanel::authorizeCommandToolCall(const PendingToolCallContext& context,
                                                const ai::AiCommandToolPlan& plan,
                                                ai::OpenAIFunctionOutput* output) {
    if (!plan.policy_decision.allowed) {
        const QJsonObject blocked = ai::AiToolDispatcher::policyDeniedResult(plan.policy_request,
                                                                             plan.policy_decision);
        output->output = QString::fromUtf8(QJsonDocument(blocked).toJson(QJsonDocument::Compact));
        traceAiEvent(QStringLiteral("tool_call"),
                     context.call->name,
                     QStringLiteral("policy_denied"),
                     context.metadata);
        appendLocalEvent(tr("Tool policy blocked %1 call: %2")
                             .arg(context.call->name, plan.policy_decision.reason));
        appendToolOutputAndContinue(std::move(*output));
        return false;
    }
    if (!plan.guard_approval_reason.isEmpty()) {
        const QString approval_preview =
            QStringLiteral("%1\n\n%2").arg(plan.guard_approval_reason, plan.preview);
        if (!confirmCommandWithUser(tr("Sensitive Package Command"), approval_preview, true)) {
            output->output = QString::fromUtf8(
                QJsonDocument(toolError(QStringLiteral("User declined sensitive package command")))
                    .toJson(QJsonDocument::Compact));
            QJsonObject metadata = context.metadata;
            metadata[QStringLiteral("guard_approval_required")] = true;
            metadata[QStringLiteral("reason")] = plan.guard_approval_reason;
            traceAiEvent(QStringLiteral("tool_call"),
                         context.call->name,
                         QStringLiteral("rejected"),
                         metadata);
            appendLocalEvent(tr("User declined sensitive package command"));
            appendToolOutputAndContinue(std::move(*output));
            return false;
        }
        return true;
    }
    if (currentAccessMode() == AccessMode::AssistedFullAccess &&
        !confirmCommandWithUser(plan.shell_label, plan.preview, plan.risky_change)) {
        output->output = QStringLiteral("{\"error\":\"User declined command\"}");
        traceAiEvent(QStringLiteral("tool_call"),
                     context.call->name,
                     QStringLiteral("rejected"),
                     context.metadata);
        appendLocalEvent(tr("User declined %1 command").arg(plan.shell_label));
        appendToolOutputAndContinue(std::move(*output));
        return false;
    }
    if (currentAccessMode() == AccessMode::UnattendedFullAccess &&
        !offerRestorePointIfNeeded(plan.preview, plan.risky_change)) {
        output->output = QStringLiteral("{\"error\":\"User cancelled before risky command\"}");
        traceAiEvent(QStringLiteral("tool_call"),
                     context.call->name,
                     QStringLiteral("cancelled"),
                     context.metadata);
        appendLocalEvent(tr("User cancelled before risky %1 command").arg(plan.shell_label));
        appendToolOutputAndContinue(std::move(*output));
        return false;
    }
    return true;
}

bool AiAssistantPanel::acquireCommandToolLease(const PendingToolCallContext& context,
                                               const ai::AiCommandToolPlan& plan,
                                               ai::OpenAIFunctionOutput* output) {
    if (!plan.policy_decision.requires_lease || !m_leaseManager) {
        return true;
    }
    const auto acquire = m_leaseManager->acquire(QStringLiteral("overseer"),
                                                 QStringList{context.call->name},
                                                 QStringLiteral("system_change"),
                                                 true);
    if (acquire.granted) {
        m_currentCommandLeaseId = acquire.lease.lease_id;
        return true;
    }
    const QJsonObject blocked = ai::AiToolDispatcher::leaseDeniedResult(plan.policy_request,
                                                                        acquire.reason);
    output->output = QString::fromUtf8(QJsonDocument(blocked).toJson(QJsonDocument::Compact));
    QJsonObject metadata = context.metadata;
    metadata[QStringLiteral("lease_denied")] = true;
    metadata[QStringLiteral("reason")] = acquire.reason;
    traceAiEvent(
        QStringLiteral("tool_call"), context.call->name, QStringLiteral("lease_denied"), metadata);
    appendLocalEvent(
        tr("Mutating lease unavailable for %1: %2").arg(context.call->name, acquire.reason));
    appendToolOutputAndContinue(std::move(*output));
    return false;
}

void AiAssistantPanel::startCommandToolCall(const PendingToolCallContext& context,
                                            const ai::AiCommandToolPlan& plan) {
    m_currentCommandId = allocateCommandId();
    m_currentCommandPreview = plan.preview;
    m_currentStdoutBuffer.clear();
    m_currentStderrBuffer.clear();
    const QString admin_suffix = plan.request.requires_admin ? QStringLiteral(" *ADMIN*")
                                                             : QString();
    appendLocalEvent(
        tr("Running %1%2 tool call %3").arg(plan.shell_label, admin_suffix, m_currentCommandId));
    Q_EMIT logOutput(ai::CredentialStore::redactSecrets(
        QStringLiteral("[%1 %2%3] $ %4")
            .arg(m_currentCommandId, plan.shell_label, admin_suffix, plan.preview)));

    if (context.call->name == QLatin1String("run_powershell")) {
        (void)m_executionBroker->startPowerShell(plan.request, m_currentCommandId);
    } else if (context.call->name == QLatin1String("run_cmd")) {
        (void)m_executionBroker->startCmd(plan.request, m_currentCommandId);
    } else {
        (void)m_executionBroker->startProcess(plan.request, m_currentCommandId);
    }
}

bool AiAssistantPanel::dispatchCommandToolCall(const PendingToolCallContext& context,
                                               const QJsonObject& args,
                                               ai::OpenAIFunctionOutput* output) {
    if (!output) {
        return false;
    }
    const ai::AiCommandToolPlan plan =
        ai::AiCommandToolPlanner::buildPlan(context.call->name,
                                            args,
                                            currentAccessToolPolicy(),
                                            ai::AiCommandToolPlanner::Options{static_cast<int>(
                                                kDefaultOutputCapKb * sak::kBytesPerKB)});
    if (!plan.guard_block_error.isEmpty()) {
        output->output = QString::fromUtf8(
            QJsonDocument(toolError(plan.guard_block_error)).toJson(QJsonDocument::Compact));
        QJsonObject metadata = context.metadata;
        metadata[QStringLiteral("guard_blocked")] = true;
        metadata[QStringLiteral("reason")] = plan.guard_block_error;
        metadata[QStringLiteral("preview")] = plan.preview.left(kMetadataPreviewMaxChars);
        traceAiEvent(
            QStringLiteral("tool_call"), context.call->name, QStringLiteral("blocked"), metadata);
        appendLocalEvent(
            tr("AI command guard blocked %1: %2").arg(plan.shell_label, plan.guard_block_error));
        appendToolOutputAndContinue(std::move(*output));
        return true;
    }
    if (!authorizeCommandToolCall(context, plan, output)) {
        return true;
    }
    if (!acquireCommandToolLease(context, plan, output)) {
        return true;
    }
    startCommandToolCall(context, plan);
    return true;
}

QString AiAssistantPanel::allocateCommandId() {
    return QStringLiteral("cmd_%1").arg(
        m_nextCommandSequence++, kCommandIdWidth, kCommandIdBase, QLatin1Char('0'));
}

bool AiAssistantPanel::confirmCommandWithUser(const QString& shell,
                                              const QString& preview,
                                              bool risky_change) {
    const QString pending_call_id = currentPendingToolCallId();
    bool resumed_approval_result = false;
    if (consumeResumedCommandApproval(
            shell, preview, risky_change, pending_call_id, &resumed_approval_result)) {
        return resumed_approval_result;
    }

    const ai::AiRunStatus previous_status = m_runState.status;
    QJsonObject metadata = commandApprovalMetadata(shell, preview, risky_change);
    const QString gate_id = beginCommandApprovalGate(&metadata);
    if (!requestCommandApprovalFromUser(shell, preview)) {
        return rejectCommandApproval(gate_id, metadata, shell, previous_status);
    }
    acceptCommandApproval(gate_id, metadata, shell, previous_status);

    const bool restore_ok = offerRestorePointIfNeeded(preview, risky_change);
    m_runState.status = previous_status == ai::AiRunStatus::Idle ? ai::AiRunStatus::Idle
                                                                 : ai::AiRunStatus::Running;
    saveRunStateSnapshot();
    emitStatusDetails();
    return restore_ok;
}

bool AiAssistantPanel::consumeResumedCommandApproval(const QString& shell,
                                                     const QString& preview,
                                                     bool risky_change,
                                                     const QString& pending_call_id,
                                                     bool* approval_result) {
    if (pending_call_id.isEmpty() || m_resumedApprovedToolCallIds.removeAll(pending_call_id) <= 0) {
        return false;
    }
    QJsonObject metadata;
    metadata[QStringLiteral("shell")] = shell;
    metadata[QStringLiteral("preview")] = preview.left(kRestorePointErrorPreviewChars);
    metadata[QStringLiteral("risky_change")] = risky_change;
    metadata[QStringLiteral("tool_call_id")] = pending_call_id;
    metadata[QStringLiteral("summary")] = tr("Resumed command approval: %1").arg(shell);
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("command_approval"),
                 QStringLiteral("resumed"),
                 metadata);
    appendLocalEvent(tr("Resumed approval for %1 command").arg(shell));
    if (approval_result) {
        *approval_result = offerRestorePointIfNeeded(preview, risky_change);
    }
    return true;
}

QJsonObject AiAssistantPanel::commandApprovalMetadata(const QString& shell,
                                                      const QString& preview,
                                                      bool risky_change) const {
    QJsonObject metadata;
    metadata[QStringLiteral("shell")] = shell;
    metadata[QStringLiteral("preview")] = preview.left(kRestorePointErrorPreviewChars);
    metadata[QStringLiteral("risky_change")] = risky_change;
    const QJsonObject turn_state = pendingToolTurnState();
    if (!turn_state.isEmpty()) {
        metadata[QStringLiteral("tool_turn")] = turn_state;
        metadata[QStringLiteral("resume_supported")] = true;
        metadata[QStringLiteral("tool_call_id")] = turn_state.value(QStringLiteral("current_call"))
                                                       .toObject()
                                                       .value(QStringLiteral("call_id"))
                                                       .toString();
    }
    metadata[QStringLiteral("summary")] = tr("Waiting for command approval: %1").arg(shell);
    metadata[QStringLiteral("question_for_human")] = tr("Approve AI command?");
    return metadata;
}

QString AiAssistantPanel::beginCommandApprovalGate(QJsonObject* metadata) {
    const QString gate_id =
        beginHumanGate(QStringLiteral("approval"),
                       QStringLiteral("command_approval"),
                       metadata->value(QStringLiteral("question_for_human")).toString(),
                       *metadata);
    (*metadata)[QStringLiteral("gate_id")] = gate_id;
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("command_approval"),
                 QStringLiteral("waiting_for_human"),
                 *metadata);
    m_runState.status = ai::AiRunStatus::WaitingForHuman;
    m_runState.message = metadata->value(QStringLiteral("summary")).toString();
    saveRunStateSnapshot();
    emitStatusDetails();
    return gate_id;
}

bool AiAssistantPanel::requestCommandApprovalFromUser(const QString& shell,
                                                      const QString& preview) {
    const auto approval = showApprovalPrompt(
        {this,
         tr("Approve AI Command"),
         tr("Do you approve this %1 command?").arg(shell),
         tr("Review the exact command before allowing the AI assistant to run it on this PC."),
         preview,
         QVector<ApprovalPromptButton>{
             {tr("Approve"),
              ApprovalPromptChoice::Accept,
              ApprovalPromptButtonStyle::Primary,
              false},
             {tr("Reject"), ApprovalPromptChoice::Reject, ApprovalPromptButtonStyle::Danger, true},
         }});
    return approval == ApprovalPromptChoice::Accept;
}

bool AiAssistantPanel::rejectCommandApproval(const QString& gate_id,
                                             QJsonObject metadata,
                                             const QString& shell,
                                             ai::AiRunStatus previous_status) {
    metadata[QStringLiteral("decision")] = QStringLiteral("rejected");
    metadata[QStringLiteral("summary")] = tr("Command approval rejected: %1").arg(shell);
    resolveHumanGate(gate_id,
                     ai::humanGateRejectedStatus(),
                     QStringLiteral("rejected"),
                     metadata.value(QStringLiteral("summary")).toString(),
                     metadata);
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("command_approval"),
                 QStringLiteral("rejected"),
                 metadata);
    m_runState.status = previous_status == ai::AiRunStatus::Idle ? ai::AiRunStatus::Idle
                                                                 : ai::AiRunStatus::Running;
    m_runState.message = tr("Command rejected");
    saveRunStateSnapshot();
    emitStatusDetails();
    return false;
}

void AiAssistantPanel::acceptCommandApproval(const QString& gate_id,
                                             QJsonObject metadata,
                                             const QString& shell,
                                             ai::AiRunStatus previous_status) {
    metadata[QStringLiteral("decision")] = QStringLiteral("approved");
    metadata[QStringLiteral("summary")] = tr("Command approval granted: %1").arg(shell);
    resolveHumanGate(gate_id,
                     ai::humanGateApprovedStatus(),
                     QStringLiteral("approved"),
                     metadata.value(QStringLiteral("summary")).toString(),
                     metadata);
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("command_approval"),
                 QStringLiteral("approved"),
                 metadata);
    m_runState.status = previous_status == ai::AiRunStatus::Idle ? ai::AiRunStatus::Idle
                                                                 : ai::AiRunStatus::Running;
    m_runState.message = tr("Command approved");
    saveRunStateSnapshot();
    emitStatusDetails();
}

bool AiAssistantPanel::offerRestorePointIfNeeded(const QString& preview, bool risky_change) {
    if (consumeResumedRestoreDecision(preview, risky_change)) {
        return true;
    }
    if (!risky_change || m_restorePointOfferedThisSession) {
        return true;
    }

    const ai::AiRunStatus previous_status = m_runState.status;
    QJsonObject metadata = restorePointOfferMetadata(preview, risky_change);
    const QString gate_id =
        beginHumanGate(QStringLiteral("approval"),
                       QStringLiteral("restore_point_offer"),
                       metadata.value(QStringLiteral("question_for_human")).toString(),
                       metadata);
    metadata[QStringLiteral("gate_id")] = gate_id;
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("restore_point_offer"),
                 QStringLiteral("waiting_for_human"),
                 metadata);
    restoreRunStatusAfterHumanDecision(ai::AiRunStatus::WaitingForHuman,
                                       metadata.value(QStringLiteral("summary")).toString());

    const auto choice =
        showApprovalPrompt({this,
                            tr("Create Restore Point?"),
                            tr("Do you want to create a restore point before this command?"),
                            tr("This AI command may change the PC. A restore point gives you a "
                               "rollback option before continuing."),
                            preview,
                            QVector<ApprovalPromptButton>{{tr("Create Restore Point"),
                                                           ApprovalPromptChoice::Accept,
                                                           ApprovalPromptButtonStyle::Primary,
                                                           true},
                                                          {tr("Proceed Without"),
                                                           ApprovalPromptChoice::Secondary,
                                                           ApprovalPromptButtonStyle::Danger,
                                                           false},
                                                          {tr("Cancel"),
                                                           ApprovalPromptChoice::Cancel,
                                                           ApprovalPromptButtonStyle::Secondary,
                                                           false}}});
    return handleRestorePointOfferChoice(
        static_cast<int>(choice), gate_id, metadata, previous_status, preview);
}

bool AiAssistantPanel::consumeResumedRestoreDecision(const QString& preview, bool risky_change) {
    const QString pending_call_id = currentPendingToolCallId();
    if (pending_call_id.isEmpty() || m_resumedRestoreToolCallIds.removeAll(pending_call_id) <= 0) {
        return false;
    }
    m_restorePointOfferedThisSession = true;
    QJsonObject metadata;
    metadata[QStringLiteral("preview")] = preview.left(kMetadataPreviewMaxChars);
    metadata[QStringLiteral("risky_change")] = risky_change;
    metadata[QStringLiteral("tool_call_id")] = pending_call_id;
    metadata[QStringLiteral("summary")] = tr("Resumed restore-point decision");
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("restore_point_offer"),
                 QStringLiteral("resumed"),
                 metadata);
    appendLocalEvent(tr("Resumed restore-point decision for pending tool call"));
    return true;
}

QJsonObject AiAssistantPanel::restorePointOfferMetadata(const QString& preview,
                                                        bool risky_change) const {
    QJsonObject metadata;
    metadata[QStringLiteral("preview")] = preview.left(kMetadataPreviewMaxChars);
    metadata[QStringLiteral("risky_change")] = risky_change;
    const QJsonObject turn_state = pendingToolTurnState();
    if (!turn_state.isEmpty()) {
        metadata[QStringLiteral("tool_turn")] = turn_state;
        metadata[QStringLiteral("resume_supported")] = true;
        metadata[QStringLiteral("tool_call_id")] = turn_state.value(QStringLiteral("current_call"))
                                                       .toObject()
                                                       .value(QStringLiteral("call_id"))
                                                       .toString();
    }
    metadata[QStringLiteral("summary")] = tr("Waiting for restore-point decision");
    metadata[QStringLiteral("question_for_human")] =
        tr("Create a Windows restore point before this risky AI command?");
    return metadata;
}

bool AiAssistantPanel::handleRestorePointOfferChoice(int choice,
                                                     const QString& gate_id,
                                                     QJsonObject metadata,
                                                     ai::AiRunStatus previous_status,
                                                     const QString& preview) {
    const auto restore_choice = static_cast<ApprovalPromptChoice>(choice);
    if (restore_choice == ApprovalPromptChoice::Cancel ||
        restore_choice == ApprovalPromptChoice::Reject) {
        metadata[QStringLiteral("decision")] = QStringLiteral("cancelled");
        metadata[QStringLiteral("summary")] = tr("Restore-point offer cancelled");
        resolveHumanGate(gate_id,
                         ai::humanGateCancelledStatus(),
                         QStringLiteral("cancelled"),
                         metadata.value(QStringLiteral("summary")).toString(),
                         metadata);
        traceAiEvent(QStringLiteral("approval"),
                     QStringLiteral("restore_point_offer"),
                     QStringLiteral("cancelled"),
                     metadata);
        restoreRunStatusAfterHumanDecision(previous_status, tr("Restore point cancelled"));
        return false;
    }
    m_restorePointOfferedThisSession = true;
    if (restore_choice == ApprovalPromptChoice::Secondary) {
        metadata[QStringLiteral("decision")] = QStringLiteral("skipped");
        metadata[QStringLiteral("summary")] =
            tr("User skipped restore point before risky AI command");
        resolveHumanGate(gate_id,
                         ai::humanGateSkippedStatus(),
                         QStringLiteral("skipped"),
                         metadata.value(QStringLiteral("summary")).toString(),
                         metadata);
        traceAiEvent(QStringLiteral("approval"),
                     QStringLiteral("restore_point_offer"),
                     QStringLiteral("skipped"),
                     metadata);
        restoreRunStatusAfterHumanDecision(previous_status, tr("Restore point skipped"));
        return true;
    }
    metadata[QStringLiteral("decision")] = QStringLiteral("create");
    metadata[QStringLiteral("summary")] =
        tr("User requested restore point before risky AI command");
    resolveHumanGate(gate_id,
                     ai::humanGateApprovedStatus(),
                     QStringLiteral("create"),
                     metadata.value(QStringLiteral("summary")).toString(),
                     metadata);
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("restore_point_offer"),
                 QStringLiteral("approved"),
                 metadata);
    restoreRunStatusAfterHumanDecision(previous_status, tr("Creating restore point"));
    return createRestorePoint()
               ? true
               : handleRestorePointFailure(gate_id, metadata, previous_status, preview);
}

bool AiAssistantPanel::handleRestorePointFailure(const QString& gate_id,
                                                 QJsonObject metadata,
                                                 ai::AiRunStatus previous_status,
                                                 const QString& preview) {
    Q_UNUSED(gate_id);
    metadata[QStringLiteral("summary")] = tr("Waiting after restore-point creation failed");
    metadata[QStringLiteral("question_for_human")] =
        tr("Restore point failed. Continue with this command?");
    const QString failure_gate_id =
        beginHumanGate(QStringLiteral("approval"),
                       QStringLiteral("restore_point_failure_continue"),
                       metadata.value(QStringLiteral("question_for_human")).toString(),
                       metadata);
    metadata[QStringLiteral("gate_id")] = failure_gate_id;
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("restore_point_failure_continue"),
                 QStringLiteral("waiting_for_human"),
                 metadata);
    restoreRunStatusAfterHumanDecision(ai::AiRunStatus::WaitingForHuman,
                                       metadata.value(QStringLiteral("summary")).toString());

    const auto failure_choice =
        showApprovalPrompt({this,
                            tr("Restore Point Failed"),
                            tr("Do you approve continuing without a restore point?"),
                            tr("The restore point was not created. Review the command again before "
                               "deciding whether to continue."),
                            preview,
                            QVector<ApprovalPromptButton>{{tr("Continue Anyway"),
                                                           ApprovalPromptChoice::Accept,
                                                           ApprovalPromptButtonStyle::Danger,
                                                           false},
                                                          {tr("Stop"),
                                                           ApprovalPromptChoice::Reject,
                                                           ApprovalPromptButtonStyle::Secondary,
                                                           true}}});
    const bool proceed = failure_choice == ApprovalPromptChoice::Accept;
    metadata[QStringLiteral("decision")] = proceed ? QStringLiteral("continue_anyway")
                                                   : QStringLiteral("stop");
    metadata[QStringLiteral("summary")] = proceed ? tr("User continued after restore-point failure")
                                                  : tr("User stopped after restore-point failure");
    resolveHumanGate(failure_gate_id,
                     proceed ? ai::humanGateApprovedStatus() : ai::humanGateRejectedStatus(),
                     metadata.value(QStringLiteral("decision")).toString(),
                     metadata.value(QStringLiteral("summary")).toString(),
                     metadata);
    traceAiEvent(QStringLiteral("approval"),
                 QStringLiteral("restore_point_failure_continue"),
                 proceed ? QStringLiteral("approved") : QStringLiteral("rejected"),
                 metadata);
    restoreRunStatusAfterHumanDecision(previous_status,
                                       proceed ? tr("Continuing without restore point")
                                               : tr("Stopped after restore point failure"));
    return proceed;
}

void AiAssistantPanel::restoreRunStatusAfterHumanDecision(ai::AiRunStatus previous_status,
                                                          const QString& message) {
    m_runState.status = previous_status == ai::AiRunStatus::Idle ? ai::AiRunStatus::Idle
                                                                 : previous_status;
    m_runState.message = message;
    saveRunStateSnapshot();
    emitStatusDetails();
}
bool AiAssistantPanel::createRestorePoint() {
    if (!m_elevationBroker) {
        return handleRestorePointBrokerUnavailable();
    }

    const QString description = restorePointDescription();
    const QJsonObject payload = restorePointPayload(restorePointScript(description));
    appendLocalEvent(tr("Requesting Windows restore point (requires admin)"));
    Q_EMIT statusMessage(tr("Creating Windows restore point..."), sak::kTimerStatusDefaultMs);
    QJsonObject trace_metadata;
    traceRestorePointStart(&trace_metadata, description);
    const auto result = m_elevationBroker->executeTask(QStringLiteral("RunPowerShell"),
                                                       tr("Create AI session restore point"),
                                                       payload);
    if (!result) {
        return handleRestorePointExecutionError(fromStringView(to_string(result.error())),
                                                trace_metadata);
    }
    return handleRestorePointResponse(result->data, trace_metadata);
}

bool AiAssistantPanel::handleRestorePointBrokerUnavailable() {
    appendLocalEvent(tr("Elevation broker unavailable; cannot create restore point"));
    QJsonObject metadata;
    metadata[QStringLiteral("summary")] = tr("Restore point failed: elevation broker unavailable");
    metadata[QStringLiteral("error_message")] = QStringLiteral("Elevation broker unavailable");
    traceAiEvent(QStringLiteral("restore_point"),
                 QStringLiteral("windows_restore_point"),
                 QStringLiteral("failed"),
                 metadata);
    return false;
}

QString AiAssistantPanel::restorePointDescription() const {
    return QStringLiteral("SAK AI session %1")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")));
}

QString AiAssistantPanel::restorePointScript(const QString& description) const {
    return QStringLiteral(
               "try { Checkpoint-Computer -Description '%1' -RestorePointType MODIFY_SETTINGS; "
               "Write-Output 'restore-point-created' } "
               "catch { Write-Error $_; exit 1 }")
        .arg(description);
}

QJsonObject AiAssistantPanel::restorePointPayload(const QString& script) const {
    QJsonObject payload;
    payload[QStringLiteral("command")] = script;
    payload[QStringLiteral("timeout_seconds")] = kRestorePointTimeoutSec;
    payload[QStringLiteral("max_output_bytes")] = kRestorePointMaxOutputBytes;
    return payload;
}

void AiAssistantPanel::traceRestorePointStart(QJsonObject* trace_metadata,
                                              const QString& description) {
    (*trace_metadata)[QStringLiteral("summary")] = tr("Creating Windows restore point");
    (*trace_metadata)[QStringLiteral("description")] = description;
    traceAiEvent(QStringLiteral("restore_point"),
                 QStringLiteral("windows_restore_point"),
                 QStringLiteral("running"),
                 *trace_metadata);
}

bool AiAssistantPanel::handleRestorePointExecutionError(const QString& error_message,
                                                        QJsonObject trace_metadata) {
    appendLocalEvent(tr("Restore point request failed: %1").arg(error_message));
    Q_EMIT statusMessage(tr("Restore point failed"), sak::kTimerStatusDefaultMs);
    trace_metadata[QStringLiteral("summary")] = tr("Restore point request failed");
    trace_metadata[QStringLiteral("error_message")] = error_message;
    traceAiEvent(QStringLiteral("restore_point"),
                 QStringLiteral("windows_restore_point"),
                 QStringLiteral("failed"),
                 trace_metadata);
    return false;
}

bool AiAssistantPanel::handleRestorePointResponse(const QJsonObject& response_data,
                                                  QJsonObject trace_metadata) {
    const QString stdout_text = response_data.value(QStringLiteral("stdout")).toString();
    const int exit_code = response_data.value(QStringLiteral("exit_code")).toInt(-1);
    if (exit_code == kRestorePointSuccessExitCode &&
        stdout_text.contains(QStringLiteral("restore-point-created"))) {
        appendLocalEvent(tr("Windows restore point created"));
        Q_EMIT statusMessage(tr("Restore point created"), sak::kTimerStatusDefaultMs);
        trace_metadata[QStringLiteral("summary")] = tr("Windows restore point created");
        trace_metadata[QStringLiteral("exit_code")] = exit_code;
        traceAiEvent(QStringLiteral("restore_point"),
                     QStringLiteral("windows_restore_point"),
                     QStringLiteral("completed"),
                     trace_metadata);
        return true;
    }
    const QString stderr_text = response_data.value(QStringLiteral("stderr")).toString();
    appendLocalEvent(
        tr("Restore point creation reported exit %1: %2")
            .arg(exit_code)
            .arg(stderr_text.trimmed().isEmpty() ? stdout_text.trimmed() : stderr_text.trimmed()));
    Q_EMIT statusMessage(tr("Restore point not created (see log)"), sak::kTimerStatusDefaultMs);
    trace_metadata[QStringLiteral("summary")] = tr("Restore point creation failed");
    trace_metadata[QStringLiteral("exit_code")] = exit_code;
    trace_metadata[QStringLiteral("error_message")] =
        stderr_text.trimmed().isEmpty()
            ? stdout_text.trimmed().left(kRestorePointErrorPreviewChars)
            : stderr_text.trimmed().left(kRestorePointErrorPreviewChars);
    traceAiEvent(QStringLiteral("restore_point"),
                 QStringLiteral("windows_restore_point"),
                 QStringLiteral("failed"),
                 trace_metadata);
    return false;
}

QJsonObject AiAssistantPanel::runScreenshotTool(const QString& reason) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    if (!m_conversationStore) {
        result[QStringLiteral("error_message")] =
            QStringLiteral("No active AI session for screenshot");
        return result;
    }
    QScreen* screen = QGuiApplication::primaryScreen();
    if (!screen) {
        result[QStringLiteral("error_message")] = QStringLiteral("No primary screen available");
        return result;
    }
    const QPixmap pixmap = screen->grabWindow(0);
    if (pixmap.isNull()) {
        result[QStringLiteral("error_message")] =
            QStringLiteral("Screen capture returned empty image");
        return result;
    }
    const QString filename =
        QStringLiteral("screenshot_%1.png")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmsszzz")));
    QString error;
    const QString path =
        m_conversationStore->artifactPath(QStringLiteral("screenshots"), filename, &error);
    if (path.isEmpty()) {
        result[QStringLiteral("error_message")] = error;
        return result;
    }
    if (!pixmap.save(path, "PNG")) {
        result[QStringLiteral("error_message")] =
            QStringLiteral("Failed to save screenshot: %1").arg(path);
        return result;
    }
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("width")] = pixmap.width();
    result[QStringLiteral("height")] = pixmap.height();
    result[QStringLiteral("reason")] = reason;
    appendArtifactRow(path, QStringLiteral("screenshot"));
    return result;
}

QJsonObject AiAssistantPanel::runDownloadTool(const QString& url_string, const QString& filename) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    if (!m_conversationStore) {
        result[QStringLiteral("error_message")] =
            QStringLiteral("No active AI session for download");
        return result;
    }
    const QUrl url(url_string);
    if (!url.isValid() || url.scheme().toLower() != QLatin1String("https")) {
        result[QStringLiteral("error_message")] =
            QStringLiteral("Only https URLs are accepted; got '%1'").arg(url_string);
        return result;
    }
    const QString safe_name = safeDownloadFileName(url, filename);
    QString error;
    const QString destination =
        m_conversationStore->artifactPath(QStringLiteral("downloads"), safe_name, &error);
    if (destination.isEmpty()) {
        result[QStringLiteral("error_message")] = error;
        return result;
    }

    QByteArray bytes;
    if (!downloadUrlBytes(url, &bytes, &error) || !writeDownloadBytes(destination, bytes, &error)) {
        result[QStringLiteral("error_message")] = error;
        return result;
    }

    const QString sha256 =
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());

    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = destination;
    result[QStringLiteral("size_bytes")] = static_cast<double>(bytes.size());
    result[QStringLiteral("sha256")] = sha256;
    result[QStringLiteral("source_url")] = url_string;
    appendArtifactRow(destination, QStringLiteral("download"));
    return result;
}

QJsonObject AiAssistantPanel::runWorkflowPowerShellTool(const QJsonObject& args,
                                                        const QString& command_preview) {
    ai::AiWorkflowPowerShellToolCallbacks callbacks;
    callbacks.confirm = [this](const QString& title, const QString& preview, bool risky) {
        return confirmCommandWithUser(title, preview, risky);
    };
    callbacks.allocate_command_id = [this]() {
        return allocateCommandId();
    };
    callbacks.execute_powershell = [this](const ai::AiCommandRequest& request,
                                          const QString& command_id) {
        return executeWorkflowPowerShellRequest(request, command_id);
    };
    callbacks.append_local_event = [this](const QString& message) {
        appendLocalEvent(message);
    };
    callbacks.log_output = [this](const QString& line) {
        Q_EMIT logOutput(line);
    };
    callbacks.record_command = [this](const QString& preview, const QJsonObject& result_json) {
        if (!m_conversationStore) {
            return;
        }
        QString error;
        (void)m_conversationStore->appendCommand(preview, result_json, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Workflow command record failed: %1").arg(error));
        }
        reloadSessionPicker();
    };
    callbacks.append_session_memory =
        [this](const QString& role, const QString& title, const QString& body) {
            appendSessionMemory(role, title, body);
        };

    return ai::AiWorkflowPowerShellToolRunner::run(
        args,
        command_preview,
        callbacks,
        ai::AiWorkflowPowerShellToolOptions{static_cast<int>(kDefaultOutputCapKb *
                                                             sak::kBytesPerKB),
                                            static_cast<int>(sak::kBytesPerKB)});
}

ai::AiCommandResult AiAssistantPanel::executeWorkflowPowerShellRequest(
    const ai::AiCommandRequest& request, const QString& command_id) {
    if (request.requires_admin) {
        return executeElevatedWorkflowPowerShellRequest(request);
    }
    return executeStandardWorkflowPowerShellRequest(request, command_id);
}

ai::AiCommandResult AiAssistantPanel::executeElevatedWorkflowPowerShellRequest(
    const ai::AiCommandRequest& request) {
    if (QThread::currentThread() == thread()) {
        return runElevatedPowerShell(request);
    }
    ai::AiCommandResult command_result;
    QSemaphore done;
    const bool invoked = QMetaObject::invokeMethod(
        this,
        [this, request, &command_result, &done]() {
            command_result = runElevatedPowerShell(request);
            done.release();
        },
        Qt::QueuedConnection);
    if (!invoked) {
        command_result.elevated = true;
        command_result.error_message =
            QStringLiteral("Could not marshal elevated PowerShell execution to UI thread");
        return command_result;
    }
    done.acquire();
    return command_result;
}

ai::AiCommandResult AiAssistantPanel::executeStandardWorkflowPowerShellRequest(
    const ai::AiCommandRequest& request, const QString& command_id) {
    ai::AiCommandResult command_result;
    bool finished = false;
    QSemaphore done;
    QThread command_thread;
    QObject* worker = new QObject;
    worker->moveToThread(&command_thread);
    QObject::connect(&command_thread, &QThread::finished, worker, &QObject::deleteLater);
    const bool caller_is_ui_thread = QThread::currentThread() == thread();
    QObject::connect(&command_thread,
                     &QThread::started,
                     worker,
                     [this,
                      worker,
                      request,
                      command_id,
                      caller_is_ui_thread,
                      &command_result,
                      &finished,
                      &done,
                      &command_thread]() {
                         auto* broker = new ai::ExecutionBroker(worker);
                         auto* cancel_timer = new QTimer(worker);
                         connectWorkflowPowerShellLogging(broker, worker);
                         QObject::connect(broker,
                                          &ai::ExecutionBroker::finished,
                                          worker,
                                          [&](const QString&, const ai::AiCommandResult& result) {
                                              command_result = result;
                                              finished = true;
                                              done.release();
                                              command_thread.quit();
                                          });
                         if (!caller_is_ui_thread) {
                             connectWorkflowPowerShellCancelPolling(cancel_timer, broker, worker);
                         }
                         (void)broker->startPowerShell(request, command_id);
                     });
    command_thread.start();
    done.acquire();
    command_thread.quit();
    command_thread.wait();
    if (!finished) {
        command_result.error_message =
            QStringLiteral("Workflow PowerShell command did not report completion");
    }
    return command_result;
}

void AiAssistantPanel::connectWorkflowPowerShellLogging(ai::ExecutionBroker* broker,
                                                        QObject* worker) {
    QObject::connect(broker,
                     &ai::ExecutionBroker::stdoutChunk,
                     worker,
                     [this](const QString& id, const QString& chunk) {
                         emitPrefixedLogLines(
                             [this](const QString& line) {
                                 QMetaObject::invokeMethod(
                                     this,
                                     [this, line]() {
                                         Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line));
                                     },
                                     Qt::QueuedConnection);
                             },
                             QStringLiteral("[%1]").arg(id),
                             chunk);
                     });
    QObject::connect(broker,
                     &ai::ExecutionBroker::stderrChunk,
                     worker,
                     [this](const QString& id, const QString& chunk) {
                         emitPrefixedLogLines(
                             [this](const QString& line) {
                                 QMetaObject::invokeMethod(
                                     this,
                                     [this, line]() {
                                         Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line));
                                     },
                                     Qt::QueuedConnection);
                             },
                             QStringLiteral("[%1 err]").arg(id),
                             chunk);
                     });
}

void AiAssistantPanel::connectWorkflowPowerShellCancelPolling(QTimer* cancel_timer,
                                                              ai::ExecutionBroker* broker,
                                                              QObject* worker) {
    cancel_timer->setInterval(kWorkflowWorkerCancelPollMs);
    QObject::connect(cancel_timer, &QTimer::timeout, worker, [this, broker]() {
        bool should_cancel = false;
        const bool invoked = QMetaObject::invokeMethod(
            this,
            [this, &should_cancel]() {
                should_cancel = m_runToken.isValid() && m_runToken.isCancellationRequested();
            },
            Qt::BlockingQueuedConnection);
        if (invoked && should_cancel && broker->isRunning()) {
            broker->cancel();
        }
    });
    cancel_timer->start();
}

QJsonObject AiAssistantPanel::runPackageManagerTool(const QJsonObject& args) {
    if (currentAccessMode() == AccessMode::ChatAndResearch) {
        return toolError(QStringLiteral("Local package management is disabled in Chat mode"));
    }
    if (!m_chocoManager || !m_chocoManager->isInitialized()) {
        return toolError(QStringLiteral("SAK bundled Chocolatey package manager is not available"));
    }

    const ai::AiPackageToolPlan plan = ai::AiPackageToolPlanner::buildPlan(args);
    if (!plan.ok()) {
        return toolError(plan.error_message);
    }

    const QJsonObject query_result = packageManagerQueryOperation(args, plan.operation, plan.query);
    if (!query_result.isEmpty()) {
        return query_result;
    }
    return packageManagerPackageOperation(
        plan.operation, plan.package_id, plan.version, plan.timeout_seconds);
}

QJsonObject AiAssistantPanel::runProviderGatewayTool(const QJsonObject& args) {
    ai::AiProviderGateway gateway;
    ai::AiProviderGatewayToolAccess access = ai::AiProviderGatewayToolAccess::ChatAndResearch;
    if (currentAccessMode() == AccessMode::AssistedFullAccess) {
        access = ai::AiProviderGatewayToolAccess::AssistedFullAccess;
    } else if (currentAccessMode() == AccessMode::UnattendedFullAccess) {
        access = ai::AiProviderGatewayToolAccess::UnattendedFullAccess;
    }

    ai::AiProviderGatewayToolCallbacks callbacks;
    callbacks.confirm = [this](const QString& title, const QString& preview, bool risky) {
        return confirmCommandWithUser(title, preview, risky);
    };
    callbacks.offer_restore_point = [this](const QString& preview, bool risky) {
        return offerRestorePointIfNeeded(preview, risky);
    };
    callbacks.allocate_command_id = [this]() {
        return allocateCommandId();
    };
    callbacks.execute_powershell = [this](const ai::AiCommandRequest& request,
                                          const QString& command_id) {
        return executeWorkflowPowerShellRequest(request, command_id);
    };
    callbacks.append_local_event = [this](const QString& message) {
        appendLocalEvent(message);
    };
    callbacks.log_output = [this](const QString& line) {
        Q_EMIT logOutput(line);
    };
    callbacks.record_command = [this](const QString& preview, const QJsonObject& result) {
        if (!m_conversationStore) {
            return;
        }
        QString error;
        (void)m_conversationStore->appendCommand(preview, result, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("App action command record failed: %1").arg(error));
        }
        reloadSessionPicker();
    };
    callbacks.append_session_memory =
        [this](const QString& role, const QString& title, const QString& body) {
            appendSessionMemory(role, title, body);
        };

    return ai::AiProviderGatewayToolRunner::run(
        args,
        &gateway,
        access,
        callbacks,
        ai::AiProviderGatewayToolOptions{static_cast<int>(kDefaultOutputCapKb * sak::kBytesPerKB),
                                         static_cast<int>(sak::kBytesPerKB),
                                         kToolOutputMaxBytes});
}

QJsonObject AiAssistantPanel::runSessionSearchTool(const QJsonObject& args) const {
    if (!m_conversationStore) {
        QJsonObject result = toolError(QStringLiteral("AI session store is not available"));
        result[QStringLiteral("failure_class")] = QStringLiteral("session_store_unavailable");
        return result;
    }
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();
    const int max_results = std::clamp(args.value(QStringLiteral("max_results")).toInt(25), 1, 100);
    QString error;
    const auto hits = m_conversationStore->searchSessions(query, max_results, &error);
    if (!error.isEmpty()) {
        QJsonObject result = toolError(error);
        result[QStringLiteral("failure_class")] = QStringLiteral("session_search_failed");
        return result;
    }

    QJsonArray results;
    for (const auto& hit : hits) {
        QJsonObject item;
        item[QStringLiteral("session_id")] = hit.session.id;
        item[QStringLiteral("session_title")] = hit.session.title;
        item[QStringLiteral("session_path")] = hit.session.path;
        item[QStringLiteral("source")] = hit.source;
        item[QStringLiteral("score")] = hit.score;
        item[QStringLiteral("snippet")] = hit.snippet;
        item[QStringLiteral("timestamp_utc")] =
            hit.timestamp_utc.isValid() ? hit.timestamp_utc.toUTC().toString(Qt::ISODateWithMs)
                                        : QString();
        results.append(item);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = QStringLiteral("search");
    result[QStringLiteral("query")] = query;
    result[QStringLiteral("result_count")] = hits.size();
    result[QStringLiteral("results")] = results;
    return result;
}

QJsonObject AiAssistantPanel::packageManagerQueryOperation(const QJsonObject& args,
                                                           const QString& operation,
                                                           const QString& query) {
    if (operation == QLatin1String("version")) {
        return packageManagerVersionResult();
    }
    if (operation == QLatin1String("search")) {
        return packageManagerSearchResult(args, query);
    }
    if (operation == QLatin1String("outdated")) {
        appendLocalEvent(tr("SAK package outdated check"));
        QJsonObject result;
        const QStringList outdated = m_chocoManager->getOutdatedPackages();
        result[QStringLiteral("success")] = true;
        result[QStringLiteral("operation")] = operation;
        result[QStringLiteral("packages")] = QJsonArray::fromStringList(outdated);
        result[QStringLiteral("package_count")] = outdated.size();
        return result;
    }
    return {};
}

QJsonObject AiAssistantPanel::packageManagerPackageOperation(const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version,
                                                             int timeout_seconds) {
    const QJsonObject preflight = packageManagerPackagePreflight(operation, package_id, version);
    if (!preflight.isEmpty()) {
        return preflight;
    }
    const QString preview = packageManagerChangePreview(operation, package_id, version);
    const QJsonObject authorization_error = authorizePackageManagerChange(preview);
    if (!authorization_error.isEmpty()) {
        return authorization_error;
    }
    return executePackageManagerPackageChange(
        operation, package_id, version, timeout_seconds, preview);
}

QJsonObject AiAssistantPanel::packageManagerPackagePreflight(const QString& operation,
                                                             const QString& package_id,
                                                             const QString& version) {
    if (package_id.isEmpty()) {
        return toolError(QStringLiteral("%1 requires package_id").arg(operation));
    }
    if (isPackageReadOperation(operation)) {
        return packageManagerReadResult(operation, package_id);
    }
    if (!isPackageChangeOperation(operation)) {
        return toolError(
            QStringLiteral("Unsupported package manager operation: %1").arg(operation));
    }
    if (operation == QLatin1String("install")) {
        const QString installed_version = m_chocoManager->getInstalledVersion(package_id);
        const bool installed = !installed_version.isEmpty() ||
                               m_chocoManager->isPackageInstalled(package_id);
        if (installed) {
            return packageAlreadyInstalledResult(operation, package_id, version, installed_version);
        }
    }
    return {};
}

QJsonObject AiAssistantPanel::authorizePackageManagerChange(const QString& preview) {
    if (currentAccessMode() == AccessMode::AssistedFullAccess &&
        !confirmCommandWithUser(tr("SAK Package Manager"), preview, true)) {
        return toolError(QStringLiteral("User declined SAK package manager operation"));
    }
    if (currentAccessMode() == AccessMode::UnattendedFullAccess &&
        !offerRestorePointIfNeeded(preview, true)) {
        return toolError(QStringLiteral("User cancelled before package manager operation"));
    }
    return {};
}

QJsonObject AiAssistantPanel::executePackageManagerPackageChange(const QString& operation,
                                                                 const QString& package_id,
                                                                 const QString& version,
                                                                 int timeout_seconds,
                                                                 const QString& preview) {
    m_chocoManager->setDefaultTimeout(timeout_seconds);
    appendLocalEvent(preview);
    Q_EMIT logOutput(QStringLiteral("[SAK Package Manager] %1").arg(preview));
    QJsonObject result = runPackageManagerChange(operation, package_id, version, timeout_seconds);
    if (operation == QLatin1String("install")) {
        result[QStringLiteral("preflight_checked")] = true;
        result[QStringLiteral("preflight_installed")] = false;
    }
    return result;
}

QJsonObject AiAssistantPanel::packageAlreadyInstalledResult(const QString& operation,
                                                            const QString& package_id,
                                                            const QString& version,
                                                            const QString& installed_version) {
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = operation;
    result[QStringLiteral("package_id")] = package_id;
    result[QStringLiteral("version")] = version;
    result[QStringLiteral("installed")] = true;
    result[QStringLiteral("installed_version")] = installed_version;
    result[QStringLiteral("preflight_checked")] = true;
    result[QStringLiteral("preflight_installed")] = true;
    result[QStringLiteral("skipped")] = true;
    result[QStringLiteral("skip_reason")] =
        QStringLiteral("Package already installed; install not run");
    result[QStringLiteral("tool_path")] = m_chocoManager->getChocoPath();
    appendLocalEvent(tr("SAK package install skipped: %1 already installed").arg(package_id));
    return result;
}

QJsonObject AiAssistantPanel::packageManagerVersionResult() const {
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = QStringLiteral("version");
    result[QStringLiteral("chocolatey_version")] = m_chocoManager->getChocoVersion();
    result[QStringLiteral("tool_path")] = m_chocoManager->getChocoPath();
    return result;
}

QJsonObject AiAssistantPanel::packageManagerSearchResult(const QJsonObject& args,
                                                         const QString& query) {
    QStringList queries = jsonStringList(args.value(QStringLiteral("queries")));
    if (queries.isEmpty() && !query.isEmpty()) {
        queries << query;
    }
    queries.removeDuplicates();
    if (queries.isEmpty()) {
        return toolError(QStringLiteral("search requires query"));
    }

    QJsonArray all_packages;
    QJsonArray search_results;
    bool all_success = true;
    QStringList errors;
    for (const auto& package_query : queries) {
        appendLocalEvent(tr("SAK package search: %1").arg(package_query));
        const auto search = m_chocoManager->searchPackage(package_query,
                                                          offline::kSearchResultsDefault);
        all_success = all_success && search.success;
        if (!search.success) {
            errors << (search.error_message.isEmpty() ? search.output.left(kMetadataPreviewMaxChars)
                                                      : search.error_message);
        }
        const auto packages = m_chocoManager->parseSearchResults(search.output);
        QJsonArray package_json = packageInfoToJson(packages);
        for (auto item : package_json) {
            QJsonObject package = item.toObject();
            package[QStringLiteral("source_query")] = package_query;
            all_packages.append(package);
        }
        QJsonObject search_json;
        search_json[QStringLiteral("query")] = package_query;
        search_json[QStringLiteral("success")] = search.success;
        search_json[QStringLiteral("exit_code")] = search.exit_code;
        search_json[QStringLiteral("error_message")] = search.error_message;
        search_json[QStringLiteral("package_count")] = static_cast<int>(packages.size());
        search_json[QStringLiteral("packages")] = package_json;
        search_results.append(search_json);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = all_success && !queries.isEmpty();
    result[QStringLiteral("operation")] = QStringLiteral("search");
    result[QStringLiteral("query")] = query;
    result[QStringLiteral("queries")] = stringListToJsonArray(queries);
    result[QStringLiteral("search_results")] = search_results;
    result[QStringLiteral("packages")] = all_packages;
    result[QStringLiteral("package_count")] = static_cast<int>(all_packages.size());
    result[QStringLiteral("error_message")] = errors.join(QStringLiteral("; "));
    return result;
}

QJsonObject AiAssistantPanel::packageManagerReadResult(const QString& operation,
                                                       const QString& package_id) const {
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = operation;
    result[QStringLiteral("package_id")] = package_id;
    if (operation == QLatin1String("is_installed")) {
        result[QStringLiteral("installed")] = m_chocoManager->isPackageInstalled(package_id);
        return result;
    }
    const QString installed_version = m_chocoManager->getInstalledVersion(package_id);
    result[QStringLiteral("installed")] = !installed_version.isEmpty();
    result[QStringLiteral("installed_version")] = installed_version;
    return result;
}

QString AiAssistantPanel::packageManagerChangePreview(const QString& operation,
                                                      const QString& package_id,
                                                      const QString& version) const {
    if (operation == QLatin1String("install")) {
        return tr("Install Chocolatey package '%1'%2 using SAK package manager")
            .arg(package_id, version.isEmpty() ? QString() : tr(" version %1").arg(version));
    }
    if (operation == QLatin1String("uninstall")) {
        return tr("Uninstall Chocolatey package '%1' using SAK package manager").arg(package_id);
    }
    return tr("Upgrade Chocolatey package '%1' using SAK package manager").arg(package_id);
}

QJsonObject AiAssistantPanel::runPackageManagerChange(const QString& operation,
                                                      const QString& package_id,
                                                      const QString& version,
                                                      int timeout_seconds) {
    ChocolateyManager::Result package_result;
    if (operation == QLatin1String("install")) {
        ChocolateyManager::InstallConfig config;
        config.package_name = package_id;
        config.version = version;
        config.version_locked = !version.isEmpty();
        config.auto_confirm = true;
        config.timeout_seconds = timeout_seconds;
        package_result = m_chocoManager->installPackage(config);
    } else if (operation == QLatin1String("uninstall")) {
        package_result = m_chocoManager->uninstallPackage(package_id, true);
    } else {
        package_result = m_chocoManager->upgradePackage(package_id, true);
    }

    QJsonObject result;
    result[QStringLiteral("operation")] = operation;
    result[QStringLiteral("package_id")] = package_id;
    result[QStringLiteral("version")] = version;
    result[QStringLiteral("success")] = package_result.success;
    result[QStringLiteral("exit_code")] = package_result.exit_code;
    result[QStringLiteral("output")] = package_result.output.left(kPackageResultOutputMaxChars);
    result[QStringLiteral("error_message")] = package_result.error_message;
    result[QStringLiteral("tool_path")] = m_chocoManager->getChocoPath();
    return result;
}
QJsonObject AiAssistantPanel::runOfflineDownloaderTool(const QJsonObject& args) {
    if (currentAccessMode() == AccessMode::ChatAndResearch) {
        return toolError(QStringLiteral("Local offline downloader is disabled in Chat mode"));
    }
    if (!m_offlineWorker || !m_packageListManager) {
        return toolError(QStringLiteral("SAK offline downloader is not available"));
    }

    ai::AiOfflineDownloaderToolCallbacks callbacks;
    callbacks.is_running = [this]() {
        return m_offlineWorker && m_offlineWorker->isRunning();
    };
    callbacks.presets_result = [this]() {
        return offlinePresetsResult();
    };
    callbacks.search_result = [this](const QJsonObject& search_args, const QString& query) {
        return offlineSearchResult(search_args, query);
    };
    callbacks.run_operation = [this](const QJsonObject& run_args, const QString& operation) {
        return offlineRunOperation(run_args, operation);
    };
    return ai::AiOfflineDownloaderToolRunner::run(args, callbacks);
}

QJsonObject AiAssistantPanel::offlineRunOperation(const QJsonObject& args,
                                                  const QString& operation) {
    QString package_error;
    const QVector<QPair<QString, QString>> packages =
        packagesFromJson(args.value(QStringLiteral("packages")).toArray(), &package_error);
    if (offlineNeedsPackages(operation) && packages.isEmpty()) {
        return toolError(package_error.isEmpty()
                             ? QStringLiteral("%1 requires at least one package").arg(operation)
                             : package_error);
    }
    QString output_error;
    const QString output_dir = offlineOutputDirectory(operation, args, &output_error);
    if (!output_error.isEmpty()) {
        return toolError(output_error);
    }
    if (!authorizeOfflineOperation(operation, args)) {
        return toolError(QStringLiteral("User declined or cancelled offline downloader operation"));
    }

    const OfflineToolRunResult run_result =
        executeOfflineOperation(operation, packages, output_dir, args);
    QJsonObject result = offlineOperationResultJson(operation, output_dir, run_result);
    appendOfflineArtifacts(operation, output_dir, run_result.manifest_written, &result);
    return result;
}

void AiAssistantPanel::appendOfflineArtifacts(const QString& operation,
                                              const QString& output_dir,
                                              const QString& manifest_path,
                                              QJsonObject* result) {
    if (!output_dir.isEmpty() && QDir(output_dir).exists()) {
        const QStringList files = filesUnderDirectory(output_dir);
        if (result) {
            (*result)[QStringLiteral("files")] =
                QJsonArray::fromStringList(files.mid(0, kPackageFileResultLimit));
            (*result)[QStringLiteral("file_count")] = files.size();
        }
        for (const auto& file : files.mid(0, kPackageArtifactDisplayLimit)) {
            appendArtifactRow(file, operation);
        }
    }
    if (!manifest_path.isEmpty()) {
        appendArtifactRow(manifest_path, QStringLiteral("offline-bundle"));
    }
}

QJsonObject AiAssistantPanel::offlinePresetsResult() const {
    QJsonArray presets;
    for (const auto& name : m_packageListManager->presetNames()) {
        const PackageList preset = m_packageListManager->preset(name);
        QJsonObject item;
        item[QStringLiteral("name")] = preset.name;
        item[QStringLiteral("description")] = preset.description;
        item[QStringLiteral("packages")] = presetToJson(preset);
        presets.append(item);
    }
    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("operation")] = QStringLiteral("presets");
    result[QStringLiteral("presets")] = presets;
    result[QStringLiteral("preset_count")] = presets.size();
    return result;
}

QJsonObject AiAssistantPanel::offlineSearchResult(const QJsonObject& args, const QString& query) {
    if (!m_chocoManager || !m_chocoManager->isInitialized()) {
        return toolError(QStringLiteral("SAK bundled Chocolatey package manager is not available"));
    }
    return packageManagerSearchResult(args, query);
}

QString AiAssistantPanel::offlineOutputDirectory(const QString& operation,
                                                 const QJsonObject& args,
                                                 QString* error_message) const {
    QString output_dir = args.value(QStringLiteral("output_dir")).toString().trimmed();
    if (!output_dir.isEmpty() || !offlineCreatesDefaultOutput(operation)) {
        return output_dir;
    }
    if (!m_conversationStore) {
        if (error_message) {
            *error_message = QStringLiteral("No active AI session for artifact output");
        }
        return {};
    }
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmsszzz"));
    const QString subdir = operation == QLatin1String("direct_download")
                               ? QStringLiteral("downloads/offline_installers_%1").arg(stamp)
                               : QStringLiteral("downloads/offline_bundle_%1").arg(stamp);
    return m_conversationStore->artifactSubdir(subdir, error_message);
}

bool AiAssistantPanel::authorizeOfflineOperation(const QString& operation,
                                                 const QJsonObject& args) {
    if (operation != QLatin1String("install_bundle")) {
        return true;
    }
    const QString manifest_path = args.value(QStringLiteral("manifest_path")).toString().trimmed();
    const QString preview =
        tr("Install offline Chocolatey deployment bundle from '%1'").arg(manifest_path);
    if (currentAccessMode() == AccessMode::AssistedFullAccess) {
        return confirmCommandWithUser(tr("SAK Offline Downloader"), preview, true);
    }
    if (currentAccessMode() == AccessMode::UnattendedFullAccess) {
        return offerRestorePointIfNeeded(preview, true);
    }
    return true;
}

AiAssistantPanel::OfflineToolRunResult AiAssistantPanel::executeOfflineOperation(
    const QString& operation,
    const QVector<QPair<QString, QString>>& packages,
    const QString& output_dir,
    const QJsonObject& args) {
    OfflineToolRunResult run;
    if (!validateOfflineOperation(operation, &run)) {
        return run;
    }

    appendOfflineOperationStartedEvent(operation, output_dir, args);
    QSemaphore done;
    QThread offline_thread;
    auto* worker = new OfflineDeploymentWorker;
    auto* context = new QObject;
    worker->moveToThread(&offline_thread);
    context->moveToThread(&offline_thread);
    QObject::connect(&offline_thread, &QThread::finished, worker, &QObject::deleteLater);
    QObject::connect(&offline_thread, &QThread::finished, context, &QObject::deleteLater);
    connectOfflineWorkerSignals(worker, context, &offline_thread, &done, &run);
    QObject::connect(&offline_thread,
                     &QThread::started,
                     context,
                     [this, worker, operation, packages, output_dir, args]() {
                         startOfflineWorkerOperation(worker, operation, packages, output_dir, args);
                     });
    offline_thread.start();
    done.acquire();
    offline_thread.quit();
    offline_thread.wait();
    return run;
}

bool AiAssistantPanel::validateOfflineOperation(const QString& operation,
                                                OfflineToolRunResult* run) const {
    const bool supported = operation == QLatin1String("direct_download") ||
                           operation == QLatin1String("build_bundle") ||
                           operation == QLatin1String("install_bundle");
    if (!supported && run) {
        run->operation_error =
            QStringLiteral("Unsupported offline downloader operation: %1").arg(operation);
    }
    return supported;
}

void AiAssistantPanel::appendOfflineOperationStartedEvent(const QString& operation,
                                                          const QString& output_dir,
                                                          const QJsonObject& args) {
    if (operation == QLatin1String("direct_download")) {
        appendLocalEvent(tr("SAK offline direct download to %1").arg(output_dir));
        return;
    }
    if (operation == QLatin1String("build_bundle")) {
        appendLocalEvent(tr("SAK offline bundle build to %1").arg(output_dir));
        return;
    }
    appendLocalEvent(tr("SAK offline bundle install: %1")
                         .arg(args.value(QStringLiteral("manifest_path")).toString().trimmed()));
}

void AiAssistantPanel::connectOfflineWorkerSignals(OfflineDeploymentWorker* worker,
                                                   QObject* context,
                                                   QThread* offline_thread,
                                                   QSemaphore* done,
                                                   OfflineToolRunResult* run) {
    QObject::connect(worker,
                     &OfflineDeploymentWorker::operationCompleted,
                     context,
                     [offline_thread, done, run](const BatchStats& stats) {
                         run->final_stats = stats;
                         run->completed = true;
                         done->release();
                         offline_thread->quit();
                     });
    QObject::connect(worker,
                     &OfflineDeploymentWorker::operationError,
                     context,
                     [offline_thread, done, run](const QString& error) {
                         run->operation_error = error;
                         done->release();
                         offline_thread->quit();
                     });
    QObject::connect(worker,
                     &OfflineDeploymentWorker::manifestWritten,
                     context,
                     [run](const QString& manifest_path) {
                         run->manifest_written = manifest_path;
                     });
    QObject::connect(
        worker, &OfflineDeploymentWorker::logMessage, context, [this, run](const QString& message) {
            run->log_lines.append(message);
            QMetaObject::invokeMethod(
                this,
                [this, message]() {
                    Q_EMIT logOutput(QStringLiteral("[SAK Offline] %1").arg(message));
                },
                Qt::QueuedConnection);
        });
    QObject::connect(worker,
                     &OfflineDeploymentWorker::packageProgress,
                     context,
                     [run](const QString& package_id, bool success, const QString& message) {
                         QJsonObject event;
                         event[QStringLiteral("package_id")] = package_id;
                         event[QStringLiteral("success")] = success;
                         event[QStringLiteral("message")] =
                             message.left(kRestorePointErrorPreviewChars);
                         run->package_events.append(event);
                     });
}

void AiAssistantPanel::startOfflineWorkerOperation(OfflineDeploymentWorker* worker,
                                                   const QString& operation,
                                                   const QVector<QPair<QString, QString>>& packages,
                                                   const QString& output_dir,
                                                   const QJsonObject& args) {
    if (operation == QLatin1String("direct_download")) {
        worker->directDownload(packages, output_dir);
        return;
    }
    if (operation == QLatin1String("build_bundle")) {
        worker->buildDeploymentBundle(
            packages, output_dir, QStringLiteral("S.A.K. Utility AI offline deployment bundle"));
        return;
    }
    const QString manifest_path = args.value(QStringLiteral("manifest_path")).toString().trimmed();
    const QString packages_dir =
        QFileInfo(manifest_path).dir().filePath(QStringLiteral("packages"));
    worker->installFromBundle(manifest_path, packages_dir);
}

QJsonObject AiAssistantPanel::offlineOperationResultJson(const QString& operation,
                                                         const QString& output_dir,
                                                         const OfflineToolRunResult& run_result) {
    QJsonObject result;
    result[QStringLiteral("success")] = run_result.completed &&
                                        run_result.operation_error.isEmpty();
    result[QStringLiteral("operation")] = operation;
    result[QStringLiteral("error_message")] = run_result.operation_error;
    result[QStringLiteral("output_dir")] = QDir::toNativeSeparators(output_dir);
    result[QStringLiteral("manifest_path")] = QDir::toNativeSeparators(run_result.manifest_written);
    result[QStringLiteral("logs")] =
        QJsonArray::fromStringList(run_result.log_lines.mid(0, kPackageLogLineResultLimit));
    result[QStringLiteral("package_events")] = run_result.package_events;

    QJsonObject stats;
    stats[QStringLiteral("total")] = run_result.final_stats.total;
    stats[QStringLiteral("completed")] = run_result.final_stats.completed;
    stats[QStringLiteral("failed")] = run_result.final_stats.failed;
    stats[QStringLiteral("cancelled")] = run_result.final_stats.cancelled;
    stats[QStringLiteral("pending")] = run_result.final_stats.pending;
    stats[QStringLiteral("total_bytes")] = static_cast<double>(run_result.final_stats.total_bytes);
    result[QStringLiteral("stats")] = stats;
    return result;
}
void AiAssistantPanel::appendArtifactRow(const QString& path, const QString& kind) {
    Q_UNUSED(kind);
    if (path.isEmpty()) {
        return;
    }
    refreshArtifactList();
    updateCredentialControls();
}

void AiAssistantPanel::refreshArtifactList() {
    if (!m_artifactsButton) {
        return;
    }
    m_artifactsButton->setText(tr("Artifacts"));
    m_artifactsButton->setEnabled(false);
    if (!m_conversationStore) {
        return;
    }
    QString error;
    const QString artifact_root_path = m_conversationStore->artifactRootDirectory(&error);
    if (artifact_root_path.isEmpty()) {
        return;
    }
    const QDir artifacts_root(artifact_root_path);
    if (!artifacts_root.exists()) {
        return;
    }
    QDirIterator iter(artifacts_root.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
    int count = 0;
    while (iter.hasNext()) {
        iter.next();
        ++count;
        if (count > kArtifactCountDisplayLimit) {
            break;
        }
    }
    m_artifactsButton->setEnabled(count > 0);
    m_artifactsButton->setText(count > 0 ? tr("Artifacts (%1)").arg(count) : tr("Artifacts"));
    m_artifactsButton->setToolTip(
        count > 0 ? tr("Open the current AI session artifact directory: %1")
                        .arg(QDir::toNativeSeparators(artifacts_root.absolutePath()))
                  : tr("No artifacts have been created for the current AI session yet"));
}

void AiAssistantPanel::onArtifactsClicked() {
    if (!m_conversationStore) {
        return;
    }
    QString error;
    const QString artifact_root_path = m_conversationStore->artifactRootDirectory(&error);
    if (artifact_root_path.isEmpty()) {
        appendLocalEvent(tr("Artifact folder unavailable: %1").arg(error));
        return;
    }
    QDirIterator iter(artifact_root_path, QDir::Files, QDirIterator::Subdirectories);
    if (!iter.hasNext()) {
        refreshArtifactList();
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(artifact_root_path));
}

bool AiAssistantPanel::hasReportableResults() const {
    if (!m_conversationStore || m_conversationStore->currentSessionId().isEmpty()) {
        return false;
    }
    return hasReportableCounters() || hasReportableWorkflowPhases() ||
           hasReportableTraceActivities() || hasReportableArtifacts();
}

bool AiAssistantPanel::hasReportableCounters() const {
    return m_toolCallsThisSession > 0 || m_runState.completed_tools > 0 ||
           m_runState.completed_subagents > 0;
}

bool AiAssistantPanel::hasReportableWorkflowPhases() const {
    for (const auto& phase : m_workflowPhaseHistory) {
        if (phase.ran && !phase.skipped &&
            (phase.success || !phase.tool_result.isEmpty() ||
             !phase.metadata.value(QStringLiteral("result")).toObject().isEmpty())) {
            return true;
        }
    }
    return false;
}

bool AiAssistantPanel::hasReportableTraceActivities() const {
    if (!m_traceStore || m_traceStore->sessionDirectory().isEmpty()) {
        return false;
    }

    QString error;
    const auto activities = m_traceStore->loadActivityEvents(&error);
    for (const auto& activity : activities) {
        if (isReportableActivityState(activity.state) && activityHasReportablePayload(activity)) {
            return true;
        }
    }
    return false;
}

bool AiAssistantPanel::hasReportableArtifacts() const {
    if (!m_conversationStore) {
        return false;
    }
    QString error;
    const QString artifact_root_path = m_conversationStore->artifactRootDirectory(&error);
    if (artifact_root_path.isEmpty()) {
        return false;
    }
    QDirIterator iter(artifact_root_path, QDir::Files, QDirIterator::Subdirectories);
    while (iter.hasNext()) {
        const QString path = iter.next();
        const QString normalized_path = QFileInfo(path)
                                            .absoluteFilePath()
                                            .replace(QLatin1Char('\\'), QLatin1Char('/'))
                                            .toLower();
        if (!normalized_path.contains(QStringLiteral("/reports/"))) {
            return true;
        }
    }
    return false;
}

void AiAssistantPanel::onGenerateReportClicked() {
    if (!hasReportableResults()) {
        appendLocalEvent(
            tr("Report export skipped: no completed AI actions or workflow results yet"));
        Q_EMIT statusMessage(tr("Run an action or workflow before exporting a report"),
                             sak::kTimerStatusDefaultMs);
        updateCredentialControls();
        return;
    }
    QString error;
    QString report_dir =
        m_conversationStore ? m_conversationStore->artifactSubdir(QStringLiteral("reports"), &error)
                            : QString();
    if (report_dir.isEmpty()) {
        report_dir = QDir::homePath();
    }
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString default_path =
        QDir(report_dir).filePath(QStringLiteral("report_%1.html").arg(stamp));
    QString selected_filter = tr("HTML Report (*.html)");
    const QString requested_path = QFileDialog::getSaveFileName(
        this,
        tr("Save AI Report"),
        default_path,
        tr("HTML Report (*.html);;Markdown Report (*.md);;Plain Text Report (*.txt)"),
        &selected_filter);
    if (requested_path.isEmpty()) {
        return;
    }

    ReportFormat format = ReportFormat::Html;
    const QString suffix = QFileInfo(requested_path).suffix();
    if (selected_filter.contains(QStringLiteral("Markdown"), Qt::CaseInsensitive) ||
        suffix.compare(QStringLiteral("md"), Qt::CaseInsensitive) == 0) {
        format = ReportFormat::Markdown;
    } else if (selected_filter.contains(QStringLiteral("Plain"), Qt::CaseInsensitive) ||
               suffix.compare(QStringLiteral("txt"), Qt::CaseInsensitive) == 0) {
        format = ReportFormat::PlainText;
    }

    const QString path = generateReport(&error, requested_path, format);
    if (path.isEmpty()) {
        appendLocalEvent(tr("Report generation failed: %1").arg(error));
        Q_EMIT statusMessage(tr("Report generation failed"), sak::kTimerStatusDefaultMs);
        return;
    }
    refreshArtifactList();
    appendArtifactRow(path, QStringLiteral("report"));
    appendLocalEvent(tr("Report written: %1").arg(path));
    Q_EMIT statusMessage(tr("Report written"), sak::kTimerStatusDefaultMs);
    QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

QString AiAssistantPanel::generateReport(QString* error_message,
                                         const QString& output_path,
                                         ReportFormat format) const {
    const PanelReportFormat output_format = panelReportFormat(format == ReportFormat::Markdown,
                                                              format == ReportFormat::PlainText);
    const QString report_path = resolvePanelReportPath(
        m_conversationStore.get(), output_path, output_format, error_message);
    if (report_path.isEmpty() || !ensureReportParentDirectory(report_path, error_message)) {
        return {};
    }

    PanelReportInputs inputs;
    inputs.conversation = m_conversationStore.get();
    inputs.trace = m_traceStore.get();
    inputs.gates = m_humanGateStore.get();
    inputs.tokens = m_tokenTracker.get();
    inputs.phases = m_workflowPhaseHistory;
    inputs.citations = m_citations;
    inputs.tool_calls = m_toolCallsThisSession;

    PanelReportData report_data = collectPanelReportData(inputs, report_path, error_message);
    appendTraceAndGateData(inputs, &report_data);
    evaluatePanelReportData(&report_data);
    const QString output_text = renderPanelReport(report_data, report_path, output_format);
    return writeReportText(report_path, output_text, error_message) ? report_path : QString();
}
void AiAssistantPanel::appendCitationsToList(const QVector<ai::OpenAIUrlCitation>& citations) {
    if (citations.isEmpty()) {
        return;
    }
    for (const auto& citation : citations) {
        if (citation.url.trimmed().isEmpty()) {
            continue;
        }
        const bool already_known = std::any_of(m_citations.cbegin(),
                                               m_citations.cend(),
                                               [&](const ai::OpenAIUrlCitation& existing) {
                                                   return existing.url == citation.url;
                                               });
        if (!already_known) {
            m_citations.append(citation);
        }
    }
}


void AiAssistantPanel::appendToolOutputAndContinue(ai::OpenAIFunctionOutput output) {
    if (!m_toolTurn.active()) {
        return;
    }
    const auto advance = m_toolTurn.appendOutput(std::move(output));
    if (!advance.ok) {
        const QString message = tr("Pending tool turn state error: %1").arg(advance.error_message);
        appendLocalEvent(message);
        appendTranscriptMessage(QStringLiteral("System"), message, true);
        traceAiEvent(QStringLiteral("tool_queue"),
                     QStringLiteral("local_tools"),
                     QStringLiteral("failed"),
                     QJsonObject{{QStringLiteral("error_message"), message}});
        resetPendingToolTurn();
        m_runState.status = ai::AiRunStatus::Failed;
        m_runState.active_tools = 0;
        m_runState.message = message;
        saveRunStateSnapshot();
        emitStatusDetails();
        setUiBusy(false);
        updateCredentialControls();
        Q_EMIT statusMessage(message, sak::kTimerStatusDefaultMs);
        return;
    }
    if (m_runState.active_tools > 0) {
        --m_runState.active_tools;
    }
    ++m_runState.completed_tools;
    emitStatusDetails();
    saveRunStateSnapshot();
    updateCredentialControls();
    m_pendingCallToken = {};
    if (!advance.finished) {
        dispatchNextToolCall();
        return;
    }
    finishToolTurnAndContinue();
}

void AiAssistantPanel::resetPendingToolTurn() {
    m_toolTurn.reset();
    m_currentCommandId.clear();
    m_pendingTurnToken = {};
    m_pendingCallToken = {};
}

QString AiAssistantPanel::currentPendingToolCallId() const {
    return m_toolTurn.currentCallId();
}

QJsonObject AiAssistantPanel::pendingToolTurnState() const {
    return m_toolTurn.toJson(!m_currentRunId.isEmpty() ? m_currentRunId : m_runState.run_id);
}

bool AiAssistantPanel::restorePendingToolTurnState(const QJsonObject& state) {
    QString error;
    if (!m_toolTurn.restore(state, &error)) {
        appendLocalEvent(tr("Pending tool turn restore failed: %1")
                             .arg(error.isEmpty() ? tr("unknown error") : error));
        return false;
    }

    const QString run_id = state.value(QStringLiteral("run_id")).toString().trimmed();
    restorePendingRunIdentity(run_id, m_toolTurn.responseId());

    m_runState.status = ai::AiRunStatus::Running;
    m_runState.active_tools = std::max(1, m_toolTurn.remainingCallCount());
    m_runState.message = tr("Resumed pending tool call");
    saveRunStateSnapshot();
    emitStatusDetails();
    return true;
}

void AiAssistantPanel::restorePendingRunIdentity(const QString& run_id,
                                                 const QString& response_id) {
    if (!run_id.isEmpty()) {
        m_currentRunId = run_id;
        m_runState.run_id = run_id;
        if (!m_runToken.isValid()) {
            m_runToken = ai::CancellationToken::createRoot(run_id);
        }
    }
    if (!m_runToken.isValid()) {
        m_pendingTurnToken = {};
        m_pendingCallToken = {};
        return;
    }
    m_pendingTurnToken =
        m_runToken.createChild(QStringLiteral("turn_%1").arg(response_id.left(kTraceTokenIdChars)));
    m_pendingCallToken = m_pendingTurnToken.createChild(
        QStringLiteral("call_%1").arg(currentPendingToolCallId().left(kTraceTokenIdChars)));
}

bool AiAssistantPanel::completeResumedToolGateWithOutput(const ai::AiHumanGate& gate,
                                                         const QJsonObject& output_json) {
    const QJsonObject turn_state = gate.metadata.value(QStringLiteral("tool_turn")).toObject();
    if (!restorePendingToolTurnState(turn_state)) {
        return false;
    }
    ai::OpenAIFunctionOutput output;
    output.call_id = currentPendingToolCallId();
    output.output = QString::fromUtf8(QJsonDocument(output_json).toJson(QJsonDocument::Compact));
    appendToolOutputAndContinue(std::move(output));
    return true;
}

ai::AiCommandResult AiAssistantPanel::runElevatedPowerShell(const ai::AiCommandRequest& request) {
    ai::AiCommandResult result;
    result.elevated = true;

    if (!m_elevationBroker) {
        result.error_message = tr("Elevated helper is not available");
        return result;
    }

    QJsonObject payload;
    payload[QStringLiteral("command")] = request.command;
    payload[QStringLiteral("timeout_seconds")] = request.timeout_seconds;
    payload[QStringLiteral("max_output_bytes")] = request.max_output_bytes;

    appendLocalEvent(tr("Requesting elevated PowerShell helper"));
    const auto elevated = m_elevationBroker->executeTask(QStringLiteral("RunPowerShell"),
                                                         tr("AI Assistant PowerShell command"),
                                                         payload);
    if (!elevated) {
        result.error_message = fromStringView(to_string(elevated.error()));
        return result;
    }

    const QJsonObject response_data = elevated->data;
    result.started = response_data.value(QStringLiteral("started")).toBool(false);
    result.cancelled = response_data.value(QStringLiteral("cancelled")).toBool(false);
    result.timed_out = response_data.value(QStringLiteral("timed_out")).toBool(false);
    result.elevated = response_data.value(QStringLiteral("elevated")).toBool(true);
    result.exit_code = response_data.value(QStringLiteral("exit_code")).toInt(-1);
    result.exit_status = response_data.value(QStringLiteral("exit_status")).toInt(0);
    result.duration_ms =
        static_cast<qint64>(response_data.value(QStringLiteral("duration_ms")).toDouble(0.0));
    result.stdout_text = response_data.value(QStringLiteral("stdout")).toString();
    result.stderr_text = response_data.value(QStringLiteral("stderr")).toString();
    result.error_message = response_data.value(QStringLiteral("error_message")).toString();

    if (!elevated->success && result.error_message.isEmpty()) {
        result.error_message = elevated->error_message.isEmpty()
                                   ? tr("Elevated PowerShell task failed")
                                   : elevated->error_message;
    }
    return result;
}

void AiAssistantPanel::continueAfterToolCalls(const ai::OpenAIResponseResult& result,
                                              QVector<ai::OpenAIFunctionOutput> outputs) {
    ai::OpenAIResponseRequest request;
    request.api_key = apiKey();
    request.model = m_modelCombo->currentText().trimmed();
    request.instructions = buildInstructions();
    request.function_outputs = std::move(outputs);
    request.reasoning_effort = m_reasoningEffortCombo->currentText().trimmed().toLower();
    request.previous_response_id = result.id;
    if (m_conversationStore) {
        request.safety_identifier =
            safetyIdentifierFromSeed(m_conversationStore->currentSessionId());
    }
    request.enable_web_search = true;
    request.enable_local_tools = currentAccessMode() != AccessMode::ChatAndResearch;
    QJsonObject metadata;
    metadata[QStringLiteral("outputs")] = request.function_outputs.size();
    traceAiEvent(QStringLiteral("tool_queue"),
                 QStringLiteral("local_tools"),
                 QStringLiteral("completed"),
                 metadata);
    appendLocalEvent(tr("Returning tool output to OpenAI"));
    m_client->createResponse(request);
}

void AiAssistantPanel::finishToolTurnAndContinue() {
    if (!m_toolTurn.active()) {
        return;
    }
    ai::OpenAIResponseResult continuation;
    continuation.id = m_toolTurn.responseId();
    QVector<ai::OpenAIFunctionOutput> outputs = m_toolTurn.takeOutputs();
    resetPendingToolTurn();
    QTimer::singleShot(0, this, [this, continuation, outputs = std::move(outputs)]() mutable {
        continueAfterToolCalls(continuation, std::move(outputs));
    });
}

AiAssistantPanel::AccessMode AiAssistantPanel::currentAccessMode() const {
    if (!m_accessModeCombo) {
        return AccessMode::AssistedFullAccess;
    }

    switch (m_accessModeCombo->currentIndex()) {
    case 0:
        return AccessMode::ChatAndResearch;
    case kAccessModeUnattendedComboIndex:
        return AccessMode::UnattendedFullAccess;
    case 1:
    default:
        return AccessMode::AssistedFullAccess;
    }
}

QString AiAssistantPanel::currentAccessModeLabel() const {
    if (!m_accessModeCombo) {
        return tr("Assisted Full Access");
    }
    return m_accessModeCombo->currentText();
}

QString AiAssistantPanel::contextItemKindLabel(ContextItem::Type type) {
    switch (type) {
    case ContextItem::Type::Workflow:
        return QStringLiteral("Workflow");
    case ContextItem::Type::Instruction:
        return QStringLiteral("Instruction");
    case ContextItem::Type::Skill:
        return QStringLiteral("Skill");
    case ContextItem::Type::TextFile:
        return QStringLiteral("Text");
    case ContextItem::Type::ImageFile:
        return QStringLiteral("Image");
    case ContextItem::Type::DocumentFile:
        return QStringLiteral("Document");
    }
    return QStringLiteral("Context");
}

QString AiAssistantPanel::contextItemLabel(const ContextItem& item) {
    if (item.type == ContextItem::Type::Workflow || item.type == ContextItem::Type::Instruction ||
        item.type == ContextItem::Type::Skill) {
        const QString name = item.display_name.isEmpty() ? contextItemKindLabel(item.type)
                                                         : item.display_name;
        return QStringLiteral("%1 (%2)").arg(name, compactByteCount(item.original_size));
    }
    const QFileInfo info(item.path);
    const QString name = item.display_name.isEmpty() ? info.fileName() : item.display_name;
    return QStringLiteral("%1 (%2)").arg(name, compactByteCount(item.original_size));
}

int AiAssistantPanel::contextChipMaxWidth() const {
    const QWidget* viewport = m_contextList->viewport();
    const int source_width = viewport ? viewport->width() : m_contextList->width();
    const int available_width = std::max(kContextChipMinAvailableWidth,
                                         source_width - kContextChipViewportReserve);
    int target_chip_width = std::clamp(available_width / kContextChipColumns,
                                       kContextChipTargetMinWidth,
                                       kContextChipTargetMaxWidth);
    target_chip_width = std::min(target_chip_width, available_width);
    return std::max(kContextChipMinWidth, target_chip_width);
}

int AiAssistantPanel::contextChipWidth(const QString& chip_text, int max_chip_width) const {
    const int measured_width = QFontMetrics(m_contextList->font()).horizontalAdvance(chip_text) +
                               kContextChipMetricPadding;
    return std::clamp(measured_width, kContextChipMinWidth, max_chip_width);
}

void AiAssistantPanel::addContextListItem(int index, int max_chip_width) {
    const auto& item = m_contextItems.at(index);
    const QString kind_label = contextItemKindLabel(item.type);
    const QString chip_text = QStringLiteral("%1: %2").arg(kind_label, contextItemLabel(item));
    const int chip_width = contextChipWidth(chip_text, max_chip_width);

    auto* row = new QListWidgetItem(m_contextList);
    row->setToolTip(item.path.isEmpty() ? item.text.left(kContextItemTooltipMaxChars) : item.path);
    row->setSizeHint(QSize(chip_width, kContextChipRowHeight));

    const ContextChipPalette palette = contextChipPalette(item.type);
    auto* chip = new QFrame(m_contextList);
    chip->setObjectName(QStringLiteral("aiContextChip"));
    chip->setFixedSize(chip_width, kContextChipHeight);
    chip->setStyleSheet(sak::ui::aiContextChipStyle(palette.background, palette.border));

    auto* chip_layout = new QHBoxLayout(chip);
    chip_layout->setContentsMargins(sak::ui::kMarginSmall,
                                    sak::ui::kCssBorderWidthDefaultPx,
                                    sak::ui::kSpacingTight,
                                    sak::ui::kCssBorderWidthDefaultPx);
    chip_layout->setSpacing(sak::ui::kSpacingTight);

    const int text_width = std::max(kContextChipTextMinWidth, chip_width - kContextChipTextReserve);
    const QString display_text =
        QFontMetrics(m_contextList->font()).elidedText(chip_text, Qt::ElideMiddle, text_width);
    auto* text = new QLabel(display_text, chip);
    text->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(palette.text_color, sak::ui::kFontSizeNote));
    text->setToolTip(chip_text + QStringLiteral("\n") + row->toolTip());
    text->setMinimumWidth(sak::ui::kUiWidthNoMinimum);
    text->setMaximumWidth(text_width);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    chip_layout->addWidget(text, 1);

    auto* remove = new QPushButton(chip);
    remove->setIcon(QIcon(QStringLiteral(":/icons/icons/icons8-close-window.svg")));
    remove->setIconSize(QSize(sak::ui::kUiIconTiny, sak::ui::kUiIconTiny));
    remove->setFixedSize(sak::ui::kUiButtonSizeMicro, sak::ui::kUiButtonSizeMicro);
    remove->setToolTip(tr("Remove %1").arg(kind_label));
    remove->setStyleSheet(sak::ui::aiContextChipRemoveStyle());
    connect(remove, &QPushButton::clicked, this, [this, index]() { removeContextItem(index); });
    chip_layout->addWidget(remove);

    m_contextList->setItemWidget(row, chip);
}

void AiAssistantPanel::refreshContextList() {
    if (!m_contextList) {
        return;
    }

    m_contextList->clear();
    m_contextList->setVisible(!m_contextItems.isEmpty());
    const int max_chip_width = contextChipMaxWidth();
    for (int i = 0; i < m_contextItems.size(); ++i) {
        addContextListItem(i, max_chip_width);
    }
    if (m_clearContextButton) {
        m_clearContextButton->setEnabled(!m_contextItems.isEmpty());
    }
    syncSessionRoleForWorkflow(attachedWorkflow());
    scheduleContextTokenRefresh();
    updateRunTelemetryLabels();
}

AiAssistantPanel::ContextChipPalette AiAssistantPanel::contextChipPalette(
    ContextItem::Type type) const {
    ContextChipPalette palette{QString::fromLatin1(sak::ui::kColorBgWhite),
                               QString::fromLatin1(sak::ui::kColorBorderDefault),
                               QString::fromLatin1(sak::ui::kColorTextBody)};
    switch (type) {
    case ContextItem::Type::Workflow:
        palette.border = QString::fromLatin1(sak::ui::kColorPrimary);
        palette.text_color = QString::fromLatin1(sak::ui::kColorTextHeading);
        break;
    case ContextItem::Type::Instruction:
        palette.background = QString::fromLatin1(sak::ui::kColorBgInfoPanel);
        palette.border = QString::fromLatin1(sak::ui::kColorPrimaryDark);
        palette.text_color = QString::fromLatin1(sak::ui::kColorTextHeading);
        break;
    case ContextItem::Type::Skill:
    case ContextItem::Type::DocumentFile:
        palette.background = QString::fromLatin1(sak::ui::kColorBgSurface);
        palette.border = QString::fromLatin1(sak::ui::kColorSuccess);
        palette.text_color = QString::fromLatin1(sak::ui::kColorTextHeading);
        break;
    case ContextItem::Type::ImageFile:
        palette.background = QString::fromLatin1(sak::ui::kColorBgWarningPanel);
        palette.border = QString::fromLatin1(sak::ui::kColorWarning);
        palette.text_color = QString::fromLatin1(sak::ui::kColorTextHeading);
        break;
    case ContextItem::Type::TextFile:
        break;
    }
    return palette;
}

void AiAssistantPanel::reloadSessionPicker() {
    if (!m_sessionCombo || !m_conversationStore) {
        return;
    }

    const QString current_id = m_conversationStore->currentSessionId();
    m_loadingSessionPicker = true;
    m_sessionCombo->clear();
    const auto sessions = m_conversationStore->listPromptedSessions();
    int selected_index = -1;
    if (current_id.isEmpty()) {
        const QString draft_title =
            m_pendingSessionTitle.trimmed().isEmpty() ? tr("AI Session") : m_pendingSessionTitle;
        m_sessionCombo->addItem(tr("%1  (new)").arg(draft_title), QString());
        selected_index = 0;
    }
    for (const auto& session : sessions) {
        const QString label = QStringLiteral("%1  %2").arg(
            session.title,
            session.updated_at.toLocalTime().toString(QStringLiteral("MM/dd HH:mm")));
        m_sessionCombo->addItem(label, session.id);
        if (session.id == current_id) {
            selected_index = m_sessionCombo->count() - 1;
        }
    }
    if (selected_index >= 0) {
        m_sessionCombo->setCurrentIndex(selected_index);
    }
    m_loadingSessionPicker = false;
}

void AiAssistantPanel::startNewPersistentSession(const QString& title) {
    if (!m_conversationStore) {
        return;
    }

    QString error;
    if (!m_conversationStore->startSession(title, &error)) {
        appendLocalEvent(tr("AI session store failed: %1").arg(error));
        return;
    }
    m_toolCallsThisSession = 0;
    m_citations.clear();
    m_nextCommandSequence = 1;
    m_restorePointOfferedThisSession = false;
    m_runState = {};
    m_taskStatus = tr("Idle");
    refreshTraceStoreForSession();
    saveRunStateSnapshot();
    refreshArtifactList();
    updateCredentialControls();
    updateTokenLabels();
    reloadSessionPicker();
    traceAiEvent(QStringLiteral("session"), QStringLiteral("session"), QStringLiteral("created"));
    appendLocalEvent(tr("AI session created: %1").arg(m_conversationStore->currentSessionId()));
}

bool AiAssistantPanel::ensurePersistentSession(const QString& title) {
    if (!m_conversationStore) {
        return false;
    }
    if (!m_conversationStore->currentSessionId().isEmpty()) {
        return true;
    }
    const QString session_title = title.trimmed().isEmpty() ? tr("AI Session") : title.trimmed();
    startNewPersistentSession(session_title);
    return !m_conversationStore->currentSessionId().isEmpty();
}

QString AiAssistantPanel::workflowTitleForChatRename(const QString& workflow_id) const {
    if (workflow_id.trimmed().isEmpty() || !m_workflowStore) {
        return {};
    }
    const auto* workflow = m_workflowStore->workflowById(workflow_id);
    return workflow ? workflow->title : QString{};
}

void AiAssistantPanel::autoRenameDefaultChatFromFirstPrompt(const QString& message,
                                                            const QString& workflow_id) {
    if (!m_conversationStore || m_conversationStore->currentSessionId().isEmpty()) {
        return;
    }
    const QString current_title = m_conversationStore->currentSessionInfo().title;
    if (!ai::isDefaultChatTitle(current_title)) {
        return;
    }

    const QString generated_title =
        ai::chatTitleFromFirstPrompt(message, workflowTitleForChatRename(workflow_id));
    if (generated_title.isEmpty() ||
        generated_title.compare(current_title, Qt::CaseInsensitive) == 0) {
        return;
    }

    QString error;
    if (!m_conversationStore->renameCurrentSession(generated_title, &error)) {
        appendLocalEvent(tr("AI chat auto-rename failed: %1").arg(error));
        return;
    }
    m_pendingSessionTitle = generated_title;
    appendLocalEvent(tr("AI chat auto-renamed: %1").arg(generated_title));
}

void AiAssistantPanel::loadSessionTranscript(const QString& session_id) {
    if (!m_conversationStore || !m_transcriptView) {
        return;
    }

    QString error;
    const QStringList lines = m_conversationStore->loadTranscriptLines(session_id, &error);
    m_citations.clear();
    m_transcriptView->clearMessages(false);
    for (const auto& line : lines) {
        appendLoadedTranscriptLine(line);
    }
    QString response_error;
    m_previousResponseId = m_conversationStore->latestAssistantResponseId(session_id,
                                                                          &response_error);
    restoreSessionRoleForSession(session_id);
    renderTranscriptMessages(true);
    if (!error.isEmpty()) {
        appendLocalEvent(tr("Could not load AI transcript: %1").arg(error));
    }
    if (!response_error.isEmpty()) {
        appendLocalEvent(tr("Could not restore AI response chain: %1").arg(response_error));
    }
    refreshArtifactList();
    updateCredentialControls();
}

void AiAssistantPanel::removeContextItem(int index) {
    if (index < 0 || index >= m_contextItems.size()) {
        return;
    }
    const QString label = contextItemLabel(m_contextItems.at(index));
    m_contextItems.removeAt(index);
    refreshContextList();
    appendLocalEvent(tr("Removed AI context: %1").arg(label));
    Q_EMIT statusMessage(tr("AI context removed"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::addInstructionFile(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        appendLocalEvent(tr("Instruction file not found: %1").arg(path));
        return;
    }
    if (info.suffix().compare(QStringLiteral("md"), Qt::CaseInsensitive) != 0) {
        appendLocalEvent(tr("Instruction file skipped; markdown required: %1").arg(path));
        return;
    }
    if (info.size() > kContextMaxFileBytes) {
        appendLocalEvent(tr("Instruction file skipped; too large: %1").arg(path));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        appendLocalEvent(tr("Could not read instruction file: %1").arg(path));
        return;
    }
    const QByteArray bytes = file.readAll();
    QString text = QString::fromUtf8(bytes.left(kContextTextPreviewBytes)).trimmed();
    if (bytes.size() > kContextTextPreviewBytes) {
        text += QStringLiteral("\n...[instruction truncated to %1 bytes]")
                    .arg(kContextTextPreviewBytes);
    }
    if (text.isEmpty()) {
        appendLocalEvent(tr("Instruction file empty: %1").arg(path));
        return;
    }

    ContextItem item;
    item.type = ContextItem::Type::Instruction;
    item.display_name = info.fileName();
    item.path = info.absoluteFilePath();
    item.mime_type = QStringLiteral("text/markdown");
    item.text = text;
    item.original_size = info.size();
    m_contextItems.append(item);
    refreshContextList();
    persistContextItem(item);
    appendLocalEvent(tr("Added instruction file: %1").arg(info.fileName()));
}

void AiAssistantPanel::addContextFile(const QString& path) {
    QFileInfo info(path);
    if (!info.exists() || !info.isFile()) {
        appendLocalEvent(tr("Context file not found: %1").arg(path));
        return;
    }
    if (info.size() > kContextMaxFileBytes) {
        appendLocalEvent(tr("Context file skipped; too large: %1").arg(path));
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        appendLocalEvent(tr("Could not read context file: %1").arg(path));
        return;
    }
    QByteArray bytes = file.readAll();

    QMimeDatabase mime_database;
    const QString mime_type = mime_database.mimeTypeForFile(info).name();

    ContextItem item = contextItemFromFile(info, std::move(bytes), mime_type);
    m_contextItems.append(item);
    refreshContextList();
    persistContextItem(item);
    appendLocalEvent(tr("Added context file: %1").arg(info.fileName()));
}

AiAssistantPanel::ContextItem AiAssistantPanel::contextItemFromFile(
    const QFileInfo& info, QByteArray bytes, const QString& mime_type) const {
    ContextItem item;
    item.display_name = info.fileName();
    item.path = info.absoluteFilePath();
    item.mime_type = mime_type;
    item.original_size = info.size();

    if (isImageMime(mime_type)) {
        item.type = ContextItem::Type::ImageFile;
        item.bytes = std::move(bytes);
    } else if (isTextLikeFile(info, mime_type)) {
        item.type = ContextItem::Type::TextFile;
        item.text = QString::fromUtf8(bytes.left(kContextTextPreviewBytes));
        if (bytes.size() > kContextTextPreviewBytes) {
            item.text += QStringLiteral("\n...[context truncated to %1 bytes]")
                             .arg(kContextTextPreviewBytes);
        }
    } else if (isPdfFile(info, mime_type)) {
        item.type = ContextItem::Type::DocumentFile;
        item.bytes = std::move(bytes);
    } else {
        item.type = ContextItem::Type::TextFile;
        item.text = tr("The user attached a local file that is not directly embedded. Path: "
                       "%1\nMIME type: %2\nSize: %3")
                        .arg(item.path, mime_type, compactByteCount(info.size()));
    }
    return item;
}

void AiAssistantPanel::persistContextItem(const ContextItem& item) {
    if (!m_conversationStore || m_conversationStore->currentSessionId().isEmpty()) {
        return;
    }
    QString error;
    (void)m_conversationStore->appendContext(contextItemKindLabel(item.type),
                                             contextItemLabel(item),
                                             item.path,
                                             item.original_size,
                                             &error);
    if (!error.isEmpty()) {
        appendLocalEvent(tr("Context log failed: %1").arg(error));
    }
}

void AiAssistantPanel::updateAccessStatus() {
    if (!m_accessStatusLabel) {
        return;
    }

    switch (currentAccessMode()) {
    case AccessMode::ChatAndResearch:
        m_accessStatusLabel->setText(tr("Local execution disabled"));
        m_accessStatusLabel->setStyleSheet(statusLabelStyle(sak::ui::kStatusColorIdle));
        break;
    case AccessMode::AssistedFullAccess:
        m_accessStatusLabel->setText(tr("Approval required for commands"));
        m_accessStatusLabel->setStyleSheet(statusLabelStyle(sak::ui::kStatusColorWarning));
        break;
    case AccessMode::UnattendedFullAccess:
        m_accessStatusLabel->setText(tr("Unattended execution enabled"));
        m_accessStatusLabel->setStyleSheet(statusLabelStyle(sak::ui::kStatusColorError));
        break;
    }
    emitStatusDetails();
}

void AiAssistantPanel::appendLocalEvent(const QString& message) {
    Q_EMIT logOutput(ai::CredentialStore::redactSecrets(message));
}

void AiAssistantPanel::appendSessionMemory(const QString& kind,
                                           const QString& title,
                                           const QString& text) {
    if (!m_conversationStore || m_conversationStore->currentSessionId().isEmpty() ||
        text.trimmed().isEmpty()) {
        return;
    }
    QString error;
    if (!m_conversationStore->appendMemoryEntry(kind, title, text, &error) && !error.isEmpty()) {
        appendLocalEvent(tr("Memory write failed: %1").arg(error));
    }
}

void AiAssistantPanel::refreshTraceStoreForSession() {
    if (!m_conversationStore) {
        return;
    }
    const QString session_dir = m_conversationStore->currentSessionInfo().path;
    if (m_traceStore) {
        m_traceStore->setSessionDirectory(session_dir);
    }
    if (m_runStateStore) {
        m_runStateStore->setSessionDirectory(session_dir);
    }
    if (m_humanGateStore) {
        m_humanGateStore->setSessionDirectory(session_dir);
    }
}

void AiAssistantPanel::saveRunStateSnapshot() {
    if (!m_runStateStore) {
        return;
    }
    if (m_runStateStore->sessionDirectory().isEmpty()) {
        refreshTraceStoreForSession();
    }
    if (m_runStateStore->sessionDirectory().isEmpty()) {
        return;
    }
    QString error;
    if (!m_runStateStore->saveSnapshot(m_runState, &error) && !error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI run-state write failed: %1").arg(error)));
    }
}

void AiAssistantPanel::loadRunStateSnapshotForSession() {
    if (!m_runStateStore) {
        return;
    }
    if (m_runStateStore->sessionDirectory().isEmpty()) {
        refreshTraceStoreForSession();
    }
    QString error;
    ai::AiRunState loaded = m_runStateStore->loadSnapshot(&error);
    if (!error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI run-state read failed: %1").arg(error)));
        return;
    }
    if (m_humanGateStore && m_humanGateStore->sessionDirectory().isEmpty()) {
        refreshTraceStoreForSession();
    }
    mergePendingHumanGate(&loaded);
    markStaleRunCancelled(&loaded);
    applyLoadedRunState(loaded);
}

void AiAssistantPanel::mergePendingHumanGate(ai::AiRunState* loaded) {
    if (!loaded || loaded->has_pending_human_gate || !m_humanGateStore) {
        return;
    }
    QString error;
    const auto pending = m_humanGateStore->latestPendingGate(&error);
    if (!error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI human-gate read failed: %1").arg(error)));
        return;
    }
    if (pending.gate_id.isEmpty()) {
        return;
    }
    loaded->has_pending_human_gate = true;
    loaded->pending_human_gate = pending;
    loaded->run_id = pending.run_id;
    loaded->workflow_id = pending.workflow_id;
    loaded->phase_id = pending.phase_id;
    loaded->status = ai::AiRunStatus::WaitingForHuman;
    loaded->message = pending.question;
}

void AiAssistantPanel::markStaleRunCancelled(ai::AiRunState* loaded) {
    if (!loaded || loaded->has_pending_human_gate || ai::isTerminalRunStatus(loaded->status) ||
        loaded->status == ai::AiRunStatus::Idle) {
        return;
    }
    loaded->status = ai::AiRunStatus::Cancelled;
    loaded->active_subagents = 0;
    loaded->active_tools = 0;
    loaded->message = tr("Previous run did not finish before the app closed.");
    appendLocalEvent(tr("Marked stale AI run as cancelled: %1").arg(loaded->run_id));
    QString stale_error;
    (void)m_runStateStore->saveSnapshot(*loaded, &stale_error);
    if (!stale_error.isEmpty()) {
        Q_EMIT logOutput(ai::CredentialStore::redactSecrets(
            tr("AI stale run-state write failed: %1").arg(stale_error)));
    }
}

void AiAssistantPanel::applyLoadedRunState(const ai::AiRunState& loaded) {
    m_runState = loaded;
    m_pendingWorkflowRunId.clear();
    if (m_runState.has_pending_human_gate) {
        m_taskStatus = tr("Waiting for human input");
        m_pendingWorkflowRunId = m_runState.pending_human_gate.kind ==
                                         QLatin1String("workflow_input")
                                     ? m_runState.pending_human_gate.run_id
                                     : QString();
        appendLocalEvent(
            tr("AI session has pending human gate: %1").arg(m_runState.pending_human_gate.name));
    } else if (m_runState.status == ai::AiRunStatus::Idle) {
        m_taskStatus = tr("Idle");
    } else {
        m_taskStatus = ai::runStatusToString(m_runState.status);
    }
    emitStatusDetails();
    updateCredentialControls();
}

QString AiAssistantPanel::beginHumanGate(const QString& kind,
                                         const QString& name,
                                         const QString& question,
                                         const QJsonObject& metadata) {
    ai::AiHumanGate gate;
    gate.gate_id = QStringLiteral("gate_%1").arg(
        QUuid::createUuid().toString(QUuid::WithoutBraces).left(kTraceTokenIdChars));
    gate.run_id = !m_runState.run_id.isEmpty() ? m_runState.run_id : m_currentRunId;
    if (gate.run_id.isEmpty() && m_conversationStore) {
        gate.run_id = QStringLiteral("gate_wait_%1").arg(m_conversationStore->currentSessionId());
    }
    gate.workflow_id = m_runState.workflow_id;
    gate.phase_id = m_runState.phase_id;
    gate.kind = kind;
    gate.name = name;
    gate.status = ai::humanGateWaitingStatus();
    gate.question = question;
    gate.created_utc = QDateTime::currentDateTimeUtc();
    gate.metadata = metadata;

    m_runState.run_id = gate.run_id;
    m_runState.status = ai::AiRunStatus::WaitingForHuman;
    m_runState.message = metadata.value(QStringLiteral("summary")).toString(question);
    m_runState.has_pending_human_gate = true;
    m_runState.pending_human_gate = gate;

    if (m_humanGateStore) {
        if (m_humanGateStore->sessionDirectory().isEmpty()) {
            refreshTraceStoreForSession();
        }
        QString error;
        if (!m_humanGateStore->appendGate(gate, &error) && !error.isEmpty()) {
            Q_EMIT logOutput(ai::CredentialStore::redactSecrets(
                tr("AI human-gate write failed: %1").arg(error)));
        }
    }
    saveRunStateSnapshot();
    emitStatusDetails();
    updateCredentialControls();
    return gate.gate_id;
}

void AiAssistantPanel::resolveHumanGate(const QString& gate_id,
                                        const QString& status,
                                        const QString& decision,
                                        const QString& response_summary,
                                        const QJsonObject& metadata) {
    const ai::AiHumanGate gate =
        resolvedGateFromState(gate_id, status, decision, response_summary, metadata);
    appendHumanGateRecord(gate);
    clearPendingGateIfResolved(gate);
    saveRunStateSnapshot();
    emitStatusDetails();
    updateCredentialControls();
}

ai::AiHumanGate AiAssistantPanel::resolvedGateFromState(const QString& gate_id,
                                                        const QString& status,
                                                        const QString& decision,
                                                        const QString& response_summary,
                                                        const QJsonObject& metadata) const {
    ai::AiHumanGate gate = (m_runState.has_pending_human_gate &&
                            m_runState.pending_human_gate.gate_id == gate_id)
                               ? m_runState.pending_human_gate
                               : ai::AiHumanGate{};
    gate.gate_id =
        gate_id.isEmpty()
            ? QStringLiteral("gate_%1").arg(
                  QUuid::createUuid().toString(QUuid::WithoutBraces).left(kTraceTokenIdChars))
            : gate_id;
    if (gate.run_id.isEmpty()) {
        gate.run_id = !m_runState.run_id.isEmpty() ? m_runState.run_id : m_currentRunId;
    }
    if (gate.workflow_id.isEmpty()) {
        gate.workflow_id = m_runState.workflow_id;
    }
    if (gate.phase_id.isEmpty()) {
        gate.phase_id = m_runState.phase_id;
    }
    gate.status = status;
    gate.decision = decision;
    gate.response_summary = response_summary;
    gate.resolved_utc = QDateTime::currentDateTimeUtc();
    gate.metadata = metadata;
    if (!gate.created_utc.isValid()) {
        gate.created_utc = gate.resolved_utc;
    }
    return gate;
}

void AiAssistantPanel::appendHumanGateRecord(const ai::AiHumanGate& gate) {
    if (m_humanGateStore) {
        if (m_humanGateStore->sessionDirectory().isEmpty()) {
            refreshTraceStoreForSession();
        }
        QString error;
        if (!m_humanGateStore->appendGate(gate, &error) && !error.isEmpty()) {
            Q_EMIT logOutput(ai::CredentialStore::redactSecrets(
                tr("AI human-gate write failed: %1").arg(error)));
        }
    }
}

void AiAssistantPanel::clearPendingGateIfResolved(const ai::AiHumanGate& gate) {
    if (m_runState.has_pending_human_gate &&
        m_runState.pending_human_gate.gate_id == gate.gate_id) {
        m_runState.has_pending_human_gate = false;
        m_runState.pending_human_gate = {};
    }
}

ai::AiToolPolicy AiAssistantPanel::currentAccessToolPolicy() const {
    switch (currentAccessMode()) {
    case AccessMode::ChatAndResearch:
        return ai::AiToolPolicy::NoLocalExecution;
    case AccessMode::AssistedFullAccess:
        return ai::AiToolPolicy::MutatingRequiresLease;
    case AccessMode::UnattendedFullAccess:
    default:
        return ai::AiToolPolicy::MutatingRequiresLease;
    }
}

void AiAssistantPanel::registerToolDispatcherHandlers() {
    if (!ensureToolDispatcherReady()) {
        return;
    }
    m_toolDispatcher->clearHandlers();
    m_toolDispatcher->setLeaseManager(m_leaseManager.get());
    m_toolDispatcher->setHealthLedger(m_toolHealthLedger.get());
    registerToolAvailabilityCheckers();
    registerToolHandlers();
}

bool AiAssistantPanel::ensureToolDispatcherReady() {
    if (!m_toolDispatcher) {
        QJsonObject metadata;
        metadata[QStringLiteral("dispatcher_missing")] = true;
        metadata[QStringLiteral("error_message")] =
            QStringLiteral("AI tool dispatcher is not initialized");
        traceAiEvent(QStringLiteral("tool_dispatcher"),
                     QStringLiteral("register_handlers"),
                     QStringLiteral("failed"),
                     metadata);
        appendLocalEvent(tr("AI tool dispatcher initialization failed: dispatcher missing"));
        return false;
    }
    return true;
}

void AiAssistantPanel::registerToolAvailabilityCheckers() {
    registerDownloadFileAvailabilityChecker();
    registerPackageManagerAvailabilityChecker();
    registerOfflineDownloaderAvailabilityChecker();
    registerProviderGatewayAvailabilityChecker();
    registerSessionSearchAvailabilityChecker();
}

void AiAssistantPanel::registerDownloadFileAvailabilityChecker() {
    m_toolDispatcher->registerAvailabilityChecker(
        QStringLiteral("download_file"),
        [](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            const QString url = args.value(QStringLiteral("url")).toString().trimmed();
            if (!url.startsWith(QStringLiteral("https://"), Qt::CaseInsensitive)) {
                QJsonObject result =
                    toolError(QStringLiteral("download_file requires an https URL"));
                result[QStringLiteral("failure_class")] = QStringLiteral("invalid_request");
                return result;
            }
            return QJsonObject{{QStringLiteral("success"), true}};
        });
}

void AiAssistantPanel::registerPackageManagerAvailabilityChecker() {
    m_toolDispatcher->registerAvailabilityChecker(
        QStringLiteral("sak_package_manager"),
        [this](const QJsonObject&, const ai::AiToolPolicyDecision&) {
            if (!m_chocoManager || !m_chocoManager->isInitialized()) {
                QJsonObject result = toolError(
                    QStringLiteral("SAK bundled Chocolatey package manager is not available"));
                result[QStringLiteral("failure_class")] =
                    QStringLiteral("package_manager_unavailable");
                return result;
            }
            return QJsonObject{{QStringLiteral("success"), true}};
        });
}

void AiAssistantPanel::registerOfflineDownloaderAvailabilityChecker() {
    m_toolDispatcher->registerAvailabilityChecker(
        QStringLiteral("sak_offline_downloader"),
        [this](const QJsonObject&, const ai::AiToolPolicyDecision&) {
            if (!m_offlineWorker || !m_packageListManager) {
                QJsonObject result =
                    toolError(QStringLiteral("SAK offline downloader is not available"));
                result[QStringLiteral("failure_class")] =
                    QStringLiteral("offline_downloader_unavailable");
                return result;
            }
            if (m_offlineWorker->isRunning()) {
                QJsonObject result =
                    toolError(QStringLiteral("SAK offline downloader is already running"));
                result[QStringLiteral("failure_class")] = QStringLiteral("tool_busy");
                return result;
            }
            return QJsonObject{{QStringLiteral("success"), true}};
        });
}

void AiAssistantPanel::registerProviderGatewayAvailabilityChecker() {
    m_toolDispatcher->registerAvailabilityChecker(
        QStringLiteral("sak_provider_gateway"),
        [](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            ai::AiProviderGateway gateway;
            QString error;
            QJsonObject result = gateway.checkAvailability(args, &error);
            if (!error.isEmpty() &&
                result.value(QStringLiteral("error_message")).toString().isEmpty()) {
                result[QStringLiteral("error_message")] = error;
            }
            return result;
        });
}

void AiAssistantPanel::registerSessionSearchAvailabilityChecker() {
    m_toolDispatcher->registerAvailabilityChecker(
        QStringLiteral("sak_session_search"),
        [this](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            if (!m_conversationStore) {
                QJsonObject result = toolError(QStringLiteral("AI session store is not available"));
                result[QStringLiteral("failure_class")] =
                    QStringLiteral("session_store_unavailable");
                return result;
            }
            if (args.value(QStringLiteral("query")).toString().trimmed().isEmpty()) {
                QJsonObject result = toolError(QStringLiteral("sak_session_search requires query"));
                result[QStringLiteral("failure_class")] = QStringLiteral("invalid_request");
                return result;
            }
            return QJsonObject{{QStringLiteral("success"), true}};
        });
}

void AiAssistantPanel::registerToolHandlers() {
    m_toolDispatcher->registerHandler(
        QStringLiteral("take_screenshot"),
        [this](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            return runScreenshotTool(args.value(QStringLiteral("reason")).toString());
        });
    m_toolDispatcher->registerHandler(
        QStringLiteral("download_file"),
        [this](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            return runDownloadTool(args.value(QStringLiteral("url")).toString(),
                                   args.value(QStringLiteral("filename")).toString());
        });
    m_toolDispatcher->registerHandler(
        QStringLiteral("run_powershell"),
        [this](const QJsonObject& args, const ai::AiToolPolicyDecision&) {
            return runWorkflowPowerShellTool(
                args, args.value(QStringLiteral("command_preview")).toString());
        });
    m_toolDispatcher->registerHandler(QStringLiteral("sak_package_manager"),
                                      [this](const QJsonObject& args,
                                             const ai::AiToolPolicyDecision&) {
                                          return runPackageManagerTool(args);
                                      });
    m_toolDispatcher->registerHandler(QStringLiteral("sak_offline_downloader"),
                                      [this](const QJsonObject& args,
                                             const ai::AiToolPolicyDecision&) {
                                          return runOfflineDownloaderTool(args);
                                      });
    m_toolDispatcher->registerHandler(QStringLiteral("sak_provider_gateway"),
                                      [this](const QJsonObject& args,
                                             const ai::AiToolPolicyDecision&) {
                                          return runProviderGatewayTool(args);
                                      });
    m_toolDispatcher->registerHandler(QStringLiteral("sak_session_search"),
                                      [this](const QJsonObject& args,
                                             const ai::AiToolPolicyDecision&) {
                                          return runSessionSearchTool(args);
                                      });
}

const ai::WorkflowTemplate* AiAssistantPanel::attachedWorkflow() const {
    if (!m_workflowStore) {
        return nullptr;
    }
    for (const auto& item : m_contextItems) {
        if (item.type != ContextItem::Type::Workflow) {
            continue;
        }
        if (item.path.startsWith(QStringLiteral("workflow:"))) {
            const QString id = item.path.mid(9);
            return m_workflowStore->workflowById(id);
        }
    }
    return nullptr;
}

bool AiAssistantPanel::ensureWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                            const QString& user_message,
                                            QJsonObject* input_values,
                                            const QJsonObject& initial_values,
                                            bool preserve_run_id) {
    if (!input_values) {
        return false;
    }
    *input_values = mergeWorkflowInputValues(workflowInputValues(workflow, user_message),
                                             initial_values);
    if (!preserve_run_id) {
        m_pendingWorkflowRunId.clear();
    }
    return collectRequiredWorkflowInputs(workflow, user_message, input_values) &&
           collectClarifyingWorkflowInputs(workflow, user_message, input_values);
}

bool AiAssistantPanel::collectRequiredWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                     const QString& user_message,
                                                     QJsonObject* input_values) {
    Q_UNUSED(user_message);
    for (const auto& input : workflow.required_inputs) {
        const QString id = input.id.trimmed();
        if (!input.required || id.isEmpty() || workflowInputHasValue(input_values->value(id))) {
            continue;
        }
        const QString label = input.label.trimmed().isEmpty() ? id : input.label.trimmed();
        const QString prompt =
            tr("Missing required workflow input for %1:\n%2").arg(workflow.title, label);
        const auto provided_value = promptWorkflowInputValue(this, input, workflow.title, prompt);
        if (!provided_value.has_value()) {
            appendLocalEvent(tr("Workflow input cancelled: %1").arg(label));
            m_taskStatus = tr("Waiting for human input");
            emitStatusDetails();
            updateCredentialControls();
            return false;
        }
        (*input_values)[id] = *provided_value;
        QJsonObject metadata;
        metadata[QStringLiteral("workflow_id")] = workflow.id;
        metadata[QStringLiteral("input_id")] = id;
        metadata[QStringLiteral("input_value_summary")] = workflowInputSummary(*provided_value);
        traceAiEvent(QStringLiteral("workflow_input"),
                     QStringLiteral("required_input"),
                     QStringLiteral("completed"),
                     metadata);
    }
    return true;
}

bool AiAssistantPanel::collectClarifyingWorkflowInputs(const ai::WorkflowTemplate& workflow,
                                                       const QString& user_message,
                                                       QJsonObject* input_values) {
    const auto clarification =
        ai::AiWorkflowClarifier::analyze(workflow, user_message, *input_values);
    for (const auto& question : clarification.questions) {
        const auto input_it = std::find_if(workflow.required_inputs.begin(),
                                           workflow.required_inputs.end(),
                                           [&question](const ai::WorkflowRequiredInput& input) {
                                               return input.id.trimmed() == question.input_id;
                                           });
        if (input_it == workflow.required_inputs.end()) {
            continue;
        }
        const QString label = question.label.trimmed().isEmpty() ? question.input_id
                                                                 : question.label.trimmed();
        const QString prompt = tr("Overseer needs clarification before running %1:\n%2")
                                   .arg(workflow.title, question.question);
        const auto provided_value =
            promptWorkflowInputValue(this, *input_it, workflow.title, prompt);
        if (!provided_value.has_value()) {
            appendLocalEvent(tr("Workflow clarification cancelled: %1").arg(label));
            updateCredentialControls();
            return false;
        }
        (*input_values)[question.input_id] = *provided_value;
        QJsonObject metadata;
        metadata[QStringLiteral("workflow_id")] = workflow.id;
        metadata[QStringLiteral("input_id")] = question.input_id;
        metadata[QStringLiteral("input_value_summary")] = workflowInputSummary(*provided_value);
        traceAiEvent(QStringLiteral("workflow_input"),
                     QStringLiteral("overseer_clarification"),
                     QStringLiteral("completed"),
                     metadata);
    }
    return true;
}
bool AiAssistantPanel::resumeWorkflowInputGate(const ai::AiHumanGate& gate) {
    Q_UNUSED(gate);
    sak::showInformationLogged(
        this,
        tr("Workflow Input"),
        tr("This workflow input gate is not active in the current chat. Start "
           "the workflow again to provide fresh inputs."));
    updateCredentialControls();
    return false;
}
bool AiAssistantPanel::resumeApprovalGate(const ai::AiHumanGate& gate) {
    Q_UNUSED(gate);
    sak::showInformationLogged(
        this,
        tr("AI Approval"),
        tr("This approval prompt is no longer active. Re-run the action so "
           "SAK can show a fresh approval prompt with the current command."));
    updateCredentialControls();
    return false;
}
bool AiAssistantPanel::resumeWorkflowRecoveryGate(const ai::AiHumanGate& gate) {
    QJsonObject metadata;
    QJsonObject resume_state;
    const ai::WorkflowTemplate* workflow = recoveryWorkflowForGate(gate, &metadata, &resume_state);
    if (!workflow) {
        return false;
    }
    if (!resolveWorkflowRecoveryChoice(gate, &metadata)) {
        return true;
    }

    resume_state[QStringLiteral("human_decision")] = QStringLiteral("continue_degraded");
    resume_state[QStringLiteral("resumed_gate_id")] = gate.gate_id;
    const QString user_message =
        resume_state.value(QStringLiteral("user_message")).toString().trimmed();
    const QJsonObject input_values = resume_state.value(QStringLiteral("input_values")).toObject();
    if (user_message.isEmpty()) {
        appendLocalEvent(tr("Workflow recovery resume failed: original request missing"));
        sak::showWarningLogged(this,
                               tr("Resume Blocked"),
                               tr("This recovery gate is missing the original workflow request."));
        return false;
    }

    appendLocalEvent(tr("Resuming workflow recovery: %1").arg(workflow->title));
    appendSessionMemory(QStringLiteral("Workflow"),
                        QStringLiteral("Recovery resumed"),
                        metadata.value(QStringLiteral("summary")).toString());
    m_currentRunId.clear();
    m_runState.run_id = gate.run_id;
    m_runState.workflow_id = workflow->id;
    runWorkflowAsync(*workflow, user_message, input_values, gate.run_id, resume_state);
    return true;
}

const ai::WorkflowTemplate* AiAssistantPanel::recoveryWorkflowForGate(const ai::AiHumanGate& gate,
                                                                      QJsonObject* metadata,
                                                                      QJsonObject* resume_state) {
    if (!gate.isPending() || gate.kind != QLatin1String("workflow_recovery") ||
        gate.name != QLatin1String("phase_needs_human")) {
        sak::showInformationLogged(this,
                                   tr("AI Run Waiting"),
                                   tr("This pending gate is not a workflow recovery gate. Open Run "
                                      "Details to review it."));
        showRunDetails();
        return nullptr;
    }
    if (!m_workflowStore) {
        appendLocalEvent(tr("Workflow recovery resume failed: workflow store unavailable"));
        return nullptr;
    }
    *metadata = gate.metadata;
    *resume_state = metadata->value(QStringLiteral("workflow_resume")).toObject();
    if (resume_state->isEmpty()) {
        appendLocalEvent(tr("Workflow recovery resume failed: missing resume state"));
        sak::showWarningLogged(
            this,
            tr("Resume Blocked"),
            tr("This recovery gate does not have enough workflow state to resume automatically."));
        return nullptr;
    }
    const QString workflow_id =
        resume_state->value(QStringLiteral("workflow_id")).toString(gate.workflow_id).trimmed();
    const ai::WorkflowTemplate* workflow = m_workflowStore->workflowById(workflow_id);
    if (!workflow) {
        appendLocalEvent(
            tr("Workflow recovery resume failed: unknown workflow %1").arg(workflow_id));
        sak::showWarningLogged(this,
                               tr("Workflow Missing"),
                               tr("The workflow for this waiting run is no longer available."));
    }
    return workflow;
}

bool AiAssistantPanel::resolveWorkflowRecoveryChoice(const ai::AiHumanGate& gate,
                                                     QJsonObject* metadata) {
    const auto choice = sak::showQuestionLogged(
        this,
        tr("Resume Workflow?"),
        tr("The workflow paused after a phase needed human input. Continue from the next phase?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    const bool continue_workflow = choice == QMessageBox::Yes;
    (*metadata)[QStringLiteral("resumed_after_reopen")] = true;
    (*metadata)[QStringLiteral("decision")] =
        continue_workflow ? QStringLiteral("continue_degraded") : QStringLiteral("abort");
    (*metadata)[QStringLiteral("summary")] =
        continue_workflow ? tr("Workflow recovery continued after human decision")
                          : tr("Workflow recovery aborted by human decision");
    resolveHumanGate(gate.gate_id,
                     continue_workflow ? ai::humanGateApprovedStatus()
                                       : ai::humanGateRejectedStatus(),
                     metadata->value(QStringLiteral("decision")).toString(),
                     metadata->value(QStringLiteral("summary")).toString(),
                     *metadata);
    traceAiEvent(QStringLiteral("workflow_recovery"),
                 QStringLiteral("phase_needs_human"),
                 continue_workflow ? QStringLiteral("approved") : QStringLiteral("rejected"),
                 *metadata);
    if (!continue_workflow) {
        m_runState.status = ai::AiRunStatus::Failed;
        m_runState.message = tr("Workflow recovery aborted by human");
        saveRunStateSnapshot();
        emitStatusDetails();
    }
    return continue_workflow;
}
QJsonObject AiAssistantPanel::dispatchWorkflowToolPhase(const ai::WorkflowPhase& phase,
                                                        ai::AiToolPolicy policy,
                                                        const ai::AiWorkflowPhaseContext& context) {
    if (phase.type.compare(QStringLiteral("cleanup"), Qt::CaseInsensitive) == 0 &&
        phase.tool.trimmed().isEmpty()) {
        QJsonObject ok;
        ok[QStringLiteral("success")] = true;
        ok[QStringLiteral("cleanup")] =
            QStringLiteral("No automatic cleanup tool configured; preserved requested artifacts");
        return ok;
    }
    if (!m_toolDispatcher) {
        return toolError(QStringLiteral("No tool dispatcher"));
    }
    WorkflowToolDispatchPlan plan = workflowToolDispatchPlan(phase, policy, context);
    if (!plan.ok) {
        return plan.error;
    }
    if (!authorizeWorkflowToolPhase(phase, plan)) {
        return toolError(QStringLiteral("User declined workflow tool phase '%1'").arg(phase.id));
    }
    const auto outcome = m_toolDispatcher->dispatch(plan.policy,
                                                    plan.request,
                                                    plan.args,
                                                    phase.agent.isEmpty()
                                                        ? QStringLiteral("overseer")
                                                        : phase.agent);
    return outcome.result;
}

AiAssistantPanel::WorkflowToolDispatchPlan AiAssistantPanel::workflowToolDispatchPlan(
    const ai::WorkflowPhase& phase,
    ai::AiToolPolicy policy,
    const ai::AiWorkflowPhaseContext& context) {
    WorkflowToolDispatchPlan plan;
    if (phase.tool.trimmed().isEmpty()) {
        plan.error = toolError(QStringLiteral("Workflow phase '%1' has no tool").arg(phase.id));
        return plan;
    }
    initializeWorkflowToolPlan(phase, policy, context, &plan);
    if (phase.tool == QLatin1String("run_powershell") &&
        !prepareWorkflowPowerShellTool(phase, &plan)) {
        return plan;
    }
    if (phase.tool == QLatin1String("sak_package_manager") &&
        !prepareWorkflowPackageTool(phase, context, &plan)) {
        return plan;
    }
    if (phase.tool == QLatin1String("sak_offline_downloader") &&
        !prepareWorkflowOfflineTool(phase, context, &plan)) {
        return plan;
    }
    finalizeWorkflowToolPlan(phase, &plan);
    return plan;
}

void AiAssistantPanel::initializeWorkflowToolPlan(const ai::WorkflowPhase& phase,
                                                  ai::AiToolPolicy policy,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan) {
    plan->policy = policy;
    if (plan->policy == ai::AiToolPolicy::NoLocalExecution && phase.agent.trimmed().isEmpty()) {
        plan->policy = currentAccessToolPolicy();
    }
    plan->request.tool_name = phase.tool;
    plan->request.operation = phase.operation;
    plan->request.command_preview = phase.prompt;
    plan->args = substituteWorkflowPlaceholdersInObject(phase.arguments, context);
    if (phase.tool == QLatin1String("run_powershell") &&
        phase.arguments.value(QStringLiteral("command")).isString()) {
        plan->args[QStringLiteral("command")] = substituteWorkflowPlaceholders(
            phase.arguments.value(QStringLiteral("command")).toString(),
            context,
            WorkflowPlaceholderMode::PowerShellSingleQuoted);
    }
    plan->args[QStringLiteral("operation")] = phase.operation;
    plan->args[QStringLiteral("phase_id")] = phase.id;
    plan->args[QStringLiteral("workflow_id")] = context.workflow_id;
    plan->args[QStringLiteral("run_id")] = context.run_id;
    const QString query = workflowPackageQuery(context);
    if (!query.isEmpty()) {
        plan->args[QStringLiteral("query")] = query;
    }
}

bool AiAssistantPanel::prepareWorkflowPowerShellTool(const ai::WorkflowPhase& phase,
                                                     WorkflowToolDispatchPlan* plan) {
    const QString command = plan->args.value(QStringLiteral("command")).toString().trimmed();
    if (command.isEmpty()) {
        plan->error = toolError(
            QStringLiteral("Workflow PowerShell phase '%1' has no explicit arguments.command")
                .arg(phase.id));
        return false;
    }
    plan->request.command_preview = command;
    plan->request.requires_admin = plan->args.value(QStringLiteral("requires_admin")).toBool(false);
    return true;
}

void AiAssistantPanel::finalizeWorkflowToolPlan(const ai::WorkflowPhase& phase,
                                                WorkflowToolDispatchPlan* plan) {
    const QString risk = phase.risk.trimmed().toLower();
    plan->phase_risky = risk.contains(QStringLiteral("system_change")) ||
                        risk.contains(QStringLiteral("repair")) ||
                        risk.contains(QStringLiteral("uninstall")) ||
                        risk.contains(QStringLiteral("destructive"));
    if (plan->request.command_preview.trimmed().isEmpty()) {
        plan->request.command_preview = phase.prompt;
    }
    plan->args[QStringLiteral("command_preview")] = plan->request.command_preview;
    plan->ok = true;
}
bool AiAssistantPanel::prepareWorkflowPackageTool(const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan) {
    const QString operation = phase.operation.trimmed().toLower();
    const QStringList queries = workflowPackageQueries(context);
    if (operation == QLatin1String("search")) {
        if (queries.isEmpty()) {
            plan->error = toolError(QStringLiteral("Workflow package search needs an app name"));
            return false;
        }
        plan->args[QStringLiteral("query")] = queries.join(QStringLiteral(", "));
        plan->args[QStringLiteral("queries")] = stringListToJsonArray(queries);
        plan->request.command_preview =
            QStringLiteral("search package(s): %1").arg(queries.join(QStringLiteral(", ")));
        return true;
    }
    if (!isPackageReadOperation(operation) && !isPackageChangeOperation(operation)) {
        return true;
    }
    QJsonObject search_args;
    search_args[QStringLiteral("operation")] = QStringLiteral("search");
    search_args[QStringLiteral("query")] = workflowPackageQuery(context);
    const QJsonObject search_result = runPackageManagerTool(search_args);
    if (!search_result.value(QStringLiteral("success")).toBool(false)) {
        plan->error = search_result;
        return false;
    }
    QString package_error;
    QJsonObject selection_details;
    const QJsonArray packages = packagesFromToolSearch(
        search_result, workflowPackageQuery(context), &package_error, &selection_details);
    if (packages.isEmpty()) {
        plan->error = toolError(package_error);
        mergePackageSelectionError(&plan->error, selection_details);
        return false;
    }
    const QJsonObject resolved = packages.at(0).toObject();
    plan->args[QStringLiteral("package_id")] =
        resolved.value(QStringLiteral("package_id")).toString();
    plan->args[QStringLiteral("resolved_package")] =
        resolved.value(QStringLiteral("resolved_from")).toObject();
    plan->request.command_preview =
        QStringLiteral("%1 package %2")
            .arg(operation, plan->args.value(QStringLiteral("package_id")).toString());
    return true;
}

bool AiAssistantPanel::prepareWorkflowOfflineTool(const ai::WorkflowPhase& phase,
                                                  const ai::AiWorkflowPhaseContext& context,
                                                  WorkflowToolDispatchPlan* plan) {
    const QString operation = phase.operation.trimmed().toLower();
    const QStringList queries = workflowPackageQueries(context);
    if (operation == QLatin1String("search")) {
        if (queries.isEmpty()) {
            plan->error = toolError(QStringLiteral("Workflow offline search needs an app name"));
            return false;
        }
        plan->args[QStringLiteral("query")] = queries.join(QStringLiteral(", "));
        plan->args[QStringLiteral("queries")] = stringListToJsonArray(queries);
        plan->request.command_preview =
            QStringLiteral("search offline package(s): %1").arg(queries.join(QStringLiteral(", ")));
        return true;
    }
    if (!offlineNeedsPackages(operation)) {
        return true;
    }
    QJsonArray resolved_packages;
    for (const auto& package_query : queries) {
        QJsonObject search_args;
        search_args[QStringLiteral("operation")] = QStringLiteral("search");
        search_args[QStringLiteral("query")] = package_query;
        const QJsonObject search_result = runOfflineDownloaderTool(search_args);
        QString package_error;
        QJsonObject selection_details;
        const QJsonArray packages = packagesFromToolSearch(
            search_result, package_query, &package_error, &selection_details);
        if (packages.isEmpty()) {
            plan->error = toolError(package_error);
            mergePackageSelectionError(&plan->error, selection_details);
            return false;
        }
        resolved_packages.append(packages.at(0));
    }
    plan->args[QStringLiteral("packages")] = resolved_packages;
    plan->request.command_preview = QStringLiteral("%1 offline package(s): %2")
                                        .arg(operation, queries.join(QStringLiteral(", ")));
    return true;
}

bool AiAssistantPanel::authorizeWorkflowToolPhase(const ai::WorkflowPhase& phase,
                                                  const WorkflowToolDispatchPlan& plan) {
    const auto decision = ai::evaluateToolPolicy(plan.policy, plan.request);
    const bool needs_gate = decision.allowed && (plan.phase_risky || plan.request.requires_admin ||
                                                 decision.risky_change);
    if (!needs_gate) {
        return true;
    }
    const QString label = phase.tool == QLatin1String("run_powershell")
                              ? QStringLiteral("workflow PowerShell")
                              : QStringLiteral("workflow %1").arg(phase.tool);
    if (currentAccessMode() == AccessMode::AssistedFullAccess) {
        return confirmCommandWithUser(label, plan.request.command_preview, true);
    }
    if (currentAccessMode() == AccessMode::UnattendedFullAccess) {
        return offerRestorePointIfNeeded(plan.request.command_preview, true);
    }
    return true;
}
class AiAssistantPanel::PanelToolExecutor : public ai::IAiToolExecutor {
public:
    explicit PanelToolExecutor(AiAssistantPanel* panel) : m_panel(panel) {}

    QJsonObject runToolPhase(const ai::WorkflowPhase& phase,
                             ai::AiToolPolicy policy,
                             const ai::AiWorkflowPhaseContext& context,
                             const ai::CancellationToken& token) override {
        QPointer<AiAssistantPanel> panel = m_panel;
        if (!panel) {
            QJsonObject err;
            err[QStringLiteral("success")] = false;
            err[QStringLiteral("error_message")] = QStringLiteral("Panel gone");
            return err;
        }
        if (token.isValid() && token.isCancellationRequested()) {
            QJsonObject err;
            err[QStringLiteral("success")] = false;
            err[QStringLiteral("cancelled")] = true;
            err[QStringLiteral("error_message")] = token.cancelReason();
            return err;
        }
        QJsonObject result;
        if (QThread::currentThread() == panel->thread()) {
            result = panel->dispatchWorkflowToolPhase(phase, policy, context);
            return result;
        }
        const bool invoked = QMetaObject::invokeMethod(
            panel.data(),
            [panel, phase, policy, context, &result]() {
                if (!panel) {
                    result = toolError(QStringLiteral("Panel gone"));
                    return;
                }
                result = panel->dispatchWorkflowToolPhase(phase, policy, context);
            },
            Qt::BlockingQueuedConnection);
        if (!invoked) {
            return toolError(QStringLiteral("Could not marshal workflow tool phase to UI thread"));
        }
        return result;
    }

private:
    QPointer<AiAssistantPanel> m_panel;
};

void AiAssistantPanel::appendPhaseStartedToTranscript(const ai::WorkflowPhase& phase) {
    if (phase.id.trimmed().isEmpty()) {
        return;
    }

    m_runState.status = ai::AiRunStatus::Running;
    m_runState.phase_id = phase.id;
    m_activeWorkflowCurrentPhase = phase.id;
    int& start_count = m_activeWorkflowPhaseStartCounts[phase.id];
    if (start_count == 0) {
        if (isWorkflowDelegatePhaseType(phase.type)) {
            ++m_runState.active_subagents;
        } else if (isWorkflowToolPhaseType(phase.type)) {
            ++m_runState.active_tools;
        }
    }
    ++start_count;

    const QString owner = workflowPhaseOwnerLabel(phase);
    m_taskStatus = tr("Workflow phase %1").arg(phase.id);
    if (m_transcriptView) {
        QString line = tr("[phase %1] running").arg(phase.id);
        if (!owner.isEmpty()) {
            line += tr(" (%1)").arg(owner);
        }
        appendTranscriptMessage(QStringLiteral("Workflow"), line);
    }

    QJsonObject metadata;
    metadata[QStringLiteral("workflow_id")] = m_runState.workflow_id;
    metadata[QStringLiteral("phase_id")] = phase.id;
    metadata[QStringLiteral("phase_type")] = phase.type;
    metadata[QStringLiteral("agent_id")] = phase.agent;
    metadata[QStringLiteral("tool_name")] = phase.tool;
    metadata[QStringLiteral("operation")] = phase.operation;
    metadata[QStringLiteral("risk")] = phase.risk;
    metadata[QStringLiteral("summary")] = tr("Started workflow phase %1").arg(phase.id);
    traceAiEvent(QStringLiteral("workflow_phase"), phase.id, QStringLiteral("running"), metadata);
    updateWorkflowProgressUi();
    emitStatusDetails();
    saveRunStateSnapshot();
    setActivityIndicator(tr("Workflow: %1").arg(phase.id), true);
    Q_EMIT statusMessage(tr("Workflow phase: %1").arg(phase.id), kWorkflowPhaseStatusMessageMs);
}

void AiAssistantPanel::appendPhaseToTranscript(const ai::AiPhaseExecution& execution) {
    m_workflowPhaseHistory.append(execution);
    m_runState.phase_id = execution.phase_id;
    updatePhaseRunCounters(execution);

    QStringList transcript_details;
    QJsonObject metadata = phaseTranscriptMetadata(execution, &transcript_details);
    const QString phase_chat = phaseTranscriptLine(execution, transcript_details);
    recordPhaseTranscript(execution, phase_chat, metadata, transcript_details);
    traceAiEvent(QStringLiteral("workflow_phase"),
                 execution.phase_id,
                 phaseTraceStatus(execution, metadata),
                 metadata);
    emitStatusDetails();
    saveRunStateSnapshot();
    updateWorkflowProgressUi();
    setActivityIndicator(execution.success ? tr("Workflow running")
                                           : tr("Workflow needs attention"),
                         m_workflowRunActive);
}

void AiAssistantPanel::recordPhaseTranscript(const ai::AiPhaseExecution& execution,
                                             const QString& phase_chat,
                                             const QJsonObject& metadata,
                                             const QStringList& transcript_details) {
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Workflow"), phase_chat);
    }
    if (m_conversationStore && (execution.ran || !transcript_details.isEmpty())) {
        QString error;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("Workflow"), phase_chat, metadata, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
    }
    if (!transcript_details.isEmpty() &&
        execution.phase_type.compare(QStringLiteral("delegate"), Qt::CaseInsensitive) == 0) {
        appendSessionMemory(QStringLiteral("Subagent"),
                            execution.phase_id,
                            transcript_details.join(QStringLiteral("\n")));
    }
}

void AiAssistantPanel::updatePhaseRunCounters(const ai::AiPhaseExecution& execution) {
    if (execution.ran && !execution.skipped) {
        const int start_count = m_activeWorkflowPhaseStartCounts.take(execution.phase_id);
        if (isWorkflowDelegatePhaseType(execution.phase_type)) {
            if (start_count > 0) {
                m_runState.active_subagents = std::max(0, m_runState.active_subagents - 1);
            }
            ++m_runState.completed_subagents;
        } else if (isWorkflowToolPhaseType(execution.phase_type)) {
            if (start_count > 0) {
                m_runState.active_tools = std::max(0, m_runState.active_tools - 1);
            }
            ++m_runState.completed_tools;
        }
    }
    m_activeWorkflowCompletedPhaseCount = workflowCompletedPhaseCount();
}

QJsonObject AiAssistantPanel::phaseTranscriptMetadata(const ai::AiPhaseExecution& execution,
                                                      QStringList* transcript_details) {
    QJsonObject metadata;
    metadata[QStringLiteral("workflow_id")] = m_runState.workflow_id;
    metadata[QStringLiteral("phase_id")] = execution.phase_id;
    metadata[QStringLiteral("phase_type")] = execution.phase_type;
    metadata[QStringLiteral("agent_id")] = execution.agent_id;
    metadata[QStringLiteral("duration_ms")] = static_cast<double>(execution.duration_ms);
    metadata[QStringLiteral("success")] = execution.success;
    metadata[QStringLiteral("skipped")] = execution.skipped;
    if (!execution.error_message.isEmpty()) {
        metadata[QStringLiteral("error_message")] = execution.error_message;
    }
    if (!execution.skip_reason.isEmpty()) {
        metadata[QStringLiteral("skip_reason")] = execution.skip_reason;
    }
    if (!execution.tool_result.isEmpty()) {
        metadata[QStringLiteral("tool_result")] = execution.tool_result;
        const QString summary = ai::toolResultChatSummary(execution.tool_result,
                                                          ai::CredentialStore::redactSecrets);
        if (!summary.isEmpty() && transcript_details) {
            *transcript_details << summary;
        }
    }
    if (!execution.metadata.isEmpty()) {
        metadata[QStringLiteral("phase_metadata")] = execution.metadata;
        appendRecoveryMetadata(execution, &metadata);
        appendSubagentTranscriptDetails(
            execution.metadata.value(QStringLiteral("result")).toObject(),
            &metadata,
            transcript_details);
    }
    return metadata;
}

void AiAssistantPanel::appendRecoveryMetadata(const ai::AiPhaseExecution& execution,
                                              QJsonObject* metadata) {
    const QJsonObject recovery =
        execution.metadata.value(QStringLiteral("recovery_decision")).toObject();
    if (!metadata || recovery.isEmpty()) {
        return;
    }
    (*metadata)[QStringLiteral("recovery_action")] =
        recovery.value(QStringLiteral("action")).toString();
    (*metadata)[QStringLiteral("recovery_reason")] =
        recovery.value(QStringLiteral("reason")).toString();
    if (metadata->value(QStringLiteral("recovery_action")).toString() !=
        QLatin1String("ask_human")) {
        return;
    }
    const QString question = recovery.value(QStringLiteral("reason")).toString().trimmed().isEmpty()
                                 ? execution.error_message
                                 : recovery.value(QStringLiteral("reason")).toString();
    (*metadata)[QStringLiteral("question_for_human")] = question;
    QJsonObject resume_state;
    resume_state[QStringLiteral("schema")] = QStringLiteral("sak.ai.workflow_recovery_resume.v1");
    resume_state[QStringLiteral("run_id")] = m_runState.run_id;
    resume_state[QStringLiteral("workflow_id")] = m_runState.workflow_id;
    resume_state[QStringLiteral("user_message")] = m_activeWorkflowUserMessage;
    resume_state[QStringLiteral("input_values")] = m_activeWorkflowInputValues;
    resume_state[QStringLiteral("phase_history")] = phaseHistoryToJson(m_workflowPhaseHistory);
    resume_state[QStringLiteral("resume_start_phase_index")] = workflowCompletedPhaseCount();
    (*metadata)[QStringLiteral("workflow_resume")] = resume_state;
    const QString gate_id = beginHumanGate(QStringLiteral("workflow_recovery"),
                                           QStringLiteral("phase_needs_human"),
                                           question,
                                           *metadata);
    (*metadata)[QStringLiteral("gate_id")] = gate_id;
}

void AiAssistantPanel::appendSubagentTranscriptDetails(const QJsonObject& subagent_result,
                                                       QJsonObject* metadata,
                                                       QStringList* transcript_details) const {
    if (!metadata || !transcript_details || subagent_result.isEmpty()) {
        return;
    }
    (*metadata)[QStringLiteral("subagent_result")] = subagent_result;
    const QString summary = subagent_result.value(QStringLiteral("summary")).toString().trimmed();
    if (!summary.isEmpty()) {
        (*metadata)[QStringLiteral("summary")] = summary.left(kDefaultPreviewMaxChars);
        *transcript_details << tr("  summary: %1")
                                   .arg(ai::CredentialStore::redactSecrets(
                                       summary.left(kDefaultCleanReportMaxChars)));
    }
    for (const auto& finding : findingSummaries(subagent_result.value(QStringLiteral("findings")),
                                                kSubagentTranscriptFindingLimit)) {
        *transcript_details << tr("  finding: %1").arg(ai::CredentialStore::redactSecrets(finding));
    }
    const QStringList actions =
        jsonStringList(subagent_result.value(QStringLiteral("actions_taken")))
            .mid(0, kSubagentTranscriptActionLimit);
    if (!actions.isEmpty()) {
        *transcript_details
            << tr("  actions: %1")
                   .arg(ai::CredentialStore::redactSecrets(
                       actions.join(QStringLiteral("; ")).left(kDefaultCleanReportMaxChars)));
    }
    const QStringList next_steps =
        jsonStringList(subagent_result.value(QStringLiteral("recommended_next_steps")))
            .mid(0, kSubagentTranscriptNextStepLimit);
    if (!next_steps.isEmpty()) {
        *transcript_details
            << tr("  next: %1")
                   .arg(ai::CredentialStore::redactSecrets(
                       next_steps.join(QStringLiteral("; ")).left(kDefaultCleanReportMaxChars)));
    }
}

QString AiAssistantPanel::phaseTranscriptLine(const ai::AiPhaseExecution& execution,
                                              const QStringList& transcript_details) const {
    QString line =
        execution.skipped
            ? tr("[phase %1] skipped (%2)").arg(execution.phase_id, execution.skip_reason)
        : execution.success
            ? tr("[phase %1] ok (%2 ms)").arg(execution.phase_id).arg(execution.duration_ms)
            : tr("[phase %1] FAILED: %2").arg(execution.phase_id, execution.error_message);
    if (!transcript_details.isEmpty()) {
        line += QStringLiteral("\n") + transcript_details.join(QStringLiteral("\n"));
    }
    return line;
}
void AiAssistantPanel::onWorkflowRunFinished() {
    if (!m_workflowRunWatcher) {
        return;
    }
    const auto result = m_workflowRunWatcher->result();
    m_workflowRunActive = false;
    m_runState.status = result.status;
    m_runState.active_subagents = 0;
    m_runState.active_tools = 0;
    finishWorkflowProgressUi(result);

    const QString result_text = workflowResultText(result);
    recordWorkflowResult(result, result_text);
    finishWorkflowRunState(result);
    dispatchQueuedPromptIfIdle();
}

QString AiAssistantPanel::workflowResultText(const ai::AiOrchestratorResult& result) const {
    QStringList lines;
    lines << tr("Workflow %1 finished with status %2")
                 .arg(result.workflow_id, ai::runStatusToString(result.status));
    if (!result.error_message.isEmpty()) {
        lines << tr("message: %1").arg(result.error_message);
    }
    if (!result.cleanup_failures.isEmpty()) {
        lines << tr("cleanup failures: %1").arg(result.cleanup_failures.join(QStringLiteral(", ")));
    }
    QStringList findings;
    QStringList actions;
    QStringList next_steps;
    for (const auto& phase : result.phases) {
        const QJsonObject subagent_result =
            phase.metadata.value(QStringLiteral("result")).toObject();
        findings << findingSummaries(subagent_result.value(QStringLiteral("findings")),
                                     kSubagentPhaseFindingLimit);
        actions << jsonStringList(subagent_result.value(QStringLiteral("actions_taken")));
        next_steps << jsonStringList(
            subagent_result.value(QStringLiteral("recommended_next_steps")));
    }
    findings.removeDuplicates();
    actions.removeDuplicates();
    next_steps.removeDuplicates();
    appendListSection(&lines,
                      tr("Findings"),
                      findings.mid(0, kSubagentSummarySectionLimit),
                      tr("No structured findings recorded."));
    appendListSection(&lines,
                      tr("Actions completed"),
                      actions.mid(0, kSubagentSummarySectionLimit),
                      tr("No actions recorded."));
    appendListSection(&lines,
                      tr("Next steps"),
                      next_steps.mid(0, kSubagentSummarySectionLimit),
                      tr("No next steps recorded."));
    return lines.join(QLatin1Char('\n'));
}

void AiAssistantPanel::recordWorkflowResult(const ai::AiOrchestratorResult& result,
                                            const QString& workflow_result_text) {
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Workflow Results"), workflow_result_text);
    }
    if (m_conversationStore) {
        QJsonObject transcript_metadata = result.toJson();
        transcript_metadata[QStringLiteral("workflow_id")] = result.workflow_id;
        transcript_metadata[QStringLiteral("run_id")] = result.run_id;
        QString error;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("Workflow Results"), workflow_result_text, transcript_metadata, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
    }
    appendLocalEvent(tr("Workflow run finished: %1 (%2 phases)")
                         .arg(result.workflow_id)
                         .arg(result.phases.size()));
    appendSessionMemory(QStringLiteral("Workflow"),
                        QStringLiteral("Run finished"),
                        result.toJson().value(QStringLiteral("status")).toString() +
                            QStringLiteral(" ") + result.error_message);
}

void AiAssistantPanel::finishWorkflowRunState(const ai::AiOrchestratorResult& result) {
    const QString trace_status =
        result.status == ai::AiRunStatus::Completed         ? QStringLiteral("completed")
        : result.status == ai::AiRunStatus::Cancelled       ? QStringLiteral("cancelled")
        : result.status == ai::AiRunStatus::WaitingForHuman ? QStringLiteral("waiting_for_human")
                                                            : QStringLiteral("failed");
    finishAiRunTrace(trace_status, result.toJson());
    m_pendingSteeringMessages.clear();
    setUiBusy(false);
    m_runState.status = result.status;
    m_runState.active_subagents = 0;
    m_runState.active_tools = 0;
    m_taskStatus = result.status == ai::AiRunStatus::Completed   ? tr("Workflow complete")
                   : result.status == ai::AiRunStatus::Cancelled ? tr("Workflow cancelled")
                   : result.status == ai::AiRunStatus::WaitingForHuman
                       ? tr("Waiting for human input")
                       : tr("Workflow failed");
    if (result.status == ai::AiRunStatus::WaitingForHuman) {
        setActivityIndicator(tr("Waiting for human input"), false);
    }
    finishWorkflowProgressUi(result);
    emitStatusDetails();
    saveRunStateSnapshot();
    updateCredentialControls();
}
void AiAssistantPanel::runWorkflowAsync(const ai::WorkflowTemplate& workflow,
                                        const QString& user_message,
                                        const QJsonObject& input_values,
                                        const QString& preferred_run_id,
                                        const QJsonObject& resume_state) {
    if (m_workflowRunActive) {
        appendLocalEvent(tr("Workflow run already in progress"));
        return;
    }
    beginWorkflowRunUiState(workflow, user_message, input_values, preferred_run_id, resume_state);
    resetWorkflowRunWatcher();
    startWorkflowRunFuture(workflow, user_message, input_values, resume_state);
}

void AiAssistantPanel::beginWorkflowRunUiState(const ai::WorkflowTemplate& workflow,
                                               const QString& user_message,
                                               const QJsonObject& input_values,
                                               const QString& preferred_run_id,
                                               const QJsonObject& resume_state) {
    setUiBusy(true);
    setActivityIndicator(tr("Running workflow"), true);
    startAiRunTrace(user_message, m_modelCombo->currentText().trimmed(), preferred_run_id);
    m_activeWorkflowUserMessage = user_message;
    m_activeWorkflowInputValues = input_values;
    m_workflowPhaseHistory =
        phaseHistoryFromJson(resume_state.value(QStringLiteral("phase_history")).toArray());
    m_workflowRunActive = true;
    m_runState.workflow_id = workflow.id;
    m_runState.phase_id.clear();
    m_taskStatus = tr("Workflow starting");
    beginWorkflowProgressUi(workflow, resume_state);
    saveRunStateSnapshot();
    emitStatusDetails();
    QJsonObject input_metadata;
    input_metadata[QStringLiteral("workflow_id")] = workflow.id;
    input_metadata[QStringLiteral("workflow_title")] = workflow.title;
    input_metadata[QStringLiteral("input_values")] = input_values;
    input_metadata[QStringLiteral("summary")] =
        tr("Workflow inputs resolved for %1").arg(workflow.title);
    traceAiEvent(QStringLiteral("workflow_input"),
                 QStringLiteral("required_inputs"),
                 QStringLiteral("completed"),
                 input_metadata);
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Workflow"),
                                tr("Workflow run started: %1 (%2 phases)")
                                    .arg(workflow.title)
                                    .arg(workflow.phases.size()));
    }
}

void AiAssistantPanel::resetWorkflowRunWatcher() {
    if (m_workflowRunWatcher) {
        m_workflowRunWatcher->disconnect();
        m_workflowRunWatcher->deleteLater();
        m_workflowRunWatcher = nullptr;
    }
    m_workflowRunWatcher = new QFutureWatcher<ai::AiOrchestratorResult>(this);
    connect(m_workflowRunWatcher,
            &QFutureWatcher<ai::AiOrchestratorResult>::finished,
            this,
            &AiAssistantPanel::onWorkflowRunFinished);
}

void AiAssistantPanel::startWorkflowRunFuture(const ai::WorkflowTemplate& workflow,
                                              const QString& user_message,
                                              const QJsonObject& input_values,
                                              const QJsonObject& resume_state) {
    const QString api_key = apiKey();
    const QString model = m_modelCombo->currentText().trimmed();
    const QString reasoning = m_reasoningEffortCombo->currentText().trimmed().toLower();
    const QString run_id = m_currentRunId;
    ai::CancellationToken token = m_runToken;
    const ai::WorkflowTemplate workflow_copy = workflow;
    const QJsonObject input_values_copy = input_values;
    const QJsonObject resume_state_copy = resume_state;
    QPointer<AiAssistantPanel> panel_guard(this);
    PanelToolExecutor* executor = new PanelToolExecutor(this);
    const WorkflowRunLaunch launch{panel_guard,
                                   workflow_copy,
                                   run_id,
                                   token,
                                   api_key,
                                   model,
                                   reasoning,
                                   input_values_copy,
                                   resume_state_copy,
                                   user_message,
                                   executor};

    QFuture<ai::AiOrchestratorResult> future =
        QtConcurrent::run([launch]() { return AiAssistantPanel::executeWorkflowRun(launch); });
    m_workflowRunWatcher->setFuture(future);
}

ai::AiOrchestratorResult AiAssistantPanel::executeWorkflowRun(const WorkflowRunLaunch& launch) {
    ai::AiSubagentRunner runner(nullptr);
    runner.setModelClientFactory([]() {
        auto client = std::make_unique<ai::OpenAIResponsesModelClient>();
        client->setEnableWebSearch(true);
        return client;
    });
    configureWorkflowRunner(&runner);
    ai::AiOrchestrator orchestrator(&runner, launch.executor);
    orchestrator.setOptions(workflowOrchestrationOptions(launch));
    connectWorkflowOrchestratorCallbacks(&orchestrator, launch.panel_guard);
    const auto result = orchestrator.run(launch.workflow, launch.run_id, launch.token);
    delete launch.executor;
    return result;
}

void AiAssistantPanel::configureWorkflowRunner(ai::AiSubagentRunner* runner) {
    ai::AiSubagentRunnerOptions runner_opts;
    runner_opts.max_retries = kWorkflowRunnerMaxRetries;
    runner_opts.wall_clock_timeout_ms = kWorkflowWallClockTimeoutMs;
    runner->setOptions(runner_opts);
}

ai::AiOrchestrationOptions AiAssistantPanel::workflowOrchestrationOptions(
    const WorkflowRunLaunch& launch) {
    ai::AiOrchestrationOptions opts;
    opts.api_key = launch.api_key;
    opts.default_model = launch.model;
    opts.default_reasoning_effort = launch.reasoning;
    opts.max_parallel_subagents = kWorkflowMaxParallelSubagents;
    opts.user_message = launch.user_message;
    opts.input_values = launch.input_values;
    applyWorkflowResumeState(&opts, launch.resume_state);
    return opts;
}

void AiAssistantPanel::applyWorkflowResumeState(ai::AiOrchestrationOptions* options,
                                                const QJsonObject& resume_state) {
    if (resume_state.isEmpty()) {
        return;
    }
    options->resume_enabled = true;
    options->resume_start_phase_index =
        resume_state.value(QStringLiteral("resume_start_phase_index")).toInt(0);
    options->resume_prior_phases =
        phaseHistoryFromJson(resume_state.value(QStringLiteral("phase_history")).toArray());
    options->resume_flags =
        stringSetFromJson(resume_state.value(QStringLiteral("flags")).toArray());
    options->resume_phase_results = resume_state.value(QStringLiteral("phase_results")).toObject();
}

void AiAssistantPanel::connectWorkflowOrchestratorCallbacks(
    ai::AiOrchestrator* orchestrator, QPointer<AiAssistantPanel> panel_guard) {
    orchestrator->setPhaseStartedCallback([panel_guard](const ai::WorkflowPhase& phase) {
        if (!panel_guard) {
            return;
        }
        QMetaObject::invokeMethod(
            panel_guard.data(),
            [panel_guard, phase]() {
                if (panel_guard) {
                    panel_guard->appendPhaseStartedToTranscript(phase);
                }
            },
            Qt::QueuedConnection);
    });
    orchestrator->setPhaseCompletedCallback([panel_guard](const ai::AiPhaseExecution& execution) {
        if (!panel_guard) {
            return;
        }
        QMetaObject::invokeMethod(
            panel_guard.data(),
            [panel_guard, execution]() {
                if (panel_guard) {
                    panel_guard->appendPhaseToTranscript(execution);
                }
            },
            Qt::QueuedConnection);
    });
}

void AiAssistantPanel::startAiRunTrace(const QString& message,
                                       const QString& model,
                                       const QString& preferred_run_id) {
    const QString requested_run_id = preferred_run_id.trimmed();
    m_currentRunId = requested_run_id.isEmpty() ? QStringLiteral("run_%1_%2")
                                                      .arg(QDateTime::currentDateTimeUtc().toString(
                                                               QStringLiteral("yyyyMMddHHmmsszzz")),
                                                           QUuid::createUuid()
                                                               .toString(QUuid::WithoutBraces)
                                                               .left(kShortTraceTokenIdChars))
                                                : requested_run_id;
    m_runToken = ai::CancellationToken::createRoot(m_currentRunId);
    m_runState = {};
    m_runState.run_id = m_currentRunId;
    m_runState.status = ai::AiRunStatus::Running;
    m_runState.message = message.left(kRunStateMessageMaxChars);
    clearWorkflowProgressUi();
    saveRunStateSnapshot();

    QJsonObject metadata;
    metadata[QStringLiteral("model")] = model;
    metadata[QStringLiteral("prompt_sha256")] = QString::fromLatin1(
        QCryptographicHash::hash(message.toUtf8(), QCryptographicHash::Sha256).toHex());
    metadata[QStringLiteral("message_chars")] = message.size();
    metadata[QStringLiteral("context_items")] = m_contextItems.size();
    metadata[QStringLiteral("access_mode")] = currentAccessModeLabel();
    if (!requested_run_id.isEmpty()) {
        metadata[QStringLiteral("resumed_run_id")] = true;
    }
    traceAiEvent(
        QStringLiteral("run"), QStringLiteral("ai_request"), QStringLiteral("started"), metadata);
}

void AiAssistantPanel::traceAiEvent(const QString& kind,
                                    const QString& name,
                                    const QString& status,
                                    const QJsonObject& metadata) {
    if (!m_traceStore) {
        return;
    }
    if (m_traceStore->sessionDirectory().isEmpty()) {
        refreshTraceStoreForSession();
    }

    const QString run_id = currentTraceRunId();
    if (run_id.isEmpty()) {
        return;
    }

    appendTraceEventRecord(run_id, kind, name, status, metadata);
}

QString AiAssistantPanel::currentTraceRunId() const {
    if (!m_currentRunId.isEmpty()) {
        return m_currentRunId;
    }
    if (!m_runState.run_id.isEmpty()) {
        return m_runState.run_id;
    }
    return m_conversationStore ? m_conversationStore->currentSessionId() : QString();
}

void AiAssistantPanel::appendTraceEventRecord(const QString& run_id,
                                              const QString& kind,
                                              const QString& name,
                                              const QString& status,
                                              const QJsonObject& metadata) {
    QString error;
    const auto event = ai::TraceStore::event({run_id, kind, name, status, metadata});
    if (!m_traceStore->appendEvent(event, &error) && !error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI trace write failed: %1").arg(error)));
    }
    appendTraceActivityRecord(event);
    QJsonObject replay_metadata = metadata;
    replay_metadata[QStringLiteral("trace_kind")] = kind;
    replay_metadata[QStringLiteral("trace_name")] = name;
    if (kind == QLatin1String("tool_call")) {
        replay_metadata[QStringLiteral("tool_name")] = name;
    }
    error.clear();
    if (!m_traceStore->appendReplayEvent(
            run_id, QStringLiteral("%1:%2").arg(kind, name), status, replay_metadata, &error) &&
        !error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI replay trace write failed: %1").arg(error)));
    }
}

void AiAssistantPanel::appendTraceActivityRecord(const ai::AiTraceEvent& event) {
    const QString session_id = m_conversationStore ? m_conversationStore->currentSessionId()
                                                   : QString();
    auto activity = ai::TraceStore::activityEvent(
        {session_id,
         event.run_id,
         event.kind,
         activityStateFromTrace(event.kind, event.status),
         activitySummaryFromTrace(event.name, event.status, event.metadata),
         event.metadata});
    activity.token_usage = event.token_usage;
    QString error;
    error.clear();
    if (!m_traceStore->appendActivityEvent(activity, &error) && !error.isEmpty()) {
        Q_EMIT logOutput(
            ai::CredentialStore::redactSecrets(tr("AI activity write failed: %1").arg(error)));
    }
}

void AiAssistantPanel::finishAiRunTrace(const QString& status, const QJsonObject& metadata) {
    if (m_currentRunId.isEmpty()) {
        return;
    }
    traceAiEvent(QStringLiteral("run"), QStringLiteral("ai_request"), status, metadata);
    m_runState.status = status == QLatin1String("completed")   ? ai::AiRunStatus::Completed
                        : status == QLatin1String("cancelled") ? ai::AiRunStatus::Cancelled
                        : status == QLatin1String("waiting_for_human")
                            ? ai::AiRunStatus::WaitingForHuman
                            : ai::AiRunStatus::Failed;
    saveRunStateSnapshot();
    if (ai::isTerminalRunStatus(m_runState.status)) {
        m_currentRunId.clear();
    }
}

void AiAssistantPanel::onAccessModeChanged(int index) {
    (void)index;
    updateAccessStatus();
    appendLocalEvent(tr("Access mode set to %1").arg(currentAccessModeLabel()));
    Q_EMIT statusMessage(tr("AI access mode: %1").arg(currentAccessModeLabel()),
                         sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onPromptTemplateSelected(int index) {
    if (!m_promptTemplateCombo || index <= 0) {
        return;
    }
    const QString title = m_promptTemplateCombo->itemText(index);
    const QString item_data = m_promptTemplateCombo->itemData(index).toString();
    if (m_workflowStore) {
        if (const auto* workflow = m_workflowStore->workflowById(item_data)) {
            applyWorkflowTemplate(*workflow);
            QSignalBlocker blocker(m_promptTemplateCombo);
            m_promptTemplateCombo->setCurrentIndex(0);
            if (m_workflowDetailsButton) {
                m_workflowDetailsButton->setEnabled(false);
            }
            if (m_addWorkflowButton) {
                m_addWorkflowButton->setEnabled(false);
            }
            hideWorkflowDetails();
            return;
        }
    }
    const QString prompt = item_data;
    applyPromptTemplate(title, prompt);
    QSignalBlocker blocker(m_promptTemplateCombo);
    m_promptTemplateCombo->setCurrentIndex(0);
    if (m_workflowDetailsButton) {
        m_workflowDetailsButton->setEnabled(false);
    }
    if (m_addWorkflowButton) {
        m_addWorkflowButton->setEnabled(false);
    }
    hideWorkflowDetails();
}

void AiAssistantPanel::onAddWorkflowClicked() {
    if (!m_promptTemplateCombo) {
        return;
    }
    onPromptTemplateSelected(m_promptTemplateCombo->currentIndex());
}

void AiAssistantPanel::onRunDetailsClicked() {
    showRunDetails();
}

void AiAssistantPanel::onResumeGateClicked() {
    if (!m_runState.has_pending_human_gate || !m_runState.pending_human_gate.isPending()) {
        Q_EMIT statusMessage(tr("No waiting AI run to resume"), sak::kTimerStatusDefaultMs);
        updateCredentialControls();
        return;
    }
    if (isAiBusy()) {
        Q_EMIT statusMessage(tr("AI is busy; resume after current work stops"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    if (!ai::OpenAIResponsesClient::hasUsableApiKey(apiKey())) {
        Q_EMIT statusMessage(tr("Load OpenAI API key before resuming workflow"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    const ai::AiHumanGate gate = m_runState.pending_human_gate;
    bool resumed = false;
    if (gate.kind == QLatin1String("workflow_input")) {
        resumed = resumeWorkflowInputGate(gate);
    } else if (gate.kind == QLatin1String("approval")) {
        resumed = resumeApprovalGate(gate);
    } else if (gate.kind == QLatin1String("workflow_recovery")) {
        resumed = resumeWorkflowRecoveryGate(gate);
    } else {
        sak::showInformationLogged(
            this,
            tr("AI Run Waiting"),
            tr("This waiting gate is not resumable yet. Open Run Details to review it."));
        showRunDetails();
    }
    if (!resumed) {
        updateCredentialControls();
    }
}

void AiAssistantPanel::onWorkflowDetailsClicked() {
    if (!m_promptTemplateCombo || !m_workflowStore) {
        return;
    }
    const int index = m_promptTemplateCombo->currentIndex();
    if (index <= 0) {
        Q_EMIT statusMessage(tr("Choose a workflow first"), sak::kTimerStatusDefaultMs);
        return;
    }

    const QString item_data = m_promptTemplateCombo->itemData(index).toString();
    const auto* workflow = m_workflowStore->workflowById(item_data);
    if (!workflow) {
        if (m_workflowDetailsPanel && m_workflowDetailsBody && m_workflowDetailsTitle) {
            m_workflowDetailsTitle->setText(m_promptTemplateCombo->itemText(index));
            m_workflowDetailsBody->setPlainText(
                tr("No structured details available for this entry."));
            m_workflowDetailsPanel->setVisible(true);
            m_workflowDetailsCurrentId.clear();
        }
        return;
    }
    showWorkflowDetails(*workflow);
}

QString AiAssistantPanel::runDetailsText() const {
    QStringList lines = runDetailsSummaryLines();
    lines << QString() << tr("Tool / Provider Health");
    lines << runDetailsHealthLines();
    lines << QString() << tr("Phase History");
    lines << runDetailsPhaseLines();
    lines << QString() << tr("Recent Activity");
    lines << runDetailsActivityLines();
    return lines.join(QLatin1Char('\n'));
}

QStringList AiAssistantPanel::runDetailsHealthLines() const {
    QStringList lines;
    lines << runDetailsToolHealthLines();
    lines << runDetailsProviderHealthLines();
    return lines;
}

QStringList AiAssistantPanel::runDetailsToolHealthLines() const {
    QStringList lines;
    if (m_toolHealthLedger) {
        const QJsonArray records =
            m_toolHealthLedger->snapshot().value(QStringLiteral("records")).toArray();
        if (records.isEmpty()) {
            lines << tr("_no tool/provider health records_");
        } else {
            QVector<QJsonObject> objects;
            objects.reserve(records.size());
            for (const auto& value : records) {
                objects.append(value.toObject());
            }
            std::sort(
                objects.begin(), objects.end(), [](const QJsonObject& lhs, const QJsonObject& rhs) {
                    const QString left = lhs.value(QStringLiteral("last_failure_utc")).toString() +
                                         lhs.value(QStringLiteral("last_success_utc")).toString();
                    const QString right = rhs.value(QStringLiteral("last_failure_utc")).toString() +
                                          rhs.value(QStringLiteral("last_success_utc")).toString();
                    return left > right;
                });
            const int limit = std::min(static_cast<int>(objects.size()), 12);
            for (int i = 0; i < limit; ++i) {
                lines << runDetailsHealthRecordLine(objects.at(i));
            }
        }
    } else {
        lines << tr("_tool health ledger unavailable_");
    }
    return lines;
}

QStringList AiAssistantPanel::runDetailsProviderHealthLines() const {
    QStringList lines;
    ai::AiProviderRegistry registry;
    QString provider_error;
    const QJsonArray providers =
        registry.providerStatuses(&provider_error).value(QStringLiteral("providers")).toArray();
    if (!provider_error.isEmpty()) {
        lines << tr("- provider registry error: %1").arg(provider_error);
    } else {
        for (const auto& value : providers) {
            const QJsonObject provider = value.toObject();
            const QString id = provider.value(QStringLiteral("id")).toString();
            const bool available = provider.value(QStringLiteral("available")).toBool(false);
            const QString reason = provider.value(QStringLiteral("missing_reason")).toString();
            lines << QStringLiteral("- provider %1: %2%3")
                         .arg(id,
                              available ? QStringLiteral("available")
                                        : QStringLiteral("unavailable"),
                              reason.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(reason));
        }
    }
    return lines;
}

QString AiAssistantPanel::runDetailsHealthRecordLine(const QJsonObject& record) const {
    const QString key = record.value(QStringLiteral("key")).toString();
    const QString failure_class = record.value(QStringLiteral("last_failure_class")).toString();
    const QString disabled_until = record.value(QStringLiteral("disabled_until_utc")).toString();
    const QString error = ai::CredentialStore::redactSecrets(
        record.value(QStringLiteral("last_error_message")).toString().left(180));
    QStringList tags;
    if (!failure_class.isEmpty()) {
        tags << QStringLiteral("failure=%1").arg(failure_class);
    }
    if (!disabled_until.isEmpty()) {
        tags << QStringLiteral("disabled_until=%1").arg(disabled_until);
    }
    tags << QStringLiteral("latency=%1ms")
                .arg(record.value(QStringLiteral("last_latency_ms")).toInt());
    if (!error.isEmpty()) {
        tags << QStringLiteral("error=%1").arg(error);
    }
    return QStringLiteral("- %1 [%2]").arg(key, tags.join(QStringLiteral(", ")));
}

QStringList AiAssistantPanel::runDetailsSummaryLines() const {
    QStringList lines;
    lines << tr("Run: %1").arg(m_runState.run_id.isEmpty() ? tr("none") : m_runState.run_id);
    lines << tr("Status: %1").arg(ai::runStatusToString(m_runState.status));
    lines << tr("Workflow: %1")
                 .arg(m_runState.workflow_id.isEmpty() ? tr("none") : m_runState.workflow_id);
    lines << tr("Phase: %1").arg(m_runState.phase_id.isEmpty() ? tr("none") : m_runState.phase_id);
    lines << tr("Agents: %1 active / %2 done")
                 .arg(m_runState.active_subagents)
                 .arg(m_runState.completed_subagents);
    lines << tr("Tools: %1 active / %2 done")
                 .arg(m_runState.active_tools)
                 .arg(m_runState.completed_tools);
    if (!m_runState.message.trimmed().isEmpty()) {
        lines << tr("Message: %1").arg(ai::CredentialStore::redactSecrets(m_runState.message));
    }
    if (m_runState.has_pending_human_gate) {
        const auto& gate = m_runState.pending_human_gate;
        lines << tr("Pending gate: %1/%2")
                     .arg(gate.kind.isEmpty() ? tr("gate") : gate.kind,
                          gate.name.isEmpty() ? tr("human") : gate.name);
        lines << tr("Question: %1").arg(ai::CredentialStore::redactSecrets(gate.question));
    }
    return lines;
}

QStringList AiAssistantPanel::runDetailsPhaseLines() const {
    QStringList lines;
    if (m_workflowPhaseHistory.isEmpty()) {
        lines << tr("_no phase history_");
        return lines;
    }
    const int start = std::max(0, static_cast<int>(m_workflowPhaseHistory.size()) - 12);
    for (int i = start; i < m_workflowPhaseHistory.size(); ++i) {
        const auto& phase = m_workflowPhaseHistory.at(i);
        const QString status = phase.skipped   ? QStringLiteral("skipped")
                               : phase.success ? QStringLiteral("ok")
                                               : QStringLiteral("failed");
        QStringList tags;
        if (!phase.agent_id.isEmpty()) {
            tags << QStringLiteral("agent=%1").arg(phase.agent_id);
        }
        if (!phase.phase_type.isEmpty()) {
            tags << QStringLiteral("type=%1").arg(phase.phase_type);
        }
        lines << QStringLiteral("- %1 %2 (%3 ms)%4")
                     .arg(phase.phase_id,
                          status,
                          QString::number(phase.duration_ms),
                          tags.isEmpty()
                              ? QString()
                              : QStringLiteral(" [%1]").arg(tags.join(QStringLiteral(", "))));
    }
    return lines;
}

QStringList AiAssistantPanel::runDetailsActivityLines() const {
    const QVector<ai::AiActivityEvent> filtered = filteredRunDetailsActivities();
    if (filtered.isEmpty()) {
        return {tr("_no activity events_")};
    }
    QStringList lines;
    const int start = std::max(0, static_cast<int>(filtered.size()) - 30);
    for (int i = start; i < filtered.size(); ++i) {
        lines << runDetailsActivityLine(filtered.at(i));
    }
    return lines;
}

QVector<ai::AiActivityEvent> AiAssistantPanel::filteredRunDetailsActivities() const {
    if (!m_traceStore || m_traceStore->sessionDirectory().isEmpty()) {
        return {};
    }
    const auto activities = m_traceStore->loadActivityEvents();
    QVector<ai::AiActivityEvent> filtered;
    const QString active_run_id = !m_runState.run_id.isEmpty() ? m_runState.run_id : m_currentRunId;
    for (const auto& activity : activities) {
        if (active_run_id.isEmpty() || activity.run_id.isEmpty() ||
            activity.run_id == active_run_id) {
            filtered.append(activity);
        }
    }
    return filtered.isEmpty() ? activities : filtered;
}

QString AiAssistantPanel::runDetailsActivityLine(const ai::AiActivityEvent& activity) const {
    const QString when = activity.timestamp_utc.isValid()
                             ? activity.timestamp_utc.toString(QStringLiteral("HH:mm:ss"))
                             : QStringLiteral("--:--:--");
    return QStringLiteral("- %1 %2 %3: %4")
        .arg(when,
             activity.state.isEmpty() ? QStringLiteral("unknown") : activity.state,
             activity.kind.isEmpty() ? QStringLiteral("activity") : activity.kind,
             ai::CredentialStore::redactSecrets(activity.summary));
}
QString AiAssistantPanel::runDetailsHtml() const {
    QString markdown = QStringLiteral("# Run Details\n\n````text\n%1\n````\n")
                           .arg(runDetailsText().toHtmlEscaped());
    markdown.replace(QStringLiteral("````"), QStringLiteral("```"));
    QTextDocument document;
    document.setDefaultStyleSheet(sak::ui::aiRunDetailsMarkdownStyle());
    document.setMarkdown(markdown);
    return document.toHtml();
}
void AiAssistantPanel::showRunDetails() {
    auto* dialog = new QDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(tr("AI Run Details"));
    dialog->setWindowModality(Qt::ApplicationModal);
    dialog->resize(kRunDetailsDialogWidth, kRunDetailsDialogHeight);

    auto* layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium,
                               sak::ui::kMarginMedium);
    layout->setSpacing(sak::ui::kSpacingSmall);

    auto* title = new QLabel(tr("Run Details"), dialog);
    title->setStyleSheet(sak::ui::fontSizeWeightColorStyle(
        sak::ui::kFontSizeSection, kFontWeightBold, sak::ui::kColorTextHeading));
    title->setToolTip(tr("Current or most recent AI run state"));
    setAccessible(title, tr("AI run details title"));
    layout->addWidget(title);

    auto* body = new QTextBrowser(dialog);
    body->setReadOnly(true);
    body->setOpenExternalLinks(false);
    body->setHtml(runDetailsHtml());
    body->setStyleSheet(sak::ui::aiRunDetailsBodyStyle());
    body->setToolTip(tr("Run summary, live phase timeline, pending gates, and recent activity"));
    setAccessible(body,
                  tr("AI run details timeline"),
                  tr("Run summary, workflow phase timeline, and activity events"));
    layout->addWidget(body, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, dialog);
    auto* refresh_button = buttons->addButton(tr("Refresh"), QDialogButtonBox::ActionRole);
    refresh_button->setToolTip(tr("Refresh this dialog with the latest run state"));
    connect(refresh_button, &QPushButton::clicked, this, [this, body]() {
        if (body) {
            body->setHtml(runDetailsHtml());
        }
    });
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::close);
    layout->addWidget(buttons);

    dialog->open();
}

void AiAssistantPanel::showWorkflowDetails(const ai::WorkflowTemplate& workflow) {
    if (!m_workflowDetailsPanel || !m_workflowDetailsBody || !m_workflowDetailsTitle) {
        return;
    }
    m_workflowDetailsCurrentId = workflow.id;
    m_workflowDetailsTitle->setText(workflow.title);
    m_workflowDetailsBody->setHtml(workflowDetailsHtml(workflow));
    m_workflowDetailsPanel->setVisible(true);
}
void AiAssistantPanel::hideWorkflowDetails() {
    if (m_workflowDetailsPanel) {
        m_workflowDetailsPanel->setVisible(false);
    }
    m_workflowDetailsCurrentId.clear();
}

void AiAssistantPanel::clearWorkflowProgressUi() {
    m_activeWorkflowPhaseOrder.clear();
    m_activeWorkflowTitle.clear();
    m_activeWorkflowCurrentPhase.clear();
    m_activeWorkflowPhaseStartCounts.clear();
    m_activeWorkflowCompletedPhaseCount = 0;
    if (m_workflowProgressBar) {
        m_workflowProgressBar->setVisible(false);
        m_workflowProgressBar->setRange(0, 1);
        m_workflowProgressBar->setValue(0);
        m_workflowProgressBar->setFormat(tr("Workflow idle"));
        m_workflowProgressBar->setToolTip(
            tr("Live workflow progress. Shows the active phase and completed phase count."));
    }
    Q_EMIT progressUpdate(1, 1);
}

int AiAssistantPanel::workflowCompletedPhaseCount() const {
    if (m_activeWorkflowPhaseOrder.isEmpty() || m_workflowPhaseHistory.isEmpty()) {
        return 0;
    }

    QSet<QString> completed_phase_ids;
    for (const auto& execution : m_workflowPhaseHistory) {
        if (!execution.phase_id.trimmed().isEmpty()) {
            completed_phase_ids.insert(execution.phase_id);
        }
    }

    int completed = 0;
    for (const auto& phase_id : m_activeWorkflowPhaseOrder) {
        if (completed_phase_ids.contains(phase_id)) {
            ++completed;
        }
    }
    return completed;
}

void AiAssistantPanel::beginWorkflowProgressUi(const ai::WorkflowTemplate& workflow,
                                               const QJsonObject& /*resume_state*/) {
    m_activeWorkflowPhaseOrder.clear();
    m_activeWorkflowTitle = workflow.title.trimmed().isEmpty() ? workflow.id : workflow.title;
    m_activeWorkflowCurrentPhase.clear();
    m_activeWorkflowPhaseStartCounts.clear();
    for (const auto& phase : workflow.phases) {
        if (!phase.id.trimmed().isEmpty()) {
            m_activeWorkflowPhaseOrder.append(phase.id);
        }
    }

    m_runState.active_subagents = 0;
    m_runState.active_tools = 0;
    m_runState.completed_subagents = 0;
    m_runState.completed_tools = 0;
    for (const auto& execution : m_workflowPhaseHistory) {
        if (!execution.ran || execution.skipped) {
            continue;
        }
        if (isWorkflowDelegatePhaseType(execution.phase_type)) {
            ++m_runState.completed_subagents;
        } else if (isWorkflowToolPhaseType(execution.phase_type)) {
            ++m_runState.completed_tools;
        }
    }
    m_activeWorkflowCompletedPhaseCount = workflowCompletedPhaseCount();
    updateWorkflowProgressUi();
}

void AiAssistantPanel::finishWorkflowProgressUi(const ai::AiOrchestratorResult& result) {
    m_activeWorkflowPhaseStartCounts.clear();
    if (result.status == ai::AiRunStatus::Completed) {
        m_activeWorkflowCompletedPhaseCount = m_activeWorkflowPhaseOrder.size();
    } else {
        m_activeWorkflowCompletedPhaseCount = workflowCompletedPhaseCount();
    }
    if (!result.phases.isEmpty()) {
        m_activeWorkflowCurrentPhase = result.phases.last().phase_id;
    }
    updateWorkflowProgressUi();
}

void AiAssistantPanel::updateWorkflowProgressUi() {
    const int total = m_activeWorkflowPhaseOrder.size();
    if (!m_workflowProgressBar || total <= 0) {
        if (m_workflowProgressBar) {
            m_workflowProgressBar->setVisible(false);
        }
        Q_EMIT progressUpdate(1, 1);
        return;
    }

    const bool has_active_phase = m_workflowRunActive &&
                                  !m_activeWorkflowPhaseStartCounts.isEmpty();
    const int completed = std::clamp(m_activeWorkflowCompletedPhaseCount, 0, total);
    const int maximum = std::max(1, total * 2);
    const int current_value = workflowProgressValue(completed, total, has_active_phase);
    const QString current_phase = m_activeWorkflowCurrentPhase.trimmed().isEmpty()
                                      ? tr("starting")
                                      : m_activeWorkflowCurrentPhase.trimmed();
    const QString format =
        workflowProgressFormat(completed, total, has_active_phase, current_phase);

    m_workflowProgressBar->setVisible(true);
    m_workflowProgressBar->setRange(0, maximum);
    m_workflowProgressBar->setValue(current_value);
    m_workflowProgressBar->setFormat(format);
    m_workflowProgressBar->setToolTip(
        tr("Workflow %1: %2 of %3 phases complete. Current phase: %4.")
            .arg(m_activeWorkflowTitle.isEmpty() ? m_runState.workflow_id : m_activeWorkflowTitle)
            .arg(completed)
            .arg(total)
            .arg(current_phase));
    Q_EMIT progressUpdate(current_value, maximum);
}

QString AiAssistantPanel::workflowProgressFormat(int completed,
                                                 int total,
                                                 bool has_active_phase,
                                                 const QString& current_phase) const {
    if (has_active_phase) {
        const int phase_index = m_activeWorkflowPhaseOrder.indexOf(current_phase);
        const int display_index = phase_index >= 0 ? phase_index + 1 : completed + 1;
        return tr("Running %1/%2: %3")
            .arg(std::clamp(display_index, 1, total))
            .arg(total)
            .arg(current_phase);
    }
    if (m_runState.status == ai::AiRunStatus::WaitingForHuman) {
        return tr("Waiting: %1/%2 complete").arg(completed).arg(total);
    }
    if (m_runState.status == ai::AiRunStatus::Cancelled) {
        return tr("Cancelled: %1/%2 complete").arg(completed).arg(total);
    }
    if (m_runState.status == ai::AiRunStatus::Failed) {
        return tr("Failed: %1/%2 complete").arg(completed).arg(total);
    }
    return completed >= total ? tr("Completed %1/%2").arg(completed).arg(total)
                              : tr("%1/%2 complete").arg(completed).arg(total);
}

int AiAssistantPanel::workflowProgressValue(int completed, int total, bool has_active_phase) const {
    const int maximum = std::max(1, total * 2);
    return std::clamp((completed * kProgressCompletedStepWeight) + (has_active_phase ? 1 : 0),
                      0,
                      maximum);
}

void AiAssistantPanel::rebuildWorkflowDetailsView() {
    if (m_workflowDetailsCurrentId.isEmpty() || !m_workflowStore) {
        return;
    }
    if (const auto* workflow = m_workflowStore->workflowById(m_workflowDetailsCurrentId)) {
        showWorkflowDetails(*workflow);
    } else {
        hideWorkflowDetails();
    }
}

void AiAssistantPanel::onSessionSelected(int index) {
    if (m_loadingSessionPicker || index < 0 || !m_sessionCombo || !m_conversationStore) {
        return;
    }

    const QString session_id = m_sessionCombo->itemData(index).toString();
    if (session_id.isEmpty() || session_id == m_conversationStore->currentSessionId()) {
        return;
    }

    QString error;
    if (!m_conversationStore->openSession(session_id, &error)) {
        appendLocalEvent(tr("Could not open AI session: %1").arg(error));
        return;
    }

    m_previousResponseId.clear();
    if (m_tokenTracker) {
        m_tokenTracker->reset();
        updateTokenLabels();
    }
    m_contextItems.clear();
    refreshContextList();
    refreshTraceStoreForSession();
    loadRunStateSnapshotForSession();
    traceAiEvent(QStringLiteral("session"), QStringLiteral("session"), QStringLiteral("opened"));
    loadSessionTranscript(session_id);
    scheduleContextTokenRefresh();
    appendLocalEvent(tr("Opened AI session: %1").arg(session_id));
}

void AiAssistantPanel::onRenameSessionClicked() {
    if (!m_conversationStore) {
        return;
    }
    const auto info = m_conversationStore->currentSessionInfo();
    bool accepted = false;
    const QString title =
        QInputDialog::getText(
            this, tr("Rename AI Chat"), tr("Chat name"), QLineEdit::Normal, info.title, &accepted)
            .trimmed();
    if (!accepted || title.isEmpty()) {
        return;
    }
    QString error;
    if (!m_conversationStore->renameCurrentSession(title, &error)) {
        appendLocalEvent(tr("AI session rename failed: %1").arg(error));
        Q_EMIT statusMessage(tr("AI chat rename failed"), sak::kTimerStatusDefaultMs);
        return;
    }
    appendSessionMemory(QStringLiteral("Session"),
                        QStringLiteral("Renamed"),
                        tr("Chat renamed to %1").arg(title));
    reloadSessionPicker();
    refreshArtifactList();
    appendLocalEvent(tr("AI session renamed: %1").arg(title));
    Q_EMIT statusMessage(tr("AI chat renamed"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onNewSessionClicked() {
    if (isAiBusy()) {
        Q_EMIT statusMessage(tr("Stop the active AI run before starting a new chat"),
                             sak::kTimerStatusDefaultMs);
        return;
    }
    const QString title = tr("AI Session");
    m_previousResponseId.clear();
    m_currentRunId.clear();
    m_pendingWorkflowRunId.clear();
    resetSessionRole();
    m_activeUserMessage.clear();
    m_activeWorkflowUserMessage.clear();
    m_activeWorkflowInputValues = {};
    m_workflowPhaseHistory.clear();
    m_activeWorkflowPhaseOrder.clear();
    m_activeWorkflowTitle.clear();
    m_activeWorkflowCurrentPhase.clear();
    m_activeWorkflowPhaseStartCounts.clear();
    m_activeWorkflowCompletedPhaseCount = 0;
    m_toolTurn.reset();
    m_pendingTurnToken = {};
    m_pendingCallToken = {};
    m_currentCommandPreview.clear();
    m_currentStdoutBuffer.clear();
    m_currentStderrBuffer.clear();
    m_pendingSteeringMessages.clear();
    m_queuedUserMessages.clear();
    m_resumedApprovedToolCallIds.clear();
    m_resumedRestoreToolCallIds.clear();
    m_citations.clear();
    m_toolCallsThisSession = 0;
    m_toolTurnIterations = 0;
    m_toolNamesThisMessage.clear();
    m_toolFailureClassesThisMessage.clear();
    m_runToken = {};
    m_runState = {};
    m_taskStatus = tr("Idle");
    resetContextTokenCount(ai::OpenAIResponsesClient::hasUsableApiKey(apiKey()) ? tr("pending")
                                                                                : tr("key needed"));
    if (m_tokenTracker) {
        m_tokenTracker->reset();
        updateTokenLabels();
    }
    if (m_messageEdit) {
        m_messageEdit->clear();
    }
    if (m_transcriptView) {
        m_transcriptView->clearMessages(false);
    }
    m_contextItems.clear();
    refreshContextList();
    m_pendingSessionTitle = title;
    if (m_conversationStore) {
        m_conversationStore->clearCurrentSession();
    }
    refreshTraceStoreForSession();
    saveRunStateSnapshot();
    updateWorkflowProgressUi();
    reloadSessionPicker();
    refreshArtifactList();
    updateCredentialControls();
    emitStatusDetails();
    appendLocalEvent(tr("New AI chat workspace ready as %1; it will auto-rename after first prompt")
                         .arg(m_pendingSessionTitle));
    Q_EMIT statusMessage(tr("AI chat workspace ready"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onLoadApiKeyClicked() {
    if (ai::OpenAIResponsesClient::hasUsableApiKey(apiKey())) {
        const auto choice =
            sak::showQuestionLogged(this,
                                    tr("Clear OpenAI API Key"),
                                    tr("Clear the loaded OpenAI API key from this session and the "
                                       "encrypted app credential file?"),
                                    QMessageBox::Yes | QMessageBox::No,
                                    QMessageBox::No);
        if (choice != QMessageBox::Yes) {
            return;
        }

        QString error;
        const bool deleted = !m_credentialStore || m_credentialStore->deleteApiKey(&error);
        m_apiKey.clear();
        if (deleted) {
            resetContextTokenCount(tr("key needed"));
            setApiKeyStatus(tr("Not loaded"),
                            sak::ui::kStatusColorError,
                            QString(QChar(kStatusMarkerError)),
                            sak::ui::kStatusColorError);
            appendLocalEvent(tr("OpenAI API key cleared"));
            Q_EMIT statusMessage(tr("OpenAI API key cleared"), sak::kTimerStatusDefaultMs);
        } else {
            setApiKeyStatus(tr("Credential clear failed"),
                            sak::ui::kStatusColorError,
                            QString(QChar(kStatusMarkerError)),
                            sak::ui::kStatusColorError);
            appendLocalEvent(tr("Credential clear failed: %1").arg(error));
            Q_EMIT statusMessage(tr("OpenAI credential clear failed"), sak::kTimerStatusDefaultMs);
        }
        updateCredentialControls();
        return;
    }

    bool accepted = false;
    const QString key = QInputDialog::getText(this,
                                              tr("Load OpenAI API Key"),
                                              tr("OpenAI API Key"),
                                              QLineEdit::Password,
                                              QString(),
                                              &accepted)
                            .trimmed();
    if (!accepted) {
        return;
    }
    if (key.isEmpty()) {
        m_apiKey.clear();
        resetContextTokenCount(tr("key needed"));
        setApiKeyStatus(tr("Not loaded"),
                        sak::ui::kStatusColorError,
                        QString(QChar(kStatusMarkerError)),
                        sak::ui::kStatusColorError);
        updateCredentialControls();
        return;
    }

    m_apiKey = key;
    resetContextTokenCount(tr("counting"));
    setApiKeyStatus(tr("Key loaded; testing"),
                    sak::ui::kStatusColorWarning,
                    QStringLiteral("..."),
                    sak::ui::kStatusColorWarning);
    updateCredentialControls();
    appendLocalEvent(tr("OpenAI API key loaded into memory"));
    m_client->listModels(apiKey());
}

void AiAssistantPanel::onAddContextFilesClicked() {
    const QStringList paths = QFileDialog::getOpenFileNames(
        this,
        tr("Add context files"),
        QString(),
        tr("Context files (*.png *.jpg *.jpeg *.webp *.gif *.pdf *.txt *.md *.log *.json *.csv "
           "*.xml *.ini *.ps1 *.bat *.cmd *.doc *.docx);;All files (*.*)"));
    for (const auto& path : paths) {
        addContextFile(path);
    }
    if (!paths.isEmpty()) {
        Q_EMIT statusMessage(tr("Context files added"), sak::kTimerStatusDefaultMs);
    }
}

void AiAssistantPanel::onAddInstructionContextClicked() {
    const QStringList paths =
        QFileDialog::getOpenFileNames(this,
                                      tr("Add instruction markdown"),
                                      QString(),
                                      tr("Markdown instructions (*.md);;All files (*.*)"));
    for (const auto& path : paths) {
        addInstructionFile(path);
    }
    if (!paths.isEmpty()) {
        Q_EMIT statusMessage(tr("Instruction file(s) added"), sak::kTimerStatusDefaultMs);
    }
}

void AiAssistantPanel::onClearContextClicked() {
    m_contextItems.clear();
    refreshContextList();
    appendLocalEvent(tr("Cleared AI context"));
    Q_EMIT statusMessage(tr("AI context cleared"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onSendClicked() {
    const QString message = messageText();
    if (message.isEmpty()) {
        return;
    }
    recordPromptHistory(message);

    if (handleBusySendPrompt(message)) {
        return;
    }
    m_toolTurnIterations = 0;
    m_toolNamesThisMessage.clear();
    m_toolFailureClassesThisMessage.clear();
    updateSessionRoleForPrompt(message);

    if (startAttachedWorkflowFromPrompt(message) == WorkflowSendResult::Handled) {
        return;
    }
    appendUserTurn(message);
    m_messageEdit->clear();
    startChatRequest(message);
}

bool AiAssistantPanel::handleBusySendPrompt(const QString& message) {
    if (!isAiBusy()) {
        return false;
    }
    switch (askBusyPromptAction(message)) {
    case BusyPromptAction::ApplySteering:
        applySteeringMessage(message);
        break;
    case BusyPromptAction::QueueAfterRun:
        queuePromptForLater(message, QStringLiteral("Queued during active run"));
        break;
    case BusyPromptAction::CancelAndQueue:
        queuePromptForLater(message, QStringLiteral("Queued after stopping active run"));
        onStopClicked();
        break;
    case BusyPromptAction::Discard:
        if (m_messageEdit) {
            m_messageEdit->clear();
        }
        Q_EMIT statusMessage(tr("AI request discarded"), sak::kTimerStatusDefaultMs);
        updateCredentialControls();
        break;
    }
    return true;
}

AiAssistantPanel::WorkflowSendResult AiAssistantPanel::startAttachedWorkflowFromPrompt(
    const QString& message) {
    const ai::WorkflowTemplate* workflow = attachedWorkflow();
    if (!workflow) {
        return WorkflowSendResult::NoWorkflow;
    }
    QJsonObject workflow_inputs;
    if (!ensureWorkflowInputs(*workflow, message, &workflow_inputs)) {
        Q_EMIT statusMessage(tr("Workflow needs required input before start"),
                             sak::kTimerStatusDefaultMs);
        updateCredentialControls();
        return WorkflowSendResult::Handled;
    }
    const QString workflow_run_id = m_pendingWorkflowRunId;
    appendUserTurn(message, workflow->id, workflow_inputs);
    appendSessionMemory(QStringLiteral("User"), QStringLiteral("Workflow request"), message);
    m_messageEdit->clear();
    runWorkflowAsync(*workflow, message, workflow_inputs, workflow_run_id);
    return WorkflowSendResult::Handled;
}

void AiAssistantPanel::appendUserTurn(const QString& message,
                                      const QString& workflow_id,
                                      const QJsonObject& workflow_inputs) {
    const bool creating_session = m_conversationStore &&
                                  m_conversationStore->currentSessionId().isEmpty();
    if (creating_session) {
        (void)ensurePersistentSession(m_pendingSessionTitle);
        autoRenameDefaultChatFromFirstPrompt(message, workflow_id);
        persistSessionRoleChoice();
        for (const auto& item : std::as_const(m_contextItems)) {
            persistContextItem(item);
        }
    }
    if (m_transcriptView) {
        QString user_block = message;
        if (!m_contextItems.isEmpty()) {
            user_block += tr("\n\nContext attached: %1 item(s)").arg(m_contextItems.size());
        }
        appendTranscriptMessage(QStringLiteral("You"), user_block, true);
    }
    if (m_conversationStore) {
        QJsonObject metadata;
        metadata[QStringLiteral("context_items")] = m_contextItems.size();
        metadata[QStringLiteral("session_role")] = currentWorkflowRole();
        metadata[QStringLiteral("session_role_source")] = m_sessionRoleSource;
        if (!workflow_id.isEmpty()) {
            metadata[QStringLiteral("workflow_id")] = workflow_id;
            metadata[QStringLiteral("workflow_inputs")] = workflow_inputs;
        }
        QString error;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("You"), message, metadata, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
        reloadSessionPicker();
    }
}

ai::OpenAIResponseRequest AiAssistantPanel::buildChatRequest(const QString& message) const {
    ai::OpenAIResponseRequest request;
    request.api_key = apiKey();
    request.model = m_modelCombo->currentText().trimmed();
    request.instructions = buildInstructions();
    request.input = message;
    request.attachments = buildContextAttachments();
    request.reasoning_effort = m_reasoningEffortCombo->currentText().trimmed().toLower();
    request.previous_response_id = m_previousResponseId;
    if (m_conversationStore) {
        request.safety_identifier =
            safetyIdentifierFromSeed(m_conversationStore->currentSessionId());
    }
    request.enable_web_search = true;
    request.enable_local_tools = currentAccessMode() != AccessMode::ChatAndResearch;
    return request;
}

void AiAssistantPanel::startChatRequest(const QString& message) {
    appendSessionMemory(QStringLiteral("User"), QStringLiteral("Chat request"), message);
    m_activeUserMessage = message;
    const ai::OpenAIResponseRequest request = buildChatRequest(message);
    startAiRunTrace(message, request.model);
    appendLocalEvent(tr("OpenAI response requested with model %1").arg(request.model));
    m_client->createResponse(request);
}

void AiAssistantPanel::onStopClicked() {
    appendLocalEvent(tr("Stop requested"));
    cancelActiveRunToken();
    cancelLocalAiWork();
    finalizeStopRequest();
}

void AiAssistantPanel::cancelActiveRunToken() {
    if (m_runToken.isValid()) {
        m_runToken.cancel(QStringLiteral("user_cancelled"));
        QJsonObject metadata;
        metadata[QStringLiteral("token")] = m_runToken.toJson();
        finishAiRunTrace(QStringLiteral("cancelled"), metadata);
    }
}

void AiAssistantPanel::cancelLocalAiWork() {
    if (m_executionBroker && m_executionBroker->isRunning()) {
        m_executionBroker->cancel();
    }
    if (m_offlineWorker && m_offlineWorker->isRunning()) {
        m_offlineWorker->cancel();
    }
    m_client->cancel();
    if (m_toolTurn.active()) {
        resetPendingToolTurn();
    }
    m_activeUserMessage.clear();
    m_pendingSteeringMessages.clear();
}

void AiAssistantPanel::finalizeStopRequest() {
    const bool still_busy = isAiBusy();
    m_runState.status = still_busy ? ai::AiRunStatus::Cancelling : ai::AiRunStatus::Cancelled;
    m_taskStatus = still_busy ? tr("Cancelling") : tr("Cancelled");
    updateWorkflowProgressUi();
    emitStatusDetails();
    setActivityIndicator(still_busy ? tr("Cancelling") : QString(), still_busy);
    Q_EMIT statusMessage(tr("AI request cancelled"), sak::kTimerStatusDefaultMs);
    if (!still_busy) {
        dispatchQueuedPromptIfIdle();
    }
}

void AiAssistantPanel::onBrokerStarted(const QString& command_id) {
    m_taskStatus = tr("Running %1").arg(command_id);
    emitStatusDetails();
    setActivityIndicator(tr("Running tool"), true);
    QJsonObject metadata;
    metadata[QStringLiteral("command_id")] = command_id;
    metadata[QStringLiteral("preview")] = m_currentCommandPreview.left(kDefaultPreviewMaxChars);
    traceAiEvent(QStringLiteral("tool_call"),
                 QStringLiteral("local_command"),
                 QStringLiteral("running"),
                 metadata);
}

void AiAssistantPanel::onBrokerStdoutChunk(const QString& command_id, const QString& chunk) {
    m_currentStdoutBuffer.append(chunk);
    emitPrefixedLogLines(
        [this](const QString& line) { Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line)); },
        QStringLiteral("[%1]").arg(command_id),
        chunk);
}

void AiAssistantPanel::onBrokerStderrChunk(const QString& command_id, const QString& chunk) {
    m_currentStderrBuffer.append(chunk);
    emitPrefixedLogLines(
        [this](const QString& line) { Q_EMIT logOutput(ai::CredentialStore::redactSecrets(line)); },
        QStringLiteral("[%1 err]").arg(command_id),
        chunk);
}

void AiAssistantPanel::onBrokerFinished(const QString& command_id,
                                        const ai::AiCommandResult& result) {
    releaseCurrentCommandLease();
    m_taskStatus = tr("Idle");
    emitStatusDetails();
    appendLocalEvent(tr("Command %1 finished (exit %2%3)")
                         .arg(command_id)
                         .arg(result.exit_code)
                         .arg(result.cancelled   ? tr(", cancelled")
                              : result.timed_out ? tr(", timed out")
                                                 : QString()));

    const QJsonObject result_json = brokerResultJson(command_id, result);
    QJsonObject trace_metadata = result_json;
    trace_metadata[QStringLiteral("command_id")] = command_id;
    traceAiEvent(QStringLiteral("tool_call"),
                 QStringLiteral("local_command"),
                 result.cancelled        ? QStringLiteral("cancelled")
                 : result.timed_out      ? QStringLiteral("timed_out")
                 : result.exit_code == 0 ? QStringLiteral("completed")
                                         : QStringLiteral("failed"),
                 trace_metadata);
    m_currentStdoutBuffer.clear();
    m_currentStderrBuffer.clear();
    recordBrokerResult(command_id, result, result_json);
    appendBrokerResultToTranscript(result_json, trace_metadata);
    appendSessionMemory(QStringLiteral("Tool"),
                        QStringLiteral("Command finished"),
                        tr("%1 exit=%2 cancelled=%3 timed_out=%4")
                            .arg(m_currentCommandPreview.isEmpty()
                                     ? command_id
                                     : m_currentCommandPreview.left(kDefaultPreviewMaxChars))
                            .arg(result.exit_code)
                            .arg(result.cancelled)
                            .arg(result.timed_out));
    m_currentCommandPreview.clear();

    if (!m_toolTurn.active()) {
        if (!isAiBusy()) {
            setUiBusy(false);
            dispatchQueuedPromptIfIdle();
        }
        updateCredentialControls();
        return;
    }

    completeBrokerToolTurn(result_json);
}

void AiAssistantPanel::releaseCurrentCommandLease() {
    if (!m_currentCommandLeaseId.isEmpty() && m_leaseManager) {
        m_leaseManager->release(m_currentCommandLeaseId);
        m_currentCommandLeaseId.clear();
    }
}

QJsonObject AiAssistantPanel::brokerResultJson(const QString& command_id,
                                               const ai::AiCommandResult& result) const {
    QJsonObject result_json = result.toJson();
    result_json[QStringLiteral("command_id")] = command_id;
    if (!m_currentCommandPreview.isEmpty()) {
        result_json[QStringLiteral("preview")] = ai::CredentialStore::redactSecrets(
            m_currentCommandPreview.left(kCommandResultPreviewMaxChars));
    }
    return result_json;
}

void AiAssistantPanel::recordBrokerResult(const QString& command_id,
                                          const ai::AiCommandResult& result,
                                          const QJsonObject& result_json) {
    ai::AiToolResultRecordRequest request;
    request.command_id = command_id;
    request.command_preview = m_currentCommandPreview;
    request.result_json = result_json;
    request.record_transcript = false;
    const auto record = ai::AiToolResultRecorder::record(m_conversationStore.get(), request);
    for (const auto& error : record.errors) {
        appendLocalEvent(error);
    }
    if (record.wroteStore() && m_conversationStore) {
        reloadSessionPicker();
    }
    Q_UNUSED(result);
}

void AiAssistantPanel::appendBrokerResultToTranscript(const QJsonObject& result_json,
                                                      const QJsonObject& trace_metadata) {
    ai::AiToolResultRecordRequest request;
    request.command_id = result_json.value(QStringLiteral("command_id")).toString();
    request.command_preview = m_currentCommandPreview;
    request.result_json = result_json;
    request.transcript_metadata = trace_metadata;
    request.record_command = false;
    const auto record = ai::AiToolResultRecorder::record(m_conversationStore.get(),
                                                         request,
                                                         ai::CredentialStore::redactSecrets);
    if (!record.transcript_text.isEmpty()) {
        if (m_transcriptView) {
            appendTranscriptMessage(QStringLiteral("Tool Result"), record.transcript_text);
        }
    }
    for (const auto& error : record.errors) {
        appendLocalEvent(error);
    }
    if (record.wroteStore() && m_conversationStore) {
        reloadSessionPicker();
    }
}

void AiAssistantPanel::completeBrokerToolTurn(const QJsonObject& result_json) {
    ai::OpenAIFunctionOutput output;
    output.call_id = m_toolTurn.currentCallId();
    output.output = QString::fromUtf8(QJsonDocument(result_json).toJson(QJsonDocument::Compact));
    appendToolOutputAndContinue(std::move(output));
}

void AiAssistantPanel::onRequestStarted() {
    setUiBusy(true);
    setActivityIndicator(tr("Thinking"), true);
    traceAiEvent(QStringLiteral("model_call"),
                 QStringLiteral("responses.create"),
                 QStringLiteral("started"));
    Q_EMIT statusMessage(tr("AI request started"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onRequestFinished() {
    traceAiEvent(QStringLiteral("model_call"),
                 QStringLiteral("responses.create"),
                 QStringLiteral("finished"));
    if (isAiBusy()) {
        if (m_toolTurn.active()) {
            setActivityIndicator(tr("Using tools"), true);
        }
    } else {
        setUiBusy(false);
        dispatchQueuedPromptIfIdle();
    }
    updateCredentialControls();
}

void AiAssistantPanel::onResponseReady(const ai::OpenAIResponseResult& result) {
    m_previousResponseId = result.id;
    m_tokenTracker->addTurn(result.usage);
    updateTokenLabels();
    QJsonObject response_metadata;
    response_metadata[QStringLiteral("response_id")] = result.id;
    response_metadata[QStringLiteral("function_calls")] = result.function_calls.size();
    response_metadata[QStringLiteral("citations")] = result.citations.size();
    response_metadata[QStringLiteral("total_tokens")] =
        static_cast<double>(result.usage.total_tokens);
    traceAiEvent(QStringLiteral("model_call"),
                 QStringLiteral("responses.create"),
                 QStringLiteral("completed"),
                 response_metadata);
    if (!result.function_calls.isEmpty()) {
        handleResponseToolCalls(result, response_metadata);
        return;
    }
    handleAssistantResponse(result, response_metadata);
}

void AiAssistantPanel::handleResponseToolCalls(const ai::OpenAIResponseResult& result,
                                               const QJsonObject& response_metadata) {
    ++m_toolTurnIterations;
    if (m_toolTurnIterations > kMaxToolTurnsPerUserMessage) {
        const QString warn =
            tr("Tool-loop iteration cap (%1) reached for this message. Halting tool calls. %2")
                .arg(kMaxToolTurnsPerUserMessage)
                .arg(toolLoopCapSummary());
        appendLocalEvent(warn);
        if (m_transcriptView) {
            appendTranscriptMessage(QStringLiteral("System"), warn, true);
        }
        if (m_conversationStore) {
            QString error;
            (void)m_conversationStore->appendTranscript(QStringLiteral("System"), warn, {}, &error);
        }
        Q_EMIT statusMessage(warn, sak::kTimerStatusDefaultMs);
        QJsonObject metadata = response_metadata;
        metadata[QStringLiteral("error_message")] = warn;
        metadata[QStringLiteral("tool_loop_cap")] = kMaxToolTurnsPerUserMessage;
        metadata[QStringLiteral("tool_loop_summary")] = toolLoopCapSummary();
        finishAiRunTrace(QStringLiteral("failed"), metadata);
        m_activeUserMessage.clear();
        m_pendingSteeringMessages.clear();
        saveRunStateSnapshot();
        return;
    }
    m_toolCallsThisSession += result.function_calls.size();
    m_runState.active_tools += result.function_calls.size();
    updateTokenLabels();
    saveRunStateSnapshot();
    traceAiEvent(QStringLiteral("tool_queue"),
                 QStringLiteral("local_tools"),
                 QStringLiteral("queued"),
                 response_metadata);
    appendLocalEvent(
        tr("OpenAI requested %1 local tool call(s) (iteration %2/%3, session total %4)")
            .arg(result.function_calls.size())
            .arg(m_toolTurnIterations)
            .arg(kMaxToolTurnsPerUserMessage)
            .arg(m_toolCallsThisSession));
    beginToolTurn(result);
    setActivityIndicator(tr("Using tools"), true);
}

void AiAssistantPanel::handleAssistantResponse(const ai::OpenAIResponseResult& result,
                                               const QJsonObject& response_metadata) {
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Assistant"), assistantTextWithCitations(result));
    }
    appendCitationsToList(result.citations);
    const QJsonObject assistant_metadata = assistantResponseMetadata(result);
    persistAssistantResponse(result, assistant_metadata);
    appendSessionMemory(QStringLiteral("Assistant"),
                        QStringLiteral("Response"),
                        result.output_text.left(kOpenAiResponsePreviewMaxChars));
    appendLocalEvent(tr("OpenAI response complete (%1 tokens, %2 sources)")
                         .arg(result.usage.total_tokens)
                         .arg(result.citations.size()));
    finishAiRunTrace(QStringLiteral("completed"), response_metadata);
    m_activeUserMessage.clear();
    m_pendingSteeringMessages.clear();
    Q_EMIT statusMessage(tr("AI response complete"), sak::kTimerStatusDefaultMs);
}

QString AiAssistantPanel::assistantTextWithCitations(const ai::OpenAIResponseResult& result) const {
    if (result.citations.isEmpty()) {
        return result.output_text;
    }
    QStringList lines;
    lines << tr("Sources:");
    for (int i = 0; i < result.citations.size(); ++i) {
        const auto& citation = result.citations.at(i);
        const QString display = citation.title.isEmpty() ? citation.url : citation.title;
        lines << QStringLiteral("  [%1] %2 - %3").arg(i + 1).arg(display, citation.url);
    }
    return result.output_text + QStringLiteral("\n\n") + lines.join(QLatin1Char('\n'));
}

QJsonObject AiAssistantPanel::assistantResponseMetadata(
    const ai::OpenAIResponseResult& result) const {
    QJsonObject assistant_metadata;
    if (!result.id.trimmed().isEmpty()) {
        assistant_metadata[QStringLiteral("response_id")] = result.id.trimmed();
    }
    assistant_metadata[QStringLiteral("total_tokens")] =
        static_cast<double>(result.usage.total_tokens);
    if (!result.citations.isEmpty()) {
        QJsonArray citation_array;
        for (const auto& citation : result.citations) {
            QJsonObject obj;
            obj[QStringLiteral("url")] = citation.url;
            obj[QStringLiteral("title")] = citation.title;
            obj[QStringLiteral("start_index")] = citation.start_index;
            obj[QStringLiteral("end_index")] = citation.end_index;
            citation_array.append(obj);
        }
        assistant_metadata[QStringLiteral("citations")] = citation_array;
    }
    return assistant_metadata;
}

void AiAssistantPanel::persistAssistantResponse(const ai::OpenAIResponseResult& result,
                                                const QJsonObject& assistant_metadata) {
    if (m_conversationStore) {
        QString error;
        (void)m_conversationStore->appendTranscript(
            QStringLiteral("Assistant"), result.output_text, assistant_metadata, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
        error.clear();
        (void)m_conversationStore->writeUsage(m_tokenTracker->lastTurn(),
                                              m_tokenTracker->sessionTotal(),
                                              &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Usage log failed: %1").arg(error));
        }
        reloadSessionPicker();
    }
}

void AiAssistantPanel::onModelsReady(const QStringList& model_ids) {
    const QString current = m_modelCombo->currentText();
    for (const auto& model_id : model_ids) {
        if (m_modelCombo->findText(model_id) < 0) {
            m_modelCombo->addItem(model_id);
        }
    }
    const int current_index = m_modelCombo->findText(current);
    if (current_index >= 0) {
        m_modelCombo->setCurrentIndex(current_index);
    }
    updateRunTelemetryLabels();

    QString save_error;
    const bool saved = m_credentialStore && m_credentialStore->saveApiKey(apiKey(), &save_error);
    setApiKeyStatus(saved ? tr("Key valid and saved") : tr("Key valid"),
                    sak::ui::kStatusColorSuccess,
                    QString(QChar(kStatusMarkerSuccess)),
                    sak::ui::kStatusColorSuccess);
    if (!saved && !save_error.isEmpty()) {
        appendLocalEvent(tr("OpenAI key valid but credential save failed: %1").arg(save_error));
    }
    appendLocalEvent(tr("Loaded %1 OpenAI models").arg(model_ids.size()));
    Q_EMIT statusMessage(tr("OpenAI key valid; models loaded"), sak::kTimerStatusDefaultMs);
}

void AiAssistantPanel::onInputTokenCountReady(const QString& request_id, qint64 input_tokens) {
    if (request_id != m_contextTokenRequestId) {
        return;
    }
    m_contextInputTokens = input_tokens;
    m_contextTokenStatus = tr("exact");
    m_contextTokenRequestId.clear();
    updateRunTelemetryLabels();
    emitStatusDetails();
}

void AiAssistantPanel::onInputTokenCountFailed(const QString& request_id,
                                               const QString& error_message) {
    if (request_id != m_contextTokenRequestId) {
        return;
    }
    m_contextInputTokens = -1;
    m_contextTokenStatus = tr("unavailable");
    m_contextTokenRequestId.clear();
    updateRunTelemetryLabels();
    emitStatusDetails();
    appendLocalEvent(
        tr("OpenAI context token count unavailable: %1")
            .arg(ai::CredentialStore::redactSecrets(error_message).left(kMetadataPreviewMaxChars)));
}

void AiAssistantPanel::onRequestFailed(const QString& error_message) {
    const QString redacted = ai::CredentialStore::redactSecrets(error_message);
    if (m_transcriptView) {
        appendTranscriptMessage(QStringLiteral("Error"), redacted, true);
    }
    if (m_conversationStore) {
        QString error;
        (void)m_conversationStore->appendTranscript(QStringLiteral("Error"), redacted, {}, &error);
        if (!error.isEmpty()) {
            appendLocalEvent(tr("Transcript log failed: %1").arg(error));
        }
        reloadSessionPicker();
    }
    setApiKeyStatus(tr("Request failed"),
                    sak::ui::kStatusColorError,
                    QString(QChar(kStatusMarkerError)),
                    sak::ui::kStatusColorError);
    m_taskStatus = tr("Request failed");
    emitStatusDetails();
    appendLocalEvent(tr("AI request failed: %1").arg(redacted));
    appendSessionMemory(QStringLiteral("Error"), QStringLiteral("AI request failed"), redacted);
    QJsonObject metadata;
    metadata[QStringLiteral("error")] = redacted.left(kMetadataPreviewMaxChars);
    finishAiRunTrace(QStringLiteral("failed"), metadata);
    m_activeUserMessage.clear();
    m_pendingSteeringMessages.clear();
    Q_EMIT statusMessage(tr("AI request failed"), sak::kTimerStatusDefaultMs);
}

}  // namespace sak
