// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_nuget_version_range.cpp
/// @brief Mutation-fuzz of the NuGet version and version-range parsers (G14 parser sweep).
///
/// NuGetVersion::parse and NuGetVersionRange::parse consume version and range strings that come
/// straight out of third-party package metadata (nuspec dependency nodes, feed responses). The
/// range parser is security-relevant: it is fail-closed by contract -- an unparseable range is
/// INVALID and must satisfy NOTHING, never silently accept every version. This harness drives both
/// parsers over thousands of mutated strings and asserts, for EVERY input:
///
///   1. No crash and no hang (a ReDoS-style hang trips the ctest timeout with the reproducer
///      recorded in fuzz_harness.h).
///   2. Determinism: parsing the same text twice yields the identical verdict for both parsers.
///   3. Fail-closed: an INVALID range is never permissive -- it reports isAny() == false and
///      satisfies() == false for every probe version ([[no-fallbacks-fail-closed]]).
///   4. selectHighestSatisfying only ever returns a version that actually satisfies the range.
///
/// The seed corpus carries real shapes -- a bare minimum "1.0.0", bracketed intervals "[1.0,2.0)",
/// an exact "[1.0]", an open upper "(1.0,)", a prerelease "1.2.3-beta.1" -- plus deliberately
/// pathological strings (long digit / dot / bracket runs) so mutation reaches the interval,
/// prerelease, and rejection paths alike.

#include "sak/nuget_version_range.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>
#include <QVector>

#include <optional>
#include <vector>

namespace {

// A fixed set of probe versions used to exercise satisfies() / selectHighestSatisfying() and to
// prove the fail-closed property (an invalid range accepts none of them).
QVector<sak::NuGetVersion> probeVersions() {
    QVector<sak::NuGetVersion> versions;
    for (const char* text : {"1.0.0", "1.5.0", "2.0.0", "1.2.3-beta", "0.0.1"}) {
        if (const auto parsed = sak::NuGetVersion::parse(QString::fromLatin1(text))) {
            versions.append(*parsed);
        }
    }
    return versions;
}

std::vector<QByteArray> versionRangeCorpus() {
    return {
        QByteArray(),
        QByteArrayLiteral("1.0.0"),
        QByteArrayLiteral("[1.0,2.0)"),
        QByteArrayLiteral("[1.0]"),
        QByteArrayLiteral("(1.0,)"),
        QByteArrayLiteral("(,2.0]"),
        QByteArrayLiteral("1.2.3-beta.1"),
        QByteArrayLiteral("[1.0.0-alpha,2.0.0)"),
        QByteArrayLiteral("[1.2.3.4]") + QByteArray(64, '9'),  // long digit run
        QByteArray(48, '.') + QByteArrayLiteral("[[[(((,,,"),  // pathological punctuation
    };
}

// True if the two range parses agree on every observable: validity, is-any, and how they judge the
// probe versions. A mismatch means the parser carries hidden state or read past its input.
bool sameRangeVerdict(const sak::NuGetVersionRange& a,
                      const sak::NuGetVersionRange& b,
                      const QVector<sak::NuGetVersion>& probes) {
    if (a.isValid() != b.isValid() || a.isAny() != b.isAny()) {
        return false;
    }
    for (const auto& version : probes) {
        if (a.satisfies(version) != b.satisfies(version)) {
            return false;
        }
    }
    return true;
}

// Parse @p input as both a version and a range; return "" if every invariant held.
QString versionRangeInvariant(const QByteArray& input) {
    const QString text = QString::fromUtf8(input);
    const QVector<sak::NuGetVersion> probes = probeVersions();

    // Version parser: no crash + determinism.
    const auto v1 = sak::NuGetVersion::parse(text);
    const auto v2 = sak::NuGetVersion::parse(text);
    if (v1.has_value() != v2.has_value()) {
        return QStringLiteral("NuGetVersion::parse is non-deterministic on identical input");
    }

    // Range parser: determinism.
    const sak::NuGetVersionRange r1 = sak::NuGetVersionRange::parse(text);
    const sak::NuGetVersionRange r2 = sak::NuGetVersionRange::parse(text);
    if (!sameRangeVerdict(r1, r2, probes)) {
        return QStringLiteral("NuGetVersionRange::parse is non-deterministic on identical input");
    }

    // Fail-closed: an invalid range must be neither "any" nor satisfiable by any probe.
    if (!r1.isValid()) {
        if (r1.isAny()) {
            return QStringLiteral("invalid range reports isAny() (fail-open)");
        }
        for (const auto& version : probes) {
            if (r1.satisfies(version)) {
                return QStringLiteral("invalid range satisfies a version (fail-open)");
            }
        }
    }

    // selectHighestSatisfying must only return a version that actually satisfies the range.
    if (const auto picked = r1.selectHighestSatisfying(probes)) {
        if (!r1.satisfies(*picked)) {
            return QStringLiteral("selectHighestSatisfying returned a non-satisfying version");
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral(
            "nuget version-range fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class NuGetVersionRangeFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void parsersNeverCrashAndStayFailClosedOnAnyString() {
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return versionRangeInvariant(input);
        };
        const std::vector<QByteArray> corpus = versionRangeCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }
};

QTEST_GUILESS_MAIN(NuGetVersionRangeFuzzTests)
#include "test_fuzz_nuget_version_range.moc"
