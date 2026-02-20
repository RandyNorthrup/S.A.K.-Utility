// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

/**
 * @file uup_dump_api.cpp
 * @brief REST API client for the UUP dump service
 *
 * Implements asynchronous HTTP GET requests to the UUP dump JSON API
 * (api.uupdump.net) for querying Windows builds, languages, editions,
 * and obtaining direct download links for UUP files.
 *
 * All download URLs point to Microsoft's official CDN servers.
 * File integrity is verified via SHA-1 checksums provided by the API.
 */

#include "sak/uup_dump_api.h"
#include "sak/logger.h"

#include <QNetworkRequest>
#include <QUrlQuery>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSslConfiguration>

// ─── Construction / Destruction ──────────────────────────────────────────────

UupDumpApi::UupDumpApi(QObject* parent)
    : QObject(parent)
    , m_networkManager(new QNetworkAccessManager(this))
{
    sak::log_info("UupDumpApi initialized");
}

UupDumpApi::~UupDumpApi() {
    cancelAll();
}

// ─── Public API Methods ─────────────────────────────────────────────────────

void UupDumpApi::fetchAvailableBuilds(const QString& arch, ReleaseChannel channel) {
    sak::log_info(QString("Fetching available builds for arch=%1, channel=%2")
        .arg(arch, channelToDisplayName(channel)).toStdString());

    // Use listid.php to search for builds matching architecture and channel
    QString searchQuery = buildSearchQuery(arch, channel);

    QMap<QString, QString> params;
    params["search"] = searchQuery;
    params["sortByDate"] = "1";

    QNetworkReply* reply = sendApiRequest("/listid.php", params);
    if (reply) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onBuildsFetchReply);
    }
}

void UupDumpApi::listLanguages(const QString& updateId) {
    sak::log_info(QString("Fetching languages for build %1").arg(updateId).toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;

    QNetworkReply* reply = sendApiRequest("/listlangs.php", params);
    if (reply) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onLanguagesReply);
    }
}

void UupDumpApi::listEditions(const QString& updateId, const QString& lang) {
    sak::log_info(QString("Fetching editions for build %1, lang=%2")
        .arg(updateId, lang).toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;
    params["lang"] = lang;

    QNetworkReply* reply = sendApiRequest("/listeditions.php", params);
    if (reply) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onEditionsReply);
    }
}

void UupDumpApi::getFiles(const QString& updateId, const QString& lang, const QString& edition) {
    sak::log_info(QString("Fetching download files for build %1, lang=%2, edition=%3")
        .arg(updateId, lang, edition).toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;
    params["lang"] = lang;
    params["edition"] = edition;

    QNetworkReply* reply = sendApiRequest("/get.php", params);
    if (reply) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onFilesReply);
    }
}

void UupDumpApi::cancelAll() {
    for (QNetworkReply* reply : m_pendingReplies) {
        if (reply) {
            reply->abort();
            reply->deleteLater();
        }
    }
    m_pendingReplies.clear();
}

// ─── Static Helpers ─────────────────────────────────────────────────────────

QString UupDumpApi::channelToRing(ReleaseChannel channel) {
    switch (channel) {
        case ReleaseChannel::Retail:         return "Retail";
        case ReleaseChannel::ReleasePreview: return "ReleasePreview";
        case ReleaseChannel::Beta:           return "Beta";
        case ReleaseChannel::Dev:            return "Dev";
        case ReleaseChannel::Canary:         return "Canary";
    }
    return "Retail";
}

QString UupDumpApi::channelToDisplayName(ReleaseChannel channel) {
    switch (channel) {
        case ReleaseChannel::Retail:         return "Public Release";
        case ReleaseChannel::ReleasePreview: return "Release Preview";
        case ReleaseChannel::Beta:           return "Beta Channel";
        case ReleaseChannel::Dev:            return "Dev Channel";
        case ReleaseChannel::Canary:         return "Canary Channel";
    }
    return "Public Release";
}

QList<UupDumpApi::ReleaseChannel> UupDumpApi::allChannels() {
    return {
        ReleaseChannel::Retail,
        ReleaseChannel::ReleasePreview,
        ReleaseChannel::Beta,
        ReleaseChannel::Dev,
        ReleaseChannel::Canary
    };
}

