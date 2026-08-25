// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_install_script.cpp
/// @brief Mutation-fuzz of the Chocolatey install-script parser (G14 parser sweep).
///
/// InstallScriptParser::parse runs static, regex-heavy analysis over chocolateyInstall.ps1 scripts
/// pulled from third-party Chocolatey packages, to extract download URLs and checksums for the
/// technician. The script text is untrusted, and the parser is a hand-written pile of
/// QRegularExpression matches -- exactly the shape where a hostile string can drive catastrophic
/// backtracking (a hang) or trip a parsing edge case. This harness drives parse() over thousands of
/// mutated scripts and asserts, for EVERY input:
///
///   1. No crash and no hang (a ReDoS hang trips the ctest timeout with the reproducer recorded in
///      fuzz_harness.h).
///   2. Determinism: parsing the same text twice yields the identical result -- the parser carries
///      no state that would make a second pass diverge.
///   3. Well-formed output: every extracted resource reports a non-negative line number, so the
///      position math never underflows on a pathological script.
///
/// The seed corpus carries real shapes -- Install-ChocolateyPackage, Install-ChocolateyZipPackage,
/// Get-ChocolateyWebFile, an @packageArgs splatting block -- plus deliberately pathological strings
/// (long unbalanced quote / paren / variable runs) so mutation reaches both the happy matchers and
/// their backtracking-prone corners.

#include "sak/install_script_parser.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

constexpr int kExpectedCorpusSeeds = 6;
constexpr int kShippedFuzzIterations = 2000;  // tests/fuzz/fuzz_harness.h kDefaultIterations

QByteArray installChocolateyPackageSeed() {
    return QByteArrayLiteral(
        "$ErrorActionPreference = 'Stop'\n"
        "$packageName = 'example'\n"
        "$url = 'https://example.com/app.exe'\n"
        "$checksum = 'ABC123DEF456'\n"
        "Install-ChocolateyPackage -PackageName $packageName -FileType 'exe' "
        "-SilentArgs '/S' -Url $url -Checksum $checksum -ChecksumType 'sha256'\n");
}

QByteArray installZipPackageSeed() {
    return QByteArrayLiteral(
        "Install-ChocolateyZipPackage -PackageName 'z' "
        "-Url 'https://example.com/x86.zip' -Checksum 'AA11' -ChecksumType 'sha256' "
        "-Url64bit 'https://example.com/x64.zip' -Checksum64 'BB22' -ChecksumType64 'sha256'\n");
}

QByteArray getWebFileSeed() {
    return QByteArrayLiteral(
        "$toolsDir = Split-Path $MyInvocation.MyCommand.Definition\n"
        "Get-ChocolateyWebFile -PackageName 'w' -FileFullPath \"$toolsDir\\a.exe\" "
        "-Url 'http://host/a' -Checksum '0011AABB' -ChecksumType 'md5'\n");
}

QByteArray splattingSeed() {
    return QByteArrayLiteral(
        "$packageArgs = @{\n"
        "  packageName   = 'p'\n"
        "  fileType      = 'msi'\n"
        "  url           = 'https://e/f.msi'\n"
        "  checksum      = 'CAFE'\n"
        "  checksumType  = 'sha256'\n"
        "  silentArgs    = '/quiet'\n"
        "}\n"
        "Install-ChocolateyPackage @packageArgs\n");
}

QByteArray pathologicalSeed() {
    // Long unbalanced runs of the characters the matchers are most sensitive to: variable sigils,
    // parentheses, and quotes. If any regex is backtracking-prone, a mutant of this hangs.
    QByteArray s = "Install-ChocolateyPackage -Url ";
    s.append(QByteArray(400, '$'));
    s.append(QByteArray(200, '('));
    s.append(QByteArray(200, '\''));
    s.append("-Checksum ");
    s.append(QByteArray(300, '"'));
    return s;
}

std::vector<QByteArray> installScriptCorpus() {
    return {
        QByteArray(),
        installChocolateyPackageSeed(),
        installZipPackageSeed(),
        getWebFileSeed(),
        splattingSeed(),
        pathologicalSeed(),
    };
}

bool sameResource(const sak::DownloadResource& a, const sak::DownloadResource& b) {
    return a.url == b.url && a.url_64bit == b.url_64bit && a.checksum == b.checksum &&
           a.checksum_type == b.checksum_type && a.checksum_64bit == b.checksum_64bit &&
           a.checksum_type_64bit == b.checksum_type_64bit && a.file_name == b.file_name &&
           a.source_function == b.source_function && a.line_number == b.line_number;
}

