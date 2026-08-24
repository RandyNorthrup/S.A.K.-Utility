// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_nuget_version_range.cpp
/// @brief Unit tests for the pure NuGetVersion / NuGetVersionRange primitives
///        (B10-25: honor NuGet dependency version ranges, pick highest-satisfying).

#include "sak/nuget_version_range.h"

#include <QtTest/QtTest>

using sak::NuGetVersion;
using sak::NuGetVersionRange;

namespace {
NuGetVersion v(const char* s) {
    const auto parsed = NuGetVersion::parse(QString::fromLatin1(s));
    // Q_ASSERT compiles to nothing under QT_NO_DEBUG, which is the configuration the gate
    // builds -- so this guard evaluated nothing and `return *parsed` dereferenced an empty
    // optional. Every fixture in this file flows through here.
    if (!parsed.has_value()) {
        qFatal("NuGetVersion::parse rejected the fixture version: %s", s);
    }
    return *parsed;
}
}  // namespace

class TestNuGetVersionRange : public QObject {
    Q_OBJECT

private slots:
    // ---- NuGetVersion::parse -------------------------------------------------
    void parse_acceptsReleaseAndPrerelease();
    void parse_rejectsEmptyAndNonNumericRelease();
    void parse_ignoresBuildMetadata();

    // ---- NuGetVersion::compare ----------------------------------------------
    void compare_ordersReleaseNumericallyWithZeroPadding();
    void compare_prereleaseSortsBelowRelease();
    void compare_prereleaseIdentifierRules();
    void compare_prereleaseNumericArbitraryPrecision();

    // ---- NuGetVersionRange::parse + satisfies -------------------------------
    void range_bareVersionIsMinimumInclusive();
    void range_exactBracket();
    void range_halfOpenLowerUpper();
    void range_closedAndExclusiveIntervals();
    void range_emptyIsPermissive();
    void range_malformedRejectsAll();
    void range_invalidVersionNeverSatisfies();
    void parse_rejectsEmptyPrereleaseIdentifiers();

    // ---- selectHighestSatisfying --------------------------------------------
    void select_picksHighestInRange();
    void select_noneQualifyReturnsNullopt();
    void select_prefersStableOverPrereleaseAtEqualPrecedence();
    void select_excludesPrereleaseUnlessRangeTargetsIt();
};

void TestNuGetVersionRange::parse_acceptsReleaseAndPrerelease() {
    const auto a = NuGetVersion::parse(QStringLiteral("1.2.3"));
    QVERIFY(a.has_value());
    QVERIFY(a->isValid());
    QVERIFY(!a->isPrerelease());

    const auto b = NuGetVersion::parse(QStringLiteral("1.2.3-beta.1"));
    QVERIFY(b.has_value());
    QVERIFY(b->isPrerelease());
    QCOMPARE(b->original(), QStringLiteral("1.2.3-beta.1"));

    // NuGet 4-part release. A truncating parse still has_value(), so the revision has to be
    // shown to participate in precedence; a fifth component is rejected outright.
    const auto quad = NuGetVersion::parse(QStringLiteral("4.8.0.1"));
    QVERIFY(quad.has_value());
    QVERIFY(*quad > v("4.8.0"));
    QVERIFY(*quad < v("4.8.0.2"));
    QVERIFY(!NuGetVersion::parse(QStringLiteral("4.8.0.1.2")).has_value());
}

void TestNuGetVersionRange::parse_rejectsEmptyAndNonNumericRelease() {
    QVERIFY(!NuGetVersion::parse(QString()).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("   ")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("abc")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.x.3")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1..3")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("-beta")).has_value());
    // Only the empty and non-numeric halves of the refusal were exercised. A release component
    // wider than int32 is a malformed feed token, not a version -- and the boundary below it
    // must still parse.
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.0.2147483648")).has_value());
    QVERIFY(NuGetVersion::parse(QStringLiteral("1.0.2147483647")).has_value());
}

