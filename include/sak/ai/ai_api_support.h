// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/ai/openai_response_types.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <optional>

namespace sak::ai {

inline constexpr int kVendorApiTimeoutMs = 300'000;
inline constexpr int kVendorApiShortTimeoutMs = 60'000;
inline constexpr int kDefaultHistoryCacheEntries = 64;

/// @brief HTTPS JSON request with the app's TLS, redirect and UA policy.
[[nodiscard]] QNetworkRequest vendorJsonRequest(const QUrl& url, int timeout_ms);

[[nodiscard]] QByteArray compactJson(const QJsonObject& object);
[[nodiscard]] std::optional<QJsonObject> parseJsonObject(const QByteArray& data,
                                                         QString* error_message);

struct DataUrlParts {
    QString mime_type;
    QString base64_data;
};

/// @brief Split a base64 `data:` URL into MIME type and payload.
[[nodiscard]] std::optional<DataUrlParts> parseDataUrl(const QString& data_url);

/// @brief MIME type implied by a context file name, empty when unknown.
[[nodiscard]] QString mimeTypeForFilename(const QString& filename);

/// @brief Base64 payload + MIME type for an image or file attachment.
///
/// Accepts either a `data:` URL or raw base64 (MIME inferred from filename).
[[nodiscard]] std::optional<DataUrlParts> attachmentBinary(const OpenAIInputAttachment& attachment);

/// @brief Text attachment rendered the same way the OpenAI client does.
[[nodiscard]] QString attachmentText(const OpenAIInputAttachment& attachment);

/// @brief Human-readable note for an attachment a provider cannot ingest.
[[nodiscard]] QString unsupportedAttachmentNote(const OpenAIInputAttachment& attachment);

/// @brief Bounded store of full conversation transcripts for stateless APIs.
///
/// Each stored transcript gets a synthetic response id. A later request that
/// names that id as `previous_response_id` resumes from the stored messages,
/// matching the OpenAI Responses chaining contract the panel relies on.
class ConversationHistoryCache {
public:
    explicit ConversationHistoryCache(QString id_prefix,
                                      int max_entries = kDefaultHistoryCacheEntries);

    [[nodiscard]] QString store(const QJsonArray& messages);
    [[nodiscard]] std::optional<QJsonArray> lookup(const QString& response_id) const;
    [[nodiscard]] bool owns(const QString& response_id) const;
    void clear();
    [[nodiscard]] int size() const { return static_cast<int>(m_entries.size()); }

private:
    QString m_id_prefix;
    int m_max_entries;
    QHash<QString, QJsonArray> m_entries;
    QStringList m_order;
};

}  // namespace sak::ai
