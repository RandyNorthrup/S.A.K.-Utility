// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

#include <utility>

namespace sak::ai {

namespace {

constexpr auto kProvidersResource = ":/ai/providers/providers.json";
constexpr auto kProviderFile = "data/ai/providers/providers.json";
constexpr auto kAppManifestResourceRoot = ":/ai/app_manifests";
constexpr auto kAppManifestFileRoot = "data/ai/app_manifests";

[[nodiscard]] QString normalizedId(const QString& value) {
    QString id = value.trimmed().toLower();
    id.replace(QChar(u'\\'), QChar(u'/'));
    id.replace(QChar(u'/'), QChar(u'_'));
    return id;
}

[[nodiscard]] QJsonObject readJsonObject(const QString& path, QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error_message) {
            *error_message = QStringLiteral("Cannot open %1: %2").arg(path, file.errorString());
        }
        return {};
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error_message) {
            *error_message =
                QStringLiteral("Invalid JSON in %1: %2").arg(path, parse_error.errorString());
        }
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return doc.object();
}

[[nodiscard]] QString resolvedRelativePath(const QString& app_dir, const QString& path) {
    const QString trimmed = path.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }
    if (QDir::isAbsolutePath(trimmed)) {
        return QDir::cleanPath(trimmed);
    }
    return QDir::cleanPath(QDir(app_dir).filePath(trimmed));
}

[[nodiscard]] QJsonObject providerStatusObject(const QString& app_dir,
                                               const QJsonObject& provider) {
    QJsonObject status = provider;
    status[QStringLiteral("available")] = provider.value(QStringLiteral("enabled")).toBool(true);
    status[QStringLiteral("missing_reason")] = QString();

    const QString transport = provider.value(QStringLiteral("transport")).toString();
    if (!status.value(QStringLiteral("available")).toBool(false)) {
        status[QStringLiteral("missing_reason")] = QStringLiteral("Provider disabled");
    } else if (transport == QLatin1String("planned")) {
        status[QStringLiteral("available")] = false;
        status[QStringLiteral("missing_reason")] =
            QStringLiteral("Provider planned, not implemented");
    } else if (transport == QLatin1String("stdio")) {
        const QString command =
            resolvedRelativePath(app_dir, provider.value(QStringLiteral("command")).toString());
        status[QStringLiteral("resolved_command")] = command;
        const bool exists = QFileInfo::exists(command);
        status[QStringLiteral("available")] = exists;
        if (!exists) {
            status[QStringLiteral("missing_reason")] =
                QStringLiteral("Bundled MCP command missing");
        }
    }

    return status;
}

}  // namespace

AiProviderRegistry::AiProviderRegistry(QString app_dir)
    : m_app_dir(app_dir.trimmed().isEmpty() ? QCoreApplication::applicationDirPath()
                                            : std::move(app_dir)) {}

QJsonObject AiProviderRegistry::providersObject(QString* error_message) const {
    return readCachedJsonObject(
        providerRegistryPath(), providerRegistryResourcePath(), &m_provider_cache, error_message);
}

QJsonArray AiProviderRegistry::providers(QString* error_message) const {
    const QJsonObject object = providersObject(error_message);
    return object.value(QStringLiteral("providers")).toArray();
}

QJsonObject AiProviderRegistry::providerById(const QString& provider_id,
                                             QString* error_message) const {
    const QString wanted = normalizedId(provider_id);
    for (const auto& value : providers(error_message)) {
        const QJsonObject provider = value.toObject();
        if (normalizedId(provider.value(QStringLiteral("id")).toString()) == wanted) {
            if (error_message) {
                error_message->clear();
            }
            return provider;
        }
    }
    if (error_message) {
        *error_message = QStringLiteral("Unknown provider: %1").arg(provider_id);
    }
    return {};
}

