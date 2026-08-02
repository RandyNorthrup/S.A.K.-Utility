// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_nuget_dependency_resolver.cpp
/// @brief End-to-end tests for the transitive NuGet dependency resolver driven
///        by an in-memory feed (no network). Covers B10-25 (version-range
///        selection) and B10-28 part 1 (per-request state / concurrent safety).

#include "sak/nuget_dependency_resolver.h"

#include <QHash>
#include <QtTest/QtTest>

using sak::FeedPackageVersion;
using sak::NuGetDependency;
using sak::NuGetDependencyResolver;
using sak::ResolvedPackage;

namespace {

NuGetDependency dep(const char* id, const char* range = "") {
    return NuGetDependency{QString::fromLatin1(id), QString::fromLatin1(range)};
}

FeedPackageVersion fv(const char* version, const QVector<NuGetDependency>& deps = {}) {
    FeedPackageVersion f;
    f.version = QString::fromLatin1(version);
    f.dependencies = deps;
    return f;
}

using Feed = QHash<QString, QVector<FeedPackageVersion>>;

/// @brief Drive a resolver to completion against an in-memory feed keyed by
///        lowercased package id. Missing ids report a fetch failure.
QVector<ResolvedPackage> drive(NuGetDependencyResolver& r, const Feed& feed) {
    int guard = 0;
    while (!r.isComplete() && guard++ < 100'000) {
        const QString id = r.nextFetchId();
        const QString key = id.toLower();
        if (feed.contains(key)) {
            r.provideFeed(id, feed.value(key));
        } else {
            r.provideFeedFailure(id);
        }
    }
    return r.resolved();
}

QString versionOf(const QVector<ResolvedPackage>& resolved, const char* id) {
    for (const ResolvedPackage& p : resolved) {
        if (p.package_id.compare(QString::fromLatin1(id), Qt::CaseInsensitive) == 0) {
            return p.version;
        }
    }
    return {};
}

int countOf(const QVector<ResolvedPackage>& resolved, const char* id) {
    int n = 0;
    for (const ResolvedPackage& p : resolved) {
        if (p.package_id.compare(QString::fromLatin1(id), Qt::CaseInsensitive) == 0) {
            ++n;
        }
    }
    return n;
}

}  // namespace

class TestNuGetDependencyResolver : public QObject {
    Q_OBJECT

private slots:
    void resolvesLinearChain();
    void resolvesDiamondOnce();
    void handlesCycleWithoutLooping();
    void selectsHighestSatisfyingVersion();
    void pinnedRootExactMatch();
    void pinnedRootMissingVersionErrors();
    void unsatisfiableRangeSurfacesError();
    void fetchFailureSurfacesErrorButContinues();
    void depthCapDropsDeepDeps();
    void twoInstancesDoNotShareState();
    void conflictingDiamondSurfacesError();
    void compatibleDiamondPicksVersionSatisfyingBoth();
    void lateConstraintFlaggedByValidation();
    void resolvedPackageCarriesDirectDependencyIds();
    void parseDependencies_preservesRangesAndSkipsFrameworkMarkers();
    void parseODataFeedVersions_extractsVersionsAndDeps();
};