void TestNuGetVersionRange::parse_ignoresBuildMetadata() {
    // Build metadata does not affect precedence.
    QCOMPARE(v("1.0.0+build5").compare(v("1.0.0")), 0);
    QCOMPARE(v("1.0.0+a").compare(v("1.0.0+b")), 0);
    // Metadata is ignored for precedence but must still be WELL FORMED: an empty or
    // non-identifier metadata run is a malformed version, not a plain release. Deleting the
    // validation and keeping only the strip passes both comparisons above.
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.0.0+")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.0.0+a..b")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.0.0+a+b")).has_value());
}

void TestNuGetVersionRange::compare_ordersReleaseNumericallyWithZeroPadding() {
    QVERIFY(v("1.0") < v("1.0.1"));   // missing trailing == 0
    QVERIFY(v("1.0.0") == v("1.0"));  // zero-pad equal
    QVERIFY(v("2.0") > v("1.9.9"));
    QVERIFY(v("1.10") > v("1.9"));    // numeric, not lexical
    QVERIFY(v("10.0") > v("9.99.99"));
}

void TestNuGetVersionRange::compare_prereleaseSortsBelowRelease() {
    QVERIFY(v("1.0.0-beta") < v("1.0.0"));
    QVERIFY(v("1.0.0") > v("1.0.0-rc.1"));
    QVERIFY(v("1.0.0-alpha") < v("1.0.0-beta"));
}

void TestNuGetVersionRange::compare_prereleaseIdentifierRules() {
    // Numeric identifiers compare numerically and rank below alphanumeric.
    QVERIFY(v("1.0.0-2") < v("1.0.0-10"));
    QVERIFY(v("1.0.0-1") < v("1.0.0-alpha"));
    // A longer prerelease with a shared prefix has higher precedence.
    QVERIFY(v("1.0.0-alpha") < v("1.0.0-alpha.1"));
    // Case-insensitive identifier comparison.
    QCOMPARE(v("1.0.0-Beta").compare(v("1.0.0-beta")), 0);
}

void TestNuGetVersionRange::compare_prereleaseNumericArbitraryPrecision() {
    // Numeric prerelease identifiers compare by VALUE even beyond int64 (the old
    // toLongLong path overflowed and fell back to a lexical, inverted order).
    QVERIFY(v("1.0.0-10000000000000000000") > v("1.0.0-9999999999999999999"));
    QVERIFY(v("1.0.0-2") < v("1.0.0-10"));  // numeric, not lexical
    // Leading zeros do not change a numeric identifier's value.
    QCOMPARE(v("1.0.0-007").compare(v("1.0.0-7")), 0);
    // An all-digit identifier still ranks below an alphanumeric one.
    QVERIFY(v("1.0.0-123") < v("1.0.0-alpha"));
}

void TestNuGetVersionRange::range_bareVersionIsMinimumInclusive() {
    const auto r = NuGetVersionRange::parse(QStringLiteral("1.0"));
    QVERIFY(!r.isAny());
    QVERIFY(r.satisfies(v("1.0")));   // inclusive at the minimum
    QVERIFY(r.satisfies(v("2.5")));   // anything above
    QVERIFY(!r.satisfies(v("0.9")));  // below the minimum
}

void TestNuGetVersionRange::range_exactBracket() {
    const auto r = NuGetVersionRange::parse(QStringLiteral("[1.2.0]"));
    QVERIFY(r.satisfies(v("1.2.0")));
    QVERIFY(!r.satisfies(v("1.2.1")));
    QVERIFY(!r.satisfies(v("1.1.9")));
}

void TestNuGetVersionRange::range_halfOpenLowerUpper() {
    const auto ge = NuGetVersionRange::parse(QStringLiteral("[1.0,)"));
    QVERIFY(ge.satisfies(v("1.0")));
    QVERIFY(ge.satisfies(v("9.9")));
    QVERIFY(!ge.satisfies(v("0.5")));

    const auto gt = NuGetVersionRange::parse(QStringLiteral("(1.0,)"));
    QVERIFY(!gt.satisfies(v("1.0")));  // exclusive lower
    QVERIFY(gt.satisfies(v("1.0.1")));

    const auto le = NuGetVersionRange::parse(QStringLiteral("(,2.0]"));
    QVERIFY(le.satisfies(v("2.0")));
    QVERIFY(le.satisfies(v("0.1")));
    QVERIFY(!le.satisfies(v("2.0.1")));

    const auto lt = NuGetVersionRange::parse(QStringLiteral("(,2.0)"));
    QVERIFY(!lt.satisfies(v("2.0")));  // exclusive upper
    QVERIFY(lt.satisfies(v("1.9.9")));
}

