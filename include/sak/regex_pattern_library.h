// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file regex_pattern_library.h
/// @brief Built-in and custom regex pattern presets for Advanced Search
///
/// Provides 8 built-in regex patterns (email, URL, IPv4, phone, dates,
/// numbers, hex, identifiers) plus user-defined custom patterns with
/// persistent JSON storage.

#pragma once

#include "sak/advanced_search_types.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <type_traits>

namespace sak {

/// @brief Manages built-in and custom regex pattern presets
///
/// Provides 8 built-in patterns and supports custom user patterns with
/// persistent JSON storage. Patterns can be enabled/disabled individually
/// and combined into a single OR-joined regex for multi-pattern search.
class RegexPatternLibrary : public QObject {
    Q_OBJECT

public:
    explicit RegexPatternLibrary(QObject* parent = nullptr);
    ~RegexPatternLibrary() override = default;

    // Disable copy/move
    RegexPatternLibrary(const RegexPatternLibrary&) = delete;
    RegexPatternLibrary& operator=(const RegexPatternLibrary&) = delete;
    RegexPatternLibrary(RegexPatternLibrary&&) = delete;
    RegexPatternLibrary& operator=(RegexPatternLibrary&&) = delete;

    /// @brief Get all built-in patterns
    [[nodiscard]] QVector<RegexPatternInfo> builtinPatterns() const;

    /// @brief Get all custom user patterns
    [[nodiscard]] QVector<RegexPatternInfo> customPatterns() const;

    /// @brief Add a custom pattern
    void addCustomPattern(const QString& key, const QString& label, const QString& pattern);

    /// @brief Remove a custom pattern by key
    void removeCustomPattern(const QString& key);

    /// @brief Update a custom pattern
    void updateCustomPattern(const QString& key, const QString& label, const QString& pattern);

    /// @brief Set enabled state for a pattern (built-in or custom)
    void setPatternEnabled(const QString& key, bool enabled);

    /// @brief Get combined regex string for all enabled patterns
    [[nodiscard]] QString combinedPattern() const;

    /// @brief Get count of active (enabled) patterns
    [[nodiscard]] int activeCount() const;

    /// @brief Disable all patterns
    void clearAll();

    /// @brief Load custom patterns from persistent storage
    void loadCustomPatterns();

    /// @brief Save custom patterns to persistent storage
    void saveCustomPatterns();

Q_SIGNALS:
    /// @brief Emitted when patterns are modified (add, remove, enable, disable)
    void patternsChanged();

private:
    /// @brief Initialize the 8 built-in regex patterns
    void initBuiltinPatterns();

    QVector<RegexPatternInfo> m_builtin_patterns;
    QVector<RegexPatternInfo> m_custom_patterns;
    QString m_storage_file;
};

// ── Compile-Time Invariants (TigerStyle) ────────────────────────────────────

/// RegexPatternLibrary must inherit QObject.
static_assert(std::is_base_of_v<QObject, RegexPatternLibrary>,
              "RegexPatternLibrary must inherit QObject.");

/// RegexPatternLibrary must not be copyable.
static_assert(!std::is_copy_constructible_v<RegexPatternLibrary>,
              "RegexPatternLibrary must not be copy-constructible.");

/// RegexPatternLibrary must not be movable.
static_assert(!std::is_move_constructible_v<RegexPatternLibrary>,
              "RegexPatternLibrary must not be move-constructible.");

}  // namespace sak
