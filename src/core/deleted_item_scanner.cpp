// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file deleted_item_scanner.cpp
/// @brief Recoverable item and orphaned node scanning

#include "sak/deleted_item_scanner.h"

#include "sak/email_types.h"
#include "sak/logger.h"
#include "sak/pst_parser.h"

namespace sak {

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

    // Get all NBT node IDs
    QVector<uint64_t> all_nids = m_parser->allNodeIds();
    int nodes_scanned = 0;

    for (uint64_t nid : all_nids) {
        if (m_cancelled.load()) {
            break;
        }

        ++nodes_scanned;

        // Only look at NormalMessage type nodes (low 5 bits == 0x04)
        auto nid_type = static_cast<PstNodeType>(nid & 0x1F);
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
}

// ======================================================================
// Private helpers
// ======================================================================

void DeletedItemScanner::buildReachableSet() {
    m_reachable_nids.clear();

    PstFolderTree tree = m_parser->folderTree();

    std::function<void(const PstFolder&)> walk;
    walk = [&](const PstFolder& folder) {
        m_reachable_nids.insert(folder.node_id);

        // Load all item NIDs in this folder
        int offset = 0;
        while (true) {
            auto items = m_parser->readFolderItems(folder.node_id, offset, kScanBatchSize);
            if (!items.has_value() || items.value().isEmpty()) {
                break;
            }
            for (const auto& item : items.value()) {
                m_reachable_nids.insert(item.node_id);
            }
            offset += items.value().size();
        }

        for (const auto& child : folder.children) {
            walk(child);
        }
    };

    for (const auto& root : tree) {
        walk(root);
    }
}

std::optional<PstItemDetail> DeletedItemScanner::tryReadOrphanedNode(uint64_t nid) {
    auto result = m_parser->readItemDetail(nid);
    if (result.has_value()) {
        return result.value();
    }
    return std::nullopt;
}

}  // namespace sak
