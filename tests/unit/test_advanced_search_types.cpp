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
    // -- SearchMatch --
    void searchMatch_defaultConstruction();
    void searchMatch_valueSemantics();
    void searchMatch_contextLines();

    // -- SearchConfig --
    void searchConfig_defaultValues();
    void searchConfig_defaultExcludes();
    void searchConfig_valueSemantics();

    // -- SearchPreferences --
    void searchPreferences_defaultValues();
    void searchPreferences_copyable();

    // -- RegexPatternInfo --
    void regexPatternInfo_defaultConstruction();
    void regexPatternInfo_copyable();

    // -- Extension Sets --
    void imageExtensions_containsExpected();
    void imageExtensions_doesNotContainWrong();
    void fileMetadataExtensions_containsExpected();
    void archiveExtensions_containsExpected();

    // -- Compile-Time Invariants --
    void staticAsserts_defaultConstructible();
    void staticAsserts_copyConstructible();
};

// -- SearchMatch -------------------------------------------------------------

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

    // Move: parity with the copy half above -- every member must survive the move, not just the
    // two scalars. Only `copy` is moved from, so `original` stays a valid oracle.
    sak::SearchMatch moved = std::move(copy);
    QCOMPARE(moved.file_path, original.file_path);
    QCOMPARE(moved.line_number, original.line_number);
    QCOMPARE(moved.line_content, original.line_content);
    QCOMPARE(moved.match_start, original.match_start);
    QCOMPARE(moved.match_end, original.match_end);
    QCOMPARE(moved.context_before, original.context_before);
    QCOMPARE(moved.context_after, original.context_after);
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

// -- SearchConfig ------------------------------------------------------------

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

    // Routing/safety defaults. A fresh config must (a) walk the LOCAL tree: use_file_system_target
    // false keeps execute() off the raw/image bridge branch (advanced_search_worker.cpp:939-945),
    // whose can_advanced_search guard would NOT catch a blank target because that field defaults
    // true (file_management_file_system.h:75); and (b) NOT drop symlinked files: skip_symlinks
    // false is the GUI behavior, headless callers opt IN (app_readonly_actions.cpp:1343), so a
    // flipped default would silently OR QDir::NoSymLinks into the user's search filter
    // (advanced_search_worker.cpp:674-677). The nested target must start blank.
    QCOMPARE(config.use_file_system_target, false);
    QCOMPARE(config.skip_symlinks, false);
    QVERIFY(config.file_system_target.id.isEmpty());
    QVERIFY(config.file_system_target.root_path.isEmpty());
}

void AdvancedSearchTypesTests::searchConfig_defaultExcludes() {
    sak::SearchConfig config;

    // The default exclusion patterns are a fixed compile-time list; pin the exact ordered set
    // (subsumes the old non-empty + substring-loop checks, and catches a dropped/reordered or
    // mutated pattern -- e.g. \.git -> .git).
    QCOMPARE(config.exclude_patterns,
             (QStringList{R"(\.git)",
                          R"(\.svn)",
                          R"(__pycache__)",
                          R"(node_modules)",
                          R"(\.pyc$)",
                          R"(\.exe$)",
                          R"(\.dll$)",
                          R"(\.so$)",
                          R"(\.bin$)"}));
}

