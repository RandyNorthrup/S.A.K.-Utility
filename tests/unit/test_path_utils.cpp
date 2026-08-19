// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_path_utils.cpp
/// @brief Unit tests for path manipulation and validation utilities

#include "sak/error_codes.h"
#include "sak/path_utils.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <cstdint>
#include <filesystem>
#include <limits>

class PathUtilsTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void initTestCase();
    void cleanupTestCase();

    // isSafePath
    void isSafePath_validSubpath();
    void isSafePath_exactBase();
    void isSafePath_traversalDotDot();
    void isSafePath_embeddedTraversal();
    void isSafePath_caseInsensitiveOnWindows();
    void isSafePath_outsideBase();
    void isSafePath_absolutePathDifferentDrive();
    void isSafePath_emptyInputsRejected();  // R5-G10-9

    // matchesPattern
    void matchesPattern_starWildcard();
    void matchesPattern_questionWildcard();
    void matchesPattern_exactMatch();
    void matchesPattern_noMatch();
    void matchesPattern_emptyPatternList();
    void matchesPattern_multiplePatterns();

    // getDirectorySizeAndCount
    void dirSizeAndCount_saturatesOnOverflow();  // R5-G10-9
    void dirSizeAndCount_normalDir();
    void dirSizeAndCount_emptyDir();
    void dirSizeAndCount_nonExistentDir();
    void dirSizeAndCount_fileNotDir();

    // getAvailableSpace
    void availableSpace_validPath();
    void availableSpace_invalidPath();

    // makeRelative
    void makeRelative_validSubpath();
    void makeRelative_sameDir();
    void makeRelative_emptyInputsRejected();  // R5-G10-9

private:
    QTemporaryDir m_tempDir;
    std::filesystem::path m_basePath;
};

void PathUtilsTests::initTestCase() {
    QVERIFY(m_tempDir.isValid());
    m_basePath = m_tempDir.path().toStdWString();

    // Create test directory structure
    QDir base(m_tempDir.path());
    base.mkpath("subdir/nested");

    // Create some files
    QFile f1(m_tempDir.filePath("file1.txt"));
    QVERIFY(f1.open(QIODevice::WriteOnly));
    f1.write("Hello");
    f1.close();

    QFile f2(m_tempDir.filePath("subdir/file2.log"));
    QVERIFY(f2.open(QIODevice::WriteOnly));
    f2.write("World!!");
    f2.close();

    QFile f3(m_tempDir.filePath("subdir/nested/deep.dat"));
    QVERIFY(f3.open(QIODevice::WriteOnly));
    f3.write("DeepData123");
    f3.close();
}

void PathUtilsTests::cleanupTestCase() {}

// ============================================================================
// isSafePath Tests
// ============================================================================

void PathUtilsTests::isSafePath_validSubpath() {
    auto subPath = m_basePath / "subdir" / "file2.log";
    auto result = sak::path_utils::isSafePath(subPath, m_basePath);
    QVERIFY(result.has_value());
    QVERIFY(result.value());
}

void PathUtilsTests::isSafePath_exactBase() {
    auto result = sak::path_utils::isSafePath(m_basePath, m_basePath);
    QVERIFY(result.has_value());
    QVERIFY(result.value());
}

void PathUtilsTests::isSafePath_traversalDotDot() {
    auto maliciousPath = m_basePath / "subdir" / ".." / ".." / "etc" / "passwd";
    auto result = sak::path_utils::isSafePath(maliciousPath, m_basePath);
    // <base>/subdir exists, so weakly_canonical always anchors this path and
    // isSafePath must give a real answer -- accepting an error here would let a
    // regression that stopped answering pass silently.
    QVERIFY(result.has_value());
    QVERIFY2(!result.value(), "Path traversal via .. should be rejected");
}

void PathUtilsTests::isSafePath_embeddedTraversal() {
    auto path = m_basePath / "subdir" / ".." / ".." / "Windows" / "System32";
    auto result = sak::path_utils::isSafePath(path, m_basePath);
    QVERIFY(result.has_value());
    QVERIFY(!result.value());
}

void PathUtilsTests::isSafePath_caseInsensitiveOnWindows() {
#ifdef _WIN32
    // On Windows, paths should be case-insensitive
    auto upperBase = std::filesystem::path(m_tempDir.path().toUpper().toStdWString());
    auto lowerSub = m_basePath / "subdir" / "file2.log";
    auto result = sak::path_utils::isSafePath(lowerSub, upperBase);
    // Should still consider it safe on Windows (case-insensitive comparison)
    QVERIFY(result.has_value());
    QVERIFY(result.value());
#else
    QSKIP("Case-insensitive path test is Windows-only");
#endif
}

void PathUtilsTests::isSafePath_outsideBase() {
    auto outsidePath = std::filesystem::path("C:\\Windows\\System32\\cmd.exe");
    auto result = sak::path_utils::isSafePath(outsidePath, m_basePath);
    // Both operands exist, so canonicalization cannot fail: demand the answer.
    QVERIFY(result.has_value());
    QVERIFY(!result.value());
}

