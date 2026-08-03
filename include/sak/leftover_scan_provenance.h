// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/advanced_uninstall_types.h"
#include "sak/leftover_cleanup_guard.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
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

/// Filesystem identity BODY: case/separator/trailing-dot-insensitive normalized path (no type tag).
[[nodiscard]] inline QString fsBody(const QString& path) {
    return cleanupCanonicalLower(QDir::cleanPath(QDir::fromNativeSeparators(path.trimmed())));
}

/// Registry identity BODY: the WHOLE path lower-cased with separators collapsed -- crucially this
/// KEEPS the hive (hklm\... vs hkcu\...) so a proof for one hive can never authorize deletion in
/// another (cleanupNormalizedSubkey does not strip the hive; it just canonicalizes the string).
[[nodiscard]] inline QString regBody(const QString& path) {
    return cleanupNormalizedSubkey(path.trimmed());
}

/// Registry value NAME normalized for identity: lower-cased (Windows value-name lookup is
/// case-INSENSITIVE, so the same value under a different casing must match) but whitespace
/// PRESERVED
/// ("Foo " and "Foo" are distinct values).
[[nodiscard]] inline QString valueBody(const QString& valueName) {
    return valueName.toLower();
}

/// Scheduled-task identity BODY: leading backslash + separator style normalized, lower-cased.
[[nodiscard]] inline QString taskBody(const QString& path) {
    QString name = path.trimmed();
    name.replace(QLatin1Char('/'), QLatin1Char('\\'));
    while (name.startsWith(QLatin1Char('\\'))) {
        name.remove(0, 1);
    }
    return name.toLower();
}

/// Filesystem family (File / Folder / StartupEntry): each type gets a DISTINCT tag so a File proof
/// cannot authorize Folder (recursive) deletion of the same path, and a shortcut startup entry
/// cannot authorize a plain-file delete.
[[nodiscard]] inline QString fsFamilyKey(const LeftoverItem& item) {
    if (item.type == LeftoverItem::Type::Folder) {
        return QStringLiteral("folder|") + fsBody(item.path);
    }
    if (item.type == LeftoverItem::Type::StartupEntry) {
        return item.registryValueName.isEmpty()
                   ? QStringLiteral("startup-file|") + fsBody(item.path)
                   : QStringLiteral("startup-val|") + regBody(item.path) + QStringLiteral("|") +
                         valueBody(item.registryValueName);
    }
    return QStringLiteral("file|") + fsBody(item.path);
}

/// Registry family (RegistryKey / ShellExtension / RegistryValue): distinct tags + hive-preserving
/// body. Value names are case-normalized but whitespace-preserved (see valueBody).
[[nodiscard]] inline QString registryFamilyKey(const LeftoverItem& item) {
    if (item.type == LeftoverItem::Type::RegistryValue) {
        return QStringLiteral("regval|") + regBody(item.path) + QStringLiteral("|") +
               valueBody(item.registryValueName);
    }
    if (item.type == LeftoverItem::Type::ShellExtension) {
        return QStringLiteral("shellext|") + regBody(item.path);
    }
    return QStringLiteral("regkey|") + regBody(item.path);
}

/// Name-based family (Service / ScheduledTask / FirewallRule / other): distinct tags, lower-cased.
[[nodiscard]] inline QString namedKey(LeftoverItem::Type type, const QString& path) {
    const QString name = path.trimmed();
    if (type == LeftoverItem::Type::Service) {
        return QStringLiteral("svc|") + name.toLower();
    }
    if (type == LeftoverItem::Type::FirewallRule) {
        return QStringLiteral("fw|") + name.toLower();
    }
    if (type == LeftoverItem::Type::ScheduledTask) {
        return QStringLiteral("task|") + taskBody(name);
    }
    return QStringLiteral("other|") + name.toLower();
}

}  // namespace detail

/// A stable identity key for a leftover item, normalized so the SAME real item produces the SAME
/// key whether it came from the scanner or from the model's clean request (case/separator/trailing-
/// dot insensitive paths; lower-cased names; registry hive+subkey canonicalized). Every key is
/// TYPE-tagged and registry keys KEEP the hive, so a proof can only authorize deletion of the exact
/// same object KIND at the exact same identity -- an injected item of a different type/hive is
/// refused even if a path string coincides. Pure -> testable.
[[nodiscard]] inline QString leftoverProvenanceKey(const LeftoverItem& item) {
    switch (item.type) {
    case LeftoverItem::Type::File:
    case LeftoverItem::Type::Folder:
    case LeftoverItem::Type::StartupEntry:
        return detail::fsFamilyKey(item);
    case LeftoverItem::Type::RegistryKey:
    case LeftoverItem::Type::ShellExtension:
    case LeftoverItem::Type::RegistryValue:
        return detail::registryFamilyKey(item);
    default:
        return detail::namedKey(item.type, item.path);
    }
}

