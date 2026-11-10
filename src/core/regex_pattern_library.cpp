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
#include <QRegularExpression>
#include <QSaveFile>

#include <algorithm>
#include <optional>

namespace sak {

namespace {
// A custom-patterns file is a short JSON array; reject anything implausibly large
// BEFORE readAll() so a tampered or oversized file cannot exhaust memory.
constexpr qint64 kMaxCustomPatternsBytes = 4LL * 1024 * 1024;  // 4 MiB

// A single regex pattern is a short string; a multi-kilobyte pattern is a tampered or
// pathological entry, not a real user pattern. Cap it on every ingress (add/update/load).
constexpr int kMaxPatternLength = 4096;

// The custom-patterns list is user-curated and small; bound its size so a tampered file
// cannot inject an unbounded number of entries even while staying under the byte cap.
constexpr int kMaxCustomPatternCount = 1000;

// Enforce, for one candidate entry, the SAME invariants addCustomPattern() enforces so a
// hand-edited or tampered file cannot inject entries the in-memory API would reject:
// non-empty key+pattern, pattern-length cap, no built-in/existing-custom key collision, and
// valid regex. Returns true only for an entry that would also round-trip through the add path.
bool customEntryAccepted(const RegexPatternInfo& info,
                         const QVector<RegexPatternInfo>& builtin,
                         const QVector<RegexPatternInfo>& accepted) {
    if (info.key.isEmpty() || info.pattern.isEmpty()) {
        logWarning("RegexPatternLibrary: skipping custom pattern with empty key or pattern");
        return false;
    }
    if (info.pattern.size() > kMaxPatternLength) {
        logWarning("RegexPatternLibrary: skipping custom pattern '{}' whose length {} exceeds {}",
                   info.key.toStdString(),
                   static_cast<long long>(info.pattern.size()),
                   kMaxPatternLength);
        return false;
    }
    if (std::ranges::any_of(builtin, [&info](const auto& p) { return p.key == info.key; })) {
        logWarning("RegexPatternLibrary: skipping custom pattern '{}' (conflicts with built-in)",
                   info.key.toStdString());
        return false;
    }
    if (std::ranges::any_of(accepted, [&info](const auto& p) { return p.key == info.key; })) {
        logWarning("RegexPatternLibrary: skipping duplicate custom pattern key '{}'",
                   info.key.toStdString());
        return false;
    }
    const QRegularExpression testRegex(info.pattern);
    if (!testRegex.isValid()) {
        logWarning("RegexPatternLibrary: skipping custom pattern '{}' with invalid regex: {}",
                   info.key.toStdString(),
                   testRegex.errorString().toStdString());
        return false;
    }
    return true;
}
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
        {.key = "emails",
         .label = "Email addresses",
         .pattern = R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)",
         .enabled = false},

        {.key = "urls",
         .label = "URLs (http/https)",
         .pattern = R"(https?://[^\s]+)",
         .enabled = false},

        {.key = "ipv4",
         .label = "IPv4 addresses",
         .pattern = R"(\b(?:[0-9]{1,3}\.){3}[0-9]{1,3}\b)",
         .enabled = false},

        {.key = "phone",
         .label = "Phone numbers",
         .pattern = R"(\b(?:\+?1[-.]?)?(?:\(?[0-9]{3}\)?[-.]?)?[0-9]{3}[-.]?[0-9]{4}\b)",
         .enabled = false},

        {.key = "dates",
         .label = "Dates (various)",
         .pattern = R"(\b\d{1,4}[-/.]\d{1,2}[-/.]\d{1,4}\b)",
         .enabled = false},

        {.key = "numbers", .label = "Numbers", .pattern = R"(\b\d+\b)", .enabled = false},

        {.key = "hex",
         .label = "Hex values",
         .pattern = R"(\b0x[0-9A-Fa-f]+\b|#[0-9A-Fa-f]{6}\b)",
         .enabled = false},

        {.key = "words",
         .label = "Words/identifiers",
         .pattern = R"(\b[A-Za-z_]\w*\b)",
         .enabled = false},
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
    // Bound a single pattern's length and the total custom-pattern count so neither the
    // interactive path nor a scripted caller can grow them without limit (the same caps
    // loadCustomPatterns() enforces on a persisted file).
    if (pattern.size() > kMaxPatternLength) {
        logWarning("RegexPatternLibrary: pattern length {} exceeds cap {}, rejected",
                   static_cast<long long>(pattern.size()),
                   kMaxPatternLength);
        return;
    }
    if (m_custom_patterns.size() >= kMaxCustomPatternCount) {
        logWarning("RegexPatternLibrary: custom pattern count cap {} reached, rejected",
                   kMaxCustomPatternCount);
        return;
    }
    // Check for duplicate keys in both built-in and custom patterns
    if (std::ranges::any_of(m_builtin_patterns, [&key](const auto& p) { return p.key == key; })) {
        logWarning("RegexPatternLibrary: key '{}' conflicts with built-in pattern, rejected",
                   key.toStdString());
        return;
    }
    if (std::ranges::any_of(m_custom_patterns, [&key](const auto& p) { return p.key == key; })) {
        logWarning("RegexPatternLibrary: key '{}' already exists in custom patterns, rejected",
                   key.toStdString());
        return;
    }

