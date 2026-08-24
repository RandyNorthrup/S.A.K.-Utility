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
    void lateConstraintReselectsResolvableDiamond();
    void feedResponseForWrongIdIsIgnored();
    void parseDependencies_keepsPerFrameworkRangesForSameId();
    void resolvedPackageCarriesDirectDependencyIds();
    void parseDependencies_preservesRangesAndSkipsFrameworkMarkers();
    void parseODataFeedVersions_extractsVersionsAndDeps();
    void prereleaseExcludedForPlainRange();
    void duplicateRootDifferentVersionWarns();
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
    // resolved() is contracted as "root first, then in discovery order", and the work
    // queue is FIFO breadth-first -- pin the ORDERED catalog, not just membership.
    QStringList ordered_ids;
    for (const ResolvedPackage& p : resolved) {
        ordered_ids.append(p.package_id);
    }
    QCOMPARE(
        ordered_ids,
        QStringList(
            {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c"), QStringLiteral("d")}));
    // Each node's REAL graph level (d is reached via b, so depth 2). depth is the
    // budget a later reselection re-enqueues its replacement dependencies with, so a
    // flattened depth silently defeats the depth cap for a corrected subtree.
    QCOMPARE(resolved.at(0).depth, 0);
    QCOMPARE(resolved.at(1).depth, 1);
    QCOMPARE(resolved.at(2).depth, 1);
    QCOMPARE(resolved.at(3).depth, 2);
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

    // The test is NAMED for termination and never asserted it: drive() has its own iteration
    // guard, so a resolver that looped forever would be cut off by the harness and still produce
    // this result set. isComplete() proves the resolver terminated on its own.
    QVERIFY(r.isComplete());
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
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(),
             QStringLiteral("No available version of a satisfies range '[9.9.9]'"));
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
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(),
             QStringLiteral("No available version of c satisfies range '[5.0,)'"));
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
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(), QStringLiteral("Failed to fetch dependency feed for b"));
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
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(),
             QStringLiteral("Dependency depth cap (2) reached; deeper deps dropped"));
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
    QCOMPARE(r.errors().size(), 1);               // the conflict is surfaced
    QCOMPARE(r.errors().first(),
             QStringLiteral("No available version of d satisfies range '[1.0.0]'"));
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
    QCOMPARE(r.errors().size(), 1);                               // but the conflict is surfaced
    QCOMPARE(r.errors().first(),
             QStringLiteral("Version conflict: selected d 1.0.0 does not satisfy a required "
                            "range '[2.0.0]' declared by another package"));
}

void TestNuGetDependencyResolver::lateConstraintReselectsResolvableDiamond() {
    // d is resolved EARLY via a plain edge -> picks the highest, 2.0.0. A deeper
    // edge e -> d[1.0.0] is discovered LATER. Because d@2.0.0 violates it but a
    // satisfying version (1.0.0) IS available, the resolver must RE-SELECT d down
    // to 1.0.0 (honoring both edges) instead of merely erroring after the fact.
    Feed feed;
    feed["a"] = {fv("1.0.0", {dep("d"), dep("b")})};   // d first -> resolved first (plain)
    feed["d"] = {fv("1.0.0"), fv("2.0.0")};            // plain edge would pick 2.0.0
    feed["b"] = {fv("1.0.0", {dep("e")})};
    feed["e"] = {fv("1.0.0", {dep("d", "[1.0.0]")})};  // late exact pin to 1.0.0

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "d"), QStringLiteral("1.0.0"));  // re-selected down
    QCOMPARE(countOf(resolved, "d"), 1);
    QVERIFY(r.errors().isEmpty());  // resolvable -> corrected, not an error

    // The SUPERSEDED version's own edges must be withdrawn, not left as stale state.
    // Here d@2.0.0 -- the pick later corrected down to 1.0.0 -- is the ONLY package
    // that ever demanded z, so on reselection z must be dropped from the pending
    // queue and must never enter the closure.
    Feed feed2;
    feed2["a"] = {fv("1.0.0", {dep("b"), dep("d")})};  // b first -> e's edge lands late
    feed2["d"] = {fv("1.0.0"), fv("2.0.0", {dep("z")})};
    feed2["b"] = {fv("1.0.0", {dep("e")})};
    feed2["e"] = {fv("1.0.0", {dep("d", "[1.0.0]")})};
    feed2["z"] = {fv("1.0.0")};  // resolvable if (wrongly) still demanded

    NuGetDependencyResolver r2;
    r2.start(QStringLiteral("a"), QString());
    const auto resolved2 = drive(r2, feed2);

    QCOMPARE(versionOf(resolved2, "d"), QStringLiteral("1.0.0"));
    QVERIFY(versionOf(resolved2, "z").isEmpty());  // demanded ONLY by the dropped d@2.0.0
    QStringList ids2;
    for (const ResolvedPackage& p : resolved2) {
        ids2.append(p.package_id);
    }
    QCOMPARE(
        ids2,
        QStringList(
            {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("d"), QStringLiteral("e")}));
    QVERIFY(r2.errors().isEmpty());
}