/// Real on-disk identity fingerprint for a filesystem leftover (File / Folder / file-backed
/// StartupEntry): the object's size + creation + last-write timestamps as seen RIGHT NOW. Empty for
/// non-filesystem items (registry / service / task / registry-backed startup) and for a path that
/// does not currently resolve to an on-disk object. The proof-of-scan match folds this in: a path a
/// scan surfaced but whose object was SINCE swapped or replaced (an ancestor junction now
/// redirecting the leaf, or a delete-and-recreate) no longer satisfies the proof, because the
/// fingerprint captured at scan time does not equal the one re-read at clean time. This binds real
/// object identity on top of the normalized-text key. Cross-platform (QFileInfo) so it needs no
/// <windows.h> in this widely-included header; the delete-time by-handle GetFinalPathNameByHandle
/// re-verification (cleanup_worker and the recycle op) supplies the volume+FILE_ID-level swap
/// proof.
[[nodiscard]] inline QString fsIdentityFingerprint(const LeftoverItem& item) {
    const bool file_backed =
        item.type == LeftoverItem::Type::File || item.type == LeftoverItem::Type::Folder ||
        (item.type == LeftoverItem::Type::StartupEntry && item.registryValueName.isEmpty());
    if (!file_backed) {
        return {};
    }
    const QFileInfo info(item.path.trimmed());
    if (!info.exists()) {
        return {};
    }
    return QStringLiteral("sz:%1|bt:%2|mt:%3")
        .arg(info.size())
        .arg(info.birthTime().toMSecsSinceEpoch())
        .arg(info.lastModified().toMSecsSinceEpoch());
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
    /// sees), so any item the model legitimately copied is present. Each item is stored with the
    /// filesystem-identity fingerprint captured NOW (at scan time) so clean_leftovers can prove the
    /// object has not been swapped since. Bounded: when the cap would be exceeded the set is reset
    /// to just this scan's items (the most recent scan is the relevant one). Each call bumps the
    /// scan generation.
    void record(const QVector<LeftoverItem>& items) {
        QMutexLocker lock(&m_mutex);
        ++m_generation;
        if (m_entries.size() + items.size() > kMaxKeys) {
            m_entries.clear();
        }
        for (const LeftoverItem& item : items) {
            if (m_entries.size() >= kMaxKeys) {
                break;
            }
            m_entries.insert(leftoverProvenanceKey(item),
                             Entry{fsIdentityFingerprint(item), m_generation});
        }
    }

    /// True if @p item matches an item a prior scan surfaced this session AND its on-disk object is
    /// still the SAME one the scan fingerprinted. A filesystem item whose object was swapped or
    /// replaced since the scan (fingerprint changed) is rejected even though its path text still
    /// matches -- fail closed. Non-filesystem items (registry/service/task) carry an empty
    /// fingerprint at both record and check time, so their behavior is unchanged (text identity).
    [[nodiscard]] bool contains(const LeftoverItem& item) const {
        QMutexLocker lock(&m_mutex);
        const auto it = m_entries.constFind(leftoverProvenanceKey(item));
        if (it == m_entries.constEnd()) {
            return false;
        }
        return fsIdentityFingerprint(item) == it->fingerprint;
    }

    /// True if no scan has been recorded yet this session.
    [[nodiscard]] bool isEmpty() const {
        QMutexLocker lock(&m_mutex);
        return m_entries.isEmpty();
    }

    /// Monotonic count of record() calls this session (each scan bumps it). Exposed for tests and
    /// observability; the freshness binding itself is carried by the per-item fingerprint.
    [[nodiscard]] quint64 generation() const {
        QMutexLocker lock(&m_mutex);
        return m_generation;
    }

    /// Clear the session set (used by tests; also allows an explicit reset).
    void clear() {
        QMutexLocker lock(&m_mutex);
        m_entries.clear();
    }

private:
    LeftoverScanProvenance() = default;
    struct Entry {
        QString fingerprint;  ///< fs identity at scan time; empty for non-fs / unresolved items
        quint64 generation;   ///< scan generation that recorded this key
    };
    static constexpr int kMaxKeys = 200'000;
    mutable QMutex m_mutex;
    QHash<QString, Entry> m_entries;
    quint64 m_generation = 0;
};

}  // namespace sak
