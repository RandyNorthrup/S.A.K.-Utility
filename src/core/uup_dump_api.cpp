// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

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

#include "sak/layout_constants.h"
#include "sak/logger.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkRequest>
#include <QSslConfiguration>
#include <QUrlQuery>

#include <algorithm>
#include <cmath>
#include <limits>

constexpr int kGbDisplayPrecision = 2;
// The UUP dump JSON endpoints return small documents; a peer that streams past this
// ceiling is aborted rather than buffered into memory by readAll().
constexpr qint64 kMaxApiResponseBytes = 32LL * 1024 * 1024;

// --- Construction / Destruction ----------------------------------------------

UupDumpApi::UupDumpApi(QObject* parent)
    : QObject(parent), m_networkManager(new QNetworkAccessManager(this)) {
    sak::logInfo("UupDumpApi initialized");
}

UupDumpApi::~UupDumpApi() {
    cancelAll();
}

// --- Public API Methods -----------------------------------------------------

void UupDumpApi::fetchAvailableBuilds(const QString& arch, ReleaseChannel channel) {
    if (arch.isEmpty()) {
        const QString errorMsg = "Cannot fetch builds: no architecture was specified.";
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }
    sak::logInfo(QString("Fetching available builds for arch=%1, channel=%2")
                     .arg(arch, channelToDisplayName(channel))
                     .toStdString());

    // Use listid.php to search for builds matching architecture and channel
    const QString searchQuery = buildSearchQuery(arch, channel);

    QMap<QString, QString> params;
    params["search"] = searchQuery;
    params["sortByDate"] = "1";

    const QNetworkReply* reply = sendApiRequest("/listid.php", params);
    if (reply != nullptr) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onBuildsFetchReply);
    }
}

void UupDumpApi::listLanguages(const QString& updateId) {
    if (updateId.isEmpty()) {
        const QString errorMsg = "Cannot list languages: no build id was specified.";
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }
    sak::logInfo(QString("Fetching languages for build %1").arg(updateId).toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;

    const QNetworkReply* reply = sendApiRequest("/listlangs.php", params);
    if (reply != nullptr) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onLanguagesReply);
    }
}

void UupDumpApi::listEditions(const QString& updateId, const QString& lang) {
    if (updateId.isEmpty() || lang.isEmpty()) {
        const QString errorMsg = "Cannot list editions: a build id and a language are required.";
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }
    sak::logInfo(
        QString("Fetching editions for build %1, lang=%2").arg(updateId, lang).toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;
    params["lang"] = lang;

    const QNetworkReply* reply = sendApiRequest("/listeditions.php", params);
    if (reply != nullptr) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onEditionsReply);
    }
}

void UupDumpApi::getFiles(const QString& updateId, const QString& lang, const QString& edition) {
    if (updateId.isEmpty() || lang.isEmpty()) {
        const QString errorMsg =
            "Cannot fetch download files: a build id and a language are required.";
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }
    sak::logInfo(QString("Fetching download files for build %1, lang=%2, edition=%3")
                     .arg(updateId, lang, edition)
                     .toStdString());

    QMap<QString, QString> params;
    params["id"] = updateId;
    params["lang"] = lang;
    params["edition"] = edition;

    const QNetworkReply* reply = sendApiRequest("/get.php", params);
    if (reply != nullptr) {
        connect(reply, &QNetworkReply::finished, this, &UupDumpApi::onFilesReply);
    }
}

void UupDumpApi::cancelAll() {
    // abort() synchronously delivers finished(), whose slot calls
    // m_pendingReplies.removeOne(reply) -- mutating the very list we would be
    // range-iterating (iterator invalidation / UB). Snapshot and clear FIRST, then
    // abort from the copy; the slot's removeOne then no-ops on the empty list.
    const QList<QNetworkReply*> pending = m_pendingReplies;
    m_pendingReplies.clear();
    for (QNetworkReply* reply : pending) {
        if (reply != nullptr) {
            reply->abort();
            reply->deleteLater();
        }
    }
}

// --- Static Helpers ---------------------------------------------------------

