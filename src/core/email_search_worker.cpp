// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_search_worker.cpp
/// @brief Email search worker implementation

#include "sak/email_search_worker.h"

#include "sak/email_constants.h"
#include "sak/layout_constants.h"
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
constexpr int kSearchProgressInterval = 100;
constexpr int kSnippetContextDivisor = 2;

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

/// Whether any enabled criterion needs the full item/message detail loaded (body,
/// recipients, or attachment names). Lets matchers load the detail exactly once.
bool needsDetailLoad(const sak::EmailSearchCriteria& criteria) {
    return criteria.search_body || criteria.search_recipients || criteria.search_attachment_names;
}

}  // namespace

// ============================================================================
// PST Search
// ============================================================================

void EmailSearchWorker::search(PstParser* parser, const sak::EmailSearchCriteria& criteria) {
    if (!parser || !parser->isOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("No PST/OST file open for search"));
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

    double elapsed = timer.elapsed() / sak::kMillisecondsPerSecondF;
    Q_EMIT searchComplete(state.total_hits, elapsed);
}

void EmailSearchWorker::searchSingleFolder(PstParser* parser,
                                           const sak::EmailSearchCriteria& criteria,
                                           const QString& folder_path,
                                           const sak::PstFolder& folder,
                                           SearchState& state) {
    // Page the whole folder: readFolderItems returns only [offset, offset+limit),
    // so a single call silently skipped every item past the first kMaxItemsPerLoad.
    for (int offset = 0;; offset += sak::email::kMaxItemsPerLoad) {
        auto items = parser->readFolderItems(folder.node_id, offset, sak::email::kMaxItemsPerLoad);
        if (!items) {
            // A folder-read failure is surfaced, not silently treated as empty.
            Q_EMIT errorOccurred(
                QStringLiteral("Failed to read folder '%1' during search").arg(folder_path));
            return;
        }
        const auto& page = *items;
        if (!searchItemPage(parser, criteria, folder_path, page, state)) {
            return;  // cancelled or result cap reached
        }
        if (page.size() < sak::email::kMaxItemsPerLoad) {
            break;
        }
    }
}

