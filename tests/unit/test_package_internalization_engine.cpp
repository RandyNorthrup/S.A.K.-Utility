// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_package_internalization_engine.cpp
/// @brief Unit tests for PackageInternalizationEngine pure seams (B10-11).

#include "sak/package_internalization_engine.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;

class TestPackageInternalizationEngine : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void isSafePackageComponent_acceptsNormalIdsAndVersions();
    void isSafePackageComponent_rejectsTraversalAndSeparators();
    void binaryChecksumMatches_verifiesNamedAndInferredAlgorithms();
    void binaryChecksumMatches_rejectsMismatchAndUnresolvable();
    void installerVerified_failsClosedOnMissingChecksum();
    void sanitizedBinaryBasename_reducesUnsafeNamesToFallback();
    void isAllowedInstallerUrl_acceptsOnlyHttps();
    void nupkgHashMatches_verifiesBase64FeedHash();
    void parsePackageHashFromOData_extractsHashForVersion();
    void parseLatestVersionFromOData_picksSemverMaxWithoutLatestFlag();
    void parseLatestVersionFromOData_failsClosedOnUnparseableVersions();
    void scriptHasNetworkDownload_detectsUrlsAndRawDownloadPrimitives();
    void scriptHasNetworkDownload_detectsDynamicAndCommandDownloads();
    void scriptHasNetworkDownload_ignoresLocalOnlyScripts();
};

void TestPackageInternalizationEngine::isSafePackageComponent_acceptsNormalIdsAndVersions() {
    // Real NuGet ids (dotted) and versions (semver / prerelease / build metadata).
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("googlechrome")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("Microsoft.NET")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("7zip.install")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("1.2.3")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("1.2.3-beta.1")));
    QVERIFY(
        PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("2024.01.0+build5")));
}

void TestPackageInternalizationEngine::isSafePackageComponent_rejectsTraversalAndSeparators() {
    // A crafted id/version must not escape the work dir when concatenated into a path.
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QString()));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral(".")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("..")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("../evil")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a/b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a\\b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("C:evil")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a\nb")));
    // CODEX_REVIEW_4 M-B2-35: Windows reserved device names, trailing dot/space, and
    // the Win32-illegal filename characters must also be rejected.
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("CON")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("nul.txt")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("LPT1")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("name.")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("name ")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a<b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a*b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a?b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a|b")));
}

void TestPackageInternalizationEngine::binaryChecksumMatches_verifiesNamedAndInferredAlgorithms() {
    const QByteArray data = QByteArrayLiteral("hello");
    // Known digests of "hello".
    const QString md5 = QStringLiteral("5d41402abc4b2a76b9719d911017c592");
    const QString sha1 = QStringLiteral("aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d");
    const QString sha256 =
        QStringLiteral("2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824");

    // Explicit type hint (case-insensitive on both sides).
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QStringLiteral("md5")));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, sha256.toUpper(), QStringLiteral("sha256")));
    QVERIFY(
        PackageInternalizationEngine::binaryChecksumMatches(data, sha1, QStringLiteral("SHA1")));

    // No type hint -> algorithm inferred from the checksum's hex length.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, sha256, QString()));

    // Empty expected checksum: nothing declared, so nothing to fail on.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, QString(), QString()));
}

void TestPackageInternalizationEngine::binaryChecksumMatches_rejectsMismatchAndUnresolvable() {
    const QByteArray data = QByteArrayLiteral("hello");
    // Wrong digest for the data.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("00000000000000000000000000000000"), QStringLiteral("md5")));
    // Declared but unresolvable: unknown algorithm name.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("deadbeef"), QStringLiteral("crc32")));
    // Declared but unresolvable: no hint and a nonstandard hex length.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("deadbeef"), QString()));
}

void TestPackageInternalizationEngine::installerVerified_failsClosedOnMissingChecksum() {
    const QByteArray data = QByteArrayLiteral("hello");
    const QString md5 = QStringLiteral("5d41402abc4b2a76b9719d911017c592");
    // A resolvable, matching declared checksum verifies.
    QVERIFY(PackageInternalizationEngine::installerVerified(data, md5, QStringLiteral("md5")));
    // FAIL CLOSED: no declared checksum -> unverifiable -> refused (NOT treated as
    // verified the way the pure matcher binaryChecksumMatches would allow).
    QVERIFY(!PackageInternalizationEngine::installerVerified(data, QString(), QString()));
    QVERIFY(!PackageInternalizationEngine::installerVerified(
        data, QStringLiteral("   "), QStringLiteral("md5")));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, QString(), QString()));
    // A declared-but-mismatched checksum still fails.
    QVERIFY(!PackageInternalizationEngine::installerVerified(
        data, QStringLiteral("00000000000000000000000000000000"), QStringLiteral("md5")));
}