    // Validate regex before accepting
    const QRegularExpression testRegex(pattern);
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
    const QVector<RegexPatternInfo> previous = m_custom_patterns;  // snapshot for rollback
    const auto removed = std::ranges::remove_if(
        m_custom_patterns, [&key](const RegexPatternInfo& p) { return p.key == key; });

    if (removed.begin() != m_custom_patterns.end()) {
        m_custom_patterns.erase(removed.begin(), m_custom_patterns.end());
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
    // Cap the replacement pattern's length for the same reason add does (a stored pattern
    // must also survive the loadCustomPatterns() length cap on the next start).
    if (pattern.size() > kMaxPatternLength) {
        logWarning("RegexPatternLibrary: updated pattern length {} exceeds cap {}, rejected",
                   static_cast<long long>(pattern.size()),
                   kMaxPatternLength);
        return;
    }
    // Validate regex before accepting update
    const QRegularExpression testRegex(pattern);
    if (!testRegex.isValid()) {
        logWarning("RegexPatternLibrary: updated pattern '{}' is invalid regex: {}",
                   pattern.toStdString(),
                   testRegex.errorString().toStdString());
        return;
    }

    auto it = std::ranges::find_if(m_custom_patterns,

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
    auto builtin_it = std::ranges::find_if(m_builtin_patterns,

                                           [&key](const auto& p) { return p.key == key; });
    if (builtin_it != m_builtin_patterns.end()) {
        builtin_it->enabled = enabled;
        Q_EMIT patternsChanged();
        return;
    }

    // Then check custom patterns
    auto custom_it = std::ranges::find_if(m_custom_patterns,

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
    return static_cast<int>(std::count_if(m_builtin_patterns.begin(),
                                          m_builtin_patterns.end(),
                                          [](const auto& p) { return p.enabled; }) +
                            std::count_if(m_custom_patterns.begin(),
                                          m_custom_patterns.end(),
                                          [](const auto& p) { return p.enabled; }));
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

namespace {

/// Read the persisted custom-pattern array, or nullopt when there is nothing trustworthy to
/// read. Every rejection path is deliberate and logged: absent file, unopenable file,
/// out-of-bounds size, unparseable JSON, and a root that is not an array. A wrong root type in
/// particular is NOT an empty library -- reporting it as zero patterns would hide a corrupt or
/// tampered file behind a plausible-looking result.
std::optional<QJsonArray> readPersistedCustomPatterns(const QString& storage_file) {
    QFile file(storage_file);
    if (!file.exists()) {
        return std::nullopt;
    }
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logWarning("RegexPatternLibrary: failed to open '{}' for reading",
                   storage_file.toStdString());
        return std::nullopt;
    }

    // Bound the read: a custom-patterns file is small, so an implausibly large (or
    // unreadable-size) file is corrupt or hostile -- reject it BEFORE readAll()
    // instead of allocating from its self-reported size.
    const qint64 file_size = file.size();
    if (file_size < 0 || file_size > kMaxCustomPatternsBytes) {
        logWarning("RegexPatternLibrary: '{}' size {} is out of bounds; not loaded",
                   storage_file.toStdString(),
                   static_cast<long long>(file_size));
        return std::nullopt;
    }

    const QByteArray data = file.readAll();
    file.close();

    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        logWarning("RegexPatternLibrary: JSON parse error: {}",
                   parseError.errorString().toStdString());
        return std::nullopt;
    }
    if (!doc.isArray()) {
        logWarning("RegexPatternLibrary: '{}' is not a JSON array; not loaded",
                   storage_file.toStdString());
        return std::nullopt;
    }
    return doc.array();
}

}  // namespace

void RegexPatternLibrary::appendAcceptedCustomPatterns(const QJsonArray& entries) {
    for (const auto& val : entries) {
        // Bound the number of entries taken from a persisted (possibly tampered) file, even
        // one that stayed under the byte cap; stop scanning -- do not silently keep going --
        // once the count cap is reached.
        if (m_custom_patterns.size() >= kMaxCustomPatternCount) {
            logWarning("RegexPatternLibrary: custom pattern count cap {} reached; ignoring rest",
                       kMaxCustomPatternCount);
            return;
        }
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

        // customEntryAccepted() applies the SAME invariants addCustomPattern() enforces
        // (non-empty key+pattern, length cap, no key collision, valid regex), so a
        // hand-edited or tampered file cannot inject patterns the in-memory API would reject.
        if (customEntryAccepted(info, m_builtin_patterns, m_custom_patterns)) {
            m_custom_patterns.append(info);
        }
    }
}

void RegexPatternLibrary::loadCustomPatterns() {
    // No precondition on m_custom_patterns: this function is the populator, and a
    // fresh profile legitimately has zero saved custom patterns. (A prior inverted
    // Q_ASSERT(!empty) here aborted every debug-build construction of the library.)
    if (m_storage_file.isEmpty()) {
        return;  // persistence disabled (no usable config dir) -- fail closed
    }
    const std::optional<QJsonArray> entries = readPersistedCustomPatterns(m_storage_file);
    if (!entries) {
        return;
    }

    m_custom_patterns.clear();
    appendAcceptedCustomPatterns(*entries);

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

    const QJsonDocument doc(arr);

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