void TestNuGetDependencyResolver::resolvesLinearChain() {
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b")})};
    feed["b"] = {fv("1.0.0", {dep("c")})};
    feed["c"] = {fv("1.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(resolved.size(), 3);
    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.0.0"));
    QCOMPARE(versionOf(resolved, "b"), QStringLiteral("1.0.0"));
    QCOMPARE(versionOf(resolved, "c"), QStringLiteral("1.0.0"));
    QCOMPARE(resolved.first().package_id, QStringLiteral("a"));  // root first
    QVERIFY(r.errors().isEmpty());
}

void TestNuGetDependencyResolver::resolvesDiamondOnce() {
    // a -> b, a -> c, b -> d, c -> d. d must appear exactly once.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b"), dep("c")})};
    feed["b"] = {fv("1.0.0", {dep("d")})};
    feed["c"] = {fv("1.0.0", {dep("d")})};
    feed["d"] = {fv("1.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(resolved.size(), 4);
    QCOMPARE(countOf(resolved, "d"), 1);
    QVERIFY(r.errors().isEmpty());
}

void TestNuGetDependencyResolver::handlesCycleWithoutLooping() {
    // a -> b -> a (cycle). Must terminate; each node resolved once.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b")})};
    feed["b"] = {fv("1.0.0", {dep("a")})};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(resolved.size(), 2);
    QCOMPARE(countOf(resolved, "a"), 1);
    QCOMPARE(countOf(resolved, "b"), 1);
}

void TestNuGetDependencyResolver::selectsHighestSatisfyingVersion() {
    // a depends on c with range [2.0,3.0); feed for c offers 1.0, 2.0, 2.5, 3.0.
    // Must pick 2.5 (highest that satisfies), NOT the latest 3.0.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("c", "[2.0,3.0)")})};
    feed["c"] = {fv("1.0.0"), fv("2.0.0"), fv("2.5.0"), fv("3.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "c"), QStringLiteral("2.5.0"));
    QVERIFY(r.errors().isEmpty());
}

void TestNuGetDependencyResolver::pinnedRootExactMatch() {
    Feed feed;
    feed["a"] = {fv("1.0.0"), fv("1.2.0"), fv("2.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QStringLiteral("1.2.0"));  // pin exact
    const auto resolved = drive(r, feed);

    QCOMPARE(resolved.size(), 1);
    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.2.0"));
}

void TestNuGetDependencyResolver::pinnedRootMissingVersionErrors() {
    Feed feed;
    feed["a"] = {fv("1.0.0"), fv("2.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QStringLiteral("9.9.9"));  // not available
    const auto resolved = drive(r, feed);

    QVERIFY(resolved.isEmpty());
    QVERIFY(!r.errors().isEmpty());
}

void TestNuGetDependencyResolver::unsatisfiableRangeSurfacesError() {
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("c", "[5.0,)")})};
    feed["c"] = {fv("1.0.0"), fv("2.0.0")};  // nothing >= 5.0

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.0.0"));
    QVERIFY(versionOf(resolved, "c").isEmpty());  // unsatisfiable -> not resolved
    QVERIFY(!r.errors().isEmpty());
}

void TestNuGetDependencyResolver::fetchFailureSurfacesErrorButContinues() {
    // b is missing from the feed; a and c must still resolve.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b"), dep("c")})};
    feed["c"] = {fv("1.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.0.0"));
    QCOMPARE(versionOf(resolved, "c"), QStringLiteral("1.0.0"));
    QVERIFY(versionOf(resolved, "b").isEmpty());
    QVERIFY(!r.errors().isEmpty());
}

void TestNuGetDependencyResolver::depthCapDropsDeepDeps() {
    // Chain longer than the depth cap: only the first (cap) levels resolve.
    Feed feed;
    feed["l0"] = {fv("1.0.0", {dep("l1")})};
    feed["l1"] = {fv("1.0.0", {dep("l2")})};
    feed["l2"] = {fv("1.0.0", {dep("l3")})};
    feed["l3"] = {fv("1.0.0")};

    NuGetDependencyResolver r(/*max_depth=*/2);  // allow depth 0 and 1 only
    r.start(QStringLiteral("l0"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "l0"), QStringLiteral("1.0.0"));  // depth 0
    QCOMPARE(versionOf(resolved, "l1"), QStringLiteral("1.0.0"));  // depth 1
    QVERIFY(versionOf(resolved, "l2").isEmpty());                  // depth 2 dropped
    QVERIFY(!r.errors().isEmpty());
}

void TestNuGetDependencyResolver::twoInstancesDoNotShareState() {
    // Per-request state (B10-28): two resolvers running "concurrently" (interleaved
    // stepping) must not clobber each other's queue/visited/resolved sets.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("shared")})};
    feed["x"] = {fv("1.0.0", {dep("shared")})};
    feed["shared"] = {fv("1.0.0")};

    NuGetDependencyResolver r1;
    NuGetDependencyResolver r2;
    r1.start(QStringLiteral("a"), QString());
    r2.start(QStringLiteral("x"), QString());

    // Interleave the two resolutions step by step.
    int guard = 0;
    while ((!r1.isComplete() || !r2.isComplete()) && guard++ < 10'000) {
        for (NuGetDependencyResolver* r : {&r1, &r2}) {
            if (r->isComplete()) {
                continue;
            }
            const QString id = r->nextFetchId();
            r->provideFeed(id, feed.value(id.toLower()));
        }
    }

    QCOMPARE(r1.resolved().size(), 2);  // a + shared
    QCOMPARE(r2.resolved().size(), 2);  // x + shared
    QCOMPARE(versionOf(r1.resolved(), "a"), QStringLiteral("1.0.0"));
    QCOMPARE(versionOf(r2.resolved(), "x"), QStringLiteral("1.0.0"));
    QCOMPARE(countOf(r1.resolved(), "shared"), 1);
    QCOMPARE(countOf(r2.resolved(), "shared"), 1);
}

void TestNuGetDependencyResolver::conflictingDiamondSurfacesError() {
    // a -> b -> d[1.0.0 exact]; a -> c -> d[2.0.0 exact]. No single d satisfies
    // both edges: d must NOT be resolved, and the conflict must be surfaced (not
    // silently deduped to one edge's choice with an empty errors()).
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b"), dep("c")})};
    feed["b"] = {fv("1.0.0", {dep("d", "[1.0.0]")})};
    feed["c"] = {fv("1.0.0", {dep("d", "[2.0.0]")})};
    feed["d"] = {fv("1.0.0"), fv("2.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QVERIFY(versionOf(resolved, "d").isEmpty());  // no version satisfies both edges
    QVERIFY(!r.errors().isEmpty());               // the conflict is surfaced
}

void TestNuGetDependencyResolver::compatibleDiamondPicksVersionSatisfyingBoth() {
    // a -> b -> e[>=1.0]; a -> c -> e[>=1.5]. e has 1.0/1.5/2.0. Both edges honored
    // -> highest satisfying BOTH is 2.0.0, and no error is raised.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b"), dep("c")})};
    feed["b"] = {fv("1.0.0", {dep("e", "[1.0,)")})};
    feed["c"] = {fv("1.0.0", {dep("e", "[1.5,)")})};
    feed["e"] = {fv("1.0.0"), fv("1.5.0"), fv("2.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "e"), QStringLiteral("2.0.0"));
    QVERIFY(r.errors().isEmpty());
}

void TestNuGetDependencyResolver::lateConstraintFlaggedByValidation() {
    // d is resolved EARLY (direct dep, only 1.0.0 available) before a deeper edge
    // e -> d[2.0.0] is discovered. The final validation pass must still flag that
    // the already-selected d@1.0.0 violates the later [2.0.0] constraint.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("d"), dep("b")})};  // d listed first -> resolved first
    feed["d"] = {fv("1.0.0")};                        // only 1.0.0 exists
    feed["b"] = {fv("1.0.0", {dep("e")})};
    feed["e"] = {fv("1.0.0", {dep("d", "[2.0.0]")})};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "d"), QStringLiteral("1.0.0"));  // kept (over-include-safe)
    QVERIFY(!r.errors().isEmpty());                               // but the conflict is surfaced
}

void TestNuGetDependencyResolver::resolvedPackageCarriesDirectDependencyIds() {
    // Each ResolvedPackage records its chosen version's DIRECT dependency ids, so
    // the bundle manifest can carry real dependency provenance.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("b"), dep("c")})};
    feed["b"] = {fv("1.0.0")};
    feed["c"] = {fv("1.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    drive(r, feed);

    QStringList a_deps;
    for (const ResolvedPackage& p : r.resolved()) {
        if (p.package_id == QLatin1String("a")) {
            a_deps = p.dependencies;
        }
    }
    QCOMPARE(a_deps.size(), 2);
    QVERIFY(a_deps.contains(QStringLiteral("b")));
    QVERIFY(a_deps.contains(QStringLiteral("c")));
}

void TestNuGetDependencyResolver::parseDependencies_preservesRangesAndSkipsFrameworkMarkers() {
    const auto deps = NuGetDependencyResolver::parseDependencies(
        QStringLiteral("chocolatey-core.extension:1.3.1|dotnetfx:[4.8, ):net45|::net45"));
    QCOMPARE(deps.size(), 2);
    QCOMPARE(deps.at(0).id, QStringLiteral("chocolatey-core.extension"));
    QCOMPARE(deps.at(0).version_range, QStringLiteral("1.3.1"));
    QCOMPARE(deps.at(1).id, QStringLiteral("dotnetfx"));
    QCOMPARE(deps.at(1).version_range,
             QStringLiteral("[4.8, )"));  // range preserved, framework stripped

    QVERIFY(NuGetDependencyResolver::parseDependencies(QString()).isEmpty());
    QVERIFY(NuGetDependencyResolver::parseDependencies(QStringLiteral("::net45")).isEmpty());
}

void TestNuGetDependencyResolver::parseODataFeedVersions_extractsVersionsAndDeps() {
    const QByteArray xml =
        "<feed xmlns:d=\"d\" xmlns:m=\"m\">"
        "<entry><m:properties>"
        "<d:Version>1.2.0</d:Version>"
        "<d:Dependencies>libfoo:[1.0, ):net45|::net45</d:Dependencies>"
        "</m:properties></entry>"
        "<entry><m:properties>"
        "<d:Version>2.0.0</d:Version>"
        "<d:Dependencies></d:Dependencies>"
        "</m:properties></entry>"
        "</feed>";
    const auto versions = NuGetDependencyResolver::parseODataFeedVersions(xml);
    QCOMPARE(versions.size(), 2);
    QCOMPARE(versions.at(0).version, QStringLiteral("1.2.0"));
    QCOMPARE(versions.at(0).dependencies.size(), 1);
    QCOMPARE(versions.at(0).dependencies.at(0).id, QStringLiteral("libfoo"));
    QCOMPARE(versions.at(0).dependencies.at(0).version_range, QStringLiteral("[1.0, )"));
    QCOMPARE(versions.at(1).version, QStringLiteral("2.0.0"));
    QVERIFY(versions.at(1).dependencies.isEmpty());
}

QTEST_APPLESS_MAIN(TestNuGetDependencyResolver)
#include "test_nuget_dependency_resolver.moc"
