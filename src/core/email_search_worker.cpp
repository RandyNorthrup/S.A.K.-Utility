// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_search_worker.cpp
/// @brief Email search worker implementation

#include "sak/email_search_worker.h"

#include "sak/email_constants.h"
#include "sak/layout_constants.h"
#include "sak/mbox_parser.h"
#include "sak/pst_parser.h"

#include <QElapsedTimer>

#include <limits>

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
        const QString folder_path = parent_path.isEmpty()
                                        ? folder.display_name
                                        : parent_path + QLatin1Char('/') + folder.display_name;
        if (scope_id == 0 || scope_id == folder.node_id) {
            result.append({.folder = folder, .path = folder_path});
        }
        flattenFolderTree(folder.children, folder_path, scope_id, result);
    }
}

/// Whether any enabled criterion needs the full item/message detail loaded (body,
/// recipients, or attachment names). Lets matchers load the detail exactly once.
bool needsDetailLoad(const sak::EmailSearchCriteria& criteria) {
    return criteria.search_body || criteria.search_recipients || criteria.search_attachment_names;
}

// Count of items whose detail/property read failed during the CURRENT search. A const match
// helper cannot emit a Qt signal, so it records the failure here and the (non-const) search
// entry point surfaces a single "results may be incomplete" warning via errorOccurred -- an
// unreadable/unparseable item must not masquerade as a clean non-match (R5-P6-18), the same
// way a per-folder read failure is already surfaced. thread_local because a search runs
// synchronously on one pool thread and is single-flight per worker, so each run owns its own
// counter and never races a concurrent search on another thread.
thread_local int t_item_read_failures = 0;

}  // namespace

// ============================================================================
// PST Search
// ============================================================================

