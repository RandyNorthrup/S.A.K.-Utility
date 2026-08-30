// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file strong_index.h
/// @brief A tag-typed index newtype so two index spaces that are both plain
///        integers -- and are therefore silently interchangeable at a call site
///        -- become distinct types a mix-up cannot compile (R5-G14-19).
///
/// The whole point is compile-time distinctness, so the design deliberately
/// forbids the two implicit conversions that would defeat it:
///   * construction from the underlying integer is EXPLICIT, so a bare int does
///     not slip in where a StrongIndex is expected; and
///   * there is NO implicit conversion back to the underlying integer -- a caller
///     that needs the number asks for it by name via value().
/// Two aliases built on different Tag types are unrelated types, so passing a
/// PartitionNumber where a DiskNumber is wanted (or vice versa) is a hard error
/// rather than a silent off-by-one against the wrong index space.

#pragma once

namespace sak {

/// @tparam Tag        an empty tag struct that makes each alias a distinct type.
/// @tparam Underlying the integer type the index is stored as (defaults to the
///                    disk/partition-number width, uint32_t).
template <typename Tag, typename Underlying = unsigned int>
class StrongIndex {
public:
    using underlying_type = Underlying;

    /// A default-constructed index is 0 -- a legitimate first-element index, not
    /// a sentinel. Callers that need "no index" use std::optional<StrongIndex>.
    constexpr StrongIndex() = default;

    /// Explicit so a bare integer cannot implicitly become an index of this space.
    constexpr explicit StrongIndex(Underlying value) noexcept : m_value(value) {}

    /// The stored integer. Named access only -- there is intentionally no
    /// implicit conversion, so the underlying value is never handed out silently.
    [[nodiscard]] constexpr Underlying value() const noexcept { return m_value; }

    // A StrongIndex holds a single trivially-copyable integer, so by-value is the correct and
    // cheaper calling convention for these comparison operands; a const reference would only add
    // an indirection.
    [[nodiscard]] friend constexpr bool operator==(StrongIndex lhs, StrongIndex rhs) noexcept {
        return lhs.m_value == rhs.m_value;
    }
    [[nodiscard]] friend constexpr bool operator!=(StrongIndex lhs, StrongIndex rhs) noexcept {
        return lhs.m_value != rhs.m_value;
    }

private:
    Underlying m_value{};
};

}  // namespace sak
