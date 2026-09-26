// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ai_assistant_panel_providers.cpp
/// @brief AI Assistant panel: model provider selection, API-key vs.
/// subscription sign-in, and agent runtime permission prompts.

#include "sak/ai/ai_conversation_store.h"
#include "sak/ai_assistant_panel.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>

namespace sak {

namespace {

constexpr ushort kProviderMarkerSuccess = 0x2714;
constexpr ushort kProviderMarkerError = 0x2718;
constexpr auto kProviderSettingKey = "ai/provider";
constexpr auto kAuthModeSettingKey = "ai/auth_mode";
constexpr auto kMuseInstallCommand = "irm https://dev.meta.ai/install.ps1 | iex";

QString accountSummary(const ai::AiAccountStatus& status) {
    QString summary = status.account_label.trimmed();
    if (!status.plan.trimmed().isEmpty()) {
        summary = summary.isEmpty() ? status.plan.trimmed()
                                    : QStringLiteral("%1 (%2)").arg(summary, status.plan.trimmed());
    }
    return summary;
}

}  // namespace

ai::ModelProviderId AiAssistantPanel::currentProvider() const {
    if (!m_providerCombo) {
        return ai::ModelProviderId::OpenAI;
    }
    return ai::modelProviderFromKey(m_providerCombo->currentData().toString())
        .value_or(ai::ModelProviderId::OpenAI);
}

ai::ModelAuthMode AiAssistantPanel::currentAuthMode() const {
    if (!m_authModeCombo) {
        return ai::ModelAuthMode::ApiKey;
    }
    return ai::modelAuthModeFromKey(m_authModeCombo->currentData().toString())
        .value_or(ai::ModelAuthMode::ApiKey);
}

QString AiAssistantPanel::currentProviderLabel() const {
    const ai::ModelProviderInfo& info = ai::modelProviderInfo(currentProvider());
    return currentAuthMode() == ai::ModelAuthMode::Subscription
               ? tr("%1 via %2").arg(info.model_family, info.agent_name)
               : info.display_name;
}

bool AiAssistantPanel::hasModelCredential() const {
    if (currentAuthMode() == ai::ModelAuthMode::ApiKey) {
        return ai::hasUsableApiKey(currentProvider(), apiKey());
    }
    return m_accountStatus.provider == currentProvider() &&
           m_accountStatus.auth_mode == ai::ModelAuthMode::Subscription &&
           m_accountStatus.runtime_available && m_accountStatus.signed_in;
}

QString AiAssistantPanel::credentialStatusText() const {
    const bool subscription = currentAuthMode() == ai::ModelAuthMode::Subscription;
    if (hasModelCredential()) {
        return subscription ? tr("Signed in") : tr("Loaded");
    }
    return subscription ? tr("Signed out") : tr("No key");
}

bool AiAssistantPanel::workflowsAllowedForSelection(QString* reason) const {
    // Anthropic's conditions for hosting Claude Code do not cover third-party
    // multi-agent products running on a user's Claude plan.
    const bool claude_plan = currentProvider() == ai::ModelProviderId::Anthropic &&
                             currentAuthMode() == ai::ModelAuthMode::Subscription;
    if (claude_plan && reason) {
        *reason = tr(
            "Multi-agent workflows cannot run on a Claude subscription. Use a Claude API "
            "key, or another provider, to run workflows.");
    }
    return !claude_plan;
}

void AiAssistantPanel::connectModelProviderSignals() {
    connect(m_client.get(),
            &ai::AiChatBackend::accountStatusChanged,
            this,
            &AiAssistantPanel::onAccountStatusChanged);
    connect(m_client.get(),
            &ai::AiChatBackend::signInUrlReady,
            this,
            &AiAssistantPanel::onSignInUrlReady);
    connect(m_client.get(),
            &ai::AiChatBackend::approvalRequested,
            this,
            &AiAssistantPanel::onAgentApprovalRequested);
    connect(m_client.get(), &ai::AiChatBackend::activityText, this, [this](const QString& text) {
        if (!text.trimmed().isEmpty()) {
            appendLocalEvent(text.trimmed());
        }
    });
}

void AiAssistantPanel::restoreProviderSelection() {
    if (!m_providerCombo || !m_authModeCombo) {
        loadRememberedApiKey();
        return;
    }
    const QSettings settings;
    const QString provider = settings.value(QString::fromLatin1(kProviderSettingKey)).toString();
    const QString mode = settings.value(QString::fromLatin1(kAuthModeSettingKey)).toString();
    {
        const QSignalBlocker provider_blocker(m_providerCombo);
        const QSignalBlocker mode_blocker(m_authModeCombo);
        const int provider_index = m_providerCombo->findData(provider);
        const int mode_index = m_authModeCombo->findData(mode);
        m_providerCombo->setCurrentIndex(provider_index >= 0 ? provider_index : 0);
        m_authModeCombo->setCurrentIndex(mode_index >= 0 ? mode_index : 0);
    }
    applyProviderSelection();
}

void AiAssistantPanel::onProviderSelectionChanged() {
    QSettings settings;
    settings.setValue(QString::fromLatin1(kProviderSettingKey),
                      ai::modelProviderKey(currentProvider()));
    settings.setValue(QString::fromLatin1(kAuthModeSettingKey),
                      ai::modelAuthModeKey(currentAuthMode()));
    applyProviderSelection();
    appendLocalEvent(tr("AI provider set to %1; the next message starts a new conversation "
                        "with it")
                         .arg(currentProviderLabel()));
}

void AiAssistantPanel::applyProviderSelection() {
    const ai::ModelProviderId provider = currentProvider();
    const ai::ModelAuthMode mode = currentAuthMode();
    m_client->setActive(provider, mode);
    m_accountStatus = {};
    m_accountStatus.provider = provider;
    m_accountStatus.auth_mode = mode;
    // Response ids are provider-specific; a switched provider starts fresh.
    m_previousResponseId.clear();
    m_apiKey.clear();
    populateProviderModels();
    syncAgentContext();
    if (mode == ai::ModelAuthMode::ApiKey) {
        setApiKeyStatus(tr("Not loaded"),
                        sak::ui::kStatusColorError,
                        QString(QChar(kProviderMarkerError)),
                        sak::ui::kStatusColorError);
        loadRememberedApiKey();
    } else {
        setApiKeyStatus(tr("Checking sign-in"),
                        sak::ui::kStatusColorWarning,
                        QStringLiteral("..."),
                        sak::ui::kStatusColorWarning);
        m_client->refreshAccount();
    }
    updateCredentialControls();
    scheduleContextTokenRefresh();
    updateRunTelemetryLabels();
    emitStatusDetails();
}

void AiAssistantPanel::populateProviderModels() {
    if (!m_modelCombo) {
        return;
    }
    const ai::ModelProviderInfo& info = ai::modelProviderInfo(currentProvider());
    const QStringList models = currentAuthMode() == ai::ModelAuthMode::Subscription
                                   ? info.subscription_models
                                   : info.api_models;
    {
        const QSignalBlocker blocker(m_modelCombo);
        m_modelCombo->clear();
        m_modelCombo->addItems(models);
    }
    m_modelCombo->setToolTip(
        tr("Choose or type the %1 model for this session").arg(info.model_family));
    setAccessible(m_modelCombo,
                  tr("AI model"),
                  tr("%1 model for the active session").arg(info.model_family));
}

void AiAssistantPanel::updateProviderControls(bool busy) {
    for (QComboBox* combo : {m_providerCombo, m_authModeCombo}) {
        if (combo) {
            combo->setEnabled(!busy);
        }
    }
}

void AiAssistantPanel::syncAgentContext() {
    ai::AiApprovalPolicy policy = ai::AiApprovalPolicy::Ask;
    switch (currentAccessMode()) {
    case AccessMode::ChatAndResearch:
        policy = ai::AiApprovalPolicy::DenyAll;
        break;
    case AccessMode::UnattendedFullAccess:
        policy = ai::AiApprovalPolicy::AllowAll;
        break;
    case AccessMode::AssistedFullAccess:
        break;
    }
    m_client->setApprovalPolicy(policy);
    QString error;
    const QString workspace =
        m_conversationStore ? m_conversationStore->artifactRootDirectory(&error) : QString();
    m_client->setWorkspaceDirectory(workspace);
}

void AiAssistantPanel::updateSignInButton(bool busy) {
    if (!m_loadKeyButton) {
        return;
    }
    const QString agent = ai::modelProviderInfo(currentProvider()).agent_name;
    QString text = tr("Sign In");
    QString tip = tr("Sign in with your own subscription through %1").arg(agent);
    if (!m_accountStatus.runtime_available) {
        text = tr("Get %1").arg(agent);
        tip = tr("%1 is not available in this build").arg(agent);
    } else if (m_accountStatus.sign_in_pending) {
        text = tr("Cancel Sign-In");
        tip = tr("Stop waiting for the %1 sign-in to finish").arg(agent);
    } else if (m_accountStatus.signed_in) {
        text = tr("Sign Out");
        tip = tr("Sign out of %1 in S.A.K.").arg(agent);
    }
    m_loadKeyButton->setEnabled(!busy);
    m_loadKeyButton->setText(text);
    m_loadKeyButton->setIcon(QIcon());
    m_loadKeyButton->setStyleSheet(sak::ui::kPrimaryButtonStyle);
    m_loadKeyButton->setToolTip(tip);
    setAccessible(m_loadKeyButton, text, tip);
}

void AiAssistantPanel::handleSubscriptionCredentialClick() {
    if (!m_accountStatus.runtime_available) {
        offerAgentRuntimeInstall();
        return;
    }
    if (m_accountStatus.sign_in_pending) {
        m_client->cancelSignIn();
        return;
    }
    if (!m_accountStatus.signed_in) {
        appendLocalEvent(tr("Starting %1 sign-in").arg(currentProviderLabel()));
        m_client->startSignIn();
        return;
    }
    const auto choice = sak::showQuestionLogged(
        this,
        tr("Sign Out"),
        tr("Sign out of %1 in S.A.K.? Your subscription itself is not affected.")
            .arg(currentProviderLabel()),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::No);
    if (choice == QMessageBox::Yes) {
        m_client->signOut();
    }
}

void AiAssistantPanel::offerAgentRuntimeInstall() {
    const ai::ModelProviderInfo& info = ai::modelProviderInfo(currentProvider());
    if (currentProvider() != ai::ModelProviderId::Muse) {
        sak::showInformationLogged(
            this,
            tr("%1 Not Included").arg(info.agent_name),
            tr("This S.A.K. build does not include %1. Use a release that bundles it, or switch "
               "to API key mode.")
                .arg(info.agent_name));
        return;
    }
    // Muse Code has no redistribution grant, so it is installed per user with
    // Meta's official installer rather than shipped with S.A.K.
    const auto choice = sak::showQuestionLogged(
        this,
        tr("Install Muse Code"),
        tr("Muse subscriptions run through Meta's Muse Code. Install it now for this Windows "
           "user with Meta's official installer (no administrator rights needed)?"),
        QMessageBox::Yes | QMessageBox::No,
        QMessageBox::Yes);
    if (choice != QMessageBox::Yes) {
        return;
    }
    const bool started = QProcess::startDetached(QStringLiteral("powershell.exe"),
                                                 {QStringLiteral("-NoProfile"),
                                                  QStringLiteral("-ExecutionPolicy"),
                                                  QStringLiteral("Bypass"),
                                                  QStringLiteral("-Command"),
                                                  QString::fromLatin1(kMuseInstallCommand)});
    appendLocalEvent(started ? tr("Muse Code installer started; click Sign In when it finishes")
                             : tr("Could not start the Muse Code installer"));
    if (!started) {
        QDesktopServices::openUrl(QUrl(QStringLiteral("https://dev.meta.ai/docs/muse-code")));
    }
}

void AiAssistantPanel::onAccountStatusChanged(const ai::AiAccountStatus& status) {
    if (status.provider != currentProvider() || status.auth_mode != currentAuthMode()) {
        return;
    }
    const bool newly_signed_in = status.signed_in && !m_accountStatus.signed_in;
    m_accountStatus = status;
    const QString agent = ai::modelProviderInfo(status.provider).agent_name;
    if (!status.runtime_available) {
        setApiKeyStatus(tr("%1 not installed").arg(agent),
                        sak::ui::kStatusColorError,
                        QString(QChar(kProviderMarkerError)),
                        sak::ui::kStatusColorError);
    } else if (status.sign_in_pending) {
        setApiKeyStatus(tr("Signing in"),
                        sak::ui::kStatusColorWarning,
                        QStringLiteral("..."),
                        sak::ui::kStatusColorWarning);
    } else if (status.signed_in) {
        const QString summary = accountSummary(status);
        setApiKeyStatus(summary.isEmpty() ? tr("Signed in") : tr("Signed in: %1").arg(summary),
                        sak::ui::kStatusColorSuccess,
                        QString(QChar(kProviderMarkerSuccess)),
                        sak::ui::kStatusColorSuccess);
    } else {
        setApiKeyStatus(tr("Not signed in"),
                        sak::ui::kStatusColorError,
                        QString(QChar(kProviderMarkerError)),
                        sak::ui::kStatusColorError);
    }
    if (!status.detail.trimmed().isEmpty()) {
        appendLocalEvent(tr("%1: %2").arg(agent, status.detail.trimmed()));
    }
    if (newly_signed_in) {
        m_client->listModels({});
    }
    updateCredentialControls();
    emitStatusDetails();
}

void AiAssistantPanel::onSignInUrlReady(const QUrl& url) {
    appendLocalEvent(tr("Opening the %1 sign-in page in your browser").arg(currentProviderLabel()));
    if (!QDesktopServices::openUrl(url)) {
        appendLocalEvent(tr("Open this address to sign in: %1").arg(url.toString()));
    }
}

void AiAssistantPanel::onAgentApprovalRequested(const ai::AiAgentApproval& approval) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Allow Agent Action"));
    box.setText(approval.title);
    box.setInformativeText(approval.detail);
    QPushButton* allow_once = box.addButton(tr("Allow Once"), QMessageBox::AcceptRole);
    QPushButton* allow_session = approval.can_allow_for_session
                                     ? box.addButton(tr("Allow for This Chat"),
                                                     QMessageBox::AcceptRole)
                                     : nullptr;
    QPushButton* deny = box.addButton(tr("Deny"), QMessageBox::RejectRole);
    box.setDefaultButton(deny);
    logInfo("AI agent approval requested: {}", approval.title.toStdString());
    box.exec();
    ai::AiApprovalDecision decision = ai::AiApprovalDecision::Deny;
    if (box.clickedButton() == allow_once) {
        decision = ai::AiApprovalDecision::AllowOnce;
    } else if (allow_session && box.clickedButton() == allow_session) {
        decision = ai::AiApprovalDecision::AllowForSession;
    }
    appendLocalEvent(tr("%1: %2").arg(
        approval.title, decision == ai::AiApprovalDecision::Deny ? tr("denied") : tr("allowed")));
    m_client->resolveApproval(approval.id, decision);
}

}  // namespace sak
