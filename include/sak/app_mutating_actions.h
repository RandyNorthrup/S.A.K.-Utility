// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/app_action_registry.h"
#include "sak/partition_manager_types.h"

#include <QJsonObject>
#include <QString>
#include <QVector>

#include <optional>

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
///   email.export_mbox            -- export messages from an MBOX file
///   organizer.organize_directory -- move files into category subfolders
///   imaging.flash_image          -- write a disk image to a physical drive (catastrophic)
///   partition.apply_operation    -- apply one partition-layout op to a disk (catastrophic)
///   software.uninstall_uwp_app   -- remove an installed Store/UWP app (silent, per-name)
///   software.uninstall_program   -- silently remove an installed Win32 program (per-name)
[[nodiscard]] int registerMutatingAppActionsInto(AppActionRegistry& registry);

/// True if @p name is a UWP PackageFullName safe to interpolate into the single-quoted
/// Remove-AppxPackage -Package '<...>' PowerShell argument: ASCII alphanumerics plus '.'
/// '_' '-' and, for a provisioned (DISM PackageName) neutral ResourceId, '~' (all inert
/// inside a single-quoted PowerShell string), non-empty, length-bounded. The value passed
/// to the uninstall always comes from the SYSTEM enumeration, not model input, but this is
/// a defense-in-depth guard against a malformed/hostile name breaking out of the quotes.
/// Exposed (not anonymous) for unit testing.
[[nodiscard]] bool isSafePackageFullName(const QString& name);

/// Validate the (untrusted) argument object of partition.apply_operation BEFORE any disk
/// work: disk_number/partition_number must be non-negative integers; offset_bytes/size_bytes,
/// when present, must be finite, non-negative WHOLE numbers of the JSON number type (a
/// wrong-typed or fractional value is REFUSED, never coerced to 0 or truncated); payload, when
/// present, must be a JSON object; confirm_layout_hash is required; and dry_run, when present,
/// must be a real boolean (never coerced -- a present-but-non-bool value that read as false via
/// toBool would silently escalate a plan into a destructive apply). Returns an error result to
/// surface verbatim, or nullopt when every argument is well-formed. Pure; exposed for unit
/// testing this catastrophic op's fail-closed input gate without a real disk.
[[nodiscard]] std::optional<AppActionResult> validatePartitionApplyArgs(const QJsonObject& args);

/// Pure resolution core for software.uninstall_program (exposed for unit testing): from a RAW
/// (non-deduped) registry program list, find the single silently-uninstallable match for
/// @p name (exact, case-insensitive) and set @p out. Returns an error result -- never a wrong
/// match -- when: the name matches a SystemComponent (a hidden runtime/driver -- refused, never
/// silently removed headless); no program matches; a match has no silent uninstall command (its
/// uninstaller is interactive); or two matches resolve to DIFFERENT silent commands (genuinely
/// distinct programs -> ambiguous). A name double-registered across hives yields the SAME
/// command and resolves to one. Side-effect-free for deterministic certification.
[[nodiscard]] std::optional<AppActionResult> resolveWin32ProgramFromList(
    const QVector<ProgramInfo>& programs, const QString& name, ProgramInfo& out);

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

/// Result of resolving + safety-gating a partition.apply_operation target disk.
/// Exposed (not anonymous) so the guard can be unit-tested with synthetic
/// inventories -- a real disk is never needed to certify it.
struct PartitionApplyResolution {
    bool ok{false};         ///< true only if the disk is a safe, applyable target
    QString device_path;    ///< "\\.\PhysicalDrive<N>" when ok
    QString description;    ///< short human/model description of the target when ok
    AppActionResult error;  ///< populated (ok=false) when refused
};

/// Resolve physical @p disk_number against @p inventory and REFUSE it if the scan is
/// degraded (warnings), the observed layout drifted since preview
/// (@p confirm_layout_hash != inventory.layout_hash), the disk is missing, or it is
/// the OS/system/boot disk, a dynamic/Storage-Spaces disk, or read-only. Partition
/// apply on the running OS disk is always refused (envelope: any non-system disk);
/// use the GUI panel for OS-disk surgery. Fails CLOSED; pure and side-effect-free.
[[nodiscard]] PartitionApplyResolution resolvePartitionApplyTarget(
    const PartitionInventory& inventory, int disk_number, const QString& confirm_layout_hash);

}  // namespace sak
