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
    if (a.package_type != b.package_type || a.silent_args != b.silent_args ||
        a.uses_splatting != b.uses_splatting || a.warnings != b.warnings ||
        a.resources.size() != b.resources.size()) {
        return false;
    }
    for (int i = 0; i < a.resources.size(); ++i) {
        if (!sameResource(a.resources.at(i), b.resources.at(i))) {
            return false;
        }
    }
    return true;
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
    for (const auto& resource : first.resources) {
        if (resource.line_number < 0) {
            return QStringLiteral("resource reports a negative line number (%1)")
                .arg(resource.line_number);
        }
    }
    return {};
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
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_GUILESS_MAIN(InstallScriptFuzzTests)
#include "test_fuzz_install_script.moc"
