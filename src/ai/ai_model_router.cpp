// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_model_router.h"

#include <utility>

namespace sak::ai {

namespace {

constexpr int kAuthModeKeyStride = 16;

}  // namespace

AiModelRouter::AiModelRouter(QObject* parent)
    : AiModelRouter(&AiModelRouter::createDefaultBackend, parent) {}

AiModelRouter::AiModelRouter(BackendFactory factory, QObject* parent)
    : AiChatBackend(parent), m_factory(std::move(factory)) {}

AiModelRouter::~AiModelRouter() {
    for (auto* backend : std::as_const(m_backends)) {
        backend->cancel();
    }
}

int AiModelRouter::backendKey(ModelProviderId provider, ModelAuthMode mode) {
    return static_cast<int>(mode) * kAuthModeKeyStride + static_cast<int>(provider);
}

void AiModelRouter::setActive(ModelProviderId provider, ModelAuthMode mode) {
    m_provider = provider;
    m_mode = mode;
}

AiChatBackend* AiModelRouter::activeBackend() {
    return backendFor(m_provider, m_mode);
}

AiChatBackend* AiModelRouter::backendFor(ModelProviderId provider, ModelAuthMode mode) {
    const int key = backendKey(provider, mode);
    if (auto* existing = m_backends.value(key, nullptr)) {
        return existing;
    }
    AiChatBackend* backend = m_factory ? m_factory(provider, mode, this) : nullptr;
    if (!backend) {
        return nullptr;
    }
    m_backends.insert(key, backend);
    connectBackend(backend);
    return backend;
}

void AiModelRouter::connectBackend(AiChatBackend* backend) {
    const auto active = [this, backend]() {
        return m_backends.value(backendKey(m_provider, m_mode), nullptr) == backend;
    };
    connect(backend, &AiChatBackend::requestStarted, this, [this, active]() {
        if (active()) {
            Q_EMIT requestStarted();
        }
    });
    connect(backend, &AiChatBackend::requestFinished, this, [this, active]() {
        if (active()) {
            Q_EMIT requestFinished();
        }
    });
    connect(backend, &AiChatBackend::responseReady, this, [this, active](const auto& result) {
        if (active()) {
            Q_EMIT responseReady(result);
        }
    });
    connect(backend, &AiChatBackend::modelsReady, this, [this, active](const QStringList& ids) {
        if (active()) {
            Q_EMIT modelsReady(ids);
        }
    });
    connect(backend,
            &AiChatBackend::inputTokenCountReady,
            this,
            [this, active](const QString& request_id, qint64 tokens) {
                if (active()) {
                    Q_EMIT inputTokenCountReady(request_id, tokens);
                }
            });
    connect(backend,
            &AiChatBackend::inputTokenCountFailed,
            this,
            [this, active](const QString& request_id, const QString& error) {
                if (active()) {
                    Q_EMIT inputTokenCountFailed(request_id, error);
                }
            });
    connect(backend, &AiChatBackend::requestFailed, this, [this, active](const QString& error) {
        if (active()) {
            Q_EMIT requestFailed(error);
        }
    });
    connect(backend, &AiChatBackend::activityText, this, [this, active](const QString& text) {
        if (active()) {
            Q_EMIT activityText(text);
        }
    });
    connect(backend, &AiChatBackend::signInUrlReady, this, &AiChatBackend::signInUrlReady);
    connect(
        backend, &AiChatBackend::accountStatusChanged, this, &AiChatBackend::accountStatusChanged);
}

void AiModelRouter::createResponse(const OpenAIResponseRequest& request) {
    if (auto* backend = activeBackend()) {
        backend->createResponse(request);
        return;
    }
    Q_EMIT requestFailed(
        tr("%1 is not available in this build").arg(modelProviderInfo(m_provider).display_name));
}

void AiModelRouter::countInputTokens(const OpenAIResponseRequest& request,
                                     const QString& request_id) {
    auto* backend = activeBackend();
    if (!backend || !backend->supportsInputTokenCount()) {
        Q_EMIT inputTokenCountFailed(request_id, tr("Token counting is not available"));
        return;
    }
    backend->countInputTokens(request, request_id);
}

void AiModelRouter::listModels(const QString& credential) {
    if (auto* backend = activeBackend()) {
        backend->listModels(credential);
        return;
    }
    Q_EMIT requestFailed(
        tr("%1 is not available in this build").arg(modelProviderInfo(m_provider).display_name));
}

void AiModelRouter::cancel() {
    for (auto* backend : std::as_const(m_backends)) {
        backend->cancel();
    }
}

bool AiModelRouter::isBusy() const {
    auto* backend = m_backends.value(backendKey(m_provider, m_mode), nullptr);
    return backend && backend->isBusy();
}

bool AiModelRouter::supportsInputTokenCount() const {
    auto* backend = m_backends.value(backendKey(m_provider, m_mode), nullptr);
    return backend ? backend->supportsInputTokenCount() : m_mode == ModelAuthMode::ApiKey;
}

void AiModelRouter::refreshAccount() {
    if (auto* backend = activeBackend()) {
        backend->refreshAccount();
    }
}

void AiModelRouter::startSignIn() {
    if (auto* backend = activeBackend()) {
        backend->startSignIn();
    }
}

void AiModelRouter::cancelSignIn() {
    if (auto* backend = activeBackend()) {
        backend->cancelSignIn();
    }
}

void AiModelRouter::signOut() {
    if (auto* backend = activeBackend()) {
        backend->signOut();
    }
}

void AiModelRouter::resetConversation() {
    for (auto* backend : std::as_const(m_backends)) {
        backend->resetConversation();
    }
}

}  // namespace sak::ai
