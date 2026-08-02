// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_api_client.cpp
/// @brief NuGet v2 OData API client implementation

#include "sak/nuget_api_client.h"

#include "sak/logger.h"
#include "sak/nuget_dependency_resolver.h"
#include "sak/offline_deployment_constants.h"

#include <QDir>
#include <QDomDocument>
#include <QDomElement>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrl>

namespace sak {

namespace {
constexpr qsizetype kNugetErrorBodyPreviewChars = 500;
constexpr int kContentDispositionFilenamePrefixLength = 9;

/// @brief Key a package's captured metadata by lowercased id + concrete version.
QString metadataKey(const QString& id, const QString& version) {
    return id.toLower() + QLatin1Char('@') + version;
}
}  // namespace

/// @brief Per-resolution state: the shared transitive resolver plus the full
///        ChocoPackageMetadata captured for each fetched version (so the emitted
///        dependency tree keeps its rich fields, not just id + version).
struct NuGetApiClient::DepResolutionContext {
    NuGetDependencyResolver resolver{offline::kMaxDependencyDepth, offline::kMaxPackagesPerBuild};
    QHash<QString, ChocoPackageMetadata> metadata_by_key;
};

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
    m_cancelled = false;  // a new operation clears a prior cancel (cancel is not permanent)

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
    trackReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleSearchReply(reply); });
}

void NuGetApiClient::getPackageMetadata(const QString& package_id, const QString& version) {
    m_cancelled = false;  // a new operation clears a prior cancel (cancel is not permanent)

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
    trackReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply]() { handleMetadataReply(reply); });
}

void NuGetApiClient::getPackageVersions(const QString& package_id) {
    m_cancelled = false;  // a new operation clears a prior cancel (cancel is not permanent)

    // Build FindPackagesById URL manually — QUrlQuery double-encodes OData quotes.
    QString encoded_id = QString(QUrl::toPercentEncoding(package_id));
    QString url_str = QString("%1%2?id=%27%3%27")
                          .arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath, encoded_id);

    sak::logInfo("[NuGetApiClient] Versions: {}", url_str.toStdString());

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    trackReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, package_id]() {
        handleVersionsReply(reply, package_id);
    });
}

void NuGetApiClient::downloadNupkg(const QString& package_id,
                                   const QString& version,
                                   const QString& output_dir) {
    m_cancelled = false;  // a new operation clears a prior cancel (cancel is not permanent)

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
    trackReply(reply);

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
    m_cancelled = false;  // a new operation clears a prior cancel (cancel is not permanent)

    // Each call owns its OWN resolution state (shared_ptr captured by the reply
    // lambdas), so two overlapping resolveDependencies() calls can never share or
    // clobber one graph (B10-28 part 1). The resolver honors declared version
    // ranges and picks the highest satisfying version (B10-25).
    auto ctx = std::make_shared<DepResolutionContext>();
    ctx->resolver.start(package_id, version);

    sak::logInfo("[NuGetApiClient] Resolving dependencies for {} {}",
                 package_id.toStdString(),
                 version.toStdString());

    pumpDependencyResolution(ctx);
}

void NuGetApiClient::pumpDependencyResolution(const std::shared_ptr<DepResolutionContext>& ctx) {
    if (m_cancelled) {
        return;
    }
    if (ctx->resolver.isComplete()) {
        QVector<ChocoPackageMetadata> tree;
        for (const ResolvedPackage& pkg : ctx->resolver.resolved()) {
            const QString key = metadataKey(pkg.package_id, pkg.version);
            if (ctx->metadata_by_key.contains(key)) {
                tree.append(ctx->metadata_by_key.value(key));
            } else {
                ChocoPackageMetadata meta;
                meta.package_id = pkg.package_id;
                meta.version = pkg.version;
                tree.append(meta);
            }
        }
        for (const QString& warning : ctx->resolver.errors()) {
            sak::logWarning("[NuGetApiClient] Dependency resolution: {}", warning.toStdString());
        }
        Q_EMIT dependenciesResolved(tree);
        return;
    }

    const QString fetch_id = ctx->resolver.nextFetchId();
    const QString encoded_id = QString(QUrl::toPercentEncoding(fetch_id));
    // FindPackagesById returns EVERY version with its Dependencies, so the resolver
    // can select the highest version satisfying the declared range.
    const QString url_str =
        QString("%1%2?id=%27%3%27")
            .arg(offline::kNuGetBaseUrl, offline::kNuGetFindByIdPath, encoded_id);

    auto request = buildRequest(url_str);
    auto* reply = m_network_manager->get(request);
    trackReply(reply);

    connect(reply, &QNetworkReply::finished, this, [this, reply, ctx, fetch_id]() {
        handleResolverFeedReply(reply, ctx, fetch_id);
    });
}

void NuGetApiClient::handleResolverFeedReply(QNetworkReply* reply,
                                             const std::shared_ptr<DepResolutionContext>& ctx,
                                             const QString& fetch_id) {
    untrackReply(reply);
    reply->deleteLater();
    if (m_cancelled) {
        return;
    }

    if (reply->error() != QNetworkReply::NoError) {
        sak::logWarning("[NuGetApiClient] Dep feed error for {}: {}",
                        fetch_id.toStdString(),
                        reply->errorString().toStdString());
        ctx->resolver.provideFeedFailure(fetch_id);
        pumpDependencyResolution(ctx);
        return;
    }

    const QByteArray data = reply->readAll();
    // Capture the full metadata for every version so the emitted tree stays rich...
    const auto metas = parseODataFeed(data);
    for (const ChocoPackageMetadata& meta : metas) {
        ctx->metadata_by_key.insert(metadataKey(meta.package_id, meta.version), meta);
    }
    // ...and feed the resolver the (version, dependencies) view it consumes.
    ctx->resolver.provideFeed(fetch_id, NuGetDependencyResolver::parseODataFeedVersions(data));
    pumpDependencyResolution(ctx);
}