void EmailSearchWorker::search(PstParser* parser, const sak::EmailSearchCriteria& criteria) {
    if ((parser == nullptr) || !parser->isOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("No PST/OST file open for search"));
        Q_EMIT searchComplete(0, 0);
        return;
    }

    m_cancelled.store(false, std::memory_order_relaxed);
    t_item_read_failures = 0;
    QElapsedTimer timer;
    timer.start();

    QVector<FolderEntry> folders;
    flattenFolderTree(parser->folderTree(), {}, criteria.folder_scope_id, folders);

    SearchState state;
    for (const auto& entry : folders) {
        // Saturate: content_count comes from untrusted PST bytes, and a single crafted folder
        // (or many folders summed) can exceed INT_MAX. Accumulating that into a signed int is
        // undefined behavior; clamp to INT_MAX so the progress denominator stays well-formed.
        const qint64 sum = static_cast<qint64>(state.total_items) +
                           static_cast<qint64>(entry.folder.content_count);
        state.total_items =
            static_cast<int>(std::min<qint64>(sum, std::numeric_limits<int>::max()));
    }
    Q_EMIT progressUpdated(0, state.total_items);

    for (const auto& entry : folders) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            break;
        }
        searchSingleFolder(parser, criteria, entry.path, entry.folder, state);
    }

    if (t_item_read_failures > 0) {
        // Surface previously-silent per-item detail/property read failures so a partial result
        // is not announced as a clean, complete search (R5-P6-18). Emitted BEFORE the terminal
        // searchComplete, matching the per-folder read-failure surfacing contract the controller
        // relies on (errorOccurred is non-terminal; searchComplete is the single terminal event).
        Q_EMIT errorOccurred(QStringLiteral("Search encountered %1 item read failure(s); results "
                                            "may be incomplete")
                                 .arg(t_item_read_failures));
    }
    const double elapsed = static_cast<double>(timer.elapsed()) / sak::kMillisecondsPerSecondF;
    emitTerminal(state.total_hits, elapsed);
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
        // Fail closed against a folder that keeps returning full pages forever: advancing the
        // offset past INT_MAX is signed-overflow UB and could wrap it negative into an infinite
        // re-read loop. A real folder ends with a short page long before this bound.
        if (offset > std::numeric_limits<int>::max() - sak::email::kMaxItemsPerLoad) {
            Q_EMIT errorOccurred(QStringLiteral("Folder '%1' returned an implausibly large item "
                                                "count during search; stopping enumeration")
                                     .arg(folder_path));
            return;
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
    if ((parser == nullptr) || !parser->isOpen()) {
        Q_EMIT errorOccurred(QStringLiteral("No MBOX file open for search"));
        Q_EMIT searchComplete(0, 0);
        return;
    }

    m_cancelled.store(false, std::memory_order_relaxed);
    t_item_read_failures = 0;
    QElapsedTimer timer;
    timer.start();

    SearchState state;
    // messageCount() reflects the parser's message index, which MboxParser builds lazily on
    // the first readMessages() call -- so it reports 0 on a freshly opened mailbox and must
    // NOT be used to bound the paging loop (doing so searched nothing). The loop instead pages
    // until an offset past the end returns an empty window; total_items is refreshed from the
    // now-built index after the first read purely for the progress denominator.
    Q_EMIT progressUpdated(0, state.total_items);

    // Page the read in kMaxItemsPerLoad-sized windows instead of pulling the WHOLE mailbox
    // into one QVector: an attacker-sized message count would otherwise force a
    // mailbox-sized allocation before any filter or result cap applied. The stride is fixed
    // (never page.size()) because readMessages() skips unreadable messages, so a short page
    // does not mean the mailbox ended -- mirrors email_export_worker's paging.
    for (int offset = 0;; offset += sak::email::kMaxItemsPerLoad) {
        auto messages = parser->readMessages(offset, sak::email::kMaxItemsPerLoad);
        if (!messages) {
            Q_EMIT errorOccurred(QStringLiteral("Failed to read MBOX messages"));
            // searchComplete must be emitted on EVERY termination path. The null-parser guard
            // above already does it; a caller that gates on completion (the panel re-enables its
            // Search button there) must not wait forever after a read failure it was told about.
            Q_EMIT searchComplete(0, 0);
            return;
        }
        if (offset == 0) {
            // The index is built now; use the real count for a meaningful progress bar.
            state.total_items = parser->messageCount();
            Q_EMIT progressUpdated(0, state.total_items);
        }
        // An offset at or past the end yields an empty window: the mailbox is exhausted.
        // readMessages clamps offset to the message count, so offset only ever advances and
        // this terminates.
        if (messages->isEmpty()) {
            break;
        }
        if (!searchMessagePage(parser, criteria, *messages, state)) {
            break;  // cancelled or result cap reached
        }
        // Fail closed against a mailbox whose index somehow never exhausts: past INT_MAX the
        // offset increment is signed-overflow UB. A real mailbox ends long before this.
        if (offset > std::numeric_limits<int>::max() - sak::email::kMaxItemsPerLoad) {
            break;
        }
    }

    if (t_item_read_failures > 0) {
        // Surface previously-silent per-item detail/property read failures so a partial result
        // is not announced as a clean, complete search (R5-P6-18). Emitted BEFORE the terminal
        // searchComplete, matching the per-folder read-failure surfacing contract the controller
        // relies on (errorOccurred is non-terminal; searchComplete is the single terminal event).
        Q_EMIT errorOccurred(QStringLiteral("Search encountered %1 item read failure(s); results "
                                            "may be incomplete")
                                 .arg(t_item_read_failures));
    }
    const double elapsed = static_cast<double>(timer.elapsed()) / sak::kMillisecondsPerSecondF;
    emitTerminal(state.total_hits, elapsed);
}

bool EmailSearchWorker::searchMessagePage(MboxParser* parser,
                                          const sak::EmailSearchCriteria& criteria,
                                          const QVector<sak::MboxMessage>& page,
                                          SearchState& state) {
    for (const auto& msg : page) {
        if (m_cancelled.load(std::memory_order_relaxed)) {
            return false;
        }
        if (state.total_hits >= sak::email::kMaxSearchResults) {
            return false;
        }
        ++state.items_searched;

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
            ++state.total_hits;
        }

        if (state.items_searched % kSearchProgressInterval == 0) {
            Q_EMIT progressUpdated(state.items_searched, state.total_items);
        }
    }
    return true;
}

// ============================================================================
// Cancel
// ============================================================================

void EmailSearchWorker::cancel() {
    m_cancelled.store(true, std::memory_order_relaxed);
}

void EmailSearchWorker::emitTerminal(int total_hits, double elapsed_seconds) {
    // A pending cancel means the loops broke out early, so total_hits is only what was found
    // before the stop -- announce that as searchCancelled, not searchComplete, so the UI cannot
    // render a truncated scan as a finished one. m_cancelled is still true here (cancel() sets
    // it; only a fresh search() / searchMbox() entry clears it). A cancel that lands in the
    // narrow window between the last loop check and this call harmlessly labels an otherwise
    // complete run as cancelled -- the initiator did ask to cancel, and the hit count is honest.
    if (m_cancelled.load(std::memory_order_relaxed)) {
        Q_EMIT searchCancelled(total_hits, elapsed_seconds);
        return;
    }
    Q_EMIT searchComplete(total_hits, elapsed_seconds);
}