namespace {

/// A SearchConfig with every field set to a NON-default value, so a copy that silently re-runs
/// a default member initializer cannot pass by coincidence.
sak::SearchConfig fullyPopulatedSearchConfig() {
    sak::SearchConfig config;
    config.root_path = "/search/root";
    config.pattern = "test_pattern";
    config.case_sensitive = true;
    config.use_regex = true;
    config.whole_word = true;
    config.search_image_metadata = true;
    config.search_file_metadata = true;
    config.search_in_archives = true;
    config.hex_search = true;
    config.skip_symlinks = true;
    config.file_extensions = QStringList{"cpp", "h"};
    // Deliberately NOT the compile-time default list (pinned by searchConfig_defaultExcludes),
    // so a copy that silently re-runs the default member initializer is caught here.
    config.exclude_patterns = QStringList{R"(^build/)", R"(\.tmp$)"};
    config.context_lines = 5;
    config.max_results = 250;
    config.max_file_size = 7LL * 1024 * 1024;
    config.network_timeout_sec = 11;

    // The nested target is the member the worker actually searches through
    // (advanced_search_worker.cpp:939-945), and AdvancedSearchWorker takes SearchConfig
    // BY VALUE (advanced_search_worker.h:42) -- so the copy must carry it. A copy that
    // drops it leaves use_file_system_target true with an empty target root_path, and the
    // :940 guard does not fire because can_advanced_search defaults true
    // (file_management_file_system.h:75).
    config.use_file_system_target = true;
    config.file_system_target.id = "target-id";
    config.file_system_target.label = "Raw image";
    config.file_system_target.root_path = "/mnt/image";
    config.file_system_target.file_system = "APFS";
    config.file_system_target.source = "image-file";
    config.file_system_target.details = QStringList{"detail"};
    config.file_system_target.size_bytes = 4096;
    config.file_system_target.kind = sak::FileManagementTargetKind::ImageFile;
    config.file_system_target.local_file_system = false;
    config.file_system_target.read_only = true;
    config.file_system_target.can_browse = false;
    config.file_system_target.can_read_files = false;
    config.file_system_target.can_write_files = true;
    config.file_system_target.can_organize = false;
    config.file_system_target.can_duplicate_scan = false;
    config.file_system_target.can_advanced_search = false;
    config.file_system_target.blockers = QStringList{"blocked"};
    return config;
}

/// FileManagementTarget has no operator==, so pin it field by field. `kind` is compared as int
/// because QTest has no toString for the enum.
void compareFileSystemTargets(const sak::FileManagementTarget& actual,
                              const sak::FileManagementTarget& expected) {
    QCOMPARE(actual.id, expected.id);
    QCOMPARE(actual.label, expected.label);
    QCOMPARE(actual.root_path, expected.root_path);
    QCOMPARE(actual.file_system, expected.file_system);
    QCOMPARE(actual.source, expected.source);
    QCOMPARE(actual.details, expected.details);
    QCOMPARE(actual.size_bytes, expected.size_bytes);
    QCOMPARE(static_cast<int>(actual.kind), static_cast<int>(expected.kind));
    QCOMPARE(actual.local_file_system, expected.local_file_system);
    QCOMPARE(actual.read_only, expected.read_only);
    QCOMPARE(actual.can_browse, expected.can_browse);
    QCOMPARE(actual.can_read_files, expected.can_read_files);
    QCOMPARE(actual.can_write_files, expected.can_write_files);
    QCOMPARE(actual.can_organize, expected.can_organize);
    QCOMPARE(actual.can_duplicate_scan, expected.can_duplicate_scan);
    QCOMPARE(actual.can_advanced_search, expected.can_advanced_search);
    QCOMPARE(actual.blockers, expected.blockers);
}

void compareSearchConfigOwnFields(const sak::SearchConfig& actual,
                                  const sak::SearchConfig& expected) {
    QCOMPARE(actual.root_path, expected.root_path);
    QCOMPARE(actual.pattern, expected.pattern);
    QCOMPARE(actual.case_sensitive, expected.case_sensitive);
    QCOMPARE(actual.use_regex, expected.use_regex);
    QCOMPARE(actual.whole_word, expected.whole_word);
    QCOMPARE(actual.search_image_metadata, expected.search_image_metadata);
    QCOMPARE(actual.search_file_metadata, expected.search_file_metadata);
    QCOMPARE(actual.search_in_archives, expected.search_in_archives);
    QCOMPARE(actual.hex_search, expected.hex_search);
    QCOMPARE(actual.skip_symlinks, expected.skip_symlinks);
    QCOMPARE(actual.file_extensions, expected.file_extensions);
    QCOMPARE(actual.exclude_patterns, expected.exclude_patterns);
    QCOMPARE(actual.context_lines, expected.context_lines);
    QCOMPARE(actual.max_results, expected.max_results);
    QCOMPARE(actual.max_file_size, expected.max_file_size);
    QCOMPARE(actual.network_timeout_sec, expected.network_timeout_sec);
    QCOMPARE(actual.use_file_system_target, expected.use_file_system_target);
}

}  // namespace

