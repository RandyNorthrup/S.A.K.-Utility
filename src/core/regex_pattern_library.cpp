// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file regex_pattern_library.cpp
/// @brief Implements built-in and custom regex pattern management

#include "sak/regex_pattern_library.h"

#include "sak/app_paths.h"
#include "sak/logger.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

#include <algorithm>

namespace sak {

namespace {
// A custom-patterns file is a short JSON array; reject anything implausibly large
// BEFORE readAll() so a tampered or oversized file cannot exhaust memory.
constexpr qint64 kMaxCustomPatternsBytes = 4LL * 1024 * 1024;  // 4 MiB
}  // namespace

// -- Construction ------------------------------------------------------------

RegexPatternLibrary::RegexPatternLibrary(QObject* parent) : QObject(parent) {
    initBuiltinPatterns();

    const QString dataDir = sak::app_paths::configDirectory();
    // Fail closed: without a concrete, creatable config directory we must NOT fall
    // back to a CWD-relative "custom_regex_patterns.json" -- an elevated process would
    // then read and write an attacker-influenceable path. Leave m_storage_file empty
    // so persistence is disabled; the built-in patterns still work and custom
    // add/save refuse rather than persist to a guessed target.
    if (dataDir.isEmpty() || !QDir().mkpath(dataDir)) {
        sak::logError(
            "RegexPatternLibrary: no usable config directory; custom pattern "
            "persistence disabled");
        return;
    }
    m_storage_file = QDir(dataDir).filePath(QStringLiteral("custom_regex_patterns.json"));

    loadCustomPatterns();
}

// -- Built-in Patterns -------------------------------------------------------

void RegexPatternLibrary::initBuiltinPatterns() {
    m_builtin_patterns = {
        {"emails",
         "Email addresses",
         R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)",
         false},

        {"urls", "URLs (http/https)", R"(https?://[^\s]+)", false},

        {"ipv4", "IPv4 addresses", R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)", false},

        {"phone",
         "Phone numbers",
         R"(\b(?:\+?1[-.]?)?(?:\(?[0-9]{3}\)?[-.]?)?[0-9]{3}[-.]?[0-9]{4}\b)",
         false},

        {"dates", "Dates (various)", R"(\b\d{1,4}[-/.]\d{1,2}[-/.]\d{1,4}\b)", false},

        {"numbers", "Numbers", R"(\b\d+\b)", false},

        {"hex", "Hex values", R"(\b0x[0-9A-Fa-f]+\b|#[0-9A-Fa-f]{6}\b)", false},

        {"words", "Words/identifiers", R"(\b[A-Za-z_]\w*\b)", false},
    };
}

// -- Accessors ---------------------------------------------------------------

QVector<RegexPatternInfo> RegexPatternLibrary::builtinPatterns() const {
    return m_builtin_patterns;
}

QVector<RegexPatternInfo> RegexPatternLibrary::customPatterns() const {
    return m_custom_patterns;
}

// -- Pattern Management ------------------------------------------------------

void RegexPatternLibrary::addCustomPattern(const QString& key,
                                           const QString& label,
                                           const QString& pattern) {
    // Reject an empty key or pattern: an empty pattern is a valid regex that matches
    // the empty string at every position, and a saved empty key/pattern is silently
    // dropped on reload -- so accept only entries that round-trip.
    if (key.isEmpty() || pattern.isEmpty()) {
        logWarning("RegexPatternLibrary: empty key or pattern rejected");
        return;
    }
    // Check for duplicate keys in both built-in and custom patterns
    if (std::any_of(m_builtin_patterns.begin(), m_builtin_patterns.end(), [&key](const auto& p) {
            return p.key == key;
        })) {
        logWarning("RegexPatternLibrary: key '{}' conflicts with built-in pattern, rejected",
                   key.toStdString());
        return;
    }
    if (std::any_of(m_custom_patterns.begin(), m_custom_patterns.end(), [&key](const auto& p) {
            return p.key == key;
        })) {
        logWarning("RegexPatternLibrary: key '{}' already exists in custom patterns, rejected",
                   key.toStdString());
        return;
    }

    // Validate regex before accepting
    QRegularExpression testRegex(pattern);
    if (!testRegex.isValid()) {
        logWarning("RegexPatternLibrary: pattern '{}' is invalid regex: {}",
                   pattern.toStdString(),
                   testRegex.errorString().toStdString());
        return;
    }

    RegexPatternInfo info;
    info.key = key;
    info.label = label;
    info.pattern = pattern;
    info.enabled = false;

    m_custom_patterns.append(info);
    if (!saveCustomPatterns()) {
        // Keep memory and disk consistent: if the change cannot be persisted, do not
        // report it as applied (no patternsChanged) -- otherwise it would silently
        // vanish on the next restart.
        m_custom_patterns.removeLast();
        return;
    }
    Q_EMIT patternsChanged();
}

