// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_api_client.cpp
/// @brief NuGet v2 OData API client implementation

#include "sak/nuget_api_client.h"

#include "sak/logger.h"
#include "sak/offline_deployment_constants.h"

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace sak {

// ============================================================================
// Construction / Destruction
// ============================================================================

NuGetApiClient::NuGetApiClient(QObject* parent)
    : QObject(parent), m_network_manager(new QNetworkAccessManager(this)), m_owns_nam(true) {
    m_network_manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

NuGetApiClient::NuGetApiClient(QNetworkAccessManager* shared_nam, QObject* parent)
    : QObject(parent), m_network_manager(shared_nam), m_owns_nam(false) {
    Q_ASSERT(shared_nam);
    m_network_manager->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

NuGetApiClient::~NuGetApiClient() {
    cancel();
}

// ============================================================================
// Public API
// ============================================================================

void NuGetApiClient::searchPackages(const QString& query, int max_results) {
    if (m_cancelled) {
        return;
    }

    // Build OData URL manually — QUrlQuery double-encodes $-prefixed params
    // and single quotes that the NuGet v2 API requires.
    // Encode search term + single quotes (%27) for OData string literals.
    // Use QUrl::fromEncoded() so Qt does not re-interpret our percent-encoding.
    QString encoded_query = QString(QUrl::toPercentEncoding(query));
    QString url_str = QString(
                          "%1%2?$filter=IsLatestVersion&searchTerm=%27%3%27"
                          "&targetFramework=%27%27&includePrerelease=false"
                          "&$top=%4&$orderby=DownloadCount%20desc")
                          .arg(offline::kNuGetBaseUrl,
                               offline::kNuGetSearchPath,
                               encoded_query,
                               QString::number(max_results));

    sak::logInfo("[NuGetApiClient] Search: {}", url_str.toStdString());

    QUrl parsed_url = QUrl::fromEncoded(url_str.toUtf8());
    sak::logInfo("[NuGetApiClient] Wire URL: {}",
                 QString::fromUtf8(parsed_url.toEncoded()).toStdString());

    QNetworkRequest request{parsed_url};
    request.setHeader(QNetworkRequest::UserAgentHeader, "SAK-Utility/1.0");
    request.setRawHeader("Accept", "application/xml");
    request.setTransferTimeout(offline::kApiRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);

    auto* reply = m_network_manager->get(request);
    m_pending_ops++;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleSearchReply(reply); });
}

void NuGetApiClient::getPackageMetadata(const QString& package_id, const QString& version) {
    if (m_cancelled) {
        return;
    }

    QString url_str;
    if (version.isEmpty()) {
        url_str = QString("%1%2(Id=%27%3%27,Version=%27%27)")
                      .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagesPath, package_id);
    } else {
        url_str =
            QString("%1%2(Id=%27%3%27,Version=%27%4%27)")
                .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagesPath, package_id, version);
    }

    sak::logInfo("[NuGetApiClient] Metadata: {}", url_str.toStdString());

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    m_pending_ops++;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataReply(reply); });
}

void NuGetApiClient::getPackageVersions(const QString& package_id) {
    if (m_cancelled) {
        return;
    }

    // Build FindPackagesById URL manually — QUrlQuery double-encodes OData quotes.
    QString encoded_id = QString(QUrl::toPercentEncoding(package_id));
    QString url_str = QString("%1%2?id=%27%3%27")
                          .arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath, encoded_id);

    sak::logInfo("[NuGetApiClient] Versions: {}", url_str.toStdString());

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    m_pending_ops++;

    connect(reply, &QNetworkReply::finished, this, [this, reply, package_id]() {
        handleVersionsReply(reply, package_id);
    });
}

void NuGetApiClient::downloadNupkg(const QString& package_id,
                                   const QString& version,
                                   const QString& output_dir) {
    if (m_cancelled) {
        return;
    }

    QString url_str =
        QString("%1%2%3/%4")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetPackagePath, package_id, version);

    sak::logInfo("[NuGetApiClient] Download nupkg: {}", url_str.toStdString());

    QDir dir(output_dir);
    if (!dir.exists()) {
        dir.mkpath(".");
    }

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    m_pending_ops++;

    connect(reply,
            &QNetworkReply::downloadProgress,
            this,
            [this, package_id](qint64 received, qint64 total) {
                Q_EMIT downloadProgress(package_id, received, total);
            });

    connect(reply, &QNetworkReply::finished, this, [this, reply, package_id, output_dir]() {
        handleDownloadReply(reply, package_id, output_dir);
    });
}

