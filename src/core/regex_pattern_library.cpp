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

// -- Construction ------------------------------------------------------------

RegexPatternLibrary::RegexPatternLibrary(QObject* parent) : QObject(parent) {
    initBuiltinPatterns();

    const QString dataDir = sak::app_paths::configDirectory();
    if (!QDir().mkpath(dataDir)) {
        sak::logWarning("Failed to create app config directory: {}", dataDir.toStdString());
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
    saveCustomPatterns();
    Q_EMIT patternsChanged();
}

void RegexPatternLibrary::removeCustomPattern(const QString& key) {
    auto it = std::remove_if(m_custom_patterns.begin(),
                             m_custom_patterns.end(),
                             [&key](const RegexPatternInfo& p) { return p.key == key; });

    if (it != m_custom_patterns.end()) {
        m_custom_patterns.erase(it, m_custom_patterns.end());
        saveCustomPatterns();
        Q_EMIT patternsChanged();
    }
}

void RegexPatternLibrary::updateCustomPattern(const QString& key,
                                              const QString& label,
                                              const QString& pattern) {
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
        it->label = label;
        it->pattern = pattern;
        saveCustomPatterns();
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
    QFile file(m_storage_file);
    if (!file.exists()) {
        return;
    }

    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        logWarning("RegexPatternLibrary: failed to open '{}' for reading",
                   m_storage_file.toStdString());
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

    const QJsonArray arr = doc.array();
    m_custom_patterns.clear();

    for (const auto& val : arr) {
        const QJsonObject obj = val.toObject();
        RegexPatternInfo info;
        info.key = obj["key"].toString();
        info.label = obj["label"].toString();
        info.pattern = obj["pattern"].toString();
        info.enabled = false;  // Always start disabled

        if (!info.key.isEmpty() && !info.pattern.isEmpty()) {
            m_custom_patterns.append(info);
        }
    }

    logInfo("RegexPatternLibrary: loaded {} custom patterns", m_custom_patterns.size());
}

void RegexPatternLibrary::saveCustomPatterns() {
    // Persisting an empty list is valid (the user may have deleted every custom
    // pattern), so there is no non-empty precondition here.
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
        return;
    }

    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        logError("RegexPatternLibrary: incomplete write of custom patterns");
        file.cancelWriting();
        return;
    }
    if (!file.commit()) {
        logError("RegexPatternLibrary: failed to commit custom patterns to '{}'",
                 m_storage_file.toStdString());
        return;
    }

    logInfo("RegexPatternLibrary: saved {} custom patterns", m_custom_patterns.size());
}

}  // namespace sak