void PathUtilsTests::isSafePath_absolutePathDifferentDrive() {
#ifdef _WIN32
    // A path on D: should not be safe under a C: base
    auto baseDriveC = std::filesystem::path("C:\\TestBase");
    auto pathDriveD = std::filesystem::path("D:\\SomeFile.txt");
    auto result = sak::path_utils::isSafePath(pathDriveD, baseDriveC);
    // Unlike the cases above, whether D: is reachable is a property of the
    // running machine (an empty optical drive answers ERROR_NOT_READY, which
    // weakly_canonical turns into a hard error). Both outcomes are legitimate,
    // so assert on both: a real answer must be "not contained", and a refusal
    // must be the fail-closed invalid_path -- never a silent pass.
    if (result.has_value()) {
        QVERIFY2(!result.value(), "A D: path must not be contained by a C: base");
    } else {
        QCOMPARE(result.error(), sak::error_code::invalid_path);
    }
#else
    QSKIP("Drive letter test is Windows-only");
#endif
}

// R5-G10-9: containment has no meaning without both operands, and this answer gates
// path-traversal decisions, so an empty path or base must fail closed with invalid_path rather
// than return a guessed bool that a caller would read as "safe". This locks the observable
// fail-closed CONTRACT, which is enforced in depth -- by isSafePath's explicit empty-operand guard
// AND, redundantly, by normalize() rejecting the non-absolute weakly_canonical("") -- so it is a
// contract regression test (it catches a refactor that made empty inputs pass), not an isolation
// of one specific guard. (makeRelative_emptyInputsRejected below IS guard-specific: relative("","")
// yields "." which its guard alone rejects.)
void PathUtilsTests::isSafePath_emptyInputsRejected() {
    const auto emptyPath = sak::path_utils::isSafePath(std::filesystem::path(), m_basePath);
    QVERIFY(!emptyPath.has_value());
    QCOMPARE(emptyPath.error(), sak::error_code::invalid_path);

    const auto emptyBase = sak::path_utils::isSafePath(m_basePath / "subdir",
                                                       std::filesystem::path());
    QVERIFY(!emptyBase.has_value());
    QCOMPARE(emptyBase.error(), sak::error_code::invalid_path);

    // Non-vacuity: a real subpath under a real base still yields an answer, so the empty-input
    // reject is the contract, not a function that errors on everything.
    QVERIFY(
        sak::path_utils::isSafePath(m_basePath / "subdir" / "file2.log", m_basePath).has_value());
}

// ============================================================================
// matchesPattern Tests
// ============================================================================

void PathUtilsTests::matchesPattern_starWildcard() {
    std::vector<std::string> patterns = {"*.txt"};
    QVERIFY(sak::path_utils::matchesPattern("test.txt", patterns));
    QVERIFY(!sak::path_utils::matchesPattern("test.log", patterns));
}

void PathUtilsTests::matchesPattern_questionWildcard() {
    std::vector<std::string> patterns = {"file?.txt"};
    QVERIFY(sak::path_utils::matchesPattern("file1.txt", patterns));
    QVERIFY(sak::path_utils::matchesPattern("fileA.txt", patterns));
    QVERIFY(!sak::path_utils::matchesPattern("file12.txt", patterns));
}

void PathUtilsTests::matchesPattern_exactMatch() {
    std::vector<std::string> patterns = {"readme.md"};
    QVERIFY(sak::path_utils::matchesPattern("readme.md", patterns));
    QVERIFY(!sak::path_utils::matchesPattern("README.MD", patterns));
}

void PathUtilsTests::matchesPattern_noMatch() {
    std::vector<std::string> patterns = {"*.cpp", "*.h"};
    QVERIFY(!sak::path_utils::matchesPattern("data.json", patterns));
}

void PathUtilsTests::matchesPattern_emptyPatternList() {
    std::vector<std::string> patterns;
    QVERIFY(!sak::path_utils::matchesPattern("anything.txt", patterns));
}

void PathUtilsTests::matchesPattern_multiplePatterns() {
    std::vector<std::string> patterns = {"*.cpp", "*.h", "*.hpp"};
    QVERIFY(sak::path_utils::matchesPattern("main.cpp", patterns));
    QVERIFY(sak::path_utils::matchesPattern("header.h", patterns));
    QVERIFY(sak::path_utils::matchesPattern("template.hpp", patterns));
    QVERIFY(!sak::path_utils::matchesPattern("data.json", patterns));
}

// ============================================================================
// getDirectorySizeAndCount Tests
// ============================================================================