void NuGetApiClient::resolveDependencies(const QString& package_id, const QString& version) {
    if (m_cancelled) {
        return;
    }

    m_resolved_deps.clear();
    m_deps_to_resolve.clear();
    m_visited_deps.clear();
    m_dependency_depth = 0;

    m_deps_to_resolve.append(package_id);
    m_visited_deps.insert(package_id.toLower());

    sak::logInfo("[NuGetApiClient] Resolving dependencies for {} {}",
                 package_id.toStdString(),
                 version.toStdString());

    resolveNextDependency();
}

void NuGetApiClient::cancel() {
    m_cancelled = true;
}

bool NuGetApiClient::isBusy() const {
    return m_pending_ops > 0;
}

// ============================================================================
// Reply Handlers
// ============================================================================

void NuGetApiClient::handleSearchReply(QNetworkReply* reply) {
    m_pending_ops--;
    reply->deleteLater();

    if (m_cancelled) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        int status_code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        QByteArray body = reply->readAll();
        QString wire_url = QString::fromUtf8(reply->url().toEncoded());

        sak::logWarning("[NuGetApiClient] Search error: HTTP {} {} | {} | URL: {}",
                        status_code,
                        reason.toStdString(),
                        reply->errorString().toStdString(),
                        wire_url.toStdString());
        if (!body.isEmpty()) {
            sak::logWarning("[NuGetApiClient] Response body (first 500 chars): {}",
                            body.left(500).toStdString());
        }

        Q_EMIT errorOccurred("search", reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    auto results = parseODataFeed(data);

    sak::logInfo("[NuGetApiClient] Search returned {} results", static_cast<int>(results.size()));
    Q_EMIT searchComplete(results);
}

void NuGetApiClient::handleMetadataReply(QNetworkReply* reply) {
    m_pending_ops--;
    reply->deleteLater();

    if (m_cancelled) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString error_msg = reply->errorString();
        sak::logWarning("[NuGetApiClient] Metadata error: {}", error_msg.toStdString());
        Q_EMIT errorOccurred("metadata", error_msg);
        return;
    }

    QByteArray data = reply->readAll();
    QDomDocument doc;
    QString parse_error;
    if (!doc.setContent(data, &parse_error)) {
        sak::logWarning("[NuGetApiClient] XML parse error: {}", parse_error.toStdString());
        Q_EMIT errorOccurred("metadata", "XML parse error: " + parse_error);
        return;
    }

    QDomElement root = doc.documentElement();
    QDomElement entry = root;
    if (root.tagName() == "feed") {
        entry = root.firstChildElement("entry");
    }

    if (entry.isNull()) {
        Q_EMIT errorOccurred("metadata", "No entry found in response");
        return;
    }

    auto metadata = parseODataEntry(entry);
    Q_EMIT metadataReady(metadata);
}

void NuGetApiClient::handleVersionsReply(QNetworkReply* reply, const QString& package_id) {
    m_pending_ops--;
    reply->deleteLater();

    if (m_cancelled) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        Q_EMIT errorOccurred("versions", reply->errorString());
        return;
    }

    QByteArray data = reply->readAll();
    auto packages = parseODataFeed(data);

    QStringList versions;
    versions.reserve(packages.size());
    for (const auto& pkg : packages) {
        if (!pkg.version.isEmpty()) {
            versions.append(pkg.version);
        }
    }

    Q_EMIT versionsReady(package_id, versions);
}