QString UupDumpApi::channelToRing(ReleaseChannel channel) {
    switch (channel) {
    case ReleaseChannel::Retail:
        return "Retail";
    case ReleaseChannel::ReleasePreview:
        return "ReleasePreview";
    case ReleaseChannel::Beta:
        return "Beta";
    case ReleaseChannel::Dev:
        return "Dev";
    case ReleaseChannel::Canary:
        return "Canary";
    }
    return "Retail";
}

QString UupDumpApi::channelToDisplayName(ReleaseChannel channel) {
    switch (channel) {
    case ReleaseChannel::Retail:
        return "Public Release";
    case ReleaseChannel::ReleasePreview:
        return "Release Preview";
    case ReleaseChannel::Beta:
        return "Beta Channel";
    case ReleaseChannel::Dev:
        return "Dev Channel";
    case ReleaseChannel::Canary:
        return "Canary Channel";
    }
    return "Public Release";
}

QList<UupDumpApi::ReleaseChannel> UupDumpApi::allChannels() {
    return {ReleaseChannel::Retail,
            ReleaseChannel::ReleasePreview,
            ReleaseChannel::Beta,
            ReleaseChannel::Dev,
            ReleaseChannel::Canary};
}

// --- Reply Handlers ---------------------------------------------------------

std::optional<UupDumpApi::BuildInfo> UupDumpApi::parseBuildEntry(const QString& key,
                                                                 const QJsonObject& buildObj) {
    BuildInfo info;
    // The uuid is inside the build object; the key is only a numeric array index. This
    // used to fall back to the key when the uuid was missing, which handed callers a
    // build id that identifies nothing - every later get.php request built from it asks
    // the API for an index. Skip the entry instead: a build we cannot address is not a
    // build we can offer.
    info.uuid = buildObj["uuid"].toString();
    if (info.uuid.isEmpty()) {
        sak::logWarning("UUP dump: skipping build entry '{}' with no uuid", key.toStdString());
        return std::nullopt;
    }
    info.title = buildObj["title"].toString();
    info.build = buildObj["build"].toString();
    info.arch = buildObj["arch"].toString();
    // A wrong-typed or out-of-range "created" must not invoke double->qint64 UB: a value
    // that is not a finite, in-range timestamp sorts as oldest (0) instead of corrupting.
    const double created = buildObj["created"].toDouble();
    info.created = (std::isfinite(created) &&
                    created >= static_cast<double>(std::numeric_limits<qint64>::min()) &&
                    created < static_cast<double>(std::numeric_limits<qint64>::max()))
                       ? static_cast<qint64>(created)
                       : 0;
    return info;
}

void UupDumpApi::onBuildsFetchReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        sak::logError("onBuildsFetchReply: sender is not a QNetworkReply -- signal/slot mismatch");
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString errorMsg =
            QString("Network error fetching builds: %1").arg(reply->errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    const QByteArray data = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        const QString errorMsg = QString("JSON parse error: %1").arg(parseError.errorString());
        sak::logError(errorMsg.toStdString());
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
        if (auto info = parseBuildEntry(it.key(), it.value().toObject())) {
            builds.append(*info);
        }
    }

    // Sort by creation date (newest first)
    std::ranges::sort(builds,
                      [](const BuildInfo& a, const BuildInfo& b) { return a.created > b.created; });

    // An empty set is not a successful result: a garbage or unexpected response yields no
    // usable build, so fail closed the way the language/edition handlers do rather than
    // emitting an empty success.
    if (builds.isEmpty()) {
        sak::logWarning("API returned no usable builds");
        Q_EMIT apiError("No builds available for this architecture/channel.");
        return;
    }

    sak::logInfo(QString("Fetched %1 available builds").arg(builds.size()).toStdString());
    Q_EMIT buildsFetched(builds);
}

QMap<QString, QString> UupDumpApi::parseLangFancyNames(const QJsonObject& response) {
    QJsonObject langFancyNames = response["langFancyNames"].toObject();
    QMap<QString, QString> langNames;
    for (auto it = langFancyNames.begin(); it != langFancyNames.end(); ++it) {
        langNames[it.key()] = it.value().toString();
    }
    return langNames;
}

