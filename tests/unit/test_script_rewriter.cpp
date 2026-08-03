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

    // Fail-closed: an incomplete rewrite must NOT report success
    void rewrite_resourcesButNoUrlReplaced_fails();

    // Replacement tracking
    void rewrite_tracksReplacements();

    // Injection safety
    void rewrite_filenameWithQuote_isEscaped();

    // Quote-swallowing (B10-16)
    void urlReplacementSpan_swallowsMatchedQuotes();
    void urlReplacementSpan_keepsBareOrMismatched();
    void rewrite_quotedUrl_producesBareExpressionNotLiteral();

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

void TestScriptRewriter::rewrite_resourcesButNoUrlReplaced_fails() {
    // The script declares a download URL, but the filename map keys a DIFFERENT
    // URL, so nothing is internalized. Formerly this returned success with zero
    // replacements -- a script that still hits the network at install time. It
    // must now FAIL closed so the caller never ships an un-internalized script.
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/real.exe'
)";
    auto parsed = parser.parse(script);
    QVERIFY(!parsed.resources.isEmpty());  // a resource WAS declared

    QHash<QString, QString> filenames;     // non-empty, but keyed to a URL not present
    filenames["https://example.com/other.exe"] = "other.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(!result.success);
    QVERIFY(!result.error_message.isEmpty());
    QVERIFY(result.replacements.isEmpty());
    // The live download URL must remain in the (rejected) content, never silently
    // reported as internalized.
    QVERIFY(!result.script_content.contains(QStringLiteral("$toolsDir")));
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
// Injection Safety
// ============================================================================

void TestScriptRewriter::rewrite_filenameWithQuote_isEscaped() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/evil.zip'
)";
    auto parsed = parser.parse(script);

    // A %27-decoded URL filename carrying a single quote + injection payload.
    QHash<QString, QString> filenames;
    filenames["https://example.com/evil.zip"] = "a';Start-Process calc;'.zip";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    QCOMPARE(result.replacements.size(), 1);
    // Every single quote is doubled so the payload stays an inert literal.
    QCOMPARE(result.replacements.first().local_path,
             QString("(Join-Path $toolsDir 'a'';Start-Process calc;''.zip')"));
    QVERIFY(!result.script_content.contains("calc;'.zip')"));
}

// ============================================================================
// Quote-swallowing (B10-16)
// ============================================================================

void TestScriptRewriter::urlReplacementSpan_swallowsMatchedQuotes() {
    const QString url = QStringLiteral("http://x/f.exe");
    const int len = static_cast<int>(url.length());

    const QString sq = QStringLiteral("-Url 'http://x/f.exe' `");
    const int p1 = static_cast<int>(sq.indexOf(url));
    auto s1 = sak::ScriptRewriter::urlReplacementSpan(sq, p1, len);
    QCOMPARE(s1.start, p1 - 1);    // opening single quote consumed
    QCOMPARE(s1.length, len + 2);  // ...and the closing one

    const QString dq = QStringLiteral("-Url \"http://x/f.exe\"\n");
    const int p2 = static_cast<int>(dq.indexOf(url));
    auto s2 = sak::ScriptRewriter::urlReplacementSpan(dq, p2, len);
    QCOMPARE(s2.start, p2 - 1);
    QCOMPARE(s2.length, len + 2);
}

void TestScriptRewriter::urlReplacementSpan_keepsBareOrMismatched() {
    const QString url = QStringLiteral("http://x/f.exe");
    const int len = static_cast<int>(url.length());

    // Bare (unquoted) URL: span is exactly the URL.
    const QString bare = QStringLiteral("-OutFile http://x/f.exe more");
    const int p1 = static_cast<int>(bare.indexOf(url));
    auto s1 = sak::ScriptRewriter::urlReplacementSpan(bare, p1, len);
    QCOMPARE(s1.start, p1);
    QCOMPARE(s1.length, len);

    // Mismatched quotes are NOT a wrapping pair.
    const QString mis = QStringLiteral("x'http://x/f.exe\"y");
    const int p2 = static_cast<int>(mis.indexOf(url));
    auto s2 = sak::ScriptRewriter::urlReplacementSpan(mis, p2, len);
    QCOMPARE(s2.start, p2);
    QCOMPARE(s2.length, len);
}

void TestScriptRewriter::rewrite_quotedUrl_producesBareExpressionNotLiteral() {
    sak::InstallScriptParser parser;
    QString script = R"(
Install-ChocolateyPackage -PackageName 'testpkg' `
    -FileType 'exe' `
    -Url 'https://example.com/setup.exe'
)";
    auto parsed = parser.parse(script);

    QHash<QString, QString> filenames;
    filenames["https://example.com/setup.exe"] = "setup.exe";

    sak::ScriptRewriter rewriter;
    auto result = rewriter.rewrite(parsed, filenames);

    QVERIFY(result.success);
    // The wrapping quotes are gone: the Join-Path expression stands on its own,
    // NOT embedded in a '(...)' string literal (which would be inert).
    QVERIFY(result.script_content.contains("-Url (Join-Path $toolsDir 'setup.exe')"));
    QVERIFY(!result.script_content.contains("'(Join-Path"));
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