void NuGetApiClient::handleDownloadReply(QNetworkReply* reply,
                                         const QString& package_id,
                                         const QString& output_dir) {
    m_pending_ops--;
    reply->deleteLater();

    if (m_cancelled) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        QString error_msg = reply->errorString();
        sak::logWarning("[NuGetApiClient] Download error for {}: {}",
                        package_id.toStdString(),
                        error_msg.toStdString());
        Q_EMIT errorOccurred("download", error_msg);
        return;
    }

    QByteArray data = reply->readAll();
    if (data.isEmpty()) {
        Q_EMIT errorOccurred("download", "Empty response for " + package_id);
        return;
    }

    // Determine filename from Content-Disposition or construct from package_id
    QString filename;
    QString disposition = reply->header(QNetworkRequest::ContentDispositionHeader).toString();
    if (!disposition.isEmpty() && disposition.contains("filename=")) {
        int start = disposition.indexOf("filename=") + 9;
        filename = disposition.mid(start).trimmed();
        filename.remove('"');
    }
    if (filename.isEmpty()) {
        filename = package_id + ".nupkg";
    }

    QString file_path = QDir(output_dir).filePath(filename);
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        sak::logError("[NuGetApiClient] Cannot write to: {}", file_path.toStdString());
        Q_EMIT errorOccurred("download", "Cannot write file: " + file_path);
        return;
    }

    file.write(data);
    file.close();

    sak::logInfo("[NuGetApiClient] Downloaded {} ({} bytes)", file_path.toStdString(), data.size());
    Q_EMIT downloadComplete(package_id, file_path);
}

// ============================================================================
// Dependency Resolution
// ============================================================================

void NuGetApiClient::resolveNextDependency() {
    if (m_cancelled || m_deps_to_resolve.isEmpty()) {
        sak::logInfo("[NuGetApiClient] Dependency resolution complete: {} packages",
                     static_cast<int>(m_resolved_deps.size()));
        Q_EMIT dependenciesResolved(m_resolved_deps);
        return;
    }

    if (m_dependency_depth >= offline::kMaxDependencyDepth) {
        sak::logWarning("[NuGetApiClient] Max dependency depth reached");
        Q_EMIT dependenciesResolved(m_resolved_deps);
        return;
    }

    QString next_id = m_deps_to_resolve.takeFirst();
    m_dependency_depth++;

    // Build OData URL manually — QUrlQuery double-encodes OData quotes and $ params.
    QString encoded_id = QString(QUrl::toPercentEncoding(next_id));
    QString url_str =
        QString(
            "%1%2?$filter=IsLatestVersion%20and%20Id%20eq%20%27%3%27"
            "&searchTerm=%27%4%27&targetFramework=%27%27"
            "&includePrerelease=false")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetSearchPath, encoded_id, encoded_id);

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    m_pending_ops++;

    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_pending_ops--;
        reply->deleteLater();

        if (m_cancelled) {
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            sak::logWarning("[NuGetApiClient] Dep resolve error: {}",
                            reply->errorString().toStdString());
            resolveNextDependency();
            return;
        }

        QByteArray data = reply->readAll();
        auto results = parseODataFeed(data);

        if (!results.isEmpty()) {
            auto& pkg = results.first();
            m_resolved_deps.append(pkg);

            for (const auto& dep_id : pkg.dependency_ids) {
                QString lower_dep = dep_id.toLower();
                if (!m_visited_deps.contains(lower_dep)) {
                    m_visited_deps.insert(lower_dep);
                    m_deps_to_resolve.append(dep_id);
                }
            }
        }

        resolveNextDependency();
    });
}

// ============================================================================
// XML Parsing
// ============================================================================

QVector<ChocoPackageMetadata> NuGetApiClient::parseODataFeed(const QByteArray& xml) {
    QVector<ChocoPackageMetadata> results;

    QDomDocument doc;
    QString parse_error;
    if (!doc.setContent(xml, &parse_error)) {
        sak::logWarning("[NuGetApiClient] OData parse error: {}", parse_error.toStdString());
        return results;
    }

    QDomElement root = doc.documentElement();
    QDomNodeList entries = root.elementsByTagName("entry");

    for (int i = 0; i < entries.count(); ++i) {
        QDomElement entry = entries.at(i).toElement();
        if (!entry.isNull()) {
            results.append(parseODataEntry(entry));
        }
    }

    return results;
}

ChocoPackageMetadata NuGetApiClient::parseODataEntry(const QDomElement& entry) const {
    ChocoPackageMetadata meta;

    // Extract content src (download URL)
    QDomElement content = entry.firstChildElement("content");
    if (!content.isNull()) {
        meta.download_url = content.attribute("src");
    }

    // Extract title
    QDomElement title_elem = entry.firstChildElement("title");
    if (!title_elem.isNull()) {
        meta.title = title_elem.text();
    }

    QDomElement properties = findPropertiesElement(entry);
    if (properties.isNull()) {
        return meta;
    }

    populateBasicProperties(meta, properties);
    populateConvertedProperties(meta, properties);

    return meta;
}