bool sameResult(const sak::ParsedInstallScript& a, const sak::ParsedInstallScript& b) {
    // original_script is compared too. It was the one field of the six this helper omitted, and
    // because the helper only ever compares a parse against ANOTHER PARSE of the same bytes, the
    // omission was self-concealing: the field would agree even if the parser dropped it entirely.
    // It is not decorative -- script_rewriter refuses outright when it is empty and seeds the
    // rewritten script from it, so losing it fails every internalization rewrite.
    if (a.package_type != b.package_type || a.silent_args != b.silent_args ||
        a.uses_splatting != b.uses_splatting || a.warnings != b.warnings ||
        a.original_script != b.original_script || a.resources.size() != b.resources.size()) {
        return false;
    }
    for (int i = 0; i < a.resources.size(); ++i) {
        if (!sameResource(a.resources.at(i), b.resources.at(i))) {
            return false;
        }
    }
    return true;
}

// Every APPENDED resource must carry a real 1-based line inside the script, and a URL.
QString checkResources(const sak::ParsedInstallScript& parsed, const QString& script) {
    const int line_count = static_cast<int>(script.count(QLatin1Char('\n'))) + 1;
    for (const auto& resource : parsed.resources) {
        // `line_number < 0` is a guard this harness can NEVER reach: lineNumberAt has exactly two
        // returns, 0 and count('\n') + 1, both non-negative by construction. The knowable contract
        // is the range.
        if (resource.line_number < 1 || resource.line_number > line_count) {
            return QStringLiteral("resource line number %1 is outside the script's 1..%2 lines")
                .arg(resource.line_number)
                .arg(line_count);
        }
        // Every one of the parser's four append sites refuses a resource carrying neither url nor
        // url_64bit -- a multi-guard refuser this harness proved nothing about. A URL-less
        // resource flowing on to the internalization engine was invisible here.
        if (resource.url.isEmpty() && resource.url_64bit.isEmpty()) {
            return QStringLiteral("an appended resource carries neither url nor url_64bit");
        }
    }
    return {};
}

// Parse @p input; return "" if every invariant held, else the violated one.
QString installScriptInvariant(const QByteArray& input) {
    const QString script = QString::fromUtf8(input);
    const sak::InstallScriptParser parser;

    const sak::ParsedInstallScript first = parser.parse(script);
    const sak::ParsedInstallScript second = parser.parse(script);
    if (!sameResult(first, second)) {
        return QStringLiteral("parse is non-deterministic on identical input");
    }
    // The parser ECHOES its input back, so the harness holds an oracle the parser does not also
    // produce: the input itself. Nothing used it.
    if (first.original_script != script) {
        return QStringLiteral("parse did not echo the script it was given");
    }

    // The empty-content arm is the parser's first act and its fail-closed guard: it appends
    // exactly one warning and skips all four matchers. The corpus's first seed is empty and the
    // erase/truncate mutation operators drive inputs to blank constantly, so thousands of
    // iterations pass through this arm -- and warnings was compared only between two parses of
    // the SAME input, never against what the input deserves, so which arm answered was invisible.
    // Pinned in BOTH directions: a blank script must take it, and a non-blank one must not.
    const bool blank = script.trimmed().isEmpty();
    const bool claims_empty = first.warnings.contains(QStringLiteral("Empty script content"));
    if (blank != claims_empty) {
        return blank ? QStringLiteral("a blank script did not report empty content")
                     : QStringLiteral("a non-blank script was reported as empty content");
    }
    if (blank && !first.resources.isEmpty()) {
        return QStringLiteral("a blank script produced resources");
    }

    return checkResources(first, script);
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("install-script fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class InstallScriptFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parserNeverCrashesAndStaysDeterministicOnAnyScript() {
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return installScriptInvariant(input);
        };
        const std::vector<QByteArray> corpus = installScriptCorpus();
        const int budget = sak::fuzz::iterationsFromEnv();
        QVERIFY2(budget > 0, "the clamp must never hand run() a non-positive iteration budget");
        if (!qEnvironmentVariableIsSet("SAK_FUZZ_ITERS")) {
            QCOMPARE(budget, kShippedFuzzIterations);
        }
        const sak::fuzz::FuzzOutcome outcome =
            sak::fuzz::run(corpus, target, budget, sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. Both sides are
        // LITERALS: drawing either from corpus.size() or a second iterationsFromEnv() call is
        // self-satisfying, since if the clamp answered 0 the mutation loop would run zero times,
        // iterations_run would equal the seed count, and seed_count + 0 would still match -- and a
        // seed silently dropped from installScriptCorpus() shrinks the surface unnoticed.
        QCOMPARE(static_cast<int>(corpus.size()), kExpectedCorpusSeeds);
        QCOMPARE(outcome.iterations_run, kExpectedCorpusSeeds + budget);
    }
};

QTEST_GUILESS_MAIN(InstallScriptFuzzTests)
#include "test_fuzz_install_script.moc"
