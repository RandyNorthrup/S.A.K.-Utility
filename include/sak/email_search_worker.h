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
    /// Terminal outcome for a user-initiated cancel: the scan stopped early, so the hits
    /// carried here are PARTIAL. Emitted instead of searchComplete so a truncated search is
    /// never announced as a finished one (R5-P6-18). Like searchComplete it is a terminal
    /// event -- exactly one of the two fires per run, and both return the caller to idle.
    void searchCancelled(int partial_hits, double elapsed_seconds);
    void progressUpdated(int items_searched, int total_items);
    void errorOccurred(QString error);

private:
    std::atomic<bool> m_cancelled{false};

    /// Emit the single terminal event for a finished run: searchCancelled when a cancel is
    /// pending (m_cancelled is still set from cancel(); it is cleared only at the next search
    /// entry), searchComplete otherwise. Both PST and MBOX tails route through here so the
    /// cancel-vs-complete distinction is decided in exactly one place.
    void emitTerminal(int total_hits, double elapsed_seconds);

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

    /// Field matchers below each check their own enable flag and return nullopt when
    /// disabled, so matchPstItem stays a flat first-hit chain.
    [[nodiscard]] std::optional<MatchResult> matchPstSubject(
        const sak::PstItemSummary& item, const sak::EmailSearchCriteria& criteria) const;
    [[nodiscard]] std::optional<MatchResult> matchPstSender(
        const sak::PstItemSummary& item, const sak::EmailSearchCriteria& criteria) const;

    /// Match PST item body text. Operates on an already-loaded detail so the item
    /// is parsed only once for all detail-based fields (B7-B: reparse-once).
    [[nodiscard]] std::optional<MatchResult> matchPstItemBody(
        const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const;

    /// Match PST item attachment filenames (from an already-loaded detail).
    [[nodiscard]] std::optional<MatchResult> matchPstItemAttachments(
        const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const;

    /// Match PST item recipient fields (To/Cc/Bcc) from an already-loaded detail --
    /// criteria.search_recipients was never evaluated before (B7-34).
    [[nodiscard]] std::optional<MatchResult> matchPstItemRecipients(
        const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const;

    /// Match a specific MAPI property value (advanced search) -- criteria's
    /// mapi_property_id / mapi_property_value were never evaluated before (B7-34).
    [[nodiscard]] std::optional<MatchResult> matchPstItemMapiProperty(
        const sak::PstItemSummary& item,
        const sak::EmailSearchCriteria& criteria,
        PstParser* parser) const;

    /// Check if an MBOX message passes pre-match filters
    [[nodiscard]] bool passesMboxFilters(const sak::MboxMessage& msg,
                                         const sak::EmailSearchCriteria& criteria) const;

    /// Try to match an MBOX message against query criteria
    [[nodiscard]] std::optional<MatchResult> matchMboxItem(const sak::MboxMessage& msg,
                                                           int message_index,
                                                           const sak::EmailSearchCriteria& criteria,
                                                           MboxParser* parser) const;

    /// MBOX detail-based matchers (body/recipients/attachment names), mirroring the
    /// PST matchers so the MBOX path evaluates the same criteria (B7-34). Each reads
    /// from an already-loaded detail so the message is parsed only once.
    [[nodiscard]] std::optional<MatchResult> matchMboxBody(
        const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const;
    [[nodiscard]] std::optional<MatchResult> matchMboxRecipients(
        const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const;
    [[nodiscard]] std::optional<MatchResult> matchMboxAttachments(
        const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const;

    /// Search items within a single PST folder
    void searchSingleFolder(PstParser* parser,
                            const sak::EmailSearchCriteria& criteria,
                            const QString& folder_path,
                            const sak::PstFolder& folder,
                            SearchState& state);

    /// Search one page of folder items. Returns false when the search should stop
    /// (cancelled or the result cap was reached).
    [[nodiscard]] bool searchItemPage(PstParser* parser,
                                      const sak::EmailSearchCriteria& criteria,
                                      const QString& folder_path,
                                      const QVector<sak::PstItemSummary>& page,
                                      SearchState& state);

    /// Search one page of MBOX messages. Returns false when the search should stop
    /// (cancelled or the result cap was reached), mirroring searchItemPage.
    [[nodiscard]] bool searchMessagePage(MboxParser* parser,
                                         const sak::EmailSearchCriteria& criteria,
                                         const QVector<sak::MboxMessage>& page,
                                         SearchState& state);
};