void NuGetApiClient::cancel() {
    m_cancelled = true;
    // abort() delivers finished() synchronously, whose handler calls
    // untrackReply()->removeOne on the very list we would iterate. Snapshot and
    // clear FIRST, then abort from the copy so the handler's removeOne no-ops.
    const QList<QNetworkReply*> pending = m_pending_replies;
    m_pending_replies.clear();
    for (QNetworkReply* reply : pending) {
        if (reply) {
            reply->abort();
        }
    }
}

void NuGetApiClient::trackReply(QNetworkReply* reply) {
    m_pending_ops++;
    m_pending_replies.append(reply);
}

void NuGetApiClient::untrackReply(QNetworkReply* reply) {
    m_pending_ops--;
    m_pending_replies.removeOne(reply);
}

bool NuGetApiClient::isBusy() const {
    return m_pending_ops > 0;
}

// ============================================================================
// Reply Handlers
// ============================================================================

void NuGetApiClient::handleSearchReply(QNetworkReply* reply) {
    untrackReply(reply);
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
                            body.left(kNugetErrorBodyPreviewChars).toStdString());
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
    untrackReply(reply);
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
    const QDomDocument::ParseResult parse_result = doc.setContent(data);
    if (!parse_result) {
        sak::logWarning("[NuGetApiClient] XML parse error: {}",
                        parse_result.errorMessage.toStdString());
        Q_EMIT errorOccurred("metadata", "XML parse error: " + parse_result.errorMessage);
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
    untrackReply(reply);
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

QString NuGetApiClient::sanitizeNupkgFilename(const QString& candidate, const QString& package_id) {
    // fileName() strips ../ traversal, drive letters and absolute paths (both /
    // and \ separators on Windows) to a bare last segment.
    const QString base = QFileInfo(candidate).fileName();
    if (!base.isEmpty() && base != QStringLiteral(".") && base != QStringLiteral("..")) {
        return base;
    }
    const QString safe_id = QFileInfo(package_id).fileName();
    if (safe_id.isEmpty() || safe_id == QStringLiteral(".") || safe_id == QStringLiteral("..")) {
        return QStringLiteral("package.nupkg");
    }
    return safe_id + QStringLiteral(".nupkg");
}

void NuGetApiClient::handleDownloadReply(QNetworkReply* reply,
                                         const QString& package_id,
                                         const QString& output_dir) {
    untrackReply(reply);
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

    // Determine filename from Content-Disposition or construct from package_id.
    // Both the header name AND the package_id fallback are caller/server-supplied,
    // so both are confined to a bare basename (sanitizeNupkgFilename) -- the write
    // can never escape output_dir via ../ traversal, a drive letter or a separator.
    QString candidate;
    QString disposition = reply->header(QNetworkRequest::ContentDispositionHeader).toString();
    if (!disposition.isEmpty() && disposition.contains("filename=")) {
        int start = disposition.indexOf("filename=") + kContentDispositionFilenamePrefixLength;
        candidate = disposition.mid(start).trimmed();
        candidate.remove('"');
    }
    const QString filename = sanitizeNupkgFilename(candidate, package_id);

    QString file_path = QDir(output_dir).filePath(filename);
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly)) {
        sak::logError("[NuGetApiClient] Cannot write to: {}", file_path.toStdString());
        Q_EMIT errorOccurred("download", "Cannot write file: " + file_path);
        return;
    }

    // Fail closed on a short/failed write (disk full, quota): a truncated .nupkg must not be
    // reported as a complete download. flush() surfaces a buffered write failure (close() is void).
    const qint64 written = file.write(data);
    const bool flushed = file.flush();
    file.close();
    if (written != data.size() || !flushed) {
        file.remove();
        sak::logError("[NuGetApiClient] Incomplete write ({} of {} bytes) to: {}",
                      static_cast<long long>(written),
                      static_cast<long long>(data.size()),
                      file_path.toStdString());
        Q_EMIT errorOccurred("download", "Incomplete write for " + package_id);
        return;
    }

    sak::logInfo("[NuGetApiClient] Downloaded {} ({} bytes)", file_path.toStdString(), data.size());
    Q_EMIT downloadComplete(package_id, file_path);
}

// ============================================================================
// XML Parsing
// ============================================================================

QVector<ChocoPackageMetadata> NuGetApiClient::parseODataFeed(const QByteArray& xml) {
    QVector<ChocoPackageMetadata> results;

    QDomDocument doc;
    const QDomDocument::ParseResult parse_result = doc.setContent(xml);
    if (!parse_result) {
        sak::logWarning("[NuGetApiClient] OData parse error: {}",
                        parse_result.errorMessage.toStdString());
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

QStringList NuGetApiClient::parseDependencyString(const QString& dep_string) {
    // Delegate to the shared range-aware parser and project to bare ids (this
    // struct field carries ids only). One parsing implementation, one behavior:
    // framework-only markers ("::net45") are skipped and ids are deduplicated.
    QStringList result;
    for (const NuGetDependency& dep : NuGetDependencyResolver::parseDependencies(dep_string)) {
        result.append(dep.id);
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
