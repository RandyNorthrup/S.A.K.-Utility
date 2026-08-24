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
    const QStringList reserved_devices = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    QCOMPARE(reserved_devices.size(), qsizetype(22));  // the loop below is not vacuous
    for (const QString& device : reserved_devices) {
        QVERIFY2(!PackageInternalizationEngine::isSafePackageComponent(device), qPrintable(device));
        QVERIFY2(!PackageInternalizationEngine::isSafePackageComponent(device.toLower() +
                                                                       QStringLiteral(".txt")),
                 qPrintable(device));
    }
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
    const QString sha512 = QStringLiteral(
        "9b71d224bd62f3785d96d46ad3ea3d73319bfbc2890caadae2dff72519673ca7"
        "2323c3d99ba5c11d7c7acc6e14b8c5da0c4663475c2e5c3adef46f73bcdec043");

    // Explicit type hint (case-insensitive on both sides).
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QStringLiteral("md5")));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, sha256.toUpper(), QStringLiteral("sha256")));
    QVERIFY(
        PackageInternalizationEngine::binaryChecksumMatches(data, sha1, QStringLiteral("SHA1")));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, sha512, QStringLiteral("sha512")));

    // No type hint -> algorithm inferred from the checksum's hex length. Every
    // supported length must resolve: 32 -> MD5, 40 -> SHA1, 64 -> SHA256,
    // 128 -> SHA512 (a partial length table would still pass an md5/sha256-only check).
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, md5, QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, sha1, QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, sha256, QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, sha512, QString()));

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
    // An explicit type hint OUTRANKS the hex length: a 32-hex digest declared as
    // sha256 is hashed with SHA-256 (and so cannot match), never re-inferred as MD5
    // from its length.
    QVERIFY(!PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("5d41402abc4b2a76b9719d911017c592"), QStringLiteral("sha256")));
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
    // A feed that emits the OData properties WITHOUT the m:/d: prefixes resolves
    // through the plain-name fallback arm, not only the prefixed arm.
    const QByteArray plain = QByteArrayLiteral(
        "<feed><entry><properties><Version>3.0.0</Version>"
        "<PackageHash>CCCCHASH==</PackageHash>"
        "<PackageHashAlgorithm>SHA256</PackageHashAlgorithm></properties></entry></feed>");
    const auto plain_meta = PackageInternalizationEngine::parsePackageHashFromOData(plain, "3.0.0");
    QCOMPARE(plain_meta.first, QStringLiteral("CCCCHASH=="));
    QCOMPARE(plain_meta.second, QStringLiteral("SHA256"));
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
    // An explicit IsLatestVersion flag wins outright over SemVer ordering, and the flag text is
    // matched case-insensitively ("True" counts).
    const QByteArray flagged = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>2.0.0</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>1.5.0</d:Version>"
        "<d:IsLatestVersion>True</d:IsLatestVersion></m:properties></entry></feed>");
    QCOMPARE(PackageInternalizationEngine::parseLatestVersionFromOData(flagged),
             QStringLiteral("1.5.0"));
    // The flag's VALUE decides, not its mere PRESENCE: with every entry flagged false the
    // SemVer-max (2.0.0) wins, never the first flag-bearing entry.
    const QByteArray flagged_false = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>1.5.0</d:Version>"
        "<d:IsLatestVersion>false</d:IsLatestVersion></m:properties></entry>"
        "<entry><m:properties><d:Version>2.0.0</d:Version>"
        "<d:IsLatestVersion>false</d:IsLatestVersion></m:properties></entry></feed>");
    QCOMPARE(PackageInternalizationEngine::parseLatestVersionFromOData(flagged_false),
             QStringLiteral("2.0.0"));
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
    // ...but an unparseable entry ALONGSIDE a parseable one is skipped, not allowed
    // to poison the whole feed: the SemVer-max of the parseable entries is returned.
    const QByteArray mixed = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><d:Version>not-a-version</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>1.4.0</d:Version></m:properties></entry>"
        "<entry><m:properties><d:Version>also.bad.$$</d:Version></m:properties></entry></feed>");
    QCOMPARE(PackageInternalizationEngine::parseLatestVersionFromOData(mixed),
             QStringLiteral("1.4.0"));
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
    // irm (the fourth alias), the .exe form, and the remaining command boundaries
    // (pipe / semicolon / paren) are part of the same catalog and must all fire.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("irm $u -OutFile $o")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("curl.exe -o $out $u")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("$list | wget $u")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$x = 1; iwr $u -OutFile $o")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("$body = (irm $u)")));
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
    // Each of the three OR'd guards is reported through ONE bool, so isolate them: a scheme on a
    // line with no download token and no curl/wget/choco command leaves only the literal-URL scan
    // able to fire.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$src = 'https://ex.com/vc.exe'")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$src = 'http://ex.com/vc.exe'")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$src = 'ftp://h/f'")));
    // Every raw download primitive in the token catalog, each isolated on a line with NO literal
    // URL: a partial token list must not stay green.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Invoke-WebRequest -Uri $u -OutFile $out")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Invoke-RestMethod -Uri $u -OutFile $out")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Start-BitsTransfer -Source $src -Destination x")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("bitsadmin /transfer job $src x")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$c.DownloadFile($u, $p)")));
    // A nested Chocolatey invocation fetches another package from the feed at
    // install time and cannot be constrained to the bundle -> honestly will-fetch,
    // even though the parser extracts no URL from such a script.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("choco install somedep -y")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("cinst othertool")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("choco.exe upgrade thing -y")));
    // The spelled-out "chocolatey" alias of the same verb counts too.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("chocolatey install somedep -y")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("chocolatey upgrade thing -y")));
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
