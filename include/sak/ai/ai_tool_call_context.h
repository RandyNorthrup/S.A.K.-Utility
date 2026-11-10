// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/ai_cancellation_token.h"
#include "sak/ai/ai_prompt_assembler.h"
#include "sak/ai/ai_tool_policy.h"

#include <QString>

#include <functional>
#include <memory>

namespace sak::ai {

class ConversationStore;

/// @brief What one tool call runs with, captured when the call is dispatched and owned by it.
///
/// The assistant panel fills this on the GUI thread at dispatch (a chat tool call, a workflow
/// launch, a sub-agent's tool) and AiToolDispatcher hands it to the handler with the lease the
/// dispatch took. A handler reads its session, access mode and cancel route from here, never
/// from the panel: a session switch, a new chat or the panel's teardown while the call runs then
/// cannot redirect what it writes, widen what it may do, or swap state out from under a worker.
/// The panel's long-lived services stay on the panel (the app-action registry, package managers,
/// skill and workflow catalogs, MCP pool): nothing replaces them while it lives, and
/// AiAssistantPanel::release keeps it alive until its tool work has stopped.
struct AiToolCallContext {
    /// The chat the call was dispatched in; empty when none was open.
    QString session_id;
    /// A store bound to session_id (ConversationStore::sessionSnapshot). Const on purpose: a
    /// call writes artifacts through it, which land in its own chat whatever the panel opens
    /// meanwhile, but it cannot write the session's manifest from a stale copy. Records that
    /// touch the manifest (commands, memory) go back through the panel, which drops them when
    /// the call's chat is no longer the open one. Null when the panel had no store.
    std::shared_ptr<const ConversationStore> session_store;
    /// The access mode the user had selected when the call was dispatched.
    AiAccessMode access_mode{AiAccessMode::Invalid};
    /// The session's tool-policy ceiling for that access mode.
    AiToolPolicy session_policy{AiToolPolicy::NoLocalExecution};
    /// The run's cancel route. It shares state with the panel's token, so Stop and teardown
    /// reach the call; an invalid token never reads as cancelled.
    CancellationToken cancel_token;
    /// The user's request for the turn the call belongs to.
    QString user_message;
    /// Set by AiToolDispatcher for a dispatch that holds a mutating lease: the lease this call
    /// runs under, released by the dispatcher when the handler returns. Empty otherwise.
    QString lease_id;

    [[nodiscard]] bool cancellationRequested() const noexcept {
        return cancel_token.isValid() && cancel_token.isCancellationRequested();
    }

    /// The cancel check a long operation polls; it holds its own copy of the token, so it stays
    /// valid on any thread and after this context is gone.
    [[nodiscard]] std::function<bool()> cancelCheck() const {
        return [token = cancel_token]() {
            return token.isValid() && token.isCancellationRequested();
        };
    }
};

}  // namespace sak::ai
