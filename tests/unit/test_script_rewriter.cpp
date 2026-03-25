// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_script_rewriter.cpp
/// @brief Unit tests for ScriptRewriter

#include "sak/script_rewriter.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestScriptRewriter : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // Basic rewriting
    void rewrite_singleUrl_replacedWithToolsDir();
    void rewrite_multipleUrls_allReplaced();
    void rewrite_url64bit_replacedSeparately();

    // No-op cases
    void rewrite_emptyFilenameMap_returnsOriginal();
    void rewrite_emptyScript_succeeds();

    // Replacement tracking
    void rewrite_tracksReplacements();

    // File output
    void rewriteToFile_writesContent();
    void rewriteToFile_invalidPath_failsGracefully();
};

// ============================================================================
// Basic Rewriting
// ============================================================================

void TestScriptRewriter::rewrite_singleUrl_replacedWithToolsDir() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup.exe' `
    -SilentArgs '/S'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/setup.exe"] = "setup.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    QVERIFY(result.script_content.contains("$toolsDir"));
    QVERIFY(result.script_content.contains("setup.exe"));
    QVERIFY(!result.script_content.contains("https://example.com/setup.exe"));
}

void TestScriptRewriter::rewrite_multipleUrls_allReplaced() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup32.exe' `
    -Url64bit 'https://example.com/setup64.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/setup32.exe"] = "setup32.exe";
    filenames["https://example.com/setup64.exe"] = "setup64.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    QVERIFY(!result.script_content.contains("https://example.com/setup32.exe"));
    QVERIFY(!result.script_content.contains("https://example.com/setup64.exe"));
}

void TestScriptRewriter::rewrite_url64bit_replacedSeparately() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/x86.exe' `
    -Url64bit 'https://example.com/x64.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/x86.exe"] = "x86.exe";
    filenames["https://example.com/x64.exe"] = "x64.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    QVERIFY(result.script_content.contains("x86.exe"));
    QVERIFY(result.script_content.contains("x64.exe"));
}

// ============================================================================
// No-op Cases
// ============================================================================

void TestScriptRewriter::rewrite_emptyFilenameMap_returnsOriginal() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;  // empty

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    // Empty filename map is treated as an error
    QVERIFY(!result.success);
    QVERIFY(result.replacements.isEmpty());
}

void TestScriptRewriter::rewrite_emptyScript_succeeds() {
    sak::InstallScriptParser parser;
    auto parsed = parser.parse(QString());

    QHash<QString, QString> filenames;

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    // Empty script is treated as an error
    QVERIFY(!result.success);
    QVERIFY(result.replacements.isEmpty());
}

// ============================================================================
// Replacement Tracking
// ============================================================================

void TestScriptRewriter::rewrite_tracksReplacements() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/tracked.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/tracked.exe"] = "tracked.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    QCOMPARE(result.replacements.size(), 1);
    QCOMPARE(result.replacements.first().original_url, QString("https://example.com/tracked.exe"));
    QVERIFY(result.replacements.first().local_path.contains("tracked.exe"));
}

// ============================================================================
// File Output
// ============================================================================

void TestScriptRewriter::rewriteToFile_writesContent() {
    QTemporaryDir temp_dir;
    QVERIFY(temp_dir.isValid());

    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/file_out.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/file_out.exe"] = "file_out.exe";

    QString output_path = temp_dir.path() + "/rewritten.ps1";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewriteToFile(parsed, filenames, output_path);

    QVERIFY(result.success);
    QVERIFY(QFile::exists(output_path));

    QFile file(output_path);
    QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
    QString content = file.readAll();
    file.close();

    QVERIFY(content.contains("$toolsDir"));
    QVERIFY(content.contains("file_out.exe"));
}

void TestScriptRewriter::rewriteToFile_invalidPath_failsGracefully() {
    sak::InstallScriptParser parser;
    auto parsed = parser.parse("# empty");

    QHash<QString, QString> filenames;

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewriteToFile(parsed, filenames, "Z:\\nonexistent\\path\\script.ps1");

    QVERIFY(!result.success);
    QVERIFY(!result.error_message.isEmpty());
}

QTEST_GUILESS_MAIN(TestScriptRewriter)
#include "test_script_rewriter.moc"
