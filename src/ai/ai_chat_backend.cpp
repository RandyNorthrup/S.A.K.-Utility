// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_chat_backend.h"

namespace sak::ai {

AiChatBackend::AiChatBackend(QObject* parent) : QObject(parent) {
    qRegisterMetaType<sak::ai::AiAccountStatus>("sak::ai::AiAccountStatus");
    qRegisterMetaType<sak::ai::OpenAIResponseResult>("sak::ai::OpenAIResponseResult");
    qRegisterMetaType<sak::ai::AiAgentApproval>("sak::ai::AiAgentApproval");
}

AiChatBackend::~AiChatBackend() = default;

void AiChatBackend::refreshAccount() {
    AiAccountStatus status;
    status.provider = provider();
    status.auth_mode = authMode();
    status.runtime_available = true;
    Q_EMIT accountStatusChanged(status);
}

void AiChatBackend::startSignIn() {
    Q_EMIT requestFailed(tr("%1 does not use browser sign-in in API key mode")
                             .arg(modelProviderInfo(provider()).display_name));
}

void AiChatBackend::cancelSignIn() {}

void AiChatBackend::signOut() {}

void AiChatBackend::resetConversation() {}

void AiChatBackend::setWorkspaceDirectory(const QString& directory) {
    Q_UNUSED(directory);
}

void AiChatBackend::setApprovalPolicy(AiApprovalPolicy policy) {
    Q_UNUSED(policy);
}

void AiChatBackend::resolveApproval(const QString& approval_id, AiApprovalDecision decision) {
    Q_UNUSED(approval_id);
    Q_UNUSED(decision);
}

}  // namespace sak::ai
