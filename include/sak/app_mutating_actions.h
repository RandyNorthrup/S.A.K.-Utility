// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_action_registry.h"
#include "sak/partition_manager_types.h"

#include <QString>

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
///   email.export_mbox           -- export messages from an MBOX file
///   organizer.organize_directory -- move files into category subfolders
///   imaging.flash_image          -- write a disk image to a physical drive (catastrophic)
[[nodiscard]] int registerMutatingAppActionsInto(AppActionRegistry& registry);

/// Result of resolving + safety-validating a flash target disk. Exposed (not in an
/// anonymous namespace) so the SAFETY logic can be unit-tested with synthetic
/// inventories -- a physical disk is never needed to certify the guard.
struct FlashTargetResolution {
    bool ok{false};         ///< true only if the disk is a safe, flashable target
    QString device_path;    ///< "\\.\PhysicalDrive<N>" when ok
    QString description;    ///< short human/model description of the target when ok
    AppActionResult error;  ///< populated (ok=false) when the disk is missing/unsafe
};

/// Resolve physical @p disk_number against @p inventory and REFUSE it if it is the
/// system/OS disk (is_system or is_boot), read-only/write-protected, or not present.
/// This is the sole guard between a headless flash and a wiped disk (FlashCoordinator
/// itself has no system-drive check), so it fails CLOSED: anything not provably safe
/// is rejected. Pure and side-effect-free for deterministic certification.
[[nodiscard]] FlashTargetResolution resolveFlashTarget(const PartitionInventory& inventory,
                                                       int disk_number);

}  // namespace sak
