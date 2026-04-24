// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_search_worker.h
/// @brief Worker for full-text search across PST/OST/MBOX email items

#pragma once

#include "sak/email_types.h"

#include <QObject>
#include <QString>

#include <atomic>
#include <optional>

class PstParser;
class MboxParser;

class EmailSearchWorker : public QObject {
    Q_OBJECT

public:
    explicit EmailSearchWorker(QObject* parent = nullptr);

    /// Search within a PST/OST file
    void search(PstParser* parser, const sak::EmailSearchCriteria& criteria);

    /// Search within an MBOX file
    void searchMbox(MboxParser* parser, const sak::EmailSearchCriteria& criteria);

    /// Cancel current search
    void cancel();

    /// Result of a field match attempt
    struct MatchResult {
        QString field;
        QString context;
    };

    /// Mutable counters shared across search helpers
    struct SearchState {
        int total_hits = 0;
        int items_searched = 0;
        int total_items = 0;
    };

Q_SIGNALS:
    void searchHit(sak::EmailSearchHit hit);
    void searchComplete(int total_hits, double elapsed_seconds);
    void progressUpdated(int items_searched, int total_items);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    /// Check if text matches the search query
    [[nodiscard]] bool matchesQuery(const QString& text,
                                    const QString& query,
                                    bool case_sensitive) const;

    /// Extract a context snippet around a match
    [[nodiscard]] QString extractContextSnippet(const QString& text,
                                                const QString& query,
                                                int context_chars) const;

    /// Check if a PST item passes pre-match filters
    [[nodiscard]] bool passesItemFilters(const sak::PstItemSummary& item,
                                         const sak::EmailSearchCriteria& criteria) const;

    /// Try to match a PST item against query criteria
    [[nodiscard]] std::optional<MatchResult> matchPstItem(const sak::PstItemSummary& item,
                                                          const sak::EmailSearchCriteria& criteria,
                                                          PstParser* parser) const;

    /// Match PST item body text
    [[nodiscard]] std::optional<MatchResult> matchPstItemBody(
        const sak::PstItemSummary& item,
        const sak::EmailSearchCriteria& criteria,
        PstParser* parser) const;

    /// Match PST item attachment filenames
    [[nodiscard]] std::optional<MatchResult> matchPstItemAttachments(
        const sak::PstItemSummary& item,
        const sak::EmailSearchCriteria& criteria,
        PstParser* parser) const;

    /// Check if an MBOX message passes pre-match filters
    [[nodiscard]] bool passesMboxFilters(const sak::MboxMessage& msg,
                                         const sak::EmailSearchCriteria& criteria) const;

    /// Try to match an MBOX message against query criteria
    [[nodiscard]] std::optional<MatchResult> matchMboxItem(const sak::MboxMessage& msg,
                                                           int msg_idx,
                                                           const sak::EmailSearchCriteria& criteria,
                                                           MboxParser* parser) const;

    /// Search items within a single PST folder
    void searchSingleFolder(PstParser* parser,
                            const sak::EmailSearchCriteria& criteria,
                            const QString& folder_path,
                            const sak::PstFolder& folder,
                            SearchState& state);
};