QDomElement NuGetApiClient::findPropertiesElement(const QDomElement& entry) const {
    QDomNodeList prop_nodes = entry.elementsByTagName("m:properties");
    if (prop_nodes.isEmpty()) {
        prop_nodes = entry.elementsByTagName("properties");
    }
    if (!prop_nodes.isEmpty()) {
        return prop_nodes.at(0).toElement();
    }
    return {};
}

void NuGetApiClient::populateBasicProperties(ChocoPackageMetadata& meta,
                                             const QDomElement& properties) const {
    meta.package_id = extractProperty(properties, "Id");
    meta.version = extractProperty(properties, "Version");
    if (meta.title.isEmpty()) {
        meta.title = extractProperty(properties, "Title");
    }
    meta.description = extractProperty(properties, "Description");
    meta.authors = extractProperty(properties, "Authors");
    meta.project_url = extractProperty(properties, "ProjectUrl");
    meta.icon_url = extractProperty(properties, "IconUrl");
    meta.license_url = extractProperty(properties, "LicenseUrl");
    meta.release_notes = extractProperty(properties, "ReleaseNotes");
    meta.published = extractProperty(properties, "Published");
    meta.checksum = extractProperty(properties, "PackageHash");
    meta.checksum_type = extractProperty(properties, "PackageHashAlgorithm");
}

void NuGetApiClient::populateConvertedProperties(ChocoPackageMetadata& meta,
                                                 const QDomElement& properties) const {
    QString tags_str = extractProperty(properties, "Tags");
    if (!tags_str.isEmpty()) {
        meta.tags = tags_str.split(' ', Qt::SkipEmptyParts);
    }

    QString size_str = extractProperty(properties, "PackageSize");
    if (!size_str.isEmpty()) {
        meta.package_size_bytes = size_str.toLongLong();
    }

    QString dl_count = extractProperty(properties, "DownloadCount");
    if (!dl_count.isEmpty()) {
        meta.download_count = dl_count.toInt();
    }

    QString approved = extractProperty(properties, "IsApproved");
    meta.is_approved = (approved.compare("true", Qt::CaseInsensitive) == 0);

    QString deps_str = extractProperty(properties, "Dependencies");
    if (!deps_str.isEmpty()) {
        meta.dependency_ids = parseDependencyString(deps_str);
    }
}

QString NuGetApiClient::extractProperty(const QDomElement& properties, const QString& name) const {
    // Try d: namespace prefix first (standard OData)
    QDomElement elem = properties.firstChildElement("d:" + name);
    if (elem.isNull()) {
        elem = properties.firstChildElement(name);
    }
    if (elem.isNull()) {
        return {};
    }
    return elem.text().trimmed();
}

QStringList NuGetApiClient::parseDependencyString(const QString& dep_string) const {
    // NuGet v2 dependencies format: "id:version_range|id:version_range|..."
    // or "framework::id:version|id:version"
    QStringList result;
    QStringList parts = dep_string.split('|', Qt::SkipEmptyParts);

    for (const auto& part : parts) {
        QString trimmed = part.trimmed();
        if (trimmed.isEmpty()) {
            continue;
        }
        // Handle framework-qualified deps: "targetFramework:dep1:ver|dep2:ver"
        int colon_pos = trimmed.indexOf(':');
        if (colon_pos > 0) {
            QString dep_id = trimmed.left(colon_pos);
            // Skip framework specifiers (they contain dots like "net45")
            if (!dep_id.contains('.')) {
                result.append(dep_id);
            } else {
                // This might be "framework::dep_id:ver" — try splitting further
                QStringList sub_parts = trimmed.split(':', Qt::SkipEmptyParts);
                if (sub_parts.size() >= 2) {
                    result.append(sub_parts[0]);
                }
            }
        } else {
            result.append(trimmed);
        }
    }

    return result;
}

QNetworkRequest NuGetApiClient::buildRequest(const QString& url) const {
    QNetworkRequest request{QUrl::fromEncoded(url.toUtf8())};
    request.setHeader(QNetworkRequest::UserAgentHeader, "SAK-Utility/1.0");
    request.setRawHeader("Accept", "application/xml");
    request.setTransferTimeout(offline::kApiRequestTimeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    return request;
}

}  // namespace sak
