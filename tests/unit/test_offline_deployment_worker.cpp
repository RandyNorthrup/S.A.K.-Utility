// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_offline_deployment_worker.cpp
/// @brief Unit tests for OfflineDeploymentWorker path-safety + integrity seams
///        (B10-13 work-dir ownership, B10-14 installer-filename confinement,
///        B10-18 manifest filename/checksum/size verification before install).

#include "sak/offline_deployment_worker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using namespace sak;
using WorkDirDisposition = OfflineDeploymentWorker::WorkDirDisposition;

class TestOfflineDeploymentWorker : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void classifyWorkDir_freshWhenMissingOrEmpty();
    void classifyWorkDir_reusesOwnedMarkedDir();
    void classifyWorkDir_refusesForeignNonEmptyDir();
    void workDirSafeToDelete_guardsReparseAndOwnership();
    void safeInstallerFilename_keepsPlainNames();
    void safeInstallerFilename_confinesTraversalNames();
    void sanitizeManifestFilename_rejectsPathsAndTraversal();
    void verifyBundledPackage_acceptsMatchingChecksumAndSize();
    void verifyBundledPackage_rejectsMismatchMissingAndBadName();
    void installDispositionFor_skipsNotPackedOnlyUnderAirGap();
    void collectInstallerDownloads_collectsEveryResourceWithChecksums();
    void uniqueFilename_disambiguatesCollidingBasenames();
    void uniqueFilename_disambiguatesCaseInsensitively();
    void isChocolateyFrameworkId_matchesOnlyTheFrameworkPackage();
    void isSafeInstallToken_rejectsOptionLikeAndBlankTokens();
    void topologicalInstallOrder_installsDepsBeforeDependents();
    void topologicalInstallOrder_reportsCyclicMembers();
    void installContextForMode_bundleLocalListFeed();
    void unmetClosureDependencies_flagsMissingIntraClosureDeps();
    void isSuccessInstallExitCode_acceptsZeroAndRebootCodes();
    void nupkgFeedVerified_requiresAFeedPublishedHash();
    void nupkgFeedVerified_acceptsOnlyTheMatchingFeedHash();
};

void TestOfflineDeploymentWorker::classifyWorkDir_freshWhenMissingOrEmpty() {
    // Absent dir: create it.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(false, false, false),
             WorkDirDisposition::CreateFresh);
    // Pre-existing EMPTY dir with no marker is safe to adopt.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, false, true),
             WorkDirDisposition::CreateFresh);
}

void TestOfflineDeploymentWorker::classifyWorkDir_reusesOwnedMarkedDir() {
    // A dir bearing our ownership marker is a leftover we may reuse and delete,
    // whether or not it currently holds files.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, true, false),
             WorkDirDisposition::ReuseOwned);
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, true, true),
             WorkDirDisposition::ReuseOwned);
}

void TestOfflineDeploymentWorker::classifyWorkDir_refusesForeignNonEmptyDir() {
    // A non-empty dir we never stamped must never be created-into or wiped.
    QCOMPARE(OfflineDeploymentWorker::classifyWorkDir(true, false, false),
             WorkDirDisposition::RefuseForeign);
}

void TestOfflineDeploymentWorker::workDirSafeToDelete_guardsReparseAndOwnership() {
    // CODEX_REVIEW_4 M-B2-10: a work dir is only recursively deletable when it is not a reparse
    // point AND bears our ownership marker; a missing dir is a safe no-op.
    using W = OfflineDeploymentWorker;
    QVERIFY(W::workDirSafeToDelete(false, false, false));  // missing -> safe no-op
    QVERIFY(W::workDirSafeToDelete(false, true, true));    // missing (other flags moot) -> safe
    QVERIFY(W::workDirSafeToDelete(true, false, true));    // owned, not reparse -> deletable
    QVERIFY(!W::workDirSafeToDelete(true, true, true));    // reparse point -> refuse
    QVERIFY(!W::workDirSafeToDelete(true, false, false));  // no ownership marker -> refuse
    QVERIFY(!W::workDirSafeToDelete(true, true, false));   // reparse + unmarked -> refuse
}

