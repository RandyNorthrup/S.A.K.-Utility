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
    void parsePackageHashFromOData_prefersPrefixedSpelling();
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
    // A NEAR-MISS of a device name is still a legitimate component: the check compares
    // the WHOLE dot-base against the reserved set, never a prefix/suffix/substring, so
    // loosening it would silently refuse real package ids at the four call sites in
    // offline_deployment_worker.cpp that gate ids read from a downloaded .nuspec.
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("console2")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("nullsoft")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("auxiliary")));
    // COM10 / LPT10 are NOT reserved (only COM1-COM9 and LPT1-LPT9 are).
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("com10")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("lpt10.install")));
    // Ending with or embedding a device name is likewise not a device.
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("silicon")));
    QVERIFY(PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("winlpt1-tools")));
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
    // The remaining two Win32-illegal characters named in the predicate's own contract
    // ('>' and the double quote) are part of the same table and must be refused too --
    // a shortened kForbidden must not stay green here.
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a>b")));
    QVERIFY(!PackageInternalizationEngine::isSafePackageComponent(QStringLiteral("a\"b")));
    // ...and the consumer that turns a URL segment into an on-disk basename must
    // collapse such a name to the indexed fallback, never write it verbatim (the name
    // is also spliced into the rewritten PowerShell script, where a quote breaks out
    // of a quoted literal).
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("a\"b.exe"), 0),
             QStringLiteral("binary_1"));
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

    // Empty expected checksum: nothing declared, so nothing to fail on -- and the
    // "was anything declared?" decision is made on the TRIMMED value, so a
    // whitespace-only checksum is still "nothing declared", never an unresolvable
    // 3-character digest.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(data, QString(), QString()));
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("   "), QString()));
    // That same trim also produces the hex length the algorithm is inferred from: a
    // valid digest captured with a trailing CR/LF (as Chocolatey scripts routinely
    // yield) must still measure 64 and resolve to SHA-256, never measure 66 and
    // resolve to no algorithm at all, which would refuse a correct installer.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, sha256 + QStringLiteral("\r\n"), QString()));
    // ...and it is applied on the comparison side too, not only to pick the length:
    // with an explicit hint (where no length inference happens) a padded digest of
    // the right algorithm still matches.
    QVERIFY(PackageInternalizationEngine::binaryChecksumMatches(
        data, QStringLiteral("  ") + md5 + QStringLiteral(" "), QStringLiteral("md5")));
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
    // The refusal here is a DELEGATION to the five-guard isSafePackageComponent, so
    // prove the guards a separator scan would miss: a reserved device name, a trailing
    // dot or space, a Win32-illegal character, a control character, and bare '.'. This
    // is where an attacker-controlled URL segment (url.fileName()) becomes a filename
    // created in the tools dir and spliced by name into the rewritten install script.
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("CON"), 5),
             QStringLiteral("binary_6"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("nul.txt"), 6),
             QStringLiteral("binary_7"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("setup.exe."), 7),
             QStringLiteral("binary_8"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("setup.exe "), 8),
             QStringLiteral("binary_9"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("a*b.exe"), 10),
             QStringLiteral("binary_11"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("a\nb.exe"), 11),
             QStringLiteral("binary_12"));
    QCOMPARE(PackageInternalizationEngine::sanitizedBinaryBasename(QStringLiteral("."), 12),
             QStringLiteral("binary_13"));
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

    // The base64 compare is EXACT, unlike the CASE-INSENSITIVE hex compare in the sibling
    // binaryChecksumMatches (deliberately pinned above). Base64 is case-SIGNIFICANT: an
    // upper-cased digest decodes to a different byte string and must NOT verify. The "AAAA"
    // fixture above differs in length too, so it cannot probe case; copying the sibling's
    // Qt::CaseInsensitive compare here would collapse distinct digests onto one another in
    // the gate that authenticates the whole downloaded .nupkg (verifyNupkgIntegrity).
    QVERIFY(b64_512.toUpper() != b64_512);  // the case pins below are not vacuous
    QVERIFY(b64_256.toUpper() != b64_256);
    QVERIFY(!PackageInternalizationEngine::nupkgHashMatches(
        data, b64_512.toUpper(), QStringLiteral("SHA512")));
    QVERIFY(!PackageInternalizationEngine::nupkgHashMatches(
        data, b64_256.toUpper(), QStringLiteral("sha256")));
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
    // The version compare is EXACT, not prefix / suffix / substring / component-truncating:
    // a near-miss request must never resolve to a DIFFERENT entry's PackageHash, which
    // verifyNupkgIntegrity then trusts as the expected digest for the downloaded body.
    // "2.0" kills a startsWith/prefix compare, "2.0.0.0" kills a component-truncating
    // compare, and the shared tail "0.0" kills an endsWith/contains compare.
    const QStringList near_miss_versions = {QStringLiteral("2.0"),
                                            QStringLiteral("2.0.0.0"),
                                            QStringLiteral("0.0")};
    for (const QString& near_miss : near_miss_versions) {
        const auto near_meta = PackageInternalizationEngine::parsePackageHashFromOData(doc,
                                                                                       near_miss);
        QVERIFY2(near_meta.first.isEmpty(), qPrintable(near_miss));
        QVERIFY2(near_meta.second.isEmpty(), qPrintable(near_miss));
    }
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