// ─── Reply Handlers ─────────────────────────────────────────────────────────

void UupDumpApi::onBuildsFetchReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error fetching builds: %1").arg(reply->errorString());
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        QString errorMsg = QString("JSON parse error: %1").arg(parseError.errorString());
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching builds")) {
        return;
    }

    QJsonObject buildsObj = response["builds"].toObject();
    QList<BuildInfo> builds;

    for (auto it = buildsObj.begin(); it != buildsObj.end(); ++it) {
        QJsonObject buildObj = it.value().toObject();
        BuildInfo info;
        // The uuid is inside the build object, not the key (key is a numeric index)
        info.uuid = buildObj["uuid"].toString();
        if (info.uuid.isEmpty()) {
            info.uuid = it.key(); // fallback
        }
        info.title = buildObj["title"].toString();
        info.build = buildObj["build"].toString();
        info.arch = buildObj["arch"].toString();
        info.created = static_cast<qint64>(buildObj["created"].toDouble());
        builds.append(info);
    }

    // Sort by creation date (newest first)
    std::sort(builds.begin(), builds.end(), [](const BuildInfo& a, const BuildInfo& b) {
        return a.created > b.created;
    });

    sak::log_info(QString("Fetched %1 available builds").arg(builds.size()).toStdString());
    Q_EMIT buildsFetched(builds);
}

void UupDumpApi::onLanguagesReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error fetching languages: %1").arg(reply->errorString());
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        Q_EMIT apiError(QString("JSON parse error: %1").arg(parseError.errorString()));
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching languages")) {
        return;
    }

    // langList can be a JSON array ["en-us", ...] or object {"en-us": 1, ...}
    QStringList langCodes;
    QJsonValue langListVal = response["langList"];
    if (langListVal.isArray()) {
        for (const QJsonValue& val : langListVal.toArray()) {
            langCodes.append(val.toString());
        }
    } else if (langListVal.isObject()) {
        QJsonObject langListObj = langListVal.toObject();
        for (auto it = langListObj.begin(); it != langListObj.end(); ++it) {
            langCodes.append(it.key());
        }
    }

    QJsonObject langFancyNames = response["langFancyNames"].toObject();
    QMap<QString, QString> langNames;
    for (auto it = langFancyNames.begin(); it != langFancyNames.end(); ++it) {
        langNames[it.key()] = it.value().toString();
    }

    // Sort by friendly name for better UX
    std::sort(langCodes.begin(), langCodes.end(), [&langNames](const QString& a, const QString& b) {
        return langNames.value(a, a) < langNames.value(b, b);
    });

    if (langCodes.isEmpty()) {
        sak::log_warning("API returned empty language list");
        Q_EMIT apiError("No languages available for this build.");
        return;
    }

    sak::log_info(QString("Fetched %1 available languages").arg(langCodes.size()).toStdString());
    Q_EMIT languagesFetched(langCodes, langNames);
}

void UupDumpApi::onEditionsReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error fetching editions: %1").arg(reply->errorString());
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        Q_EMIT apiError(QString("JSON parse error: %1").arg(parseError.errorString()));
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching editions")) {
        return;
    }

    // editionList can be a JSON array ["PROFESSIONAL", ...] or object {"PROFESSIONAL": 1, ...}
    QStringList editions;
    QJsonValue edListVal = response["editionList"];
    if (edListVal.isArray()) {
        for (const QJsonValue& val : edListVal.toArray()) {
            editions.append(val.toString());
        }
    } else if (edListVal.isObject()) {
        QJsonObject edListObj = edListVal.toObject();
        for (auto it = edListObj.begin(); it != edListObj.end(); ++it) {
            editions.append(it.key());
        }
    }

    QJsonObject editionFancyNames = response["editionFancyNames"].toObject();
    QMap<QString, QString> editionNames;
    for (auto it = editionFancyNames.begin(); it != editionFancyNames.end(); ++it) {
        editionNames[it.key()] = it.value().toString();
    }

    if (editions.isEmpty()) {
        sak::log_warning("API returned empty edition list");
        Q_EMIT apiError("No editions available for this build/language.");
        return;
    }

    sak::log_info(QString("Fetched %1 available editions").arg(editions.size()).toStdString());
    Q_EMIT editionsFetched(editions, editionNames);
}