void TestPackageInternalizationEngine::sanitizedBinaryBasename_reducesUnsafeNamesToFallback() {
    // A normal basename is preserved verbatim.
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("setup.exe"), 0),
             QStringLiteral("setup.exe"));
    // Empty / traversal / separators / drive-colon collapse to the indexed fallback.
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QString(), 0),
             QStringLiteral("binary_1"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral(".."), 2),
             QStringLiteral("binary_3"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("a/b/evil.exe"),
                                                                   4),
             QStringLiteral("binary_5"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("a\\b.exe"), 1),
             QStringLiteral("binary_2"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("C:evil"), 0),
             QStringLiteral("binary_1"));
}

void TestPackageInternalizationEngine::isAllowedInstallerUrl_acceptsOnlyHttps() {
    QVERIFY(PackageInternalizationEngine::isAllowedInstallerUrl(
        QStringLiteral("https://community.chocolatey.org/api/v2/package/x")));
    QVERIFY(PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("HTTPS://Host/F")));
    // Insecure / local / malformed origins are refused (fail closed).
    QVERIFY(!PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("http://host/f")));
    QVERIFY(!PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("ftp://host/f")));
    QVERIFY(!PackageInternalizationEngine::isAllowedInstallerUrl(
        QStringLiteral("file:///C:/Windows/System32/x.dll")));
    QVERIFY(
        !PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("\\\\server\\share")));
    QVERIFY(!PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("C:\\local\\x")));
    QVERIFY(!PackageInternalizationEngine::isAllowedInstallerUrl(QString()));
    QVERIFY(
        !PackageInternalizationEngine::isAllowedInstallerUrl(QStringLiteral("https:///nohost")));
}

void TestPackageInternalizationEngine::nupkgHashMatches_verifiesBase64FeedHash() {
    const QByteArray data = QByteArrayLiteral("hello");
    const QString b64_512 =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha512).toBase64());
    const QString b64_256 =
        QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toBase64());
    // Correct algorithm + base64 digest verifies (algorithm name case-insensitive).
    QVERIFY(
        PackageInternalizationEngine::nupkgHashMatches(data, b64_512, QStringLiteral("SHA512")));
    QVERIFY(
        PackageInternalizationEngine::nupkgHashMatches(data, b64_256, QStringLiteral("sha256")));
    // Wrong algorithm, empty hash, unknown algorithm, and a mismatch all fail closed.
    QVERIFY(
        !PackageInternalizationEngine::nupkgHashMatches(data, b64_512, QStringLiteral("SHA256")));
    QVERIFY(
        !PackageInternalizationEngine::nupkgHashMatches(data, QString(), QStringLiteral("SHA512")));
    QVERIFY(
        !PackageInternalizationEngine::nupkgHashMatches(data, b64_512, QStringLiteral("crc32")));
    QVERIFY(!PackageInternalizationEngine::nupkgHashMatches(
        data, QStringLiteral("AAAA"), QStringLiteral("SHA512")));
}

void TestPackageInternalizationEngine::parsePackageHashFromOData_extractsHashForVersion() {
    const QByteArray doc = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>1.0.0</d:Version>"
        "<d:PackageHash>AAAAHASH==</d:PackageHash>"
        "<d:PackageHashAlgorithm>SHA512</d:PackageHashAlgorithm></m:properties></entry>"
        "<entry><m:properties><d:Version>2.0.0</d:Version>"
        "<d:PackageHash>BBBBHASH==</d:PackageHash>"
        "<d:PackageHashAlgorithm>SHA512</d:PackageHashAlgorithm></m:properties></entry></feed>");
    const auto meta = PackageInternalizationEngine::parsePackageHashFromOData(doc, "2.0.0");
    QCOMPARE(meta.first, QStringLiteral("BBBBHASH=="));
    QCOMPARE(meta.second, QStringLiteral("SHA512"));
    // A version absent from the feed yields an empty (fail-closed) pair.
    const auto missing = PackageInternalizationEngine::parsePackageHashFromOData(doc, "9.9.9");
    QVERIFY(missing.first.isEmpty());
    QVERIFY(missing.second.isEmpty());  // the whole pair is empty (fail closed), not just the hash
}