void RegexPatternLibrary::removeCustomPattern(const QString& key) {
    QVector<RegexPatternInfo> previous = m_custom_patterns;  // snapshot for rollback
    auto it = std::remove_if(m_custom_patterns.begin(),
                             m_custom_patterns.end(),
                             [&key](const RegexPatternInfo& p) { return p.key == key; });

    if (it != m_custom_patterns.end()) {
        m_custom_patterns.erase(it, m_custom_patterns.end());
        if (!saveCustomPatterns()) {
            // A failed persist must not leave memory diverged from disk (the removal
            // would silently reappear on restart); restore and report nothing changed.
            m_custom_patterns = previous;
            return;
        }
        Q_EMIT patternsChanged();
    }
}

void RegexPatternLibrary::updateCustomPattern(const QString& key,
                                              const QString& label,
                                              const QString& pattern) {
    // Reject an empty key or pattern for the same reason add does: an empty pattern
    // matches everywhere and would not round-trip through storage.
    if (key.isEmpty() || pattern.isEmpty()) {
        logWarning("RegexPatternLibrary: empty key or pattern rejected for update");
        return;
    }
    // Validate regex before accepting update
    QRegularExpression testRegex(pattern);
    if (!testRegex.isValid()) {
        logWarning("RegexPatternLibrary: updated pattern '{}' is invalid regex: {}",
                   pattern.toStdString(),
                   testRegex.errorString().toStdString());
        return;
    }

    auto it = std::find_if(m_custom_patterns.begin(),
                           m_custom_patterns.end(),
                           [&key](const auto& p) { return p.key == key; });
    if (it != m_custom_patterns.end()) {
        const RegexPatternInfo previous = *it;  // snapshot for rollback
        it->label = label;
        it->pattern = pattern;
        if (!saveCustomPatterns()) {
            // Do not leave the in-memory update unpersisted (it would revert on
            // restart); restore the prior value and report nothing changed.
            *it = previous;
            return;
        }
        Q_EMIT patternsChanged();
        return;
    }

    logWarning("RegexPatternLibrary: key '{}' not found for update", key.toStdString());
}

void RegexPatternLibrary::setPatternEnabled(const QString& key, bool enabled) {
    // No pattern carries an empty key, so an empty key takes the unknown-key path
    // below: both lookups miss and nothing is toggled.
    // Check built-in patterns first
    auto builtin_it = std::find_if(m_builtin_patterns.begin(),
                                   m_builtin_patterns.end(),
                                   [&key](const auto& p) { return p.key == key; });
    if (builtin_it != m_builtin_patterns.end()) {
        builtin_it->enabled = enabled;
        Q_EMIT patternsChanged();
        return;
    }

    // Then check custom patterns
    auto custom_it = std::find_if(m_custom_patterns.begin(),
                                  m_custom_patterns.end(),
                                  [&key](const auto& p) { return p.key == key; });
    if (custom_it != m_custom_patterns.end()) {
        custom_it->enabled = enabled;
        Q_EMIT patternsChanged();
        return;
    }
}

QString RegexPatternLibrary::combinedPattern() const {
    QStringList activePatterns;

    for (const auto& p : m_builtin_patterns) {
        if (p.enabled) {
            activePatterns.append(QString("(?:%1)").arg(p.pattern));
        }
    }

    for (const auto& p : m_custom_patterns) {
        if (p.enabled) {
            activePatterns.append(QString("(?:%1)").arg(p.pattern));
        }
    }

    return activePatterns.join('|');
}

int RegexPatternLibrary::activeCount() const {
    return std::count_if(m_builtin_patterns.begin(),
                         m_builtin_patterns.end(),
                         [](const auto& p) { return p.enabled; }) +
           std::count_if(m_custom_patterns.begin(), m_custom_patterns.end(), [](const auto& p) {
               return p.enabled;
           });
}

void RegexPatternLibrary::clearAll() {
    for (auto& p : m_builtin_patterns) {
        p.enabled = false;
    }
    for (auto& p : m_custom_patterns) {
        p.enabled = false;
    }
    Q_EMIT patternsChanged();
}

// -- Persistence -------------------------------------------------------------

