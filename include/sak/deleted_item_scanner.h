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

    /// Request cancellation
    void cancel();

Q_SIGNALS:
    void recoveryProgress(int items_found, int nodes_scanned);

private:
    /// Build the set of all NIDs reachable from the folder hierarchy
    void buildReachableSet();

    /// Recursively scan a folder for recoverable items
    void scanRecoverableFolder(const PstFolder& folder, QVector<PstItemDetail>& recovered);

    /// Attempt to read an orphaned NID as a message
    [[nodiscard]] std::optional<PstItemDetail> tryReadOrphanedNode(uint64_t nid);

    PstParser* m_parser;  ///< Non-owning
    QSet<uint64_t> m_reachable_nids;
    std::atomic<bool> m_cancelled{false};
};

}  // namespace sak