void TestPackageInternalizationEngine::parsePackageHashFromOData_prefersPrefixedSpelling() {
    // Both spellings in ONE properties element: the OData-prefixed child WINS, so a
    // crafted unprefixed twin cannot substitute the hash the integrity check compares.
    // Every other fixture supplies exactly one spelling, so swapping the two lookups in
    // odataChildText would be invisible.
    const QByteArray dual_child = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><m:properties><Version>4.0.0</Version><d:Version>4.0.0</d:Version>"
        "<PackageHash>PLAINHASH==</PackageHash>"
        "<d:PackageHash>PREFIXHASH==</d:PackageHash>"
        "<PackageHashAlgorithm>SHA256</PackageHashAlgorithm>"
        "<d:PackageHashAlgorithm>SHA512</d:PackageHashAlgorithm>"
        "</m:properties></entry></feed>");
    const auto dual_child_meta = PackageInternalizationEngine::parsePackageHashFromOData(dual_child,
                                                                                         "4.0.0");
    QCOMPARE(dual_child_meta.first, QStringLiteral("PREFIXHASH=="));
    QCOMPARE(dual_child_meta.second, QStringLiteral("SHA512"));

    // Both spellings as SIBLING property containers: 'm:properties' wins over a plain
    // 'properties' even when the plain one comes FIRST in document order.
    const QByteArray dual_props = QByteArrayLiteral(
        "<feed xmlns:m='m' xmlns:d='d'>"
        "<entry><properties><Version>5.0.0</Version>"
        "<PackageHash>PLAINPROPS==</PackageHash>"
        "<PackageHashAlgorithm>SHA256</PackageHashAlgorithm></properties>"
        "<m:properties><d:Version>5.0.0</d:Version>"
        "<d:PackageHash>PREFIXPROPS==</d:PackageHash>"
        "<d:PackageHashAlgorithm>SHA512</d:PackageHashAlgorithm></m:properties></entry></feed>");
    const auto dual_props_meta = PackageInternalizationEngine::parsePackageHashFromOData(dual_props,
                                                                                         "5.0.0");
    QCOMPARE(dual_props_meta.first, QStringLiteral("PREFIXPROPS=="));
    QCOMPARE(dual_props_meta.second, QStringLiteral("SHA512"));
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
    // The brace and backtick boundaries are catalog members too, and nothing else on such a
    // line can fire: a conditional download inside a script block is the ordinary
    // chocolateyInstall.ps1 shape, and a partial boundary class would call it offline-ready.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("if ($ok) { wget $u -O $dst }")));
    QVERIFY(
        PackageInternalizationEngine::scriptHasNetworkDownload(QStringLiteral("$body = `irm $u`")));
    // A URL assembled by string concatenation to dodge a literal-URL scan.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = 'http' + '://' + $host + '/f'")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = 'https' + $rest")));
    // The second concat arm exists for a scheme held in a VARIABLE: no quoted scheme
    // literal appears anywhere on the line, so only `+ '://'` is left able to decide.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = $scheme + '://' + $host + '/f'")));
    // The protocol-relative form of the same arm (the colon is optional in it).
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = $scheme + '//' + $host")));
    // ftp is the third scheme in the quoted-literal arm, isolated here with no
    // "ftp://" literal on the line for the URL scan to pick up instead.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$u = 'ftp' + $rest")));
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
    // The remaining four primitives in the same catalog, each ISOLATED on a line with no
    // literal URL, no curl/wget/choco command, and no second catalog token -- deleting any
    // one entry from kDownloadTokens must go red here.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$s = $client.DownloadString($u)")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$b = $client.DownloadData($u)")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$client = New-Object Net.WebClient")));
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("$h = [Net.Http.HttpClient]::new()")));
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
    // ...and an INDENTED banner comment is still a comment: the line is trimmed
    // before the '#' test, so leading whitespace does not expose the URL.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("    # Project homepage: https://project.example/downloads")));
    // A comment line is SKIPPED, not the end of the scan. scriptForcesNetwork feeds
    // this the WHOLE script and a real chocolateyInstall.ps1 opens with a banner
    // comment, so the executable lines AFTER it must still be scanned.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("# chocolateyInstall.ps1\n"
                       "$ErrorActionPreference = 'Stop'\n"
                       "Invoke-WebRequest -Uri $u -OutFile $out")));
    // Only a line that STARTS with '#' is a comment: a trailing '#' comment does
    // not exempt the executable code before it on the same line.
    QVERIFY(PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Invoke-WebRequest -Uri $u -OutFile $out  # fetch prerequisite")));
    // A choco EXTENSION module reference (no install/upgrade verb) is not a nested
    // install and must not trip the detector.
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(
        QStringLiteral("Import-Module chocolatey-core.extension")));
    QVERIFY(!PackageInternalizationEngine::scriptHasNetworkDownload(QString()));
}

QTEST_APPLESS_MAIN(TestPackageInternalizationEngine)
#include "test_package_internalization_engine.moc"
