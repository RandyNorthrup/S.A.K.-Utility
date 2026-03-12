// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_search_types.h
/// @brief Shared data types for the Advanced Search panel
///
/// Contains the core value types used across the search worker, controller,
/// panel UI, and metadata extractors: SearchMatch, SearchConfig,
/// SearchPreferences, and RegexPatternInfo.

#pragma once

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <type_traits>

namespace sak {

// -- SearchMatch -------------------------------------------------------------

/// @brief Represents a single search match within a file
struct SearchMatch {
    QString file_path;           ///< Full path (or archive_path/internal_path)
    int line_number = 0;         ///< 1-based line number (byte offset for binary)
    QString line_content;        ///< Content of the matching line
    int match_start = 0;         ///< Character offset of match start within line
    int match_end = 0;           ///< Character offset of match end within line
    QStringList context_before;  ///< Lines before the match (0-10)
    QStringList context_after;   ///< Lines after the match (0-10)
};

// -- SearchConfig ------------------------------------------------------------

/// @brief Search configuration / options passed to the search worker
struct SearchConfig {
    QString root_path;  ///< Directory or file to search
    QString pattern;    ///< Search pattern (text or regex)

    // Search mode flags
    bool case_sensitive = false;
    bool use_regex = false;
    bool whole_word = false;
    bool search_image_metadata = false;  ///< EXIF/GPS metadata search
    bool search_file_metadata = false;   ///< PDF/Office/audio/video metadata
    bool search_in_archives = false;     ///< Search inside ZIP/EPUB
    bool hex_search = false;             ///< Binary/hex search mode

    // Filtering
    QStringList file_extensions;      ///< Empty = all files
    QStringList exclude_patterns = {  ///< Exclusion regex patterns
        R"(\.git)",
        R"(\.svn)",
        R"(__pycache__)",
        R"(node_modules)",
        R"(\.pyc$)",
        R"(\.exe$)",
        R"(\.dll$)",
        R"(\.so$)",
        R"(\.bin$)"};

    // Limits
    int context_lines = 2;                      ///< Lines of context before/after (0-10)
    int max_results = 0;                        ///< 0 = unlimited
    qint64 max_file_size = 50LL * 1024 * 1024;  ///< 50 MB default
    int network_timeout_sec = 5;                ///< UNC path timeout
};

// -- RegexPatternInfo --------------------------------------------------------

/// @brief A regex pattern preset (built-in or user-defined)
struct RegexPatternInfo {
    QString key;           ///< Unique identifier (e.g., "emails")
    QString label;         ///< Display label (e.g., "Email addresses")
    QString pattern;       ///< Regex pattern string
    bool enabled = false;  ///< Currently active in combined search
};

// -- SearchPreferences -------------------------------------------------------

/// @brief Persistent search preferences
struct SearchPreferences {
    int max_results = 0;  ///< 0 = unlimited
    int max_preview_file_size_mb = 10;
    int max_search_file_size_mb = 50;
    int max_cache_size = 50;  ///< LRU file cache entries
    int context_lines = 2;    ///< Default context lines
};

// -- File type classification sets -------------------------------------------

/// @brief Image file extensions supporting metadata extraction
inline const QSet<QString> kImageExtensions = {
    "jpg", "jpeg", "png", "tiff", "tif", "gif", "bmp", "webp"};

/// @brief File extensions supporting metadata extraction
inline const QSet<QString> kFileMetadataExtensions = {
    // Documents
    "pdf",
    "docx",
    "xlsx",
    "pptx",
    // OpenDocument
    "odt",
    "ods",
    "odp",
    // eBooks
    "epub",
    // Structured data
    "json",
    "csv",
    "xml",
    // Rich text
    "rtf",
    // Database
    "sqlite",
    "sqlite3",
    "db",
    // Audio
    "mp3",
    "flac",
    "m4a",
    "ogg",
    "wma",
    "wav",
    // Video
    "mp4",
    "avi",
    "mkv",
    "mov",
    "wmv",
    // Screenwriting
    "fdx",
    "fountain",
    "celtx"};

/// @brief Archive extensions that can be searched internally
inline const QSet<QString> kArchiveExtensions = {"zip", "epub"};

// -- Compile-Time Invariants (TigerStyle) ------------------------------------

/// SearchMatch must be default-constructible for QVector usage.
static_assert(std::is_default_constructible_v<SearchMatch>,
              "SearchMatch must be default-constructible.");

/// SearchConfig must be default-constructible with sane defaults.
static_assert(std::is_default_constructible_v<SearchConfig>,
              "SearchConfig must be default-constructible.");

/// SearchPreferences must be default-constructible.
static_assert(std::is_default_constructible_v<SearchPreferences>,
              "SearchPreferences must be default-constructible.");

/// RegexPatternInfo must be default-constructible.
static_assert(std::is_default_constructible_v<RegexPatternInfo>,
              "RegexPatternInfo must be default-constructible.");

/// All data types must be copyable for signal/slot passing.
static_assert(std::is_copy_constructible_v<SearchMatch>,
              "SearchMatch must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<SearchConfig>,
              "SearchConfig must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<RegexPatternInfo>,
              "RegexPatternInfo must be copy-constructible for signal/slot.");
static_assert(std::is_copy_constructible_v<SearchPreferences>,
              "SearchPreferences must be copy-constructible for signal/slot.");

}  // namespace sak
