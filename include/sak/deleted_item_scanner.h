// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file deleted_item_scanner.h
/// @brief Scans for recoverable and orphaned items in an OST/PST file

#pragma once

#include "sak/email_types.h"

#include <QObject>
#include <QSet>
#include <QVector>

#include <atomic>
#include <cstdint>

class PstParser;

namespace sak {

/// @brief Scans an OST/PST file for deleted and orphaned message items
///
/// Two recovery strategies:
/// 1. Recoverable Items folder scan (soft-deleted items still in hierarchy)
/// 2. Orphaned NBT node scan (hard-deleted items, unreferenced nodes)
class DeletedItemScanner : public QObject {
    Q_OBJECT

public:
    explicit DeletedItemScanner(PstParser* parser, QObject* parent = nullptr);

    /// Scan the Recoverable Items folder hierarchy
    [[nodiscard]] QVector<PstItemDetail> scanRecoverableItems();

    /// Scan NBT for orphaned message nodes not in any folder hierarchy
    [[nodiscard]] QVector<PstItemDetail> scanOrphanedNodes();

    /// Combined: scan both sources
    [[nodiscard]] QVector<PstItemDetail> recoverAll();

    /// @brief Whether the last orphan scan's reachability set was reliable.
    ///
    /// scanOrphanedNodes() skips orphan recovery and returns an empty vector when a folder read
    /// failed while building the reachability set (classifying orphans against an incomplete set
    /// would resurrect live messages as deleted). This getter lets a caller distinguish that
    /// "scan skipped" case from a genuine "no orphans found" -- without it an empty orphan result
    /// is a fail-open honesty hole. Meaningful only after scanOrphanedNodes()/recoverAll() has run.
    [[nodiscard]] bool reachableReliable() const { return m_reachable_reliable; }

    /// @brief Whether the last recoverable-items scan completed without a read failure.
    ///
    /// scanRecoverableItems() stops a folder's item enumeration on any read error; a NON-cancelled
    /// error means the recoverable set is incomplete, so an empty/partial result must not be read
    /// as a definitive "nothing to recover". Mirrors reachableReliable() for the recoverable side
    /// (cancellation is reported separately, not as unreliability). Meaningful only after
    /// scanRecoverableItems()/recoverAll() has run.
    [[nodiscard]] bool recoverableReliable() const { return m_recoverable_reliable; }

    /// Request cancellation of an in-progress scan. Also cancels the underlying PstParser so its
    /// per-page contents-table reads abort mid-parse (the scanner's own m_cancelled only gates
    /// between reads); without that a large folder's read could outrun the caller's wall-time
    /// ceiling. Safe cross-thread: both flags are atomic stores.
    void cancel();

Q_SIGNALS:
    void recoveryProgress(int items_found, int nodes_scanned);

private:
    /// Build the set of all NIDs reachable from the folder hierarchy
    void buildReachableSet();

    /// Insert every item NID of one folder into m_reachable_nids. Returns false when the set is
    /// left INCOMPLETE -- a page read failed or the scan was cancelled (m_cancelled) -- so the
    /// caller marks the whole reachability build unreliable.
    [[nodiscard]] bool collectFolderItemNids(uint64_t folder_node_id);

    /// Recursively scan a folder for recoverable items
    void scanRecoverableFolder(const PstFolder& folder, QVector<PstItemDetail>& recovered);
    // Read the detail for each summary in one batch, appending recovered items.
    // Returns false when a cancel was observed mid-batch (caller must stop).
    [[nodiscard]] bool appendRecoverableBatch(const QVector<PstItemSummary>& items,
                                              QVector<PstItemDetail>& recovered);

    /// Attempt to read an orphaned NID as a message
    [[nodiscard]] std::optional<PstItemDetail> tryReadOrphanedNode(uint64_t nid);

    PstParser* m_parser;  ///< Non-owning
    QSet<uint64_t> m_reachable_nids;
    /// False when a folder read failed while building m_reachable_nids, meaning
    /// the set is incomplete and must not be used to classify orphans (a live
    /// message missing from it would be mis-reported as deleted).
    bool m_reachable_reliable = true;
    /// False when a NON-cancelled folder read failed during scanRecoverableItems, meaning the
    /// recovered set is incomplete (a failed read must not masquerade as "nothing to recover").
    bool m_recoverable_reliable = true;
    std::atomic<bool> m_cancelled{false};
};

}  // namespace sak