void RegexPatternLibrary::loadCustomPatterns() {
    // No precondition on m_custom_patterns: this function is the populator, and a
    // fresh profile legitimately has zero saved custom patterns. (A prior inverted
    // Q_ASSERT(!empty) here aborted every debug-build construction of the library.)
    if (m_storage_file.isEmpty()) {
        return;  // persistence disabled (no usable config dir) -- fail closed
    }
    QFile file(m_storage_file);
    if (!file.exists()) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logWarning("RegexPatternLibrary: failed to open '{}' for reading",
                   m_storage_file.toStdString());
        return;
    }

    // Bound the read: a custom-patterns file is small, so an implausibly large (or
    // unreadable-size) file is corrupt or hostile -- reject it BEFORE readAll()
    // instead of allocating from its self-reported size.
    const qint64 file_size = file.size();
    if (file_size < 0 || file_size > kMaxCustomPatternsBytes) {
        logWarning("RegexPatternLibrary: '{}' size {} is out of bounds; not loaded",
                   m_storage_file.toStdString(),
                   static_cast<long long>(file_size));
        return;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        logWarning("RegexPatternLibrary: JSON parse error: {}",
                   parseError.errorString().toStdString());
        return;
    }

    // The file must be a JSON array of pattern objects. A wrong root type is not an
    // empty library -- surface it rather than silently treating it as zero patterns.
    if (!doc.isArray()) {
        logWarning("RegexPatternLibrary: '{}' is not a JSON array; not loaded",
                   m_storage_file.toStdString());
        return;
    }

    const QJsonArray arr = doc.array();
    m_custom_patterns.clear();

    for (const auto& val : arr) {
        if (!val.isObject()) {
            logWarning("RegexPatternLibrary: skipping non-object custom-pattern entry");
            continue;
        }
        const QJsonObject obj = val.toObject();
        RegexPatternInfo info;
        info.key = obj["key"].toString();
        info.label = obj["label"].toString();
        info.pattern = obj["pattern"].toString();
        info.enabled = false;  // Always start disabled

        // Enforce the SAME invariants addCustomPattern() enforces, so a hand-edited or
        // tampered file cannot inject patterns the in-memory API would reject:
        // non-empty key+pattern, no built-in/custom key collision, and valid regex.
        if (info.key.isEmpty() || info.pattern.isEmpty()) {
            logWarning("RegexPatternLibrary: skipping custom pattern with empty key or pattern");
            continue;
        }
        if (std::any_of(m_builtin_patterns.begin(),
                        m_builtin_patterns.end(),
                        [&info](const auto& p) { return p.key == info.key; })) {
            logWarning(
                "RegexPatternLibrary: skipping custom pattern '{}' (conflicts with built-in)",
                info.key.toStdString());
            continue;
        }
        if (std::any_of(m_custom_patterns.begin(), m_custom_patterns.end(), [&info](const auto& p) {
                return p.key == info.key;
            })) {
            logWarning("RegexPatternLibrary: skipping duplicate custom pattern key '{}'",
                       info.key.toStdString());
            continue;
        }
        const QRegularExpression testRegex(info.pattern);
        if (!testRegex.isValid()) {
            logWarning("RegexPatternLibrary: skipping custom pattern '{}' with invalid regex: {}",
                       info.key.toStdString(),
                       testRegex.errorString().toStdString());
            continue;
        }

        m_custom_patterns.append(info);
    }

    logInfo("RegexPatternLibrary: loaded {} custom patterns", m_custom_patterns.size());
}

bool RegexPatternLibrary::saveCustomPatterns() {
    // Persisting an empty list is valid (the user may have deleted every custom
    // pattern), so there is no non-empty precondition here.
    if (m_storage_file.isEmpty()) {
        return false;  // persistence disabled -- fail closed, never invent a path
    }
    QJsonArray arr;

    for (const auto& p : m_custom_patterns) {
        QJsonObject obj;
        obj["key"] = p.key;
        obj["label"] = p.label;
        obj["pattern"] = p.pattern;
        arr.append(obj);
    }

    QJsonDocument doc(arr);

    // QSaveFile writes to a temporary and atomically renames on commit(), so a
    // short write / crash / full disk leaves the previously valid library intact
    // instead of truncating it to partial or empty JSON.
    QSaveFile file(m_storage_file);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("RegexPatternLibrary: failed to open '{}' for writing",
                 m_storage_file.toStdString());
        return false;
    }

    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        logError("RegexPatternLibrary: incomplete write of custom patterns");
        file.cancelWriting();
        return false;
    }
    if (!file.commit()) {
        logError("RegexPatternLibrary: failed to commit custom patterns to '{}'",
                 m_storage_file.toStdString());
        return false;
    }

    logInfo("RegexPatternLibrary: saved {} custom patterns", m_custom_patterns.size());
    return true;
}

}  // namespace sak
