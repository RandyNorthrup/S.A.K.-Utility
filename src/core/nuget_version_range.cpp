// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_version_range.cpp
/// @brief Implementation of pure NuGet version + version-range value types.

#include "sak/nuget_version_range.h"

#include <algorithm>
#include <limits>

namespace sak {

namespace {

/// @brief NuGet caps a release at four components (Major.Minor.Patch.Revision).
constexpr int kMaxReleaseComponents = 4;

/// @brief A SemVer/NuGet identifier is a non-empty run of ASCII [0-9A-Za-z-].
///        Any other byte (an underscore, '?', a non-ASCII code unit) makes the tag
///        malformed, so the version must be rejected rather than mis-compared.
bool isValidVersionIdentifier(const QString& id) {
    if (id.isEmpty()) {
        return false;
    }
    for (const QChar c : id) {
        const bool ok = (c >= QLatin1Char('0') && c <= QLatin1Char('9')) ||
                        (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                        (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || c == QLatin1Char('-');
        if (!ok) {
            return false;
        }
    }
    return true;
}

/// @brief Split "release[-prerelease][+build]" into (release, prerelease),
///        discarding build metadata. Returns false if the release part is empty.
bool splitVersionText(const QString& text, QString& release_out, QString& prerelease_out) {
    QString core = text.trimmed();
    if (core.isEmpty()) {
        return false;
    }
    // Build metadata ("+abc") is ignored for precedence -- strip it first. It must
    // still be well-formed (dot-separated ASCII identifiers); "1.0+", "1.0+a..b" and
    // "1.0+a+b" are malformed versions, not a plain release.
    const int plus = core.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        const QStringList meta_ids = core.mid(plus + 1).split(QLatin1Char('.'), Qt::KeepEmptyParts);
        for (const QString& id : meta_ids) {
            if (!isValidVersionIdentifier(id)) {
                return false;
            }
        }
        core.truncate(plus);
    }
    const int dash = core.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        prerelease_out = core.mid(dash + 1);
        release_out = core.left(dash);
        // A dash with nothing after it ("1.0.0-") is a malformed prerelease, not a
        // plain release -- reject it rather than silently dropping the tag.
        if (prerelease_out.isEmpty()) {
            return false;
        }
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
    if (segments.size() > kMaxReleaseComponents) {
        return false;  // NuGet allows at most four release components
    }
    for (const QString& segment : segments) {
        if (segment.isEmpty()) {
            return false;
        }
        bool ok = false;
        const long long value = segment.toLongLong(&ok);
        // Each release component is a non-negative 32-bit integer in NuGet; a wider
        // value is a malformed feed token, not a version, so reject it.
        if (!ok || value < 0 || value > std::numeric_limits<int>::max()) {
            return false;
        }
        out.append(value);
    }
    return !out.isEmpty();
}

/// @brief Split a prerelease tag into dot-separated identifiers, keeping empty
///        parts so a malformed separator ("alpha..1", ".beta", "rc.") is caught.
///        Returns nullopt if ANY identifier is empty (SemVer forbids that);
///        splitting with SkipEmptyParts would silently accept the garbled tag.
std::optional<QStringList> splitPrerelease(const QString& prerelease) {
    const QStringList ids = prerelease.split(QLatin1Char('.'), Qt::KeepEmptyParts);
    for (const QString& id : ids) {
        // Reject an empty identifier (a garbled ".."/leading-dot/trailing-dot) AND
        // any identifier bearing a byte outside [0-9A-Za-z-] (e.g. "alpha_1", "?").
        if (!isValidVersionIdentifier(id)) {
            return std::nullopt;
        }
    }
    return ids;
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

    if (!prerelease.isEmpty()) {
        const auto ids = splitPrerelease(prerelease);
        if (!ids.has_value()) {
            return std::nullopt;  // empty prerelease identifier -> malformed
        }
        version.m_prerelease = *ids;
    }
    version.m_original = text.trimmed();
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
    // A default-constructed (invalid) version must not compare EQUAL to a parsed
    // "0" -- order it strictly below any valid version instead of coercing absent
    // state into a real value. (Callers still gate on isValid(); this is defense.)
    if (m_valid != other.m_valid) {
        return m_valid ? 1 : -1;
    }
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

/// @brief True if @p version clears the lower bound (>= or > per inclusivity).
///        A missing bound is unbounded below (always cleared).
bool passesLower(const NuGetVersion& version,
                 const std::optional<NuGetVersion>& lower,
                 bool inclusive) {
    if (!lower.has_value()) {
        return true;
    }
    const int cmp = version.compare(*lower);
    return cmp > 0 || (cmp == 0 && inclusive);
}

/// @brief True if @p version clears the upper bound (<= or < per inclusivity).
///        A missing bound is unbounded above (always cleared).
bool passesUpper(const NuGetVersion& version,
                 const std::optional<NuGetVersion>& upper,
                 bool inclusive) {
    if (!upper.has_value()) {
        return true;
    }
    const int cmp = version.compare(*upper);
    return cmp < 0 || (cmp == 0 && inclusive);
}

}  // namespace

NuGetVersionRange NuGetVersionRange::parse(const QString& text) {
    NuGetVersionRange range;
    // A parsed range starts valid; the apply* helpers set it false on a malformed token. The
    // member defaults to false so a never-parsed range fails closed, so parse() must opt in.
    range.m_valid = true;
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
    if (!minimum.has_value()) {
        m_valid = false;  // an unparseable bare token is malformed -> reject all
        return;
    }
    m_lower = minimum;
    m_lower_inclusive = true;
}

void NuGetVersionRange::applyBracketed(const QString& trimmed) {
    const QChar open = trimmed.front();
    const QChar close = trimmed.back();
    if (close != QLatin1Char(']') && close != QLatin1Char(')')) {
        m_valid = false;  // unbalanced brackets -> malformed
        return;
    }
    const QString inner = trimmed.mid(1, trimmed.size() - 2);
    if (inner.contains(QLatin1Char(','))) {
        applyInterval(inner, open, close);
    } else {
        applyExact(inner, open, close);
    }
}

void NuGetVersionRange::applyExact(const QString& inner, QChar open, QChar close) {
    // [x] is an exact match; (x) or an empty "[]" is malformed -> reject all.
    if (open != QLatin1Char('[') || close != QLatin1Char(']')) {
        m_valid = false;
        return;
    }
    bool ok = false;
    const auto exact = parseBoundToken(inner, ok);
    if (!ok || !exact.has_value()) {
        m_valid = false;  // missing or unparseable exact version
        return;
    }
    m_lower = exact;
    m_upper = exact;
    m_lower_inclusive = true;
    m_upper_inclusive = true;
}

void NuGetVersionRange::applyInterval(const QString& inner, QChar open, QChar close) {
    const int comma = inner.indexOf(QLatin1Char(','));
    bool lower_ok = false;
    bool upper_ok = false;
    const auto lower = parseBoundToken(inner.left(comma), lower_ok);
    const auto upper = parseBoundToken(inner.mid(comma + 1), upper_ok);
    if (!lower_ok || !upper_ok) {
        m_valid = false;  // a bound present but unparseable -> malformed, reject all
        return;
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
    // A malformed range is fail-closed (accepts nothing); an invalid version is
    // never in range.
    if (!m_valid || !version.isValid()) {
        return false;
    }
    return passesLower(version, m_lower, m_lower_inclusive) &&
           passesUpper(version, m_upper, m_upper_inclusive);
}

namespace {
/// @brief True if a range bound is a prerelease version (nullopt bound is not).
bool boundIsPrerelease(const std::optional<NuGetVersion>& bound) {
    return bound.has_value() && bound->isPrerelease();
}

/// @brief True if @p candidate should replace @p best: higher precedence wins,
///        and at equal precedence a stable release displaces a prerelease.
bool candidateBeats(const NuGetVersion& candidate, const std::optional<NuGetVersion>& best) {
    if (!best.has_value()) {
        return true;
    }
    const int cmp = candidate.compare(*best);
    return cmp > 0 || (cmp == 0 && best->isPrerelease() && !candidate.isPrerelease());
}
}  // namespace

std::optional<NuGetVersion> NuGetVersionRange::selectHighestSatisfying(
    const QVector<NuGetVersion>& available) const {
    // Prerelease candidates are eligible only when a bound of this range is itself
    // a prerelease (NuGet's default excludes prerelease), so a higher prerelease
    // never shadows a satisfying stable release (e.g. 1.9.0 wins over 2.0.0-beta
    // for "[1.0,)").
    const bool allow_prerelease = boundIsPrerelease(m_lower) || boundIsPrerelease(m_upper);
    std::optional<NuGetVersion> best;
    for (const NuGetVersion& candidate : available) {
        if (!candidate.isValid() || !satisfies(candidate)) {
            continue;
        }
        if (candidate.isPrerelease() && !allow_prerelease) {
            continue;
        }
        if (candidateBeats(candidate, best)) {
            best = candidate;
        }
    }
    return best;
}

}  // namespace sak