void PathUtilsTests::dirSizeAndCount_saturatesOnOverflow() {
    // The recursive size walk accumulates entry.file_size(), which is the LOGICAL size:
    // a few attacker-created sparse files can each claim exabytes. If the running total
    // WRAPPED past UINTMAX it would report a small number, under-reporting the space a set
    // needs and letting an oversized set be judged to fit (fail open). The accumulate must
    // saturate at UINTMAX instead.
    constexpr auto kMax = std::numeric_limits<std::uintmax_t>::max();

    // Non-vacuity: a below-cap sum is exact (the guard is the overflow clamp, not a
    // constant, and not an always-saturate).
    QCOMPARE(sak::path_utils::saturatingAddSizeForTesting(5, 7), std::uintmax_t{12});
    QCOMPARE(sak::path_utils::saturatingAddSizeForTesting(1000, 2000), std::uintmax_t{3000});

    // Exact boundary: total + size == UINTMAX does not overflow and is returned as-is.
    QCOMPARE(sak::path_utils::saturatingAddSizeForTesting(kMax - 10, 10), kMax);

    // Overflow by one: an unguarded add would WRAP to 0; the guard clamps to UINTMAX. The
    // "== kMax" (never 0) is what proves it saturated rather than wrapped.
    QCOMPARE(sak::path_utils::saturatingAddSizeForTesting(kMax - 10, 11), kMax);

    // Extreme overflow: kMax + kMax would wrap to kMax-1; it must clamp to kMax.
    QCOMPARE(sak::path_utils::saturatingAddSizeForTesting(kMax, kMax), kMax);
}

void PathUtilsTests::dirSizeAndCount_normalDir() {
    auto result = sak::path_utils::getDirectorySizeAndCount(m_basePath);
    QVERIFY(result.has_value());
    // We created 3 files: "Hello"(5), "World!!"(7), "DeepData123"(11) = 23 bytes
    QCOMPARE(result.value().file_count, std::uintmax_t{3});
    QCOMPARE(result.value().total_bytes, std::uintmax_t{23});
}

void PathUtilsTests::dirSizeAndCount_emptyDir() {
    QDir(m_tempDir.path()).mkpath("empty_subdir");
    auto emptyDir = m_basePath / "empty_subdir";
    auto result = sak::path_utils::getDirectorySizeAndCount(emptyDir);
    QVERIFY(result.has_value());
    QCOMPARE(result.value().file_count, std::uintmax_t{0});
    QCOMPARE(result.value().total_bytes, std::uintmax_t{0});
}

void PathUtilsTests::dirSizeAndCount_nonExistentDir() {
    auto result = sak::path_utils::getDirectorySizeAndCount(m_basePath / "nonexistent_999");
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::file_not_found);
}

void PathUtilsTests::dirSizeAndCount_fileNotDir() {
    auto result = sak::path_utils::getDirectorySizeAndCount(m_basePath / "file1.txt");
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::not_a_directory);
}

// ============================================================================
// getAvailableSpace Tests
// ============================================================================

void PathUtilsTests::availableSpace_validPath() {
    auto result = sak::path_utils::getAvailableSpace(m_basePath);
    QVERIFY(result.has_value());
    QVERIFY(result.value() > 0);  // Should have some free space
}

void PathUtilsTests::availableSpace_invalidPath() {
    auto result =
        sak::path_utils::getAvailableSpace(std::filesystem::path("Z:\\NonExistent\\Path"));
    // GetDiskFreeSpaceEx cannot answer for a path that does not exist, and every
    // filesystem_error out of space() maps to read_error. Fail closed: no
    // fabricated byte count, and no quietly-successful query either.
    QVERIFY(!result.has_value());
    QCOMPARE(result.error(), sak::error_code::read_error);
}

// ============================================================================
// makeRelative Tests
// ============================================================================

void PathUtilsTests::makeRelative_validSubpath() {
    auto subPath = m_basePath / "subdir" / "file2.log";
    auto result = sak::path_utils::makeRelative(subPath, m_basePath);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), std::filesystem::path("subdir") / "file2.log");
}

void PathUtilsTests::makeRelative_sameDir() {
    auto result = sak::path_utils::makeRelative(m_basePath / "file1.txt", m_basePath);
    QVERIFY(result.has_value());
    QCOMPARE(result.value(), std::filesystem::path("file1.txt"));
}

// R5-G10-9: std::filesystem::relative("", "") yields "." (the caller's working directory), so
// makeRelative must reject an empty path or base rather than hand back that CWD-resolving ".".
void PathUtilsTests::makeRelative_emptyInputsRejected() {
    QVERIFY(!sak::path_utils::makeRelative(std::filesystem::path(), m_basePath).has_value());
    QVERIFY(!sak::path_utils::makeRelative(m_basePath / "file1.txt", std::filesystem::path())
                 .has_value());
    const auto bothEmpty = sak::path_utils::makeRelative(std::filesystem::path(),
                                                         std::filesystem::path());
    QVERIFY(!bothEmpty.has_value());  // must not be the "." that relative("","") would yield
    QCOMPARE(bothEmpty.error(), sak::error_code::invalid_path);

    // Non-vacuity: a real subpath under a real base still makes a relative path.
    QVERIFY(
        sak::path_utils::makeRelative(m_basePath / "subdir" / "file2.log", m_basePath).has_value());
}

QTEST_GUILESS_MAIN(PathUtilsTests)
#include "test_path_utils.moc"