void TestNuGetDependencyResolver::feedResponseForWrongIdIsIgnored() {
    // A feed response whose id does not match ANY pending fetch must be dropped,
    // never applied to the queue head -- otherwise one package would be resolved
    // to another package's versions (a wrong/hostile substitution).
    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    QCOMPARE(r.nextFetchId(), QStringLiteral("a"));

    // Answer for "b" while "a" is pending: must NOT resolve "a" to b's versions.
    r.provideFeed(QStringLiteral("b"), {fv("9.9.9")});
    QVERIFY(r.resolved().isEmpty());
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(),
             QStringLiteral("Ignored feed response for 'b': no pending fetch has that id"));
    QCOMPARE(r.nextFetchId(), QStringLiteral("a"));  // "a" is still pending

    // The correct response now resolves "a" to its own version.
    r.provideFeed(QStringLiteral("a"), {fv("1.0.0")});
    QCOMPARE(versionOf(r.resolved(), "a"), QStringLiteral("1.0.0"));
}

void TestNuGetDependencyResolver::parseDependencies_keepsPerFrameworkRangesForSameId() {
    // The same id can recur with a DIFFERENT range per target framework. Both
    // distinct ranges must be preserved (deduping by id alone would silently drop
    // the second constraint); an exact duplicate (id+range) is still collapsed.
    const auto deps = NuGetDependencyResolver::parseDependencies(
        QStringLiteral("newtonsoft.json:[9.0,):net45|newtonsoft.json:[10.0,):netstandard1.3|"
                       "newtonsoft.json:[9.0,):net46"));
    QCOMPARE(deps.size(), 2);  // [9.0,) and [10.0,) kept; the repeated [9.0,) dropped
    QCOMPARE(deps.at(0).version_range, QStringLiteral("[9.0,)"));
    QCOMPARE(deps.at(1).version_range, QStringLiteral("[10.0,)"));
    for (const NuGetDependency& d : deps) {
        QCOMPARE(d.id, QStringLiteral("newtonsoft.json"));
    }
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
    // dependencies preserve feed-declaration order (a plain QVector append, not QHash
    // iteration), so pin the exact ordered list, not just set membership.
    QCOMPARE(a_deps, QStringList({QStringLiteral("b"), QStringLiteral("c")}));
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

void TestNuGetDependencyResolver::prereleaseExcludedForPlainRange() {
    // A plain (any) range must select the highest STABLE, never a higher
    // prerelease -- matching NuGet/Chocolatey default resolution.
    Feed feed;
    feed["a"] = {fv("1.9.0"), fv("2.0.0-beta")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QString());
    const auto resolved = drive(r, feed);

    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.9.0"));
    QCOMPARE(resolved.size(), 1);
    QVERIFY(r.errors().isEmpty());

    // The OPT-IN arm of the same guard: when a relevant range's bound is ITSELF a
    // prerelease, prerelease candidates ARE eligible, so the higher 2.0.0-beta wins
    // over the stable 1.9.0 that also satisfies the range.
    Feed pre_feed;
    pre_feed["r"] = {fv("1.0.0", {dep("a", "[1.0.0-beta,)")})};
    pre_feed["a"] = {fv("1.9.0"), fv("2.0.0-beta")};

    NuGetDependencyResolver r2;
    r2.start(QStringLiteral("r"), QString());
    const auto pre_resolved = drive(r2, pre_feed);

    QCOMPARE(versionOf(pre_resolved, "a"), QStringLiteral("2.0.0-beta"));
    QVERIFY(r2.errors().isEmpty());
}

void TestNuGetDependencyResolver::duplicateRootDifferentVersionWarns() {
    // Two roots for the same id at different pins: one version is scheduled (the
    // first), and the dropped pin is surfaced as a warning rather than silently
    // discarded.
    Feed feed;
    feed["a"] = {fv("1.0.0"), fv("2.0.0")};

    NuGetDependencyResolver r;
    r.start(QStringLiteral("a"), QStringLiteral("1.0.0"));
    r.addRoot(QStringLiteral("a"), QStringLiteral("2.0.0"));
    const auto resolved = drive(r, feed);

    QCOMPARE(countOf(resolved, "a"), 1);
    QCOMPARE(versionOf(resolved, "a"), QStringLiteral("1.0.0"));
    QCOMPARE(r.errors().size(), 1);
    QCOMPARE(r.errors().first(),
             QStringLiteral("Package a requested at multiple versions; keeping the first and "
                            "ignoring the additional pin 2.0.0"));
    // The SUPPRESSION arm: re-stating a pin that is ALREADY an active constraint is
    // not a conflict and must add no second warning.
    r.addRoot(QStringLiteral("a"), QStringLiteral("1.0.0"));
    QCOMPARE(r.errors().size(), 1);
}

QTEST_APPLESS_MAIN(TestNuGetDependencyResolver)
#include "test_nuget_dependency_resolver.moc"