void TestOfflineDeploymentWorker::safeInstallerFilename_keepsPlainNames() {
    const QString fb = QStringLiteral("fallback.bin");
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("setup.exe"), fb),
             QStringLiteral("setup.exe"));
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("Chrome_x64.msi"), fb),
             QStringLiteral("Chrome_x64.msi"));
    // A leading directory is stripped to its safe basename.
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("a/b/evil.exe"), fb),
             QStringLiteral("evil.exe"));
}

void TestOfflineDeploymentWorker::safeInstallerFilename_confinesTraversalNames() {
    const QString fb = QStringLiteral("pkg_installer_1");
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QString(), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("."), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral(".."), fb), fb);
    QCOMPARE(OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("../../.."), fb), fb);
    // Whatever the platform makes of a backslash, the result never carries a
    // separator that could redirect the write outside the output dir.
    const QString bs =
        OfflineDeploymentWorker::safeInstallerFilename(QStringLiteral("bad\\path.exe"), fb);
    QVERIFY(!bs.contains(QLatin1Char('/')));
    QVERIFY(!bs.contains(QLatin1Char('\\')));
}

void TestOfflineDeploymentWorker::sanitizeManifestFilename_rejectsPathsAndTraversal() {
    QCOMPARE(OfflineDeploymentWorker::sanitizeManifestFilename(QStringLiteral("chrome.1.0.nupkg")),
             QStringLiteral("chrome.1.0.nupkg"));
    // Empty and pure-traversal names are refused outright.
    QVERIFY(OfflineDeploymentWorker::sanitizeManifestFilename(QString()).isEmpty());
    QVERIFY(OfflineDeploymentWorker::sanitizeManifestFilename(QStringLiteral("..")).isEmpty());
    // A directory-bearing name is reduced to a basename or refused -- either way
    // the result never carries a separator that could escape the source dir.
    const QString r =
        OfflineDeploymentWorker::sanitizeManifestFilename(QStringLiteral("../../etc/evil.nupkg"));
    QVERIFY(!r.contains(QLatin1Char('/')));
    QVERIFY(!r.contains(QLatin1Char('\\')));
}

void TestOfflineDeploymentWorker::verifyBundledPackage_acceptsMatchingChecksumAndSize() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray body = QByteArrayLiteral("PK\x03\x04 fake nupkg bytes");
    const QString name = QStringLiteral("pkg.1.0.nupkg");
    QFile f(QDir(dir.path()).filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(body);
    f.close();

    DeploymentManifestEntry entry;
    entry.package_id = QStringLiteral("pkg");
    entry.version = QStringLiteral("1.0");
    entry.nupkg_filename = name;
    entry.size_bytes = body.size();
    entry.checksum =
        QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha256).toHex());

    QString err;
    QVERIFY2(OfflineDeploymentWorker::verifyBundledPackage(entry, dir.path(), err),
             qPrintable(err));

    // Fail closed: a Bundle entry MUST carry both a size AND a checksum. An entry
    // missing either can no longer be verified and is rejected before install.
    DeploymentManifestEntry no_sum = entry;
    no_sum.checksum.clear();
    QString err2;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(no_sum, dir.path(), err2));

    DeploymentManifestEntry no_size = entry;
    no_size.size_bytes = 0;
    QString err3;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(no_size, dir.path(), err3));
}

