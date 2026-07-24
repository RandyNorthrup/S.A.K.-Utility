// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file app_mutating_actions.h
/// @brief Registers the app's MUTATING technician operations into an
/// AppActionRegistry so the AI assistant can run them headless -- behind the gate.
///
/// Unlike the read-only wave, every op here is marked mutating (and destructive /
/// requires_admin where that applies), so the assistant's per-action guardrail
/// (AiAssistantPanel::appActionRunGate) is ENFORCED: a chat/research session
/// refuses it, an Assisted session requires an explicit human confirmation, and an
/// Unattended session offers a restore point first. The invoke thunks call the SAME
/// headless src/core services the GUI panels drive (per the "overlap -> always use
/// app code" rule); they never re-implement the operation and never pop a dialog.
///
/// Ops run on the caller's (AI worker) thread. Ones whose engine is synchronous and
/// signal-completes inline (email export) run directly; any that need an async
/// worker use the AsyncActionInvocation bridge, exactly like the read-only search.

namespace sak {

class AppActionRegistry;

/// Register the mutating technician ops into @p registry. Returns the number
/// registered. Ids are stable and namespaced by area:
///   email.export_mbox -- export messages from an MBOX file to eml/html/text/pdf
[[nodiscard]] int registerMutatingAppActionsInto(AppActionRegistry& registry);

}  // namespace sak
