// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_api_client.h
/// @brief NuGet v2 OData API client for Chocolatey Community Repository
///
/// Provides search, metadata retrieval, package download, and dependency
/// resolution against the public Chocolatey/NuGet v2 feed. All network
/// operations are asynchronous and cancellable.

#pragma once

#include <QDomDocument>
#include <QNetworkAccessManager>
#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>

namespace sak {

/// @brief Metadata for a single Chocolatey package from the NuGet v2 feed
struct ChocoPackageMetadata {
    QString package_id;
    QString version;
    QString title;
    QString description;
    QString authors;
    QString project_url;
    QString icon_url;
    QStringList tags;
    QStringList dependency_ids;
    int download_count{0};
    qint64 package_size_bytes{0};
    QString published;
    bool is_approved{false};
    QString checksum;
    QString checksum_type;
    QString license_url;
    QString release_notes;
    QString download_url;
};

/// @brief Client for the NuGet v2 OData API (Chocolatey Community Repository)
///
/// All search/download/metadata operations are async. Results are delivered
/// via signals. Cancel with cancel().
class NuGetApiClient : public QObject {
    Q_OBJECT

public:
    explicit NuGetApiClient(QObject* parent = nullptr);
    explicit NuGetApiClient(QNetworkAccessManager* shared_nam, QObject* parent = nullptr);
    ~NuGetApiClient() override;

    NuGetApiClient(const NuGetApiClient&) = delete;
    NuGetApiClient& operator=(const NuGetApiClient&) = delete;

    /// @brief Search packages by keyword
    void searchPackages(const QString& query, int max_results = 30);

    /// @brief Get metadata for a specific package (latest or pinned version)
    void getPackageMetadata(const QString& package_id, const QString& version = QString());

    /// @brief Get all available versions for a package
    void getPackageVersions(const QString& package_id);

    /// @brief Download .nupkg to local directory
    void downloadNupkg(const QString& package_id,
                       const QString& version,
                       const QString& output_dir);

    /// @brief Resolve transitive dependencies for a package
    void resolveDependencies(const QString& package_id, const QString& version);

    /// @brief Cancel all pending operations
    void cancel();

    /// @brief Check if any operation is in progress
    [[nodiscard]] bool isBusy() const;

Q_SIGNALS:
    void searchComplete(QVector<ChocoPackageMetadata> results);
    void metadataReady(ChocoPackageMetadata metadata);
    void versionsReady(QString package_id, QStringList versions);
    void downloadProgress(QString package_id, qint64 received, qint64 total);
    void downloadComplete(QString package_id, QString local_path);
    void dependenciesResolved(QVector<ChocoPackageMetadata> dependency_tree);
    void errorOccurred(QString context, QString error_message);

private:
    void handleSearchReply(QNetworkReply* reply);
    void handleMetadataReply(QNetworkReply* reply);
    void handleVersionsReply(QNetworkReply* reply, const QString& package_id);
    void handleDownloadReply(QNetworkReply* reply,
                             const QString& package_id,
                             const QString& output_dir);
    void resolveNextDependency();

    [[nodiscard]] QVector<ChocoPackageMetadata> parseODataFeed(const QByteArray& xml);
    [[nodiscard]] ChocoPackageMetadata parseODataEntry(const QDomElement& entry);
    [[nodiscard]] QDomElement findPropertiesElement(const QDomElement& entry) const;
    void populateBasicProperties(ChocoPackageMetadata& meta, const QDomElement& properties) const;
    void populateConvertedProperties(ChocoPackageMetadata& meta,
                                     const QDomElement& properties) const;
    [[nodiscard]] QString extractProperty(const QDomElement& properties, const QString& name) const;
    [[nodiscard]] QStringList parseDependencyString(const QString& dep_string) const;
    [[nodiscard]] QNetworkRequest buildRequest(const QString& url) const;

    QNetworkAccessManager* m_network_manager;
    bool m_owns_nam{false};
    std::atomic<bool> m_cancelled{false};
    std::atomic<int> m_pending_ops{0};

    // Dependency resolution state
    QVector<ChocoPackageMetadata> m_resolved_deps;
    QStringList m_deps_to_resolve;
    QSet<QString> m_visited_deps;
    int m_dependency_depth{0};
};

}  // namespace sak