void UupDumpApi::onLanguagesReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        sak::logError("onLanguagesReply: sender is not a QNetworkReply -- signal/slot mismatch");
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString errorMsg =
            QString("Network error fetching languages: %1").arg(reply->errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    const QByteArray data = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        const QString errorMsg =
            QString("JSON parse error in languages response: %1").arg(parseError.errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching languages")) {
        return;
    }

    // langList can be a JSON array ["en-us", ...] or object {"en-us": 1, ...}
    QStringList langCodes;
    const QJsonValue langListVal = response["langList"];
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

    QMap<QString, QString> langNames = parseLangFancyNames(response);

    // Sort by friendly name for better UX
    std::ranges::sort(langCodes, [&langNames](const QString& a, const QString& b) {
        return langNames.value(a, a) < langNames.value(b, b);
    });

    if (langCodes.isEmpty()) {
        sak::logWarning("API returned empty language list");
        Q_EMIT apiError("No languages available for this build.");
        return;
    }

    sak::logInfo(QString("Fetched %1 available languages").arg(langCodes.size()).toStdString());
    Q_EMIT languagesFetched(langCodes, langNames);
}

void UupDumpApi::onEditionsReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        sak::logError("onEditionsReply: sender is not a QNetworkReply -- signal/slot mismatch");
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString errorMsg =
            QString("Network error fetching editions: %1").arg(reply->errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QJsonObject response;
    if (!parseApiResponse(reply->readAll(), "editions", response)) {
        return;
    }

    const QStringList editions = parseEditionList(response["editionList"]);

    QJsonObject editionFancyNames = response["editionFancyNames"].toObject();
    QMap<QString, QString> editionNames;
    for (auto it = editionFancyNames.begin(); it != editionFancyNames.end(); ++it) {
        editionNames[it.key()] = it.value().toString();
    }

    if (editions.isEmpty()) {
        sak::logWarning("API returned empty edition list");
        Q_EMIT apiError("No editions available for this build/language.");
        return;
    }

    sak::logInfo(QString("Fetched %1 available editions").arg(editions.size()).toStdString());
    Q_EMIT editionsFetched(editions, editionNames);
}

