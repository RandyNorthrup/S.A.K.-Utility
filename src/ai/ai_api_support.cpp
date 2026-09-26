// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_api_support.h"

#include "sak/version.h"

#include <QJsonDocument>
#include <QJsonParseError>
#include <QSslConfiguration>
#include <QUuid>

#include <algorithm>
#include <utility>

namespace sak::ai {

QNetworkRequest vendorJsonRequest(const QUrl& url, int timeout_ms) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SAK-Utility/%1 AI").arg(QString::fromLatin1(get_version())));
    request.setRawHeader("Accept", "application/json");
    request.setTransferTimeout(timeout_ms);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    QSslConfiguration ssl = QSslConfiguration::defaultConfiguration();
    ssl.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(ssl);
    return request;
}

QByteArray compactJson(const QJsonObject& object) {
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

std::optional<QJsonObject> parseJsonObject(const QByteArray& data, QString* error_message) {
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message = parse_error.error != QJsonParseError::NoError
                                 ? parse_error.errorString()
                                 : QStringLiteral("Response is not a JSON object");
        }
        return std::nullopt;
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

std::optional<DataUrlParts> parseDataUrl(const QString& data_url) {
    const QString trimmed = data_url.trimmed();
    if (!trimmed.startsWith(QLatin1String("data:"), Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    const qsizetype comma = trimmed.indexOf(QChar(u','));
    if (comma < 0) {
        return std::nullopt;
    }
    const QString header = trimmed.mid(QStringLiteral("data:").size(),
                                       comma - QStringLiteral("data:").size());
    if (!header.endsWith(QLatin1String(";base64"), Qt::CaseInsensitive)) {
        return std::nullopt;
    }
    DataUrlParts parts;
    parts.mime_type = header.left(header.size() - QStringLiteral(";base64").size()).trimmed();
    parts.base64_data = trimmed.mid(comma + 1);
    if (parts.mime_type.isEmpty() || parts.base64_data.isEmpty()) {
        return std::nullopt;
    }
    return parts;
}

QString mimeTypeForFilename(const QString& filename) {
    static const QHash<QString, QString> kMimeBySuffix{
        {QStringLiteral("pdf"), QStringLiteral("application/pdf")},
        {QStringLiteral("png"), QStringLiteral("image/png")},
        {QStringLiteral("jpg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("jpeg"), QStringLiteral("image/jpeg")},
        {QStringLiteral("gif"), QStringLiteral("image/gif")},
        {QStringLiteral("webp"), QStringLiteral("image/webp")},
        {QStringLiteral("txt"), QStringLiteral("text/plain")},
        {QStringLiteral("log"), QStringLiteral("text/plain")},
        {QStringLiteral("md"), QStringLiteral("text/plain")},
        {QStringLiteral("csv"), QStringLiteral("text/plain")},
        {QStringLiteral("json"), QStringLiteral("text/plain")},
        {QStringLiteral("xml"), QStringLiteral("text/plain")},
        {QStringLiteral("ini"), QStringLiteral("text/plain")},
    };
    const qsizetype dot = filename.lastIndexOf(QChar(u'.'));
    if (dot < 0) {
        return {};
    }
    return kMimeBySuffix.value(filename.mid(dot + 1).trimmed().toLower());
}

std::optional<DataUrlParts> attachmentBinary(const OpenAIInputAttachment& attachment) {
    const QString source = attachment.type == OpenAIInputAttachment::Type::Image
                               ? attachment.image_url
                               : attachment.file_data;
    if (auto parts = parseDataUrl(source)) {
        return parts;
    }
    const QString mime = mimeTypeForFilename(attachment.filename);
    if (source.trimmed().isEmpty() || mime.isEmpty()) {
        return std::nullopt;
    }
    return DataUrlParts{mime, source.trimmed()};
}

QString attachmentText(const OpenAIInputAttachment& attachment) {
    return attachment.label.trimmed().isEmpty()
               ? attachment.text
               : QStringLiteral("Context: %1\n%2").arg(attachment.label, attachment.text);
}

QString unsupportedAttachmentNote(const OpenAIInputAttachment& attachment) {
    const QString name = attachment.filename.trimmed().isEmpty() ? attachment.label
                                                                 : attachment.filename;
    return QStringLiteral(
               "[Attachment %1 was not sent: this provider does not accept that file "
               "type. Ask the user to paste the relevant text instead.]")
        .arg(name.trimmed().isEmpty() ? QStringLiteral("(unnamed)") : name.trimmed());
}

ConversationHistoryCache::ConversationHistoryCache(QString id_prefix, int max_entries)
    : m_id_prefix(std::move(id_prefix)), m_max_entries(std::max(max_entries, 1)) {}

QString ConversationHistoryCache::store(const QJsonArray& messages) {
    const QString id = m_id_prefix +
                       QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QChar(u'-'));
    m_entries.insert(id, messages);
    m_order.append(id);
    while (m_order.size() > m_max_entries) {
        m_entries.remove(m_order.takeFirst());
    }
    return id;
}

std::optional<QJsonArray> ConversationHistoryCache::lookup(const QString& response_id) const {
    const auto it = m_entries.constFind(response_id.trimmed());
    if (it == m_entries.cend()) {
        return std::nullopt;
    }
    return it.value();
}

bool ConversationHistoryCache::owns(const QString& response_id) const {
    return response_id.trimmed().startsWith(m_id_prefix);
}

void ConversationHistoryCache::clear() {
    m_entries.clear();
    m_order.clear();
}

}  // namespace sak::ai
