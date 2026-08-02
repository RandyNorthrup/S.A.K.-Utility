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
    void safeInstallerFilename_keepsPlainNames();
    void safeInstallerFilename_confinesTraversalNames();
    void sanitizeManifestFilename_rejectsPathsAndTraversal();
    void verifyBundledPackage_acceptsMatchingChecksumAndSize();
    void verifyBundledPackage_rejectsMismatchMissingAndBadName();
    void installDispositionFor_skipsNotPackedOnlyUnderAirGap();
    void collectInstallerUrls_collectsEveryResourceDeduped();
    void uniqueFilename_disambiguatesCollidingBasenames();
    void isChocolateyFrameworkId_matchesOnlyTheFrameworkPackage();
    void isSafeInstallToken_rejectsOptionLikeAndBlankTokens();
    void topologicalInstallOrder_installsDepsBeforeDependents();
    void installContextForMode_bundleLocalListFeed();
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

    // A checksum-less entry with a matching size still passes.
    DeploymentManifestEntry no_sum = entry;
    no_sum.checksum.clear();
    QString err2;
    QVERIFY(OfflineDeploymentWorker::verifyBundledPackage(no_sum, dir.path(), err2));
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

void TestOfflineDeploymentWorker::collectInstallerUrls_collectsEveryResourceDeduped() {
    ParsedInstallScript parsed;
    DownloadResource a;
    a.url = QStringLiteral("https://host/a32.exe");
    a.url_64bit = QStringLiteral("https://host/a64.exe");
    DownloadResource b;
    b.url = QStringLiteral("https://host/b.msi");
    b.url_64bit = QStringLiteral("https://host/a64.exe");  // duplicate of a's 64-bit URL
    parsed.resources = {a, b};

    const QStringList urls = OfflineDeploymentWorker::collectInstallerUrls(parsed);
    // Every DISTINCT installer URL across ALL resources -- not just the first.
    QCOMPARE(urls.size(), 3);
    QVERIFY(urls.contains(QStringLiteral("https://host/a32.exe")));
    QVERIFY(urls.contains(QStringLiteral("https://host/a64.exe")));
    QVERIFY(urls.contains(QStringLiteral("https://host/b.msi")));

    // Empty parse -> no urls.
    QVERIFY(OfflineDeploymentWorker::collectInstallerUrls(ParsedInstallScript{}).isEmpty());
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

QTEST_APPLESS_MAIN(TestOfflineDeploymentWorker)
#include "test_offline_deployment_worker.moc"