void TestOfflineDeploymentWorker::verifyBundledPackage_rejectsMismatchMissingAndBadName() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const QByteArray body = QByteArrayLiteral("real bytes");
    const QString name = QStringLiteral("pkg.1.0.nupkg");
    QFile f(QDir(dir.path()).filePath(name));
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(body);
    f.close();

    DeploymentManifestEntry entry;
    entry.nupkg_filename = name;
    entry.size_bytes = body.size();

    // Wrong checksum -> fail closed.
    entry.checksum = QString(64, QLatin1Char('a'));
    QString err;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(entry, dir.path(), err));
    QVERIFY(!err.isEmpty());

    // Wrong size -> reject even before hashing.
    DeploymentManifestEntry bad_size = entry;
    bad_size.checksum.clear();
    bad_size.size_bytes = body.size() + 1;
    QString err2;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(bad_size, dir.path(), err2));

    // Missing file -> reject.
    DeploymentManifestEntry missing = entry;
    missing.nupkg_filename = QStringLiteral("nope.nupkg");
    missing.checksum.clear();
    missing.size_bytes = 0;
    QString err3;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(missing, dir.path(), err3));

    // Traversal filename -> refused as invalid name.
    DeploymentManifestEntry bad_name = entry;
    bad_name.nupkg_filename = QStringLiteral("../evil.nupkg");
    bad_name.checksum.clear();
    bad_name.size_bytes = 0;
    QString err4;
    QVERIFY(!OfflineDeploymentWorker::verifyBundledPackage(bad_name, dir.path(), err4));
}

void TestOfflineDeploymentWorker::installDispositionFor_skipsNotPackedOnlyUnderAirGap() {
    using Disposition = OfflineDeploymentWorker::InstallDisposition;

    DeploymentManifestEntry fully_packed;
    fully_packed.offline_ready = true;
    DeploymentManifestEntry will_fetch;
    will_fetch.offline_ready = false;

    // Air-gap (packed_only): fully-packed -> install, will-fetch -> skip (it could
    // only fail against a deliberately disconnected target).
    QCOMPARE(OfflineDeploymentWorker::installDispositionFor(fully_packed, true),
             Disposition::Install);
    QCOMPARE(OfflineDeploymentWorker::installDispositionFor(will_fetch, true),
             Disposition::SkipNotPacked);

    // Default (install everything): both install; a will-fetch package fetches its
    // remainder at install time.
    QCOMPARE(OfflineDeploymentWorker::installDispositionFor(fully_packed, false),
             Disposition::Install);
    QCOMPARE(OfflineDeploymentWorker::installDispositionFor(will_fetch, false),
             Disposition::Install);
}

void TestOfflineDeploymentWorker::collectInstallerDownloads_collectsEveryResourceWithChecksums() {
    ParsedInstallScript parsed;
    DownloadResource a;
    a.url = QStringLiteral("https://host/a32.exe");
    a.checksum = QStringLiteral("aa32");
    a.checksum_type = QStringLiteral("sha256");
    a.url_64bit = QStringLiteral("https://host/a64.exe");
    a.checksum_64bit = QStringLiteral("aa64");
    a.checksum_type_64bit = QStringLiteral("sha256");
    DownloadResource b;
    b.url = QStringLiteral("https://host/b.msi");
    b.url_64bit = QStringLiteral("https://host/a64.exe");  // duplicate of a's 64-bit URL
    parsed.resources = {a, b};

    const QVector<InstallerDownload> got =
        OfflineDeploymentWorker::collectInstallerDownloads(parsed);
    // Every DISTINCT installer URL across ALL resources -- not just the first.
    QCOMPARE(got.size(), 3);
    auto findSum = [&](const QString& url) {
        for (const auto& d : got) {
            if (d.url == url) {
                return d.checksum;
            }
        }
        return QString(QStringLiteral("<absent>"));
    };
    // Each installer carries the checksum its resource declared.
    QCOMPARE(findSum(QStringLiteral("https://host/a32.exe")), QStringLiteral("aa32"));
    QCOMPARE(findSum(QStringLiteral("https://host/a64.exe")), QStringLiteral("aa64"));
    QCOMPARE(findSum(QStringLiteral("https://host/b.msi")), QString());

    // Empty parse -> nothing to download.
    QVERIFY(OfflineDeploymentWorker::collectInstallerDownloads(ParsedInstallScript{}).isEmpty());
}