void TestNuGetVersionRange::range_closedAndExclusiveIntervals() {
    const auto closed = NuGetVersionRange::parse(QStringLiteral("[1.0,2.0]"));
    QVERIFY(closed.satisfies(v("1.0")));
    QVERIFY(closed.satisfies(v("2.0")));
    QVERIFY(closed.satisfies(v("1.5")));
    QVERIFY(!closed.satisfies(v("2.0.1")));

    const auto half = NuGetVersionRange::parse(QStringLiteral("[1.0,2.0)"));
    QVERIFY(half.satisfies(v("1.0")));
    QVERIFY(!half.satisfies(v("2.0")));  // upper exclusive
    QVERIFY(half.satisfies(v("1.9.9")));

    const auto open = NuGetVersionRange::parse(QStringLiteral("(1.0,2.0)"));
    QVERIFY(!open.satisfies(v("1.0")));
    QVERIFY(!open.satisfies(v("2.0")));
    QVERIFY(open.satisfies(v("1.5")));
}

void TestNuGetVersionRange::range_emptyIsPermissive() {
    // An empty/whitespace range means "the feed omitted a range" -> any version.
    for (const char* s : {"", "   "}) {
        const auto r = NuGetVersionRange::parse(QString::fromLatin1(s));
        QVERIFY2(r.isValid(), QByteArray("expected valid for: ").append(s).constData());
        QVERIFY(r.isAny());
        QVERIFY(r.satisfies(v("1.0")));
        QVERIFY(r.satisfies(v("99.0")));
    }
}

void TestNuGetVersionRange::range_malformedRejectsAll() {
    // A NON-EMPTY string that does not parse is fail-closed: it is NOT permissive
    // and satisfies NOTHING (a garbled constraint must never match everything and
    // silently pull in an arbitrary version).
    // "[1.0,2.0x" isolates the close-char guard (the existing "[bad"/"(1.0" are still refused
    // downstream without it, so it was never actually tested), and "[x,2.0)" isolates the LOWER
    // half of the interval bound check -- only the upper half was.
    for (const char* s : {"not-a-range",
                          "[bad",
                          "(1.0",
                          "[]",
                          "(1.0)",
                          "[1.0,x)",
                          "1.0.x",
                          "[1.0,2.0x",
                          "[x,2.0)"}) {
        const auto r = NuGetVersionRange::parse(QString::fromLatin1(s));
        QVERIFY2(!r.isValid(), QByteArray("expected invalid for: ").append(s).constData());
        QVERIFY(!r.isAny());
        QVERIFY2(!r.satisfies(v("1.0")), QByteArray("must reject 1.0 for: ").append(s).constData());
        QVERIFY(!r.satisfies(v("99.0")));
        QVERIFY(!r.selectHighestSatisfying({v("1.0"), v("99.0")}).has_value());
    }
}

void TestNuGetVersionRange::parse_rejectsEmptyPrereleaseIdentifiers() {
    // SemVer forbids an empty prerelease identifier; a malformed separator must
    // fail the parse rather than be silently swallowed (SkipEmptyParts would hide
    // "alpha..1", ".beta", "rc." and misorder the version).
    // The refusal is a two-part predicate -- empty identifier OR a byte outside [0-9A-Za-z-] --
    // and only the empty half was probed, though the production comment names these two.
    for (const char* s :
         {"1.0.0-", "1.0.0-.beta", "1.0.0-alpha..1", "1.0.0-rc.", "1.0.0-alpha_1", "1.0.0-al?ha"}) {
        QVERIFY2(!NuGetVersion::parse(QString::fromLatin1(s)).has_value(),
                 QByteArray("expected parse failure for: ").append(s).constData());
    }
    // A well-formed multi-identifier prerelease still parses.
    QVERIFY(NuGetVersion::parse(QStringLiteral("1.0.0-alpha.1")).has_value());
}