void UupDumpApi::onFilesReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        QString errorMsg = QString("Network error fetching files: %1").arg(reply->errorString());
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QByteArray data = reply->readAll();
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        Q_EMIT apiError(QString("JSON parse error: %1").arg(parseError.errorString()));
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching files")) {
        return;
    }

    QString updateName = response["updateName"].toString();
    QJsonObject filesObj = response["files"].toObject();

    QList<FileInfo> files;
    qint64 totalSize = 0;

    for (auto it = filesObj.begin(); it != filesObj.end(); ++it) {
        QJsonObject fileObj = it.value().toObject();
        FileInfo info;
        info.fileName = it.key();
        info.sha1 = fileObj["sha1"].toString();
        info.size = fileObj["size"].toString().toLongLong();
        info.url = fileObj["url"].toString();
        info.uuid = fileObj["uuid"].toString();
        info.expire = fileObj["expire"].toString();

        // Sanitize filename — reject path traversal attempts
        if (info.fileName.contains("..") || info.fileName.contains('/') ||
            info.fileName.contains('\\')) {
            sak::log_warning("Rejected unsafe filename from API: " + info.fileName.toStdString());
            continue;
        }

        // Validate download URL scheme — only allow HTTPS
        QUrl downloadUrl(info.url);
        if (!downloadUrl.isValid() || downloadUrl.scheme().toLower() != "https") {
            sak::log_warning("Rejected non-HTTPS download URL for: " + info.fileName.toStdString());
            continue;
        }

        // Only include files with valid download URLs
        if (!info.url.isEmpty() && info.url != "null") {
            files.append(info);
            totalSize += info.size;
        }
    }

    double totalSizeGB = totalSize / (1024.0 * 1024.0 * 1024.0);
    sak::log_info(QString("Fetched %1 downloadable files (%.2f GB total)")
        .arg(files.size()).arg(totalSizeGB).toStdString());

    Q_EMIT filesFetched(updateName, files);
}

// ─── Private Helpers ────────────────────────────────────────────────────────

QNetworkReply* UupDumpApi::sendApiRequest(const QString& endpoint,
                                           const QMap<QString, QString>& params) {
    QUrl url(QString("%1%2").arg(API_BASE_URL, endpoint));
    QUrlQuery query;

    for (auto it = params.begin(); it != params.end(); ++it) {
        query.addQueryItem(it.key(), it.value());
    }
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Accept", "application/json");

    // Use TLS 1.2+
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    sak::log_info(QString("API request: %1").arg(url.toString()).toStdString());

    QNetworkReply* reply = m_networkManager->get(request);
    m_pendingReplies.append(reply);
    return reply;
}

bool UupDumpApi::checkApiError(const QJsonObject& response, const QString& context) {
    if (response.contains("error")) {
        QString errorCode = response["error"].toString();
        QString errorMsg = QString("UUP dump API error while %1: %2").arg(context, errorCode);
        sak::log_error(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return true;
    }
    return false;
}

QString UupDumpApi::buildSearchQuery(const QString& arch, ReleaseChannel channel) const {
    // Build a search query that filters builds by architecture and channel
    // The listid.php endpoint does text search on build titles
    QString query;

    // Filter by architecture
    if (arch == "amd64" || arch == "x64") {
        query = "amd64";
    } else if (arch == "arm64") {
        query = "arm64";
    }

    // Add channel-specific search terms
    switch (channel) {
        case ReleaseChannel::Retail:
            // Retail builds have version numbers like "24H2", "23H2" in their titles
            // Search for the latest Windows 11 release
            query = "windows 11 " + query;
            break;
        case ReleaseChannel::ReleasePreview:
            query = "Release Preview " + query;
            break;
        case ReleaseChannel::Beta:
            query = "Beta " + query;
            break;
        case ReleaseChannel::Dev:
            query = "Dev " + query;
            break;
        case ReleaseChannel::Canary:
            query = "Canary " + query;
            break;
    }

    return query.trimmed();
}