void AdvancedSearchTypesTests::searchConfig_valueSemantics() {
    const sak::SearchConfig config = fullyPopulatedSearchConfig();

    const sak::SearchConfig copy = config;
    compareSearchConfigOwnFields(copy, config);
    // A QCOMPARE failure inside a helper returns from the HELPER, not from this slot, so bail
    // out explicitly rather than reporting cascading failures from an already-broken copy.
    if (QTest::currentTestFailed()) {
        return;
    }
    compareFileSystemTargets(copy.file_system_target, config.file_system_target);
    if (QTest::currentTestFailed()) {
        return;
    }

    // Copy ASSIGNMENT is a separate special member and crosses the same boundary.
    sak::SearchConfig assigned;
    assigned = config;
    compareSearchConfigOwnFields(assigned, config);
    if (QTest::currentTestFailed()) {
        return;
    }
    compareFileSystemTargets(assigned.file_system_target, config.file_system_target);
}

// -- SearchPreferences -------------------------------------------------------

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

// -- RegexPatternInfo --------------------------------------------------------

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

// -- Extension Sets ----------------------------------------------------------

void AdvancedSearchTypesTests::imageExtensions_containsExpected() {
    // Exact catalog pin. This set is the EXIF/GPS routing gate
    // (advanced_search_worker.cpp:999) and is mirrored by an unlinked duplicate
    // list in shouldSearchText (advanced_search_worker.cpp:987-989), so a member
    // dropped here silently makes those files unsearchable by BOTH paths rather
    // than raising an error. Membership checks alone cannot catch that; compare
    // the whole set so any removal or addition fails.
    QStringList actual = sak::kImageExtensions.values();
    actual.sort();
    const QStringList expected{QStringLiteral("bmp"),
                               QStringLiteral("gif"),
                               QStringLiteral("heic"),
                               QStringLiteral("heif"),
                               QStringLiteral("jpeg"),
                               QStringLiteral("jpg"),
                               QStringLiteral("png"),
                               QStringLiteral("tif"),
                               QStringLiteral("tiff"),
                               QStringLiteral("webp")};
    QCOMPARE(actual, expected);
}

void AdvancedSearchTypesTests::imageExtensions_doesNotContainWrong() {
    QVERIFY(!sak::kImageExtensions.contains("cpp"));
    QVERIFY(!sak::kImageExtensions.contains("txt"));
    QVERIFY(!sak::kImageExtensions.contains("pdf"));
    QVERIFY(!sak::kImageExtensions.contains("zip"));
}

void AdvancedSearchTypesTests::fileMetadataExtensions_containsExpected() {
    // kFileMetadataExtensions is a fixed compile-time catalog and the SOLE routing switch at
    // advanced_search_worker.cpp:1004: an extension in the set goes to searchFileMetadata, one
    // outside it falls through to searchTextContent (:1019-1021) and is byte-searched as an
    // opaque binary -- exactly what the header comment says the set exists to prevent. Pin the
    // exact contents so BOTH drift directions go red: a dropped member (e.g. the Database
    // group) and a smuggled-in text/source extension.
    QStringList actual = sak::kFileMetadataExtensions.values();
    actual.sort();

    QStringList expected{"avi",      "celtx", "csv", "db",   "docx", "epub", "fdx",    "flac",
                         "fountain", "json",  "m4a", "mkv",  "mov",  "mp3",  "mp4",    "odp",
                         "ods",      "odt",   "ogg", "pdf",  "pptx", "rtf",  "sqlite", "sqlite3",
                         "wav",      "wma",   "wmv", "xlsx", "xml"};
    expected.sort();

    QCOMPARE(actual, expected);
    QCOMPARE(sak::kFileMetadataExtensions.size(), 29);

    // Negative direction (mirrors imageExtensions_doesNotContainWrong): plain text/source
    // files must NOT be diverted into metadata-only extraction.
    QVERIFY(!sak::kFileMetadataExtensions.contains("txt"));
    QVERIFY(!sak::kFileMetadataExtensions.contains("cpp"));
}

