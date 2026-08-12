// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_provider_registry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QtGlobal>

#include <utility>

namespace sak::ai {

namespace {

constexpr auto kProvidersResource = ":/ai/providers/providers.json";
constexpr auto kProviderFile = "data/ai/providers/providers.json";
constexpr auto kAppManifestResourceRoot = ":/ai/app_manifests";
constexpr auto kAppManifestFileRoot = "data/ai/app_manifests";

// Out-of-band authorization to honor an on-disk AI policy file (providers.json or an app
// manifest) IN PLACE OF the embedded Qt resource. The embedded resource is compiled into the
// executable and cannot be altered without replacing the binary; the on-disk copy staged under
// the application data directory can be rewritten by anyone able to write that directory. A
// rewritten providers.json redirects an HTTP docs endpoint to an attacker host, injects
// environment into the (elevated) MCP child process, or widens the tool allowlist; a rewritten
// app manifest supplies an executable powershell/cli "command" that app_run_action will run --
// none of which the embedded manifest would permit. The downstream gateway blunts SOME of these
// (command-within-appdir, https-only endpoint, protected-env rejection, command guard) but not
// endpoint host redirection or arbitrary non-protected env, so the trust decision must fail
// closed HERE: the disk copy is authoritative ONLY when the human operator has set this
// environment variable, which a file-drop attacker (who controls bytes on disk, not the
// launching process environment) cannot reach. Unset -> embedded resource only. Mirrors the
// SAK_LEFTOVER_TECHNICIAN_OVERRIDE out-of-band control in app_mutating_actions.cpp. The disk copy,
// when opted in, still passes every existing downstream gate -- this is an added front door,
// not a replacement for them.
[[nodiscard]] bool diskPolicyOverrideAuthorized() {
    const QString value = qEnvironmentVariable("SAK_AI_POLICY_DISK_OVERRIDE").trimmed().toLower();
    return value == QLatin1String("1") || value == QLatin1String("true") ||
           value == QLatin1String("yes") || value == QLatin1String("on");
}

[[nodiscard]] QString normalizedId(const QString& value) {
    QString id = value.trimmed().toLower();
    id.replace(QChar(u'\\'), QChar(u'/'));
    id.replace(QChar(u'/'), QChar(u'_'));
    return id;
}

// Settle a rejected config read: record @p message when the caller asked for one, then return
// the empty object that signals failure. Folding every fail-closed exit through one helper keeps
// the "set the reason, return nothing" contract identical across paths instead of repeating the
// same null-check branch at each guard.
[[nodiscard]] QJsonObject rejectConfig(QString* error_message, const QString& message) {
    if (error_message != nullptr) {
        *error_message = message;
    }
    return {};
}

[[nodiscard]] QJsonObject readJsonObject(const QString& path, QString* error_message) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return rejectConfig(error_message,
                            QStringLiteral("Cannot open %1: %2").arg(path, file.errorString()));
    }
    // Bound the read before pulling bytes: a providers/manifest config is a few KB, so an
    // attacker-planted (or otherwise malformed) multi-GB file must be refused BEFORE readAll()
    // can allocate it. A negative/unknowable size and a short read both fail closed.
    constexpr qint64 kMaxConfigBytes = 8 * 1024 * 1024;  // 8 MiB is far above any real config.
    const qint64 declared_size = file.size();
    if (declared_size < 0 || declared_size > kMaxConfigBytes) {
        return rejectConfig(error_message,
                            QStringLiteral("Config file rejected (size %1 bytes): %2")
                                .arg(declared_size)
                                .arg(path));
    }
    const QByteArray bytes = file.readAll();
    if (bytes.size() != declared_size) {
        return rejectConfig(error_message, QStringLiteral("Short read on %1").arg(path));
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        return rejectConfig(
            error_message,
            QStringLiteral("Invalid JSON in %1: %2").arg(path, parse_error.errorString()));
    }
    if (error_message != nullptr) {
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

// True only when @p command resolves to a path inside @p app_dir. A bundled stdio MCP
// command must live within the application directory; an on-disk providers.json (which
// silently overrides the embedded manifest) pointing "command" at an arbitrary absolute
// path -- or escaping via ../ -- must NOT be treated as an available bundled executable.
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
    // Strip any value carried in from the (untrusted, disk-overridable) provider object so a
    // non-stdio or forged entry cannot smuggle an un-gated command through to the launcher.
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
        // other value -- a typo, or a disk-override providers.json naming a transport with no
        // launcher -- resolves to no capability path, so fail closed rather than leaving the
        // provider permissively "available" with no way to reach it.
        status[QStringLiteral("available")] = false;
        status[QStringLiteral("missing_reason")] =
            QStringLiteral("Unknown provider transport: %1").arg(transport);
    }

