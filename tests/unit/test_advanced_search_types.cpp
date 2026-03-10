// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_advanced_search_types.cpp
/// @brief Unit tests for Advanced Search shared data types

#include "sak/advanced_search_types.h"

#include <QtTest/QtTest>

#include <type_traits>

class AdvancedSearchTypesTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── SearchMatch ──
    void searchMatch_defaultConstruction();
    void searchMatch_valueSemantics();
    void searchMatch_contextLines();

    // ── SearchConfig ──
    void searchConfig_defaultValues();
    void searchConfig_defaultExcludes();
    void searchConfig_valueSemantics();

    // ── SearchPreferences ──
    void searchPreferences_defaultValues();
    void searchPreferences_copyable();

    // ── RegexPatternInfo ──
    void regexPatternInfo_defaultConstruction();
    void regexPatternInfo_copyable();

    // ── Extension Sets ──
    void imageExtensions_containsExpected();
    void imageExtensions_doesNotContainWrong();
    void fileMetadataExtensions_containsExpected();
    void archiveExtensions_containsExpected();

    // ── Compile-Time Invariants ──
    void staticAsserts_defaultConstructible();
    void staticAsserts_copyConstructible();
};

// ── SearchMatch ─────────────────────────────────────────────────────────────

void AdvancedSearchTypesTests::searchMatch_defaultConstruction() {
    sak::SearchMatch match;

    QVERIFY(match.file_path.isEmpty());
    QCOMPARE(match.line_number, 0);
    QVERIFY(match.line_content.isEmpty());
    QCOMPARE(match.match_start, 0);
    QCOMPARE(match.match_end, 0);
    QVERIFY(match.context_before.isEmpty());
    QVERIFY(match.context_after.isEmpty());
}

void AdvancedSearchTypesTests::searchMatch_valueSemantics() {
    sak::SearchMatch original;
    original.file_path = "/test/file.cpp";
    original.line_number = 42;
    original.line_content = "int x = 42;";
    original.match_start = 8;
    original.match_end = 10;
    original.context_before = {"line before"};
    original.context_after = {"line after"};

    // Copy
    sak::SearchMatch copy = original;
    QCOMPARE(copy.file_path, original.file_path);
    QCOMPARE(copy.line_number, original.line_number);
    QCOMPARE(copy.line_content, original.line_content);
    QCOMPARE(copy.match_start, original.match_start);
    QCOMPARE(copy.match_end, original.match_end);
    QCOMPARE(copy.context_before, original.context_before);
    QCOMPARE(copy.context_after, original.context_after);

    // Move
    sak::SearchMatch moved = std::move(copy);
    QCOMPARE(moved.file_path, original.file_path);
    QCOMPARE(moved.line_number, original.line_number);
}

void AdvancedSearchTypesTests::searchMatch_contextLines() {
    sak::SearchMatch match;
    match.context_before = {"line 1", "line 2", "line 3"};
    match.context_after = {"line 5", "line 6"};

    QCOMPARE(match.context_before.size(), 3);
    QCOMPARE(match.context_after.size(), 2);
    QCOMPARE(match.context_before[0], "line 1");
    QCOMPARE(match.context_after[1], "line 6");
}

// ── SearchConfig ────────────────────────────────────────────────────────────

void AdvancedSearchTypesTests::searchConfig_defaultValues() {
    sak::SearchConfig config;

    QVERIFY(config.root_path.isEmpty());
    QVERIFY(config.pattern.isEmpty());
    QCOMPARE(config.case_sensitive, false);
    QCOMPARE(config.use_regex, false);
    QCOMPARE(config.whole_word, false);
    QCOMPARE(config.search_image_metadata, false);
    QCOMPARE(config.search_file_metadata, false);
    QCOMPARE(config.search_in_archives, false);
    QCOMPARE(config.hex_search, false);
    QVERIFY(config.file_extensions.isEmpty());
    QCOMPARE(config.context_lines, 2);
    QCOMPARE(config.max_results, 0);
    QCOMPARE(config.max_file_size, 50LL * 1024 * 1024);
    QCOMPARE(config.network_timeout_sec, 5);
}

void AdvancedSearchTypesTests::searchConfig_defaultExcludes() {
    sak::SearchConfig config;

    // Default exclusion patterns should be non-empty
    QVERIFY(!config.exclude_patterns.isEmpty());

    // Should contain common exclusion patterns
    bool hasGit = false;
    bool hasNodeModules = false;
    for (const auto& pattern : config.exclude_patterns) {
        if (pattern.contains("git")) {
            hasGit = true;
        }
        if (pattern.contains("node_modules")) {
            hasNodeModules = true;
        }
    }
    QVERIFY2(hasGit, "Default excludes should contain .git");
    QVERIFY2(hasNodeModules, "Default excludes should contain node_modules");
}

void AdvancedSearchTypesTests::searchConfig_valueSemantics() {
    sak::SearchConfig config;
    config.root_path = "/search/root";
    config.pattern = "test_pattern";
    config.case_sensitive = true;
    config.use_regex = true;
    config.context_lines = 5;

    sak::SearchConfig copy = config;
    QCOMPARE(copy.root_path, config.root_path);
    QCOMPARE(copy.pattern, config.pattern);
    QCOMPARE(copy.case_sensitive, true);
    QCOMPARE(copy.use_regex, true);
    QCOMPARE(copy.context_lines, 5);
}

