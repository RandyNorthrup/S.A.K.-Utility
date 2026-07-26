// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file app_readonly_actions.h
/// @brief Registers the app's read-only technician operations into an
/// AppActionRegistry so the AI assistant can run them headless.
///
/// These are the "safe-first" wave: each op only reads system/file state (no
/// mutation, no elevation), so it is marked read_only and the assistant's
/// per-action human-gate is skipped -- exactly like a research/query tool. The
/// invoke thunks call the SAME headless src/core services the GUI panels drive
/// (StorageInventoryWorker, VulnerabilityScanner, FileImageSource), never a
/// re-implementation, per the "overlap -> always use app code" rule.
///
/// Most ops run synchronously on the caller's (AI worker) thread (no event loop,
/// no GUI, no controller). The few that can block on an unreachable target or a
/// large tree (find_in_files, network.ping/traceroute/port_scan) instead run on a
/// WorkerBase thread driven by AsyncActionInvocation, giving a hard wall-time
/// ceiling and a real cancel -- the same pattern the mutating ops use.

namespace sak {

class AppActionRegistry;

/// Register the read-only technician ops into @p registry. Returns the number
/// registered. Ids are stable and namespaced by area:
///   partition.list_inventory         -- disks/partitions/volumes snapshot
///   partition.preview_operation      -- plan (never execute) a layout op
///   network.list_adapters            -- network adapter enumeration
///   network.dns_query                -- resolve a hostname (DNS)
///   network.ping / .traceroute       -- ICMP reachability / path probes
///   network.port_scan                -- TCP-connect scan (LOCAL/private targets only)
///   security.list_installed_programs -- installed-program inventory
///   security.scan_vulnerabilities    -- CVE/advisory scan of installed programs
///   imaging.identify_image           -- detect a disk-image file's format
[[nodiscard]] int registerReadOnlyAppActionsInto(AppActionRegistry& registry);

}  // namespace sak