void TestOfflineDeploymentWorker::uniqueFilename_disambiguatesCollidingBasenames() {
    QSet<QString> used;
    // Two distinct URLs whose basename is 'setup.exe' must map to different files.
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("setup.exe"), used),
             QStringLiteral("setup.exe"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("setup.exe"), used),
             QStringLiteral("setup_1.exe"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("setup.exe"), used),
             QStringLiteral("setup_2.exe"));
    // A distinct name is untouched; an extension-less name still disambiguates.
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("other.msi"), used),
             QStringLiteral("other.msi"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("installer"), used),
             QStringLiteral("installer"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("installer"), used),
             QStringLiteral("installer_1"));
}

void TestOfflineDeploymentWorker::uniqueFilename_disambiguatesCaseInsensitively() {
    QSet<QString> used;
    // On a case-insensitive filesystem (NTFS) 'Setup.exe' and 'setup.exe' are the
    // same file, so the second must be disambiguated rather than silently overwrite
    // the first. The original case of the returned name is preserved.
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("Setup.exe"), used),
             QStringLiteral("Setup.exe"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("setup.exe"), used),
             QStringLiteral("setup_1.exe"));
    QCOMPARE(OfflineDeploymentWorker::uniqueFilename(QStringLiteral("SETUP.EXE"), used),
             QStringLiteral("SETUP_2.EXE"));
}

void TestOfflineDeploymentWorker::isChocolateyFrameworkId_matchesOnlyTheFrameworkPackage() {
    // The framework package itself must be recognized (case-insensitive) so it is
    // never bundled/installed onto a target; helper/extension packages must NOT.
    QVERIFY(OfflineDeploymentWorker::isChocolateyFrameworkId(QStringLiteral("chocolatey")));
    QVERIFY(OfflineDeploymentWorker::isChocolateyFrameworkId(QStringLiteral("Chocolatey")));
    QVERIFY(
        !OfflineDeploymentWorker::isChocolateyFrameworkId(QStringLiteral("chocolatey.extension")));
    QVERIFY(!OfflineDeploymentWorker::isChocolateyFrameworkId(
        QStringLiteral("chocolatey-core.extension")));
    QVERIFY(!OfflineDeploymentWorker::isChocolateyFrameworkId(QStringLiteral("git.install")));
}

void TestOfflineDeploymentWorker::isSafeInstallToken_rejectsOptionLikeAndBlankTokens() {
    // Normal ids/versions pass.
    QVERIFY(OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("git.install")));
    QVERIFY(OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("1.2.3-beta.1")));
    // A tampered manifest must not inject a choco flag: reject option-like or
    // whitespace/blank tokens.
    QVERIFY(!OfflineDeploymentWorker::isSafeInstallToken(QString()));
    QVERIFY(!OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("--production")));
    QVERIFY(!OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("-x")));
    QVERIFY(!OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("a b")));
    QVERIFY(!OfflineDeploymentWorker::isSafeInstallToken(QStringLiteral("a\tb")));
}

