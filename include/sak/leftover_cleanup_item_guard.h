// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/leftover_cleanup_guard.h"

#include <QString>

/// @file leftover_cleanup_item_guard.h
/// @brief LeftoverItem-level dispatch over the fail-closed clean_leftovers screens.
///
/// Kept separate from leftover_cleanup_guard.h so that header stays QtCore-only (the primitive
/// path/registry/service/task/firewall screens are unit-tested without pulling in QtGui). This
/// dispatch needs the full LeftoverItem definition, which drags in advanced_uninstall_types.h
/// (ProgramInfo carries a QImage icon), so it lives here and is included only by the two callers
/// that actually resolve a LeftoverItem: the AI clean_leftovers op and the GUI Advanced Uninstall
/// cleanup path.
namespace sak {

/// The fail-closed refusal for an already-resolved leftover item (empty = allowed). The dispatch
/// mirrors CleanupWorker::cleanSingleItem exactly so the item the guard clears is the item the
/// worker acts on (StartupEntry with a value name routes to a registry-value delete; without, to a
/// file delete; ShellExtension routes to a registry-key delete). Shared by BOTH callers so neither
/// can reach CleanupWorker with an OS-critical / unrecoverable target.
[[nodiscard]] inline QString cleanupItemRefusal(const LeftoverItem& item) {
    using Type = LeftoverItem::Type;
    const Type type = item.type;
    if (type == Type::RegistryKey || type == Type::ShellExtension) {
        return registryKeyDeletionRefusal(item.path);
    }
    if (type == Type::RegistryValue) {
        return registryValueDeletionRefusal(item.path, item.registryValueName);
    }
    if (type == Type::Service) {
        return serviceDeletionRefusal(item.path);
    }
    if (type == Type::ScheduledTask) {
        return scheduledTaskDeletionRefusal(item.path);
    }
    if (type == Type::FirewallRule) {
        return firewallRuleDeletionRefusal(item.path);
    }
    // A registry-backed StartupEntry deletes a value; a file-backed one deletes a file.
    if (type == Type::StartupEntry && !item.registryValueName.isEmpty()) {
        return registryValueDeletionRefusal(item.path, item.registryValueName);
    }
    // File, Folder, and a file-backed StartupEntry all delete a filesystem path.
    return filePathDeletionRefusal(item.path);
}

}  // namespace sak