void TestPackageInternalizationEngine::
    parseLatestVersionFromOData_picksSemverMaxWithoutLatestFlag() {
    // No IsLatestVersion flag: must pick the SemVer-max (1.10.0 > 1.9.0 > 1.2.0),
    // NOT the feed's last entry (1.9.0) and NOT a string sort (which ranks "1.9" high).
    const QByteArray unsorted = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>1.2.0</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>1.10.0</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>1.9.0</d:Version></m:properties></entry></feed>");
    QCOMPARE(PackageInternalizationEngine::parseLatestVersionFromOData(unsorted),
             QStringLiteral("1.10.0"));
    // An explicit IsLatestVersion flag wins outright over SemVer ordering.
    const QByteArray flagged = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>2.0.0</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>1.5.0</d:Version>"
        "<d:IsLatestVersion>true</d:IsLatestVersion></m:properties></entry></feed>");
    QCOMPARE(PackageInternalizationEngine::parseLatestVersionFromOData(flagged),
             QStringLiteral("1.5.0"));
}

void TestPackageInternalizationEngine::
    parseLatestVersionFromOData_failsClosedOnUnparseableVersions() {
    // No entry is flagged latest and NO version parses as a valid SemVer: the result
    // must be empty (fail closed), never a fall back to the feed's raw last entry --
    // trusting unparseable feed order is the downgrade vector this must avoid.
    const QByteArray garbage = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>not-a-version</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>also.bad.$$</d:Version></m:properties></entry></feed>");
    QVERIFY(PackageInternalizationEngine::parseLatestVersionFromOData(garbage).isEmpty());
}

void TestPackageInternalizationEngine::
    scriptHasNetworkDownload_detectsDynamicAndCommandDownloads() {
    // curl/wget/iwr/irm as a COMMAND with a variable-built URL argument (no literal
    // URL on the line) must still register as a network fetch.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("curl -o $out $downloadUrl")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("wget $u -O $dst")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("iwr $u -OutFile $o")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Start-Process curl -ArgumentList $u")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("& wget $u")));
    // A URL assembled by string concatenation to dodge a literal-URL scan.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = 'http' + '://' + $host + '/f'")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = 'https' + $rest")));
    // The word "confirm" contains "irm" but is not a command -> must NOT trip.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Read-Host -Prompt 'Please confirm the settings'")));
    // curl/wget as a bare local-file argument (not a command) must NOT trip.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Copy-Item curl-8.0.0.zip $dest")));
}

void TestPackageInternalizationEngine::
    scriptHasNetworkDownload_detectsUrlsAndRawDownloadPrimitives() {
    // A literal remote URL is a download indicator (the honest offline signal).
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Invoke-WebRequest -Uri 'https://ex.com/vc.exe' -OutFile $out")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$c = New-Object Net.WebClient; $c.DownloadFile($u, $p)")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Start-BitsTransfer -Source http://host/f -Destination x")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("curl -o x ftp://h/f")));
    // A nested Chocolatey invocation fetches another package from the feed at
    // install time and cannot be constrained to the bundle -> honestly will-fetch,
    // even though the parser extracts no URL from such a script.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("choco install somedep -y")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("cinst othertool")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("choco.exe upgrade thing -y")));
}

void TestPackageInternalizationEngine::scriptHasNetworkDownload_ignoresLocalOnlyScripts() {
    // A config-only script (no download) is offline-ready.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Set-ItemProperty HKLM:\\Software\\App -Name Enabled -Value 1")));
    // An install from a LOCAL file (the internalized case) is not a network fetch.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Install-ChocolateyInstallPackage -File (Join-Path $toolsDir 'app.exe')")));
    // curl/wget appearing ONLY as the rewriter's local filename (not a command
    // with a URL) must not be read as a network fetch.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral(
        "Install-ChocolateyZipPackage -File (Join-Path $toolsDir 'curl-8.0.0.zip')")));
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Copy-Item (Join-Path $toolsDir 'wget.exe') $dest")));
    // A full-line comment carrying a homepage URL is documentation, not a download.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("# Project homepage: https://project.example/downloads")));
    // A choco EXTENSION module reference (no install/upgrade verb) is not a nested
    // install and must not trip the detector.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Import-Module chocolatey-core.extension")));
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(QString()));
}

QTEST_APPLESS_MAIN(TestPackageInternalizationEngine)
#include "test_package_internalization_engine.moc"