void AdvancedSearchTypesTests::archiveExtensions_containsExpected() {
    QVERIFY(sak::kArchiveExtensions.contains("zip"));
    QVERIFY(sak::kArchiveExtensions.contains("epub"));
    QVERIFY(!sak::kArchiveExtensions.contains("tar"));
    QVERIFY(!sak::kArchiveExtensions.contains("gz"));
    // Pin the EXACT contents, not just two members: searchFile()
    // (advanced_search_worker.cpp:1009-1012) hands every member of this set to
    // searchArchive(), which is a ZIP-only reader (:2508-2581), and sets
    // handled_as_special so shouldSearchText() (:983-986) skips the text
    // fallback. A silently added extension would therefore be parsed by a
    // reader that cannot read it AND lose its byte-level search.
    QCOMPARE(sak::kArchiveExtensions, (QSet<QString>{"zip", "epub"}));
}

// -- Compile-Time Invariants Verification ------------------------------------

void AdvancedSearchTypesTests::staticAsserts_defaultConstructible() {
    QVERIFY(std::is_default_constructible_v<sak::SearchMatch>);
    QVERIFY(std::is_default_constructible_v<sak::SearchConfig>);
    QVERIFY(std::is_default_constructible_v<sak::SearchPreferences>);
    QVERIFY(std::is_default_constructible_v<sak::RegexPatternInfo>);
}

void AdvancedSearchTypesTests::staticAsserts_copyConstructible() {
    // Copy-CONSTRUCTIBILITY of all four types is already gated at compile time by the
    // static_asserts at advanced_search_types.h:194-201 (and default-constructibility at
    // :178-191), so a runtime QVERIFY of those traits can never observe false -- the binary
    // only exists because they held. Pin the traits nothing gates: ASSIGNABILITY, which
    // production actually leans on -- AdvancedSearchController::setPreferences does
    // `m_preferences = prefs;` (advanced_search_controller.cpp:311), and QVector<SearchMatch>
    // needs element assignment for erase/insert/reallocation. Adding a const or reference
    // member to any of these structs silently deletes operator= and flips these to false.
    QVERIFY(std::is_copy_assignable_v<sak::SearchMatch>);
    QVERIFY(std::is_move_assignable_v<sak::SearchMatch>);
    QVERIFY(std::is_copy_assignable_v<sak::SearchConfig>);
    QVERIFY(std::is_move_assignable_v<sak::SearchConfig>);
    QVERIFY(std::is_copy_assignable_v<sak::SearchPreferences>);
    QVERIFY(std::is_copy_assignable_v<sak::RegexPatternInfo>);

    // Behavioural half: the exact assignment advanced_search_controller.cpp:311 performs,
    // pinned field-by-field so a hand-written lossy operator= (which keeps the traits true)
    // is caught too.
    sak::SearchPreferences source;
    source.max_results = 1000;
    source.max_preview_file_size_mb = 20;
    source.max_search_file_size_mb = 100;
    source.max_cache_size = 75;
    source.context_lines = 7;

    sak::SearchPreferences target;
    target = source;

    QCOMPARE(target.max_results, 1000);
    QCOMPARE(target.max_preview_file_size_mb, 20);
    QCOMPARE(target.max_search_file_size_mb, 100);
    QCOMPARE(target.max_cache_size, 75);
    QCOMPARE(target.context_lines, 7);
}

QTEST_GUILESS_MAIN(AdvancedSearchTypesTests)
#include "test_advanced_search_types.moc"
