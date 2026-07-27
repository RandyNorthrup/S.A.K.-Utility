// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file deleted_item_scanner.cpp
/// @brief Recoverable item and orphaned node scanning

#include "sak/deleted_item_scanner.h"

#include "sak/email_types.h"
#include "sak/error_codes.h"
#include "sak/logger.h"
#include "sak/pst_parser.h"

namespace sak {

namespace {
constexpr uint64_t kPstNodeTypeMask = 0x1F;
}

namespace {
/// Well-known NID for the Recoverable Items folder
constexpr uint64_t kRecoverableItemsFolderNid = 0x0301;
constexpr int kScanBatchSize = 200;
}  // namespace

// ======================================================================
// Construction
// ======================================================================

DeletedItemScanner::DeletedItemScanner(PstParser* parser, QObject* parent)
    : QObject(parent), m_parser(parser) {
    Q_ASSERT(parser != nullptr);
}

// ======================================================================
// Public API
// ======================================================================

QVector<PstItemDetail> DeletedItemScanner::scanRecoverableItems() {
    QVector<PstItemDetail> recovered;
    m_recoverable_reliable = true;

    if (!m_parser->isOpen()) {
        return recovered;
    }

    // Look for Recoverable Items folder (NID 0x0301) in the folder tree
    PstFolderTree tree = m_parser->folderTree();

    // Search for the recoverable items folder by NID
    std::function<const PstFolder*(const QVector<PstFolder>&)> findFolder;
    findFolder = [&](const QVector<PstFolder>& folders) -> const PstFolder* {
        for (const auto& folder : folders) {
            if (folder.node_id == kRecoverableItemsFolderNid) {
                return &folder;
            }
            auto* found = findFolder(folder.children);
            if (found) {
                return found;
            }
        }
        return nullptr;
    };

    const PstFolder* recov_folder = findFolder(tree);
    if (!recov_folder) {
        logInfo("DeletedItemScanner: no Recoverable Items folder found");
        return recovered;
    }

    scanRecoverableFolder(*recov_folder, recovered);

    logInfo("DeletedItemScanner: recovered {} items from Recoverable Items",
            std::to_string(recovered.size()));
    return recovered;
}

void DeletedItemScanner::scanRecoverableFolder(const PstFolder& folder,
                                               QVector<PstItemDetail>& recovered) {
    if (m_cancelled.load()) {
        return;
    }

    int offset = 0;
    while (!m_cancelled.load()) {
        auto items_result = m_parser->readFolderItems(folder.node_id, offset, kScanBatchSize);
        if (!items_result.has_value()) {
            // A read error leaves the recovered set incomplete. Cancellation is reported through
            // the caller's timeout channel, not as unreliability; any OTHER error means a real
            // failure that must not masquerade as "nothing to recover" (fail-open honesty hole).
            if (items_result.error() != error_code::operation_cancelled) {
                m_recoverable_reliable = false;
            }
            break;
        }

        const auto& items = items_result.value();
        if (items.isEmpty()) {
            break;
        }

        for (const auto& summary : items) {
            if (m_cancelled.load()) {
                return;
            }

            auto detail = m_parser->readItemDetail(summary.node_id);
            if (detail.has_value()) {
                recovered.append(detail.value());
                Q_EMIT recoveryProgress(recovered.size(), 0);
            }
        }

        offset += items.size();
    }

    for (const auto& child : folder.children) {
        scanRecoverableFolder(child, recovered);
    }
}

QVector<PstItemDetail> DeletedItemScanner::scanOrphanedNodes() {
    QVector<PstItemDetail> recovered;

    if (!m_parser->isOpen()) {
        return recovered;
    }

    // Build the set of NIDs reachable from the folder hierarchy
    buildReachableSet();
    if (!m_reachable_reliable) {
        // The reachability set is incomplete; classifying orphans against it would
        // mis-report live messages as deleted, so skip orphan recovery entirely.
        logWarning(
            "DeletedItemScanner: reachability set incomplete (read error or cancel); "
            "skipping orphan scan");
        return recovered;
    }

    // Get all NBT node IDs
    QVector<uint64_t> all_nids = m_parser->allNodeIds();
    int nodes_scanned = 0;

    for (uint64_t nid : all_nids) {
        if (m_cancelled.load()) {
            break;
        }

        ++nodes_scanned;

        // Only look at NormalMessage type nodes (low 5 bits == 0x04)
        auto nid_type = static_cast<PstNodeType>(nid & kPstNodeTypeMask);
        if (nid_type != PstNodeType::NormalMessage) {
            continue;
        }

        // Skip if it's in the reachable set (not orphaned)
        if (m_reachable_nids.contains(nid)) {
            continue;
        }

        auto item = tryReadOrphanedNode(nid);
        if (item.has_value()) {
            recovered.append(item.value());
        }

        if (nodes_scanned % kScanBatchSize == 0) {
            Q_EMIT recoveryProgress(recovered.size(), nodes_scanned);
        }
    }

    logInfo("DeletedItemScanner: recovered {} orphaned items from {} nodes",
            std::to_string(recovered.size()),
            std::to_string(nodes_scanned));
    return recovered;
}

QVector<PstItemDetail> DeletedItemScanner::recoverAll() {
    auto recoverable = scanRecoverableItems();
    if (m_cancelled.load()) {
        return recoverable;
    }

    auto orphaned = scanOrphanedNodes();
    recoverable.append(orphaned);
    return recoverable;
}

void DeletedItemScanner::cancel() {
    m_cancelled.store(true);
    // Propagate to the parser so an in-flight per-page contents-table read aborts mid-parse. The
    // scanner's own m_cancelled is only polled BETWEEN reads, so without this a single large
    // folder's read (readContentsTable re-parses the whole table per page) could outrun a caller's
    // wall-time ceiling. m_parser is non-owning but outlives this scanner; cancel() is an atomic
    // store, safe to call from a monitor thread while a scan runs.
    if (m_parser != nullptr) {
        m_parser->cancel();
    }
}

// ======================================================================
// Private helpers
// ======================================================================

void DeletedItemScanner::buildReachableSet() {
    m_reachable_nids.clear();
    m_reachable_reliable = true;

    PstFolderTree tree = m_parser->folderTree();

    std::function<void(const PstFolder&)> walk;
    walk = [&](const PstFolder& folder) {
        if (!m_reachable_reliable) {
            return;
        }
        m_reachable_nids.insert(folder.node_id);
        // A read failure OR a cancel (e.g. a caller's wall-time ceiling) leaves the set incomplete;
        // mark it unreliable and stop so scanOrphanedNodes skips classification rather than
        // resurrecting live messages as orphans against a partial reachable set.
        if (!collectFolderItemNids(folder.node_id)) {
            m_reachable_reliable = false;
            return;
        }
        for (const auto& child : folder.children) {
            walk(child);
        }
    };

    for (const auto& root : tree) {
        if (!m_reachable_reliable) {
            break;
        }
        walk(root);
    }
}

bool DeletedItemScanner::collectFolderItemNids(uint64_t folder_node_id) {
    int offset = 0;
    while (!m_cancelled.load()) {
        auto items = m_parser->readFolderItems(folder_node_id, offset, kScanBatchSize);
        if (!items.has_value()) {
            return false;  // read failure -> reachable set incomplete
        }
        if (items.value().isEmpty()) {
            return true;  // end of folder
        }
        for (const auto& item : items.value()) {
            m_reachable_nids.insert(item.node_id);
        }
        offset += items.value().size();
    }
    return false;  // cancelled -> incomplete
}

std::optional<PstItemDetail> DeletedItemScanner::tryReadOrphanedNode(uint64_t nid) {
    auto result = m_parser->readItemDetail(nid);
    if (result.has_value()) {
        return result.value();
    }
    return std::nullopt;
}

}  // namespace sak