void TestNuGetVersionRange::range_invalidVersionNeverSatisfies() {
    NuGetVersion invalid;  // default-constructed, not valid
    QVERIFY(!invalid.isValid());
    // The invalid version must sort strictly BELOW a parsed "0" rather than zero-padding into
    // equality with it. satisfies() alone cannot see this -- its own isValid guard fires first.
    QVERIFY(invalid < v("0"));
    QVERIFY(v("0") > invalid);
    QVERIFY(!(invalid == v("0")));
    QVERIFY(!NuGetVersionRange::parse(QStringLiteral("[1.0,)")).satisfies(invalid));
    QVERIFY(!NuGetVersionRange::parse(QString()).satisfies(invalid));  // even a permissive range
}

void TestNuGetVersionRange::select_picksHighestInRange() {
    const QVector<NuGetVersion> avail{v("1.0.0"), v("1.5.0"), v("2.0.0"), v("2.5.0")};
    const auto r = NuGetVersionRange::parse(QStringLiteral("[1.0,2.0]"));
    const auto best = r.selectHighestSatisfying(avail);
    QVERIFY(best.has_value());
    QCOMPARE(best->original(), QStringLiteral("2.0.0"));  // 2.5 is out of range
}

void TestNuGetVersionRange::select_noneQualifyReturnsNullopt() {
    const QVector<NuGetVersion> avail{v("1.0.0"), v("1.1.0")};
    const auto r = NuGetVersionRange::parse(QStringLiteral("[2.0,)"));
    QVERIFY(!r.selectHighestSatisfying(avail).has_value());
    QVERIFY(!r.selectHighestSatisfying(QVector<NuGetVersion>{}).has_value());
}

void TestNuGetVersionRange::select_prefersStableOverPrereleaseAtEqualPrecedence() {
    // 2.0.0 (stable) and 2.0.0-rc both "present"; stable wins. The lower bound must itself be a
    // PRERELEASE, or prereleases are excluded from the candidate set before the preference is
    // ever consulted -- under the old stable "[1.0,)" bound the rc was merely filtered out, so
    // this test never exercised the tie-break it is named for. Both input orders, so the answer
    // cannot come from iteration order.
    const QVector<NuGetVersion> avail{v("2.0.0-rc.1"), v("2.0.0"), v("1.9.0")};
    const auto r = NuGetVersionRange::parse(QStringLiteral("[1.0.0-alpha,)"));
    const auto best = r.selectHighestSatisfying(avail);
    QVERIFY(best.has_value());
    QCOMPARE(best->original(), QStringLiteral("2.0.0"));
    QVERIFY(!best->isPrerelease());
    const QVector<NuGetVersion> reversed{v("1.9.0"), v("2.0.0"), v("2.0.0-rc.1")};
    const auto best_reversed = r.selectHighestSatisfying(reversed);
    QVERIFY(best_reversed.has_value());
    QCOMPARE(best_reversed->original(), QStringLiteral("2.0.0"));
}

void TestNuGetVersionRange::select_excludesPrereleaseUnlessRangeTargetsIt() {
    // A plain range must NOT pick a higher prerelease over a satisfying stable
    // (NuGet default: prerelease is excluded). 2.0.0-beta is higher-precedence
    // than 1.9.0 yet must lose.
    const QVector<NuGetVersion> avail{v("2.0.0-beta"), v("1.9.0")};
    const auto plain =
        NuGetVersionRange::parse(QStringLiteral("[1.0,)")).selectHighestSatisfying(avail);
    QVERIFY(plain.has_value());
    QCOMPARE(plain->original(), QStringLiteral("1.9.0"));

    // When a bound is itself a prerelease, prerelease candidates ARE eligible.
    const auto targeted =
        NuGetVersionRange::parse(QStringLiteral("[2.0.0-alpha,)")).selectHighestSatisfying(avail);
    QVERIFY(targeted.has_value());
    QCOMPARE(targeted->original(), QStringLiteral("2.0.0-beta"));

    // A plain range with ONLY a prerelease available qualifies nothing.
    const QVector<NuGetVersion> only_pre{v("2.0.0-beta")};
    QVERIFY(!NuGetVersionRange::parse(QStringLiteral("[1.0,)"))
                 .selectHighestSatisfying(only_pre)
                 .has_value());
}

QTEST_APPLESS_MAIN(TestNuGetVersionRange)
#include "test_nuget_version_range.moc"