    return status;
}

// The effective backing file for one cached config read: its path (on-disk copy or embedded
// resource), the on-disk modification time used for cache invalidation, and which of the two won.
struct ResolvedConfigSource {
    QString m_path;
    QDateTime m_last_modified_utc;
    bool m_file_exists{false};
};

// Decide which file answers a config read and capture its modification time. The embedded Qt
// resource is the tamper-proof authority. The on-disk copy under the application directory is
// honored only when a human operator has opted in out of band (see diskPolicyOverrideAuthorized);
// without that opt-in a manifest dropped beside the exe is ignored and the embedded resource
// loads, so a file-drop attacker cannot silently override AI provider/app policy. When the opt-in
// is absent, treat the disk file as if it did not exist -- an app manifest with no embedded
// counterpart then fails closed as "unavailable". Only the disk copy carries a meaningful mtime;
// the resource reports none, so its slot stays null and the freshness check never keys on it.
[[nodiscard]] ResolvedConfigSource resolveConfigSource(const QString& file_path,
                                                       const QString& resource_path) {
    const bool file_exists = diskPolicyOverrideAuthorized() && QFileInfo::exists(file_path);
    return {.m_path = file_exists ? file_path : resource_path,
            .m_last_modified_utc = file_exists ? QFileInfo(file_path).lastModified().toUTC()
                                               : QDateTime{},
            .m_file_exists = file_exists};
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
            if (error_message != nullptr) {
                error_message->clear();
            }
            return provider;
        }
    }
    if ((error_message != nullptr) && !error_message->isEmpty()) {
        // providers() recorded an open/parse failure; surface THAT error rather than masking it
        // as "Unknown provider".
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
    if (error_message != nullptr) {
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
        if (error_message != nullptr) {
            *error_message = QStringLiteral("App id is empty");
        }
        return {};
    }
    // Bound cache growth: appManifest is reachable with AI-supplied ids, and operator[] below
    // inserts an entry for every distinct id (even a miss). Cap the map so a flood of unique ids
    // cannot exhaust memory; a post-clear miss simply re-reads from disk/resource.
    constexpr int kMaxManifestCacheEntries = 256;
    if (!m_app_manifest_cache.contains(id) &&
        m_app_manifest_cache.size() >= kMaxManifestCacheEntries) {
        m_app_manifest_cache.clear();
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
    if (error_message != nullptr) {
        error_message->clear();
    }
    return manifest;
}

QString AiProviderRegistry::providerRegistryPath() const {
    return QDir(m_app_dir).filePath(QString::fromLatin1(kProviderFile));
}

QString AiProviderRegistry::providerRegistryResourcePath() {
    return QString::fromLatin1(kProvidersResource);
}

QString AiProviderRegistry::appManifestPath(const QString& app_id) const {
    return QDir(m_app_dir).filePath(
        QStringLiteral("%1/%2.json")
            .arg(QString::fromLatin1(kAppManifestFileRoot), normalizedId(app_id)));
}

QString AiProviderRegistry::appManifestResourcePath(const QString& app_id) {
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
                                                     QString* error_message) {
    if (cache == nullptr) {
        return {};
    }
    const ResolvedConfigSource source = resolveConfigSource(file_path, resource_path);

    if (cache->valid && cache->path == source.m_path &&
        (!source.m_file_exists || cache->last_modified_utc == source.m_last_modified_utc)) {
        if (error_message != nullptr) {
            error_message->clear();
        }
        return cache->object;
    }

    QJsonObject object = readJsonObject(source.m_path, error_message);
    if (object.isEmpty()) {
        cache->valid = false;
        return {};
    }
    cache->path = source.m_path;
    cache->last_modified_utc = source.m_last_modified_utc;
    cache->object = object;
    cache->valid = true;
    return object;
}

}  // namespace sak::ai