void TestOfflineDeploymentWorker::topologicalInstallOrder_installsDepsBeforeDependents() {
    auto mk = [](const char* id, const QStringList& deps) {
        DeploymentManifestEntry e;
        e.package_id = QString::fromLatin1(id);
        e.dependencies = deps;
        return e;
    };
    auto indexOf = [](const QVector<DeploymentManifestEntry>& v, const char* id) {
        for (int i = 0; i < v.size(); ++i) {
            if (v[i].package_id == QLatin1String(id)) {
                return i;
            }
        }
        return -1;
    };

    // a -> b -> c (a depends on b, b depends on c): install order must be c, b, a.
    const QVector<DeploymentManifestEntry> chain{mk("a", {"b"}), mk("b", {"c"}), mk("c", {})};
    const auto ordered = OfflineDeploymentWorker::topologicalInstallOrder(chain);
    QCOMPARE(ordered.size(), 3);
    QVERIFY(indexOf(ordered, "c") < indexOf(ordered, "b"));
    QVERIFY(indexOf(ordered, "b") < indexOf(ordered, "a"));

    // A dependency cycle must not drop packages: all are still returned.
    const QVector<DeploymentManifestEntry> cyclic{mk("x", {"y"}), mk("y", {"x"})};
    QCOMPARE(OfflineDeploymentWorker::topologicalInstallOrder(cyclic).size(), 2);
}

void TestOfflineDeploymentWorker::installContextForMode_bundleLocalListFeed() {
    const auto bundle = OfflineDeploymentWorker::installContextForMode(
        PayloadMode::Bundle, QStringLiteral("C:/bundle/packages"));
    QCOMPARE(bundle.source, QStringLiteral("C:/bundle/packages"));  // local dir
    QVERIFY(bundle.ignore_dependencies);  // we install every closure member ourselves
    QVERIFY(bundle.verify_local);         // verify each local .nupkg

    const auto list = OfflineDeploymentWorker::installContextForMode(
        PayloadMode::List, QStringLiteral("C:/bundle/packages"));
    QVERIFY(list.source.startsWith(QStringLiteral("http")));  // the Chocolatey feed
    QVERIFY(!list.ignore_dependencies);                       // choco resolves from the feed
    QVERIFY(!list.verify_local);                              // nothing local to verify
}

void TestOfflineDeploymentWorker::topologicalInstallOrder_reportsCyclicMembers() {
    auto mk = [](const char* id, const QStringList& deps) {
        DeploymentManifestEntry e;
        e.package_id = QString::fromLatin1(id);
        e.dependencies = deps;
        return e;
    };
    // A clean chain reports NO cycle.
    QStringList clean_cyclic;
    const auto clean = OfflineDeploymentWorker::topologicalInstallOrder(
        {mk("a", {"b"}), mk("b", {})}, &clean_cyclic);
    QCOMPARE(clean.size(), 2);
    QVERIFY(clean_cyclic.isEmpty());

    // A cycle is surfaced (both members) so the caller can warn -- but nothing is
    // dropped from the returned order.
    QStringList cyclic;
    const auto ordered =
        OfflineDeploymentWorker::topologicalInstallOrder({mk("x", {"y"}), mk("y", {"x"})}, &cyclic);
    QCOMPARE(ordered.size(), 2);
    QCOMPARE(cyclic.size(), 2);
    QVERIFY(cyclic.contains(QStringLiteral("x")));
    QVERIFY(cyclic.contains(QStringLiteral("y")));
}

void TestOfflineDeploymentWorker::unmetClosureDependencies_flagsMissingIntraClosureDeps() {
    auto job = [](const char* id, const QStringList& deps) {
        BatchInternalizationJob j;
        j.package_id = QString::fromLatin1(id);
        j.dependencies = deps;
        return j;
    };

    // A complete closure: every declared dependency is present in the job set.
    QVERIFY(OfflineDeploymentWorker::unmetClosureDependencies({job("a", {"b"}), job("b", {})})
                .isEmpty());

    // The excluded Chocolatey framework is never counted as missing.
    QVERIFY(OfflineDeploymentWorker::unmetClosureDependencies({job("git.install", {"chocolatey"})})
                .isEmpty());

    // A dependency absent from the payload is flagged (self-contained guarantee broken).
    const QStringList missing =
        OfflineDeploymentWorker::unmetClosureDependencies({job("a", {"b", "c"}), job("b", {})});
    QCOMPARE(missing.size(), 1);
    QCOMPARE(missing.first(), QStringLiteral("c"));

    // A requested package re-appended to be attempted directly carries no dep edges,
    // so its own (unknown) deps never falsely trip the check.
    QVERIFY(OfflineDeploymentWorker::unmetClosureDependencies({job("direct", {})}).isEmpty());
}