bool EmailSearchWorker::searchItemPage(PstParser* parser,
                                       const sak::EmailSearchCriteria& criteria,
                                       const QString& folder_path,
                                       const QVector<sak::PstItemSummary>& page,
                                       SearchState& state) {
    for (const auto& item : page) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        if (state.total_hits >= sak::email::kMaxSearchResults) {
            return false;
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

        if (state.items_searched % kSearchProgressInterval == 0) {
            Q_EMIT progressUpdated(state.items_searched, state.total_items);
        }
    }
    return true;
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

        // Use the message's REAL index, not the loop position: readMessages() skips
        // unreadable messages, so a skip shifts every later position and the body
        // lookup in matchMboxItem would fetch the wrong message (B7-28).
        auto match = matchMboxItem(msg, msg.message_index, criteria, parser);
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

        if (msg_idx % kSearchProgressInterval == 0) {
            Q_EMIT progressUpdated(msg_idx, total_items);
        }
    }

    double elapsed = timer.elapsed() / sak::kMillisecondsPerSecondF;
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

    int start = std::max(0, pos - context_chars / kSnippetContextDivisor);
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
    // First enabled field to hit wins. Each matcher checks its own enable flag and
    // returns nullopt when disabled, so this stays a flat chain (no unwieldy branch).
    if (auto r = matchPstSubject(item, criteria)) {
        return r;
    }
    if (auto r = matchPstSender(item, criteria)) {
        return r;
    }
    // Body/attachment/recipient all live in the item detail: load it ONCE and share
    // it across those matchers instead of each re-parsing the same item (reparse-once).
    if (needsDetailLoad(criteria)) {
        auto detail = parser->readItemDetail(item.node_id);
        if (detail) {
            if (auto r = matchPstItemBody(*detail, criteria)) {
                return r;
            }
            if (auto r = matchPstItemAttachments(*detail, criteria)) {
                return r;
            }
            if (auto r = matchPstItemRecipients(*detail, criteria)) {
                return r;
            }
        }
    }
    return matchPstItemMapiProperty(item, criteria, parser);
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstSubject(
    const sak::PstItemSummary& item, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_subject ||
        !matchesQuery(item.subject, criteria.query_text, criteria.case_sensitive)) {
        return std::nullopt;
    }
    return MatchResult{QStringLiteral("subject"),
                       extractContextSnippet(item.subject,
                                             criteria.query_text,
                                             sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstSender(
    const sak::PstItemSummary& item, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_sender) {
        return std::nullopt;
    }
    if (!matchesQuery(item.sender_name, criteria.query_text, criteria.case_sensitive) &&
        !matchesQuery(item.sender_email, criteria.query_text, criteria.case_sensitive)) {
        return std::nullopt;
    }
    return MatchResult{QStringLiteral("sender"),
                       item.sender_name + QStringLiteral(" <") + item.sender_email +
                           QStringLiteral(">")};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemRecipients(
    const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_recipients) {
        return std::nullopt;
    }
    const QStringList fields = {detail.display_to, detail.display_cc, detail.display_bcc};
    for (const QString& field : fields) {
        if (!field.isEmpty() && matchesQuery(field, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{QStringLiteral("recipient"), field};
        }
    }
    return std::nullopt;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemMapiProperty(
    const sak::PstItemSummary& item,
    const sak::EmailSearchCriteria& criteria,
    PstParser* parser) const {
    if (criteria.mapi_property_id == 0) {
        return std::nullopt;
    }
    auto props = parser->readItemProperties(item.node_id);
    if (!props) {
        return std::nullopt;
    }
    for (const auto& prop : *props) {
        if (prop.tag_id != criteria.mapi_property_id) {
            continue;
        }
        // An empty target value means "any item that HAS this property"; otherwise
        // the formatted value must match the query.
        if (criteria.mapi_property_value.isEmpty() || matchesQuery(prop.display_value,
                                                                   criteria.mapi_property_value,
                                                                   criteria.case_sensitive)) {
            return MatchResult{QStringLiteral("mapi_property"), prop.display_value};
        }
    }
    return std::nullopt;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemBody(
    const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_body) {
        return std::nullopt;
    }
    const QString& body = detail.body_plain.isEmpty() ? detail.body_html : detail.body_plain;
    if (!matchesQuery(body, criteria.query_text, criteria.case_sensitive)) {
        return std::nullopt;
    }
    return MatchResult{
        QStringLiteral("body"),
        extractContextSnippet(body, criteria.query_text, sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemAttachments(
    const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_attachment_names || detail.attachments.isEmpty()) {
        return std::nullopt;
    }
    for (const auto& att : detail.attachments) {
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
    int message_index,
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

    // Body/recipient/attachment-name all live in the message detail: load it ONCE and
    // share it, mirroring the PST matchers so the MBOX path honors the same criteria
    // (previously only subject/sender/body were evaluated -- B7-34).
    if (!needsDetailLoad(criteria)) {
        return std::nullopt;
    }
    auto detail = parser->readMessageDetail(message_index);
    if (!detail) {
        return std::nullopt;
    }
    if (auto r = matchMboxBody(*detail, criteria)) {
        return r;
    }
    if (auto r = matchMboxRecipients(*detail, criteria)) {
        return r;
    }
    return matchMboxAttachments(*detail, criteria);
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchMboxBody(
    const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_body) {
        return std::nullopt;
    }
    const QString& body = detail.body_plain.isEmpty() ? detail.body_html : detail.body_plain;
    if (!matchesQuery(body, criteria.query_text, criteria.case_sensitive)) {
        return std::nullopt;
    }
    return MatchResult{
        QStringLiteral("body"),
        extractContextSnippet(body, criteria.query_text, sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchMboxRecipients(
    const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_recipients) {
        return std::nullopt;
    }
    const QStringList fields = {detail.to, detail.cc, detail.bcc};
    for (const QString& field : fields) {
        if (!field.isEmpty() && matchesQuery(field, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{QStringLiteral("recipient"), field};
        }
    }
    return std::nullopt;
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchMboxAttachments(
    const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_attachment_names || detail.attachments.isEmpty()) {
        return std::nullopt;
    }
    for (const auto& att : detail.attachments) {
        const QString& att_name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        if (matchesQuery(att_name, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{QStringLiteral("attachment"), att_name};
        }
    }
    return std::nullopt;
}
