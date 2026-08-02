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
    Q_ASSERT(parsed.has_value());
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
    void range_emptyAndMalformedArePermissive();
    void range_invalidVersionNeverSatisfies();

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

    // NuGet 4-part release.
    QVERIFY(NuGetVersion::parse(QStringLiteral("4.8.0.1")).has_value());
}

void TestNuGetVersionRange::parse_rejectsEmptyAndNonNumericRelease() {
    QVERIFY(!NuGetVersion::parse(QString()).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("   ")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("abc")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1.x.3")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("1..3")).has_value());
    QVERIFY(!NuGetVersion::parse(QStringLiteral("-beta")).has_value());
}

void TestNuGetVersionRange::parse_ignoresBuildMetadata() {
    // Build metadata does not affect precedence.
    QCOMPARE(v("1.0.0+build5").compare(v("1.0.0")), 0);
    QCOMPARE(v("1.0.0+a").compare(v("1.0.0+b")), 0);
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

void TestNuGetVersionRange::range_emptyAndMalformedArePermissive() {
    // Empty, unparseable, or malformed-bracket ranges accept everything so a
    // needed dependency is never dropped (over-include is safe).
    for (const char* s : {"", "   ", "not-a-range", "[bad", "(1.0", "[]", "(1.0)"}) {
        const auto r = NuGetVersionRange::parse(QString::fromLatin1(s));
        QVERIFY2(r.isAny() || r.satisfies(v("1.0")),
                 QByteArray("expected permissive for: ").append(s).constData());
        QVERIFY(r.satisfies(v("1.0")));
        QVERIFY(r.satisfies(v("99.0")));
    }
}

void TestNuGetVersionRange::range_invalidVersionNeverSatisfies() {
    NuGetVersion invalid;  // default-constructed, not valid
    QVERIFY(!invalid.isValid());
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
    // 2.0.0 (stable) and 2.0.0-rc both "present"; stable wins.
    const QVector<NuGetVersion> avail{v("2.0.0-rc.1"), v("2.0.0"), v("1.9.0")};
    const auto r = NuGetVersionRange::parse(QStringLiteral("[1.0,)"));
    const auto best = r.selectHighestSatisfying(avail);
    QVERIFY(best.has_value());
    QCOMPARE(best->original(), QStringLiteral("2.0.0"));
    QVERIFY(!best->isPrerelease());
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