void TestOfflineDeploymentWorker::isSuccessInstallExitCode_acceptsZeroAndRebootCodes() {
    QVERIFY(OfflineDeploymentWorker::isSuccessInstallExitCode(0));
    QVERIFY(OfflineDeploymentWorker::isSuccessInstallExitCode(1641));  // reboot initiated
    QVERIFY(OfflineDeploymentWorker::isSuccessInstallExitCode(3010));  // reboot required
    QVERIFY(!OfflineDeploymentWorker::isSuccessInstallExitCode(1));    // generic failure
    QVERIFY(!OfflineDeploymentWorker::isSuccessInstallExitCode(-1));
}

// R5-P7-25: the direct-download harvester used to fetch the feed .nupkg through
// downloadFileFromUrl with NO checksum, which (correctly) fails closed on an
// unverifiable installer -- so the whole harvester was dead by construction. The repair
// is not to weaken that gate but to authenticate the .nupkg against its OWN trust root:
// the hash the NuGet feed publishes. This seam is that decision.
void TestOfflineDeploymentWorker::nupkgFeedVerified_requiresAFeedPublishedHash() {
    const QByteArray body = QByteArrayLiteral("nupkg-bytes");

    // No hash from the feed means there is nothing trusted to compare against: REFUSE
    // rather than commit an unauthenticated package whose install script drives every
    // later download and whose tools/ binaries are harvested straight into the bundle.
    QVERIFY(!OfflineDeploymentWorker::nupkgFeedVerified(body, QString(), QStringLiteral("SHA512")));
    QVERIFY(!OfflineDeploymentWorker::nupkgFeedVerified(
        body, QStringLiteral("   "), QStringLiteral("SHA512")));

    // A hash the feed published under an algorithm we cannot compute is equally
    // unverifiable, so it must not pass either.
    const QString sha512 =
        QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha512).toBase64());
    QVERIFY(!OfflineDeploymentWorker::nupkgFeedVerified(body, sha512, QStringLiteral("SHA999")));
    QVERIFY(!OfflineDeploymentWorker::nupkgFeedVerified(body, sha512, QString()));
}

void TestOfflineDeploymentWorker::nupkgFeedVerified_acceptsOnlyTheMatchingFeedHash() {
    const QByteArray body = QByteArrayLiteral("nupkg-bytes");
    const QString sha512 =
        QString::fromLatin1(QCryptographicHash::hash(body, QCryptographicHash::Sha512).toBase64());

    // The feed's PackageHash is BASE64 (not hex, unlike a script-declared installer
    // checksum), so the matching form is what NuGet actually publishes.
    QVERIFY(OfflineDeploymentWorker::nupkgFeedVerified(body, sha512, QStringLiteral("SHA512")));

    // A tampered body under the same published hash is refused.
    QVERIFY(!OfflineDeploymentWorker::nupkgFeedVerified(
        QByteArrayLiteral("nupkg-bytez"), sha512, QStringLiteral("SHA512")));

    // So is a substituted hash for the genuine body.
    const QString other_hash = QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("other"), QCryptographicHash::Sha512)
            .toBase64());
    QVERIFY(
        !OfflineDeploymentWorker::nupkgFeedVerified(body, other_hash, QStringLiteral("SHA512")));

    // The installer gate is UNCHANGED and still fails closed on an absent checksum: that
    // is why the .nupkg needed its own trust root rather than a weakened downloader.
    QVERIFY(!PackageInternalizationEngine::installerVerified(body, QString(), QString()));
}

QTEST_APPLESS_MAIN(TestOfflineDeploymentWorker)
#include "test_offline_deployment_worker.moc"
