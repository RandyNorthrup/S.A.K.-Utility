// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

namespace sak::ai {

class AiProviderRegistry {
public:
    explicit AiProviderRegistry(QString app_dir = {});

    [[nodiscard]] QJsonObject providersObject(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonArray providers(QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerById(const QString& provider_id,
                                           QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatus(const QString& provider_id,
                                             QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject providerStatuses(QString* error_message = nullptr) const;

    [[nodiscard]] QJsonObject appManifest(const QString& app_id,
                                          QString* error_message = nullptr) const;
    [[nodiscard]] QJsonObject appCapabilities(const QString& app_id,
                                              const QString& action,
                                              QString* error_message = nullptr) const;
    void clearCache() const;

private:
    struct CachedJsonObject {
        QString path;
        QDateTime last_modified_utc;
        QJsonObject object;
        bool valid{false};
    };

    [[nodiscard]] QString providerRegistryPath() const;
    [[nodiscard]] QString providerRegistryResourcePath() const;
    [[nodiscard]] QString appManifestPath(const QString& app_id) const;
    [[nodiscard]] QString appManifestResourcePath(const QString& app_id) const;
    [[nodiscard]] QJsonObject readCachedJsonObject(const QString& file_path,
                                                   const QString& resource_path,
                                                   CachedJsonObject* cache,
                                                   QString* error_message) const;

    QString m_app_dir;
    mutable CachedJsonObject m_provider_cache;
    mutable QHash<QString, CachedJsonObject> m_app_manifest_cache;
};

}  // namespace sak::ai
