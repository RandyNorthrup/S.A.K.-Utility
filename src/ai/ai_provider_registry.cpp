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

[[nodiscard]] QString normalizedId(const QString& value) {
    QString id = value.trimmed().toLower();
    id.replace(QChar(u'\\'), QChar(u'/'));
    id.replace(QChar(u'/'), QChar(u'_'));
    return id;
}

// Read the catalog compiled into the executable. A resource that is missing or does not parse
// to a JSON object is a broken build; it is reported (and every provider read fails with that
// reason) rather than replaced by an empty catalog that would read as "no providers".
[[nodiscard]] QJsonObject readEmbeddedCatalog(QString* error_message) {
    const QString path = QString::fromLatin1(kProvidersResource);
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error_message = QStringLiteral("Cannot open %1: %2").arg(path, file.errorString());
        return {};
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        *error_message =
            QStringLiteral("Invalid JSON in %1: %2").arg(path, parse_error.errorString());
        return {};
    }
    error_message->clear();
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

// True only when @p command resolves to a path inside @p app_dir. A bundled stdio MCP command
// must live within the application directory; a catalog entry pointing "command" at an
// arbitrary absolute path -- or escaping via ../ -- must NOT be treated as an available bundled
// executable.
[[nodiscard]] bool commandWithinAppDir(const QString& app_dir, const QString& command) {
    if (command.isEmpty()) {
        return false;
    }
    const QString base = QDir::cleanPath(QDir(app_dir).absolutePath());
    const QString target = QDir::cleanPath(QFileInfo(command).absoluteFilePath());
    const bool lexical_within = target == base ||
                                target.startsWith(base + QLatin1Char('/'), Qt::CaseInsensitive);
    if (!lexical_within) {
        return false;
    }
    // A lexical cleanPath resolves "../" but NOT symlinks/junctions, so a within-dir
    // command that is a symlink to an executable OUTSIDE the app dir would still pass.
    // When the command exists, also require its CANONICAL path (links resolved) to stay
    // within the canonical app dir. A command that does not exist yet keeps the lexical
    // verdict -- its absence is reported separately as "missing".
    const QString canonical_base = QFileInfo(app_dir).canonicalFilePath();
    const QString canonical_target = QFileInfo(command).canonicalFilePath();
    if (canonical_base.isEmpty() || canonical_target.isEmpty()) {
        return true;
    }
    return canonical_target == canonical_base ||
           canonical_target.startsWith(canonical_base + QLatin1Char('/'), Qt::CaseInsensitive);
}

[[nodiscard]] QJsonObject providerStatusObject(const QString& app_dir,
                                               const QJsonObject& provider) {
    QJsonObject status = provider;
    status[QStringLiteral("available")] = provider.value(QStringLiteral("enabled")).toBool(true);
    status[QStringLiteral("missing_reason")] = QString();
    // resolved_command is a COMPUTED output that only the gated stdio branch below may set.
    // Strip any value carried in on the provider object so a non-stdio or forged entry cannot
    // smuggle an un-gated command through to the launcher.
    status.remove(QStringLiteral("resolved_command"));

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
        const bool within = commandWithinAppDir(app_dir, command);
        const bool exists = within && QFileInfo::exists(command);
        status[QStringLiteral("available")] = exists;
        if (!within) {
            status[QStringLiteral("available")] = false;
            status[QStringLiteral("missing_reason")] =
                QStringLiteral("Bundled MCP command must resolve within the application directory");
        } else if (!exists) {
            status[QStringLiteral("missing_reason")] =
                QStringLiteral("Bundled MCP command missing");
        }
    } else if (transport != QLatin1String("http") && transport != QLatin1String("native")) {
        // "http" (docs over HTTP) and "native" (filesystem/vendor/package providers served
        // in-process) are the only remaining transports the gateway can actually dispatch. Any
        // other value -- a typo, or a transport with no launcher -- resolves to no capability
        // path, so fail closed rather than leaving the provider permissively "available" with no
        // way to reach it.
        status[QStringLiteral("available")] = false;
        status[QStringLiteral("missing_reason")] =
            QStringLiteral("Unknown provider transport: %1").arg(transport);
    }

    return status;
}

}  // namespace

AiProviderRegistry::AiProviderRegistry(QString app_dir)
    : m_app_dir(app_dir.trimmed().isEmpty() ? QCoreApplication::applicationDirPath()
                                            : std::move(app_dir))
    , m_catalog_error()
    , m_catalog(readEmbeddedCatalog(&m_catalog_error)) {}

#ifdef SAK_AI_TEST_HOOKS
AiProviderRegistry AiProviderRegistry::withCatalogForTesting(QString app_dir, QJsonObject catalog) {
    AiProviderRegistry registry(std::move(app_dir));
    registry.m_catalog = std::move(catalog);
    registry.m_catalog_error.clear();
    return registry;
}
#endif  // SAK_AI_TEST_HOOKS

QJsonObject AiProviderRegistry::providersObject(QString* error_message) const {
    if (error_message != nullptr) {
        *error_message = m_catalog_error;
    }
    return m_catalog;
}

QJsonArray AiProviderRegistry::providers(QString* error_message) const {
    return providersObject(error_message).value(QStringLiteral("providers")).toArray();
}

QJsonObject AiProviderRegistry::providerById(const QString& provider_id,
                                             QString* error_message) const {
    const QString wanted = normalizedId(provider_id);
    for (const auto& value : providers(error_message)) {
        const QJsonObject provider = value.toObject();
        if (normalizedId(provider.value(QStringLiteral("id")).toString()) == wanted) {
            return provider;
        }
    }
    if (!m_catalog_error.isEmpty()) {
        // The catalog itself could not be read; report THAT rather than "Unknown provider".
        return {};
    }
    if (error_message != nullptr) {
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
    return providerStatusObject(m_app_dir, provider);
}

QJsonObject AiProviderRegistry::providerStatuses(QString* error_message) const {
    QJsonArray statuses;
    for (const auto& value : providers(error_message)) {
        statuses.append(providerStatusObject(m_app_dir, value.toObject()));
    }
    QJsonObject result;
    result[QStringLiteral("providers")] = statuses;
    result[QStringLiteral("provider_count")] = statuses.size();
    return result;
}

}  // namespace sak::ai