QJsonObject AiProviderRegistry::providerStatus(const QString& provider_id,
                                               QString* error_message) const {
    const QJsonObject provider = providerById(provider_id, error_message);
    if (provider.isEmpty()) {
        return {};
    }
    if (error_message) {
        error_message->clear();
    }
    return providerStatusObject(m_app_dir, provider);
}

QJsonObject AiProviderRegistry::providerStatuses(QString* error_message) const {
    QJsonArray statuses;
    for (const auto& value : providers(error_message)) {
        const QJsonObject provider = value.toObject();
        statuses.append(providerStatusObject(m_app_dir, provider));
    }
    QJsonObject result;
    result[QStringLiteral("providers")] = statuses;
    result[QStringLiteral("provider_count")] = statuses.size();
    return result;
}

QJsonObject AiProviderRegistry::appManifest(const QString& app_id, QString* error_message) const {
    const QString id = normalizedId(app_id);
    if (id.isEmpty()) {
        if (error_message) {
            *error_message = QStringLiteral("App id is empty");
        }
        return {};
    }
    return readCachedJsonObject(
        appManifestPath(id), appManifestResourcePath(id), &m_app_manifest_cache[id], error_message);
}

QJsonObject AiProviderRegistry::appCapabilities(const QString& app_id,
                                                const QString& action,
                                                QString* error_message) const {
    QJsonObject manifest = appManifest(app_id, error_message);
    if (manifest.isEmpty()) {
        return {};
    }
    if (!action.trimmed().isEmpty()) {
        const QJsonObject actions = manifest.value(QStringLiteral("actions")).toObject();
        const QJsonObject requested = actions.value(normalizedId(action)).toObject();
        if (requested.isEmpty()) {
            manifest[QStringLiteral("requested_action_supported")] = false;
            manifest[QStringLiteral("requested_action")] = normalizedId(action);
        } else {
            manifest[QStringLiteral("requested_action_supported")] =
                requested.value(QStringLiteral("supported")).toBool(false);
            manifest[QStringLiteral("requested_action")] = normalizedId(action);
            manifest[QStringLiteral("requested_action_profile")] = requested;
        }
    }
    if (error_message) {
        error_message->clear();
    }
    return manifest;
}

QString AiProviderRegistry::providerRegistryPath() const {
    return QDir(m_app_dir).filePath(QString::fromLatin1(kProviderFile));
}

QString AiProviderRegistry::providerRegistryResourcePath() const {
    return QString::fromLatin1(kProvidersResource);
}

QString AiProviderRegistry::appManifestPath(const QString& app_id) const {
    return QDir(m_app_dir).filePath(
        QStringLiteral("%1/%2.json")
            .arg(QString::fromLatin1(kAppManifestFileRoot), normalizedId(app_id)));
}

QString AiProviderRegistry::appManifestResourcePath(const QString& app_id) const {
    return QStringLiteral("%1/%2.json")
        .arg(QString::fromLatin1(kAppManifestResourceRoot), normalizedId(app_id));
}

void AiProviderRegistry::clearCache() const {
    m_provider_cache = {};
    m_app_manifest_cache.clear();
}

QJsonObject AiProviderRegistry::readCachedJsonObject(const QString& file_path,
                                                     const QString& resource_path,
                                                     CachedJsonObject* cache,
                                                     QString* error_message) const {
    if (!cache) {
        return {};
    }
    const bool file_exists = QFileInfo::exists(file_path);
    const QString path = file_exists ? file_path : resource_path;
    const QDateTime last_modified = file_exists ? QFileInfo(file_path).lastModified().toUTC()
                                                : QDateTime{};

    if (cache->valid && cache->path == path &&
        (!file_exists || cache->last_modified_utc == last_modified)) {
        if (error_message) {
            error_message->clear();
        }
        return cache->object;
    }

    QJsonObject object = readJsonObject(path, error_message);
    if (object.isEmpty()) {
        cache->valid = false;
        return {};
    }
    cache->path = path;
    cache->last_modified_utc = last_modified;
    cache->object = object;
    cache->valid = true;
    return object;
}

}  // namespace sak::ai
