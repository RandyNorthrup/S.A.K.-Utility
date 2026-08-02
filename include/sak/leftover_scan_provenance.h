// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/leftover_cleanup_guard.h"

#include <QDir>
#include <QMutex>
#include <QSet>
#include <QString>
#include <QVector>

/// @file leftover_scan_provenance.h
/// @brief Session-scoped proof-of-scan binding for the AI assistant's software.clean_leftovers op.
///
/// clean_leftovers deletes model-supplied items. The fail-closed denylist
/// (leftover_cleanup_guard.h) stops OS-critical targets, but a prompt-injected model could still
/// fabricate a plausible-looking user-file path that clears the denylist and have a rubber-stamping
/// human confirm it. This module closes that gap the same way setAdapterDhcp resolves adapter names
/// against a SYSTEM-sourced set: every item software.scan_leftovers actually FOUND is recorded
/// here, and clean_leftovers refuses any item that was never surfaced by a prior scan this session
/// -- unless an explicit technician-override is passed (loudly surfaced, and the denylist still
/// applies on top).
namespace sak {

namespace detail {

/// Filesystem identity: case/separator/trailing-dot-insensitive normalized path.
[[nodiscard]] inline QString fsProvenanceKey(const QString& path) {
    return QStringLiteral("fs|") +
           cleanupCanonicalLower(QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed())));
}

/// Registry key identity: hive-prefixed subkey canonicalized (case/separator-insensitive).
[[nodiscard]] inline QString regKeyProvenanceKey(const QString& path) {
    QString subkey;
    if (!cleanupRegistryHive(path.trimmed(), subkey)) {
        return QStringLiteral("regkey|") + path.trimmed().toLower();
    }
    return QStringLiteral("regkey|") + cleanupNormalizedSubkey(subkey);
}

/// Registry value identity: canonical key + exact value name (value names are significant).
[[nodiscard]] inline QString regValProvenanceKey(const QString& path, const QString& valueName) {
    QString subkey;
    const QString base = cleanupRegistryHive(path.trimmed(), subkey)
                             ? cleanupNormalizedSubkey(subkey)
                             : path.trimmed().toLower();
    return QStringLiteral("regval|") + base + QStringLiteral("|") + valueName;
}

/// StartupEntry is either a filesystem shortcut or a registry Run value.
[[nodiscard]] inline QString startupProvenanceKey(const LeftoverItem& item) {
    return item.registryValueName.isEmpty()
               ? fsProvenanceKey(item.path)
               : regValProvenanceKey(item.path, item.registryValueName);
}

/// Name-based identity (service / scheduled task / firewall rule): lower-cased, task path
/// normalized.
[[nodiscard]] inline QString namedProvenanceKey(LeftoverItem::Type type, const QString& path) {
    const QString name = path.trimmed();
    if (type == LeftoverItem::Type::Service) {
        return QStringLiteral("svc|") + name.toLower();
    }
    if (type == LeftoverItem::Type::FirewallRule) {
        return QStringLiteral("fw|") + name.toLower();
    }
    if (type == LeftoverItem::Type::ScheduledTask) {
        QString normalized = name;
        normalized.replace(QLatin1Char('/'), QLatin1Char('\\'));
        while (normalized.startsWith(QLatin1Char('\\'))) {
            normalized.remove(0, 1);
        }
        return QStringLiteral("task|") + normalized.toLower();
    }
    return QStringLiteral("other|") + name.toLower();
}

}  // namespace detail

/// A stable identity key for a leftover item, normalized so the SAME real item produces the SAME
/// key whether it came from the scanner or from the model's clean request
/// (case/separator/trailing-dot insensitive for paths; lower-cased names; registry hive+subkey
/// canonicalized). Pure -> testable.
[[nodiscard]] inline QString leftoverProvenanceKey(const LeftoverItem& item) {
    switch (item.type) {
    case LeftoverItem::Type::File:
    case LeftoverItem::Type::Folder:
        return detail::fsProvenanceKey(item.path);
    case LeftoverItem::Type::StartupEntry:
        return detail::startupProvenanceKey(item);
    case LeftoverItem::Type::RegistryKey:
    case LeftoverItem::Type::ShellExtension:
        return detail::regKeyProvenanceKey(item.path);
    case LeftoverItem::Type::RegistryValue:
        return detail::regValProvenanceKey(item.path, item.registryValueName);
    default:
        return detail::namedProvenanceKey(item.type, item.path);
    }
}

/// Process-wide, thread-safe session record of every leftover item a scan surfaced. clean_leftovers
/// checks each requested item against it before deletion.
class LeftoverScanProvenance {
public:
    static LeftoverScanProvenance& instance() {
        static LeftoverScanProvenance store;
        return store;
    }

    /// Record every item a scan found (the FULL scanned vector, not the truncated sample the model
    /// sees), so any item the model legitimately copied is present. Bounded: when the cap would be
    /// exceeded the set is reset to just this scan's items (the most recent scan is the relevant
    /// one).
    void record(const QVector<LeftoverItem>& items) {
        QMutexLocker lock(&m_mutex);
        if (m_keys.size() + items.size() > kMaxKeys) {
            m_keys.clear();
        }
        for (const LeftoverItem& item : items) {
            if (m_keys.size() >= kMaxKeys) {
                break;
            }
            m_keys.insert(leftoverProvenanceKey(item));
        }
    }

    /// True if @p item matches an item a prior scan surfaced this session.
    [[nodiscard]] bool contains(const LeftoverItem& item) const {
        QMutexLocker lock(&m_mutex);
        return m_keys.contains(leftoverProvenanceKey(item));
    }

    /// True if no scan has been recorded yet this session.
    [[nodiscard]] bool isEmpty() const {
        QMutexLocker lock(&m_mutex);
        return m_keys.isEmpty();
    }

    /// Clear the session set (used by tests; also allows an explicit reset).
    void clear() {
        QMutexLocker lock(&m_mutex);
        m_keys.clear();
    }

private:
    LeftoverScanProvenance() = default;
    static constexpr int kMaxKeys = 200'000;
    mutable QMutex m_mutex;
    QSet<QString> m_keys;
};

}  // namespace sak
