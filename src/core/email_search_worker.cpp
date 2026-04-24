// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_search_worker.cpp
/// @brief Email search worker implementation

#include "sak/email_search_worker.h"

#include "sak/email_constants.h"
#include "sak/logger.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QElapsedTimer>

// ============================================================================
// Construction
// ============================================================================

EmailSearchWorker::EmailSearchWorker(QObject* parent) : QObject(parent) {}

// ============================================================================
// File-Scope Helpers
// ============================================================================

namespace {

struct FolderEntry {
    sak::PstFolder folder;
    QString path;
};

void flattenFolderTree(const sak::PstFolderTree& tree,
                       const QString& parent_path,
                       uint64_t scope_id,
                       QVector<FolderEntry>& result) {
    for (const auto& folder : tree) {
        QString folder_path = parent_path.isEmpty()
                                  ? folder.display_name
                                  : parent_path + QLatin1Char('/') + folder.display_name;
        if (scope_id == 0 || scope_id == folder.node_id) {
            result.append({folder, folder_path});
        }
        flattenFolderTree(folder.children, folder_path, scope_id, result);
    }
}

}  // namespace

// ============================================================================
// PST Search
// ============================================================================

void EmailSearchWorker::search(PstParser* parser, const sak::EmailSearchCriteria& criteria) {
    if (!parser || !parser->isOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("No PST file open for search"));
        Q_EMIT searchComplete(0, 0);
        return;
    }

    m_cancelled.store(false, std::memory_order_relaxed);
    QElapsedTimer timer;
    timer.start();

    QVector<FolderEntry> folders;
    flattenFolderTree(parser->folderTree(), {}, criteria.folder_scope_id, folders);

    SearchState state;
    for (const auto& entry : folders) {
        state.total_items += entry.folder.content_count;
    }
    Q_EMIT progressUpdated(0, state.total_items);

    for (const auto& entry : folders) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            break;
        }
        searchSingleFolder(parser, criteria, entry.path, entry.folder, state);
    }

    double elapsed = timer.elapsed() / 1000.0;
    Q_EMIT searchComplete(state.total_hits, elapsed);
}

void EmailSearchWorker::searchSingleFolder(PstParser* parser,
                                           const sak::EmailSearchCriteria& criteria,
                                           const QString& folder_path,
                                           const sak::PstFolder& folder,
                                           SearchState& state) {
    auto items = parser->readFolderItems(folder.node_id, 0, sak::email::kMaxItemsPerLoad);
    if (!items) {
        return;
    }

    for (const auto& item : *items) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            break;
        }
        if (state.total_hits >= sak::email::kMaxSearchResults) {
            break;
        }
        ++state.items_searched;

        if (!passesItemFilters(item, criteria)) {
            continue;
        }

        auto match = matchPstItem(item, criteria, parser);
        if (match) {
            sak::EmailSearchHit hit;
            hit.item_node_id = item.node_id;
            hit.item_type = item.item_type;
            hit.subject = item.subject;
            hit.sender = item.sender_name;
            hit.date = item.date;
            hit.context_snippet = match->context;
            hit.match_field = match->field;
            hit.folder_path = folder_path;

            Q_EMIT searchHit(hit);
            ++state.total_hits;
        }

        if (state.items_searched % 100 == 0) {
            Q_EMIT progressUpdated(state.items_searched, state.total_items);
        }
    }
}

// ============================================================================
// MBOX Search
// ============================================================================

void EmailSearchWorker::searchMbox(MboxParser* parser, const sak::EmailSearchCriteria& criteria) {
    if (!parser || !parser->isOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("No MBOX file open for search"));
        Q_EMIT searchComplete(0, 0);
        return;
    }

    m_cancelled.store(false, std::memory_order_relaxed);
    QElapsedTimer timer;
    timer.start();
    int total_hits = 0;
    int total_items = parser->messageCount();

    Q_EMIT progressUpdated(0, total_items);

    auto messages = parser->readMessages(0, total_items);
    if (!messages) {
        Q_EMIT errorOccurred(QStringLiteral("Failed to read MBOX messages"));
        return;
    }

    for (int msg_idx = 0; msg_idx < messages->size(); ++msg_idx) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            break;
        }
        if (total_hits >= sak::email::kMaxSearchResults) {
            break;
        }

        const auto& msg = (*messages)[msg_idx];
        if (!passesMboxFilters(msg, criteria)) {
            continue;
        }

        auto match = matchMboxItem(msg, msg_idx, criteria, parser);
        if (match) {
            sak::EmailSearchHit hit;
            hit.item_node_id = static_cast<uint64_t>(msg.message_index);
            hit.item_type = sak::EmailItemType::Email;
            hit.subject = msg.subject;
            hit.sender = msg.from;
            hit.date = msg.date;
            hit.context_snippet = match->context;
            hit.match_field = match->field;
            hit.folder_path = QStringLiteral("MBOX");

            Q_EMIT searchHit(hit);
            ++total_hits;
        }

        if (msg_idx % 100 == 0) {
            Q_EMIT progressUpdated(msg_idx, total_items);
        }
    }

    double elapsed = timer.elapsed() / 1000.0;
    Q_EMIT searchComplete(total_hits, elapsed);
}

// ============================================================================
// Cancel
// ============================================================================

void EmailSearchWorker::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

// ============================================================================
// Internal — Text Matching
// ============================================================================

