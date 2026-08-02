// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file nuget_version_range.h
/// @brief Pure NuGet package version + version-range value types.
///
/// Implements the subset of NuGet SemVer 2.0 version comparison and the NuGet
/// interval-notation version-range grammar needed to resolve Chocolatey/NuGet
/// dependency graphs correctly (honoring declared ranges instead of always
/// grabbing "latest"). Everything here is PURE -- no network, no Qt event loop,
/// no shared state -- so the dependency resolver built on top is fully
/// unit-testable with an in-memory feed.
///
/// Supported range grammar (NuGet "Package versioning" spec):
///   ""            -> any version (permissive; used when a feed omits a range)
///   1.0           -> minimum inclusive   ( >= 1.0 )
///   [1.0]         -> exact               ( == 1.0 )
///   [1.0,)        -> >= 1.0
///   (1.0,)        -> >  1.0
///   (,2.0]        -> <= 2.0
///   (,2.0)        -> <  2.0
///   [1.0,2.0]     -> >= 1.0 and <= 2.0
///   [1.0,2.0)     -> >= 1.0 and <  2.0
///   (1.0,2.0)     -> >  1.0 and <  2.0

#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

namespace sak {

/// @brief A parsed NuGet package version (release parts + optional prerelease).
///
/// Release components are compared numerically, padded with zeros to equal
/// length. A version bearing a prerelease tag ("1.0.0-beta") sorts BELOW the
/// same release without one ("1.0.0"), per SemVer. Build metadata ("+abc") is
/// ignored for comparison. A version whose release contains a non-numeric
/// segment is invalid (incomparable) rather than mis-ordered.
class NuGetVersion {
public:
    NuGetVersion() = default;

    /// @brief Parse @p text; returns std::nullopt if it is not a valid version.
    [[nodiscard]] static std::optional<NuGetVersion> parse(const QString& text);

    [[nodiscard]] bool isValid() const { return m_valid; }
    [[nodiscard]] bool isPrerelease() const { return !m_prerelease.isEmpty(); }
    [[nodiscard]] const QString& original() const { return m_original; }

    /// @brief Three-way compare: <0 if this < other, 0 if equal, >0 if greater.
    ///        Requires both valid (callers gate on isValid()).
    [[nodiscard]] int compare(const NuGetVersion& other) const;

    [[nodiscard]] bool operator==(const NuGetVersion& o) const { return compare(o) == 0; }
    [[nodiscard]] bool operator<(const NuGetVersion& o) const { return compare(o) < 0; }
    [[nodiscard]] bool operator<=(const NuGetVersion& o) const { return compare(o) <= 0; }
    [[nodiscard]] bool operator>(const NuGetVersion& o) const { return compare(o) > 0; }
    [[nodiscard]] bool operator>=(const NuGetVersion& o) const { return compare(o) >= 0; }

private:
    static int compareRelease(const QVector<long long>& a, const QVector<long long>& b);
    static int comparePrerelease(const QStringList& a, const QStringList& b);
    static int comparePrereleaseIdentifier(const QString& a, const QString& b);

    QString m_original;
    QVector<long long> m_release;  ///< numeric release components (>= 1 when valid)
    QStringList m_prerelease;      ///< dot-separated prerelease identifiers ("" = release)
    bool m_valid{false};
};

/// @brief A parsed NuGet version range with optional lower/upper bounds.
///
/// An unparseable or empty range is PERMISSIVE (satisfied by every valid
/// version). This is deliberate: for offline-bundle completeness, over-including
/// a dependency is safe, whereas dropping one because its range was unusual
/// would produce a bundle that cannot install offline.
class NuGetVersionRange {
public:
    NuGetVersionRange() = default;

    /// @brief Parse NuGet interval notation (see file header). Never fails: an
    ///        unrecognized string yields an any-version (permissive) range.
    [[nodiscard]] static NuGetVersionRange parse(const QString& text);

    /// @brief True if @p version falls within this range. An invalid version is
    ///        never satisfied. A permissive (any) range accepts every valid one.
    [[nodiscard]] bool satisfies(const NuGetVersion& version) const;

    /// @brief True if this range accepts every valid version (empty/unparseable).
    [[nodiscard]] bool isAny() const { return !m_lower.has_value() && !m_upper.has_value(); }

    /// @brief From @p available, the highest version that satisfies this range,
    ///        preferring a stable release over a prerelease at equal precedence.
    ///        std::nullopt if none qualify. Invalid entries are ignored.
    [[nodiscard]] std::optional<NuGetVersion> selectHighestSatisfying(
        const QVector<NuGetVersion>& available) const;

    [[nodiscard]] const QString& original() const { return m_original; }

private:
    void applyBareMinimum(const QString& token);
    void applyBracketed(const QString& trimmed);
    void applyExact(const QString& inner, QChar open, QChar close);
    void applyInterval(const QString& inner, QChar open, QChar close);

    QString m_original;
    std::optional<NuGetVersion> m_lower;
    std::optional<NuGetVersion> m_upper;
    bool m_lower_inclusive{false};
    bool m_upper_inclusive{false};
};

}  // namespace sak
