// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file regex_pattern_library.cpp
/// @brief Implements built-in and custom regex pattern management

#include "sak/regex_pattern_library.h"

#include "sak/logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace sak {

// ── Construction ────────────────────────────────────────────────────────────

RegexPatternLibrary::RegexPatternLibrary(QObject* parent) : QObject(parent) {
    initBuiltinPatterns();

    // Determine storage file location
    const QString appDir = QCoreApplication::applicationDirPath();
    const QString portableIni = appDir + "/portable.ini";

    if (QFile::exists(portableIni)) {
        // Portable mode: store alongside the executable
        m_storage_file = appDir + "/custom_regex_patterns.json";
    } else {
        // Installed mode: use standard app data location
        const QString dataDir =
            QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (!QDir().mkpath(dataDir)) {
            sak::logWarning("Failed to create app data directory: {}", dataDir.toStdString());
        }
        m_storage_file = dataDir + "/custom_regex_patterns.json";
    }

    loadCustomPatterns();
}

// ── Built-in Patterns ───────────────────────────────────────────────────────

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

// ── Accessors ───────────────────────────────────────────────────────────────

QVector<RegexPatternInfo> RegexPatternLibrary::builtinPatterns() const {
    return m_builtin_patterns;
}

QVector<RegexPatternInfo> RegexPatternLibrary::customPatterns() const {
    return m_custom_patterns;
}

// ── Pattern Management ──────────────────────────────────────────────────────

void RegexPatternLibrary::addCustomPattern(const QString& key,
                                           const QString& label,
                                           const QString& pattern) {
    // Check for duplicate keys in both built-in and custom patterns
    for (const auto& p : m_builtin_patterns) {
        if (p.key == key) {
            logWarning("RegexPatternLibrary: key '{}' conflicts with built-in pattern, rejected",
                       key.toStdString());
            return;
        }
    }
    for (const auto& p : m_custom_patterns) {
        if (p.key == key) {
            logWarning("RegexPatternLibrary: key '{}' already exists in custom patterns, rejected",
                       key.toStdString());
            return;
        }
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

    for (auto& p : m_custom_patterns) {
        if (p.key == key) {
            p.label = label;
            p.pattern = pattern;
            saveCustomPatterns();
            Q_EMIT patternsChanged();
            return;
        }
    }

    logWarning("RegexPatternLibrary: key '{}' not found for update", key.toStdString());
}

void RegexPatternLibrary::setPatternEnabled(const QString& key, bool enabled) {
    Q_ASSERT(!key.isEmpty());
    // Check built-in patterns first
    for (auto& p : m_builtin_patterns) {
        if (p.key == key) {
            p.enabled = enabled;
            Q_EMIT patternsChanged();
            return;
        }
    }

    // Then check custom patterns
    for (auto& p : m_custom_patterns) {
        if (p.key == key) {
            p.enabled = enabled;
            Q_EMIT patternsChanged();
            return;
        }
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
    int count = 0;
    for (const auto& p : m_builtin_patterns) {
        if (p.enabled) {
            ++count;
        }
    }
    for (const auto& p : m_custom_patterns) {
        if (p.enabled) {
            ++count;
        }
    }
    return count;
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

// ── Persistence ─────────────────────────────────────────────────────────────

void RegexPatternLibrary::loadCustomPatterns() {
    Q_ASSERT(!m_custom_patterns.empty());
    Q_ASSERT(!m_custom_patterns.isEmpty());
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
    Q_ASSERT(!m_custom_patterns.empty());
    Q_ASSERT(!m_custom_patterns.isEmpty());
    QJsonArray arr;

    for (const auto& p : m_custom_patterns) {
        QJsonObject obj;
        obj["key"] = p.key;
        obj["label"] = p.label;
        obj["pattern"] = p.pattern;
        arr.append(obj);
    }

    QJsonDocument doc(arr);

    QFile file(m_storage_file);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        logError("RegexPatternLibrary: failed to open '{}' for writing",
                 m_storage_file.toStdString());
        return;
    }

    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        logError("RegexPatternLibrary: incomplete write of custom patterns");
        return;
    }
    file.close();

    logInfo("RegexPatternLibrary: saved {} custom patterns", m_custom_patterns.size());
}

}  // namespace sak