bool EmailSearchWorker::matchesQuery(const QString& text,
                                     const QString& query,
                                     bool case_sensitive) const {
    if (query.isEmpty() || text.isEmpty()) {
        return false;
    }
    Qt::CaseSensitivity cs = case_sensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    return text.contains(query, cs);
}

QString EmailSearchWorker::extractContextSnippet(const QString& text,
                                                 const QString& query,
                                                 int context_chars) const {
    if (text.isEmpty() || query.isEmpty()) {
        return {};
    }

    int pos = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (pos < 0) {
        return text.left(context_chars);
    }

    int start = std::max(0, pos - context_chars / 2);
    int length = std::min(context_chars, static_cast<int>(text.size()) - start);

    QString snippet = text.mid(start, length);
    if (start > 0) {
        snippet.prepend(QStringLiteral("..."));
    }
    if (start + length < text.size()) {
        snippet.append(QStringLiteral("..."));
    }

    return snippet;
}

// ============================================================================
// Internal — Item Filtering & Matching
// ============================================================================

bool EmailSearchWorker::passesItemFilters(const sak::PstItemSummary& item,
                                          const sak::EmailSearchCriteria& criteria) const {
    if (criteria.item_type_filter != sak::EmailItemType::Unknown &&
        item.item_type != criteria.item_type_filter) {
        return false;
    }
    if (criteria.date_from.isValid() && item.date < criteria.date_from) {
        return false;
    }
    if (criteria.date_to.isValid() && item.date > criteria.date_to) {
        return false;
    }
    if (criteria.has_attachment_only && !item.has_attachments) {
        return false;
    }
    return true;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItem(
    const sak::PstItemSummary& item,
    const sak::EmailSearchCriteria& criteria,
    PstParser* parser) const {
    if (criteria.search_subject &&
        matchesQuery(item.subject, criteria.query_text, criteria.case_sensitive)) {
        return MatchResult{QStringLiteral("subject"),
                           extractContextSnippet(item.subject,
                                                 criteria.query_text,
                                                 sak::email::kSearchContextSnippetChars)};
    }

    if (criteria.search_sender &&
        (matchesQuery(item.sender_name, criteria.query_text, criteria.case_sensitive) ||
         matchesQuery(item.sender_email, criteria.query_text, criteria.case_sensitive))) {
        return MatchResult{QStringLiteral("sender"),
                           item.sender_name + QStringLiteral(" <") + item.sender_email +
                               QStringLiteral(">")};
    }

    if (criteria.search_body) {
        auto result = matchPstItemBody(item, criteria, parser);
        if (result) {
            return result;
        }
    }

    if (criteria.search_attachment_names && item.has_attachments) {
        return matchPstItemAttachments(item, criteria, parser);
    }

    return std::nullopt;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemBody(
    const sak::PstItemSummary& item,
    const sak::EmailSearchCriteria& criteria,
    PstParser* parser) const {
    auto detail = parser->readItemDetail(item.node_id);
    if (!detail) {
        return std::nullopt;
    }
    const QString& body = detail->body_plain.isEmpty() ? detail->body_html : detail->body_plain;
    if (!matchesQuery(body, criteria.query_text, criteria.case_sensitive)) {
        return std::nullopt;
    }
    return MatchResult{
        QStringLiteral("body"),
        extractContextSnippet(body, criteria.query_text, sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemAttachments(
    const sak::PstItemSummary& item,
    const sak::EmailSearchCriteria& criteria,
    PstParser* parser) const {
    auto detail = parser->readItemDetail(item.node_id);
    if (!detail) {
        return std::nullopt;
    }
    for (const auto& att : detail->attachments) {
        const QString& att_name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        if (matchesQuery(att_name, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{QStringLiteral("attachment"), att_name};
        }
    }
    return std::nullopt;
}

bool EmailSearchWorker::passesMboxFilters(const sak::MboxMessage& msg,
                                          const sak::EmailSearchCriteria& criteria) const {
    if (criteria.date_from.isValid() && msg.date < criteria.date_from) {
        return false;
    }
    if (criteria.date_to.isValid() && msg.date > criteria.date_to) {
        return false;
    }
    if (criteria.has_attachment_only && !msg.has_attachments) {
        return false;
    }
    return true;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchMboxItem(
    const sak::MboxMessage& msg,
    int msg_idx,
    const sak::EmailSearchCriteria& criteria,
    MboxParser* parser) const {
    if (criteria.search_subject &&
        matchesQuery(msg.subject, criteria.query_text, criteria.case_sensitive)) {
        return MatchResult{QStringLiteral("subject"),
                           extractContextSnippet(msg.subject,
                                                 criteria.query_text,
                                                 sak::email::kSearchContextSnippetChars)};
    }

    if (criteria.search_sender &&
        matchesQuery(msg.from, criteria.query_text, criteria.case_sensitive)) {
        return MatchResult{QStringLiteral("sender"), msg.from};
    }

    if (criteria.search_body) {
        auto detail = parser->readMessageDetail(msg_idx);
        if (detail) {
            const QString& body = detail->body_plain.isEmpty() ? detail->body_html
                                                               : detail->body_plain;
            if (matchesQuery(body, criteria.query_text, criteria.case_sensitive)) {
                return MatchResult{QStringLiteral("body"),
                                   extractContextSnippet(body,
                                                         criteria.query_text,
                                                         sak::email::kSearchContextSnippetChars)};
            }
        }
    }

    return std::nullopt;
}