// ── SearchPreferences ───────────────────────────────────────────────────────

void AdvancedSearchTypesTests::searchPreferences_defaultValues() {
    sak::SearchPreferences prefs;

    QCOMPARE(prefs.max_results, 0);
    QCOMPARE(prefs.max_preview_file_size_mb, 10);
    QCOMPARE(prefs.max_search_file_size_mb, 50);
    QCOMPARE(prefs.max_cache_size, 50);
    QCOMPARE(prefs.context_lines, 2);
}

void AdvancedSearchTypesTests::searchPreferences_copyable() {
    sak::SearchPreferences prefs;
    prefs.max_results = 1000;
    prefs.max_preview_file_size_mb = 20;

    sak::SearchPreferences copy = prefs;
    QCOMPARE(copy.max_results, 1000);
    QCOMPARE(copy.max_preview_file_size_mb, 20);
}

// ── RegexPatternInfo ────────────────────────────────────────────────────────

void AdvancedSearchTypesTests::regexPatternInfo_defaultConstruction() {
    sak::RegexPatternInfo info;

    QVERIFY(info.key.isEmpty());
    QVERIFY(info.label.isEmpty());
    QVERIFY(info.pattern.isEmpty());
    QCOMPARE(info.enabled, false);
}

void AdvancedSearchTypesTests::regexPatternInfo_copyable() {
    sak::RegexPatternInfo info;
    info.key = "emails";
    info.label = "Email addresses";
    info.pattern = R"(\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}\b)";
    info.enabled = true;

    sak::RegexPatternInfo copy = info;
    QCOMPARE(copy.key, info.key);
    QCOMPARE(copy.label, info.label);
    QCOMPARE(copy.pattern, info.pattern);
    QCOMPARE(copy.enabled, true);
}

// ── Extension Sets ──────────────────────────────────────────────────────────

void AdvancedSearchTypesTests::imageExtensions_containsExpected() {
    QVERIFY(sak::kImageExtensions.contains("jpg"));
    QVERIFY(sak::kImageExtensions.contains("jpeg"));
    QVERIFY(sak::kImageExtensions.contains("png"));
    QVERIFY(sak::kImageExtensions.contains("tiff"));
    QVERIFY(sak::kImageExtensions.contains("gif"));
    QVERIFY(sak::kImageExtensions.contains("bmp"));
    QVERIFY(sak::kImageExtensions.contains("webp"));
}

void AdvancedSearchTypesTests::imageExtensions_doesNotContainWrong() {
    QVERIFY(!sak::kImageExtensions.contains("cpp"));
    QVERIFY(!sak::kImageExtensions.contains("txt"));
    QVERIFY(!sak::kImageExtensions.contains("pdf"));
    QVERIFY(!sak::kImageExtensions.contains("zip"));
}

void AdvancedSearchTypesTests::fileMetadataExtensions_containsExpected() {
    QVERIFY(sak::kFileMetadataExtensions.contains("pdf"));
    QVERIFY(sak::kFileMetadataExtensions.contains("docx"));
    QVERIFY(sak::kFileMetadataExtensions.contains("xlsx"));
    QVERIFY(sak::kFileMetadataExtensions.contains("pptx"));
    QVERIFY(sak::kFileMetadataExtensions.contains("epub"));
    QVERIFY(sak::kFileMetadataExtensions.contains("mp3"));
    QVERIFY(sak::kFileMetadataExtensions.contains("mp4"));
    QVERIFY(sak::kFileMetadataExtensions.contains("json"));
    QVERIFY(sak::kFileMetadataExtensions.contains("csv"));
}

void AdvancedSearchTypesTests::archiveExtensions_containsExpected() {
    QVERIFY(sak::kArchiveExtensions.contains("zip"));
    QVERIFY(sak::kArchiveExtensions.contains("epub"));
    QVERIFY(!sak::kArchiveExtensions.contains("tar"));
    QVERIFY(!sak::kArchiveExtensions.contains("gz"));
}

// ── Compile-Time Invariants Verification ────────────────────────────────────

void AdvancedSearchTypesTests::staticAsserts_defaultConstructible() {
    QVERIFY(std::is_default_constructible_v<sak::SearchMatch>);
    QVERIFY(std::is_default_constructible_v<sak::SearchConfig>);
    QVERIFY(std::is_default_constructible_v<sak::SearchPreferences>);
    QVERIFY(std::is_default_constructible_v<sak::RegexPatternInfo>);
}

void AdvancedSearchTypesTests::staticAsserts_copyConstructible() {
    QVERIFY(std::is_copy_constructible_v<sak::SearchMatch>);
    QVERIFY(std::is_copy_constructible_v<sak::SearchConfig>);
    QVERIFY(std::is_copy_constructible_v<sak::SearchPreferences>);
    QVERIFY(std::is_copy_constructible_v<sak::RegexPatternInfo>);
}

QTEST_GUILESS_MAIN(AdvancedSearchTypesTests)
#include "test_advanced_search_types.moc"