// ============================================================================
// Internal -- Text Matching
// ============================================================================

bool EmailSearchWorker::matchesQuery(const QString& text,
                                     const QString& query,
                                     bool case_sensitive) const {
    if (query.isEmpty() || text.isEmpty()) {
        return false;
    }
    const Qt::CaseSensitivity cs = case_sensitive ? Qt::CaseSensitive : Qt::CaseInsensitive;
    return text.contains(query, cs);
}

QString EmailSearchWorker::extractContextSnippet(const QString& text,
                                                 const QString& query,
                                                 int context_chars) const {
    if (text.isEmpty() || query.isEmpty()) {
        return {};
    }

    const qsizetype pos = text.indexOf(query, 0, Qt::CaseInsensitive);
    if (pos < 0) {
        return text.left(context_chars);
    }

    const qsizetype start = std::max<qsizetype>(0, pos - (context_chars / kSnippetContextDivisor));
    const qsizetype length = std::min<qsizetype>(context_chars, text.size() - start);

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
// Internal -- Item Filtering & Matching
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
        } else {
            // A detail read that fails on a damaged/crafted item must not masquerade as a clean
            // non-match: record it so the search entry point warns the results may be incomplete
            // (R5-P6-18).
            ++t_item_read_failures;
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
    return MatchResult{.field = QStringLiteral("subject"),
                       .context = extractContextSnippet(item.subject,
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
    return MatchResult{.field = QStringLiteral("sender"),
                       .context = item.sender_name + QStringLiteral(" <") + item.sender_email +
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
            return MatchResult{.field = QStringLiteral("recipient"), .context = field};
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
        // Record the read failure (see t_item_read_failures) so it is surfaced as a
        // possibly-incomplete result rather than a silent non-match (R5-P6-18).
        ++t_item_read_failures;
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
            return MatchResult{.field = QStringLiteral("mapi_property"),
                               .context = prop.display_value};
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
    return MatchResult{.field = QStringLiteral("body"),
                       .context = extractContextSnippet(
                           body, criteria.query_text, sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchPstItemAttachments(
    const sak::PstItemDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_attachment_names || detail.attachments.isEmpty()) {
        return std::nullopt;
    }
    for (const auto& att : detail.attachments) {
        const QString& att_name = att.long_filename.isEmpty() ? att.filename : att.long_filename;
        if (matchesQuery(att_name, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{.field = QStringLiteral("attachment"), .context = att_name};
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
        return MatchResult{.field = QStringLiteral("subject"),
                           .context =
                               extractContextSnippet(msg.subject,
                                                     criteria.query_text,
                                                     sak::email::kSearchContextSnippetChars)};
    }

    if (criteria.search_sender &&
        matchesQuery(msg.from, criteria.query_text, criteria.case_sensitive)) {
        return MatchResult{.field = QStringLiteral("sender"), .context = msg.from};
    }

    // Body/recipient/attachment-name all live in the message detail: load it ONCE and
    // share it, mirroring the PST matchers so the MBOX path honors the same criteria
    // (previously only subject/sender/body were evaluated -- B7-34).
    if (!needsDetailLoad(criteria)) {
        return std::nullopt;
    }
    auto detail = parser->readMessageDetail(message_index);
    if (!detail) {
        // Record the read failure (see t_item_read_failures) so it is surfaced as a
        // possibly-incomplete result rather than a silent non-match (R5-P6-18).
        ++t_item_read_failures;
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
    return MatchResult{.field = QStringLiteral("body"),
                       .context = extractContextSnippet(
                           body, criteria.query_text, sak::email::kSearchContextSnippetChars)};
}

std::optional<EmailSearchWorker::MatchResult> EmailSearchWorker::matchMboxRecipients(
    const sak::MboxMessageDetail& detail, const sak::EmailSearchCriteria& criteria) const {
    if (!criteria.search_recipients) {
        return std::nullopt;
    }
    const QStringList fields = {detail.to, detail.cc, detail.bcc};
    for (const QString& field : fields) {
        if (!field.isEmpty() && matchesQuery(field, criteria.query_text, criteria.case_sensitive)) {
            return MatchResult{.field = QStringLiteral("recipient"), .context = field};
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
            return MatchResult{.field = QStringLiteral("attachment"), .context = att_name};
        }
    }
    return std::nullopt;
}