void UupDumpApi::onFilesReply() {
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (reply == nullptr) {
        sak::logError("onFilesReply: sender is not a QNetworkReply -- signal/slot mismatch");
        return;
    }

    m_pendingReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() != QNetworkReply::NoError) {
        const QString errorMsg =
            QString("Network error fetching files: %1").arg(reply->errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    const QByteArray data = reply->readAll();
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        const QString errorMsg =
            QString("JSON parse error in files response: %1").arg(parseError.errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return;
    }

    QJsonObject root = doc.object();
    QJsonObject response = root["response"].toObject();

    if (checkApiError(response, "fetching files")) {
        return;
    }

    const QString updateName = response["updateName"].toString();

    QList<FileInfo> files;
    if (!collectValidFiles(response["files"].toObject(), files)) {
        return;  // incomplete set -- apiError already emitted
    }

    Q_EMIT filesFetched(updateName, files);
}

bool UupDumpApi::collectValidFiles(const QJsonObject& filesObj, QList<FileInfo>& out) {
    qint64 totalSize = 0;
    for (auto it = filesObj.begin(); it != filesObj.end(); ++it) {
        auto maybeInfo = parseAndValidateFileEntry(it.key(), it.value().toObject());
        if (maybeInfo.has_value()) {
            const qint64 entrySize = maybeInfo.value().size;
            out.append(maybeInfo.value());
            // Saturate rather than let attacker-controlled positive sizes overflow signed
            // qint64 (UB); the running total is only used for the GB log line below.
            totalSize = (entrySize > std::numeric_limits<qint64>::max() - totalSize)
                            ? std::numeric_limits<qint64>::max()
                            : totalSize + entrySize;
        }
    }

    // An empty or all-rejected file object is not a buildable set: a missing/wrong-typed
    // response.files must fail closed, not report success with nothing to build.
    if (out.isEmpty()) {
        const QString errorMsg =
            "UUP file set is empty or contained no usable entries. Refusing to build.";
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return false;
    }

    // A UUP ISO needs every listed file; silently dropping unsafe/unverifiable
    // entries and reporting success would build from an incomplete set. Refuse.
    const int droppedCount = static_cast<int>(filesObj.size() - out.size());
    if (droppedCount > 0) {
        const QString errorMsg =
            QString(
                "UUP file set incomplete: %1 of %2 entries rejected (unsafe, bad URL, or "
                "missing/invalid SHA-1). Refusing to build from a partial set.")
                .arg(droppedCount)
                .arg(filesObj.size());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return false;
    }

    const double totalSizeGB = static_cast<double>(totalSize) / sak::kBytesPerGBf;
    sak::logInfo(QString("Fetched %1 downloadable files (%2 GB total)")
                     .arg(out.size())
                     .arg(totalSizeGB, 0, 'f', kGbDisplayPrecision)
                     .toStdString());
    return true;
}

// --- Private Helpers --------------------------------------------------------

bool UupDumpApi::isValidSha1(const QString& sha1) {
    constexpr int kSha1HexLength = 40;
    if (sha1.size() != kSha1HexLength) {
        return false;
    }
    return std::ranges::all_of(sha1, [](const QChar ch) {
        return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
               (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
               (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
    });
}

namespace {

// Code units below the first printable ASCII character are C0 control characters (incl. NUL);
// 0x7f is DEL.
constexpr char16_t kFirstPrintableCodeUnit = 0x20;
constexpr char16_t kDeleteControlCodeUnit = 0x7f;

// True if @p s holds any C0 control character (incl. NUL) or DEL. Such a byte in a field
// serialized into the aria2 input file could truncate the line or break out of the record.
bool hasAria2ControlChar(const QString& s) {
    return std::ranges::any_of(s, [](const QChar ch) {
        return ch.unicode() < kFirstPrintableCodeUnit || ch.unicode() == kDeleteControlCodeUnit;
    });
}

// True if @p fileName is a Windows reserved device name (CON, NUL, COM1, ...), with or without an
// extension: aria2 out=con / out=con.esd would open the console device instead of creating a file.
bool isReservedDosDeviceName(const QString& fileName) {
    const int dot = static_cast<int>(fileName.indexOf(QLatin1Char('.')));
    const QString base = (dot < 0 ? fileName : fileName.left(dot)).toUpper();
    static const QStringList kReserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return kReserved.contains(base);
}

// The out= name must not escape the download dir, alias another file, or normalize (via a trailing
// dot/space Windows trims) to a different on-disk name than the one we integrity-checked.
bool isUnsafeAria2FileName(const QString& fileName) {
    if (fileName.isEmpty() || fileName.contains(QLatin1String("..")) ||
        fileName.contains(QLatin1Char('/')) || fileName.contains(QLatin1Char('\\')) ||
        fileName.contains(QLatin1Char(':'))) {
        return true;  // out= path confinement / drive / NTFS ADS
    }
    const QChar last = fileName.back();
    return last == QLatin1Char('.') || last == QLatin1Char(' ') ||
           isReservedDosDeviceName(fileName);
}

}  // namespace

bool UupDumpApi::isSafeAria2FileEntry(const FileInfo& info) {
    if (isUnsafeAria2FileName(info.fileName)) {
        return false;
    }
    // A control character (incl. NUL, CR, LF, TAB) in ANY serialized field could truncate the
    // aria2 input line or break out of the record to inject an out=/uri directive.
    return !hasAria2ControlChar(info.fileName) && !hasAria2ControlChar(info.url) &&
           !hasAria2ControlChar(info.sha1);
}

// The download URL must parse, be HTTPS (or plain HTTP only for Microsoft's UUP CDN, which serves
// no HTTPS but is SHA-1 integrity-checked), and be non-empty. Logs the specific reason; a false
// here trips the incomplete-set refusal rather than shipping an unfetchable entry.
bool UupDumpApi::isAcceptableDownloadUrl(const FileInfo& info) {
    // Validate download URL scheme
    const QUrl downloadUrl(info.url);
    if (!downloadUrl.isValid()) {
        sak::logWarning("Rejected invalid download URL for: " + info.fileName.toStdString());
        return false;
    }

    const QString scheme = downloadUrl.scheme().toLower();
    const QString host = downloadUrl.host().toLower();
    if (scheme == "http" && host.endsWith(".microsoft.com")) {
        // Microsoft's UUP CDN does not support HTTPS -- allow HTTP since
        // every file is integrity-verified via SHA-1 checksums.
        sak::logDebug("Allowing HTTP Microsoft CDN URL for: " + info.fileName.toStdString());
    } else if (scheme != "https") {
        sak::logWarning("Rejected non-HTTPS download URL for: " + info.fileName.toStdString());
        return false;
    }

    // Only include files with valid download URLs
    if (info.url.isEmpty() || info.url == "null") {
        return false;
    }
    return true;
}

std::optional<UupDumpApi::FileInfo> UupDumpApi::parseAndValidateFileEntry(
    const QString& key, const QJsonObject& fileObj) {
    FileInfo info;
    info.fileName = key;
    info.sha1 = fileObj["sha1"].toString();
    bool sizeOk = false;
    info.size = fileObj["size"].toString().toLongLong(&sizeOk);
    info.url = fileObj["url"].toString();
    info.uuid = fileObj["uuid"].toString();
    info.expire = fileObj["expire"].toString();

    // A missing, non-numeric, overflowed, or negative size is not a valid file record: reject
    // it (which then trips the incomplete-set refusal) instead of accepting a coerced 0.
    if (!sizeOk || info.size < 0) {
        sak::logWarning("Rejected file entry with missing/invalid size: " +
                        info.fileName.toStdString());
        return std::nullopt;
    }

    // Reject entries that could inject aria2 directives or escape the download dir when
    // serialized into the aria2 input file (traversal in fileName, CR/LF/TAB in any field).
    if (!isSafeAria2FileEntry(info)) {
        sak::logWarning("Rejected unsafe file entry from API: " + info.fileName.toStdString());
        return std::nullopt;
    }

    if (!isAcceptableDownloadUrl(info)) {
        return std::nullopt;
    }

    // Require a well-formed SHA-1: the Microsoft CDN is served over plain HTTP
    // (allowed above) purely because each file is integrity-verified. A missing
    // or malformed checksum means we cannot verify it -- reject rather than ship
    // an unverifiable file into the ISO build.
    if (!isValidSha1(info.sha1)) {
        sak::logWarning("Rejected file entry with missing/invalid SHA-1: " +
                        info.fileName.toStdString());
        return std::nullopt;
    }

    return info;
}

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
    request.setRawHeader("User-Agent", "SAK-Utility/1.0");

    // Use TLS 1.2+
    QSslConfiguration sslConfig = QSslConfiguration::defaultConfiguration();
    sslConfig.setProtocol(QSsl::TlsV1_2OrLater);
    request.setSslConfiguration(sslConfig);

    sak::logInfo(QString("API request: %1").arg(url.toString()).toStdString());

    QNetworkReply* reply = m_networkManager->get(request);
    // Fail closed on an oversized response: abort once the peer streams past the ceiling so
    // the reply handlers' readAll() can never buffer an unbounded body into memory.
    connect(reply, &QNetworkReply::downloadProgress, reply, [reply](qint64 received, qint64 total) {
        if (received > kMaxApiResponseBytes || total > kMaxApiResponseBytes) {
            reply->abort();
        }
    });
    m_pendingReplies.append(reply);
    return reply;
}

bool UupDumpApi::parseApiResponse(const QByteArray& data,
                                  const QString& context,
                                  QJsonObject& response) {
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        const QString errorMsg =
            QString("JSON parse error in %1 response: %2").arg(context, parseError.errorString());
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return false;
    }

    response = doc.object()["response"].toObject();
    return !checkApiError(response, "fetching " + context);
}

QStringList UupDumpApi::parseEditionList(const QJsonValue& edListVal) const {
    QStringList editions;
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
    return editions;
}

bool UupDumpApi::checkApiError(const QJsonObject& response, const QString& context) {
    if (response.contains("error")) {
        const QString errorCode = response["error"].toString();
        const QString errorMsg = QString("UUP dump API error while %1: %2").arg(context, errorCode);
        sak::logError(errorMsg.toStdString());
        Q_EMIT apiError(errorMsg);
        return true;
    }
    return false;
}

QString UupDumpApi::buildSearchQuery(const QString& arch, ReleaseChannel channel) const {
    // fetchAvailableBuilds, the only caller, rejects an empty arch before reaching here.
    Q_ASSERT(!arch.isEmpty());
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
