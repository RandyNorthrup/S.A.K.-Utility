// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_version_range.cpp
/// @brief Implementation of pure NuGet version + version-range value types.

#include "sak/nuget_version_range.h"

#include <algorithm>

namespace sak {

namespace {

/// @brief Split "release[-prerelease][+build]" into (release, prerelease),
///        discarding build metadata. Returns false if the release part is empty.
bool splitVersionText(const QString& text, QString& release_out, QString& prerelease_out) {
    QString core = text.trimmed();
    if (core.isEmpty()) {
        return false;
    }
    // Build metadata ("+abc") is ignored for precedence -- strip it first.
    const int plus = core.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        core.truncate(plus);
    }
    const int dash = core.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        prerelease_out = core.mid(dash + 1);
        release_out = core.left(dash);
    } else {
        prerelease_out.clear();
        release_out = core;
    }
    return !release_out.isEmpty();
}

/// @brief True if @p s is a non-empty run of ASCII digits. This is SemVer's
///        actual test for a NUMERIC prerelease identifier -- not "toLongLong
///        succeeds", which both accepts a leading '-' sign and silently fails on
///        values above LLONG_MAX, either of which misorders valid identifiers.
bool isAllDigits(const QString& s) {
    if (s.isEmpty()) {
        return false;
    }
    for (const QChar c : s) {
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) {
            return false;
        }
    }
    return true;
}

/// @brief Drop leading zeros, keeping at least one digit ("007" -> "7", "0" -> "0").
QString stripLeadingZeros(const QString& s) {
    qsizetype i = 0;
    while (i < s.size() - 1 && s.at(i) == QLatin1Char('0')) {
        ++i;
    }
    return s.mid(i);
}

/// @brief Compare two all-digit identifiers by numeric value with arbitrary
///        precision (no overflow): fewer significant digits is smaller, then
///        equal-length compares lexically (which equals numeric order).
int compareNumericIdentifiers(const QString& a, const QString& b) {
    const QString na = stripLeadingZeros(a);
    const QString nb = stripLeadingZeros(b);
    if (na.size() != nb.size()) {
        return (na.size() < nb.size()) ? -1 : 1;
    }
    return QString::compare(na, nb);
}

/// @brief Parse dotted numeric release components. Returns false on any empty or
///        non-numeric segment (an unusual version is incomparable, not misread).
bool parseReleaseComponents(const QString& release, QVector<long long>& out) {
    const QStringList segments = release.split(QLatin1Char('.'));
    for (const QString& segment : segments) {
        if (segment.isEmpty()) {
            return false;
        }
        bool ok = false;
        const long long value = segment.toLongLong(&ok);
        if (!ok || value < 0) {
            return false;
        }
        out.append(value);
    }
    return !out.isEmpty();
}

}  // namespace

std::optional<NuGetVersion> NuGetVersion::parse(const QString& text) {
    QString release;
    QString prerelease;
    if (!splitVersionText(text, release, prerelease)) {
        return std::nullopt;
    }

    NuGetVersion version;
    if (!parseReleaseComponents(release, version.m_release)) {
        return std::nullopt;
    }

    version.m_original = text.trimmed();
    if (!prerelease.isEmpty()) {
        version.m_prerelease = prerelease.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    }
    version.m_valid = true;
    return version;
}

int NuGetVersion::compareRelease(const QVector<long long>& a, const QVector<long long>& b) {
    const qsizetype count = std::max(a.size(), b.size());
    for (qsizetype i = 0; i < count; ++i) {
        const long long lhs = (i < a.size()) ? a.at(i) : 0;
        const long long rhs = (i < b.size()) ? b.at(i) : 0;
        if (lhs != rhs) {
            return (lhs < rhs) ? -1 : 1;
        }
    }
    return 0;
}

int NuGetVersion::comparePrereleaseIdentifier(const QString& a, const QString& b) {
    // SemVer classifies an identifier as NUMERIC iff it is all ASCII digits --
    // NOT "toLongLong parses it", which would accept a leading '-' sign and would
    // silently fail (falling back to a lexical compare) above LLONG_MAX.
    const bool a_num = isAllDigits(a);
    const bool b_num = isAllDigits(b);
    if (a_num && b_num) {
        return compareNumericIdentifiers(a, b);
    }
    // A numeric identifier has lower precedence than an alphanumeric one.
    if (a_num != b_num) {
        return a_num ? -1 : 1;
    }
    return QString::compare(a, b, Qt::CaseInsensitive);
}

int NuGetVersion::comparePrerelease(const QStringList& a, const QStringList& b) {
    // No prerelease outranks any prerelease (1.0.0 > 1.0.0-beta).
    if (a.isEmpty() != b.isEmpty()) {
        return a.isEmpty() ? 1 : -1;
    }
    if (a.isEmpty()) {
        return 0;  // both are plain releases
    }
    const qsizetype count = std::min(a.size(), b.size());
    for (qsizetype i = 0; i < count; ++i) {
        const int cmp = comparePrereleaseIdentifier(a.at(i), b.at(i));
        if (cmp != 0) {
            return (cmp < 0) ? -1 : 1;
        }
    }
    if (a.size() == b.size()) {
        return 0;
    }
    // A longer prerelease with a common prefix has higher precedence.
    return (a.size() < b.size()) ? -1 : 1;
}

int NuGetVersion::compare(const NuGetVersion& other) const {
    const int release_cmp = compareRelease(m_release, other.m_release);
    if (release_cmp != 0) {
        return release_cmp;
    }
    return comparePrerelease(m_prerelease, other.m_prerelease);
}

// ============================================================================
// NuGetVersionRange
// ============================================================================

namespace {

/// @brief Parse one interval endpoint token into an optional bound version.
///        An empty token means "unbounded on this side".
std::optional<NuGetVersion> parseBoundToken(const QString& token, bool& ok_out) {
    ok_out = true;
    const QString trimmed = token.trimmed();
    if (trimmed.isEmpty()) {
        return std::nullopt;
    }
    const auto parsed = NuGetVersion::parse(trimmed);
    if (!parsed.has_value()) {
        ok_out = false;
    }
    return parsed;
}

}  // namespace

NuGetVersionRange NuGetVersionRange::parse(const QString& text) {
    NuGetVersionRange range;
    range.m_original = text.trimmed();
    const QString trimmed = range.m_original;

    if (trimmed.isEmpty()) {
        return range;  // permissive: any version
    }
    const bool bracketed = trimmed.startsWith(QLatin1Char('[')) ||
                           trimmed.startsWith(QLatin1Char('('));
    if (bracketed) {
        range.applyBracketed(trimmed);
    } else {
        range.applyBareMinimum(trimmed);  // bare version => minimum-inclusive
    }
    return range;
}

void NuGetVersionRange::applyBareMinimum(const QString& token) {
    const auto minimum = NuGetVersion::parse(token);
    if (minimum.has_value()) {
        m_lower = minimum;
        m_lower_inclusive = true;
    }
    // An unparseable bare token leaves the range permissive.
}

void NuGetVersionRange::applyBracketed(const QString& trimmed) {
    const QChar open = trimmed.front();
    const QChar close = trimmed.back();
    if (close != QLatin1Char(']') && close != QLatin1Char(')')) {
        return;  // malformed brackets -> permissive
    }
    const QString inner = trimmed.mid(1, trimmed.size() - 2);
    if (inner.contains(QLatin1Char(','))) {
        applyInterval(inner, open, close);
    } else {
        applyExact(inner, open, close);
    }
}

void NuGetVersionRange::applyExact(const QString& inner, QChar open, QChar close) {
    // [x] is an exact match; (x) is invalid -> permissive.
    if (open != QLatin1Char('[') || close != QLatin1Char(']')) {
        return;
    }
    bool ok = false;
    const auto exact = parseBoundToken(inner, ok);
    if (ok && exact.has_value()) {
        m_lower = exact;
        m_upper = exact;
        m_lower_inclusive = true;
        m_upper_inclusive = true;
    }
}

void NuGetVersionRange::applyInterval(const QString& inner, QChar open, QChar close) {
    const int comma = inner.indexOf(QLatin1Char(','));
    bool lower_ok = false;
    bool upper_ok = false;
    const auto lower = parseBoundToken(inner.left(comma), lower_ok);
    const auto upper = parseBoundToken(inner.mid(comma + 1), upper_ok);
    if (!lower_ok || !upper_ok) {
        return;  // a bound present but unparseable -> permissive (over-include)
    }
    if (lower.has_value()) {
        m_lower = lower;
        m_lower_inclusive = (open == QLatin1Char('['));
    }
    if (upper.has_value()) {
        m_upper = upper;
        m_upper_inclusive = (close == QLatin1Char(']'));
    }
}

bool NuGetVersionRange::satisfies(const NuGetVersion& version) const {
    if (!version.isValid()) {
        return false;
    }
    if (m_lower.has_value()) {
        const int cmp = version.compare(*m_lower);
        if (cmp < 0 || (cmp == 0 && !m_lower_inclusive)) {
            return false;
        }
    }
    if (m_upper.has_value()) {
        const int cmp = version.compare(*m_upper);
        if (cmp > 0 || (cmp == 0 && !m_upper_inclusive)) {
            return false;
        }
    }
    return true;
}

std::optional<NuGetVersion> NuGetVersionRange::selectHighestSatisfying(
    const QVector<NuGetVersion>& available) const {
    std::optional<NuGetVersion> best;
    for (const NuGetVersion& candidate : available) {
        if (!candidate.isValid() || !satisfies(candidate)) {
            continue;
        }
        if (!best.has_value()) {
            best = candidate;
            continue;
        }
        const int cmp = candidate.compare(*best);
        // Higher precedence wins; at equal precedence prefer a stable release
        // over a prerelease (they compare equal only if both stable).
        if (cmp > 0 || (cmp == 0 && best->isPrerelease() && !candidate.isPrerelease())) {
            best = candidate;
        }
    }
    return best;
}

}  // namespace sak
