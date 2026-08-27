// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_admin.h
/// @brief Enable, disable and rename a network adapter -- the headless half of the three
///        operations the diagnostic panel used to run through its own private netsh calls.
///
/// R5-IDX-19c: these three were the last adapter operations a technician could perform from the
/// GUI that the assistant could not perform at all. The registered network ids covered DHCP,
/// static IP and DNS, but nothing brought an adapter up or down or renamed one, so the
/// "assistant drives the same code the buttons drive" claim was false for them in the strongest
/// possible way -- there was no headless code to drive.
///
/// Everything that can be decided WITHOUT touching the machine is a pure function here, so the
/// interesting parts (name validation, resolution against the system's own list, and the exact
/// argument vector handed to netsh) are unit-testable with no adapter present.

#pragma once

#include <QString>
#include <QStringList>

namespace sak {

/// @brief The outcome of an adapter administration attempt.
struct AdapterAdminOutcome {
    /// True only when netsh ran AND reported success. A refusal before netsh runs is false.
    bool succeeded{false};

    /// Human-readable outcome. On failure this is the reason, never an empty string -- a caller
    /// that surfaces it must always have something honest to show.
    QString message;
};

/// @brief The largest adapter name this code will accept.
/// Windows itself allows a long connection name; the cap exists so a pathological value cannot
/// be pushed into a command line, not because 255 is a documented Windows limit.
inline constexpr int kMaxAdapterNameLength = 255;

/// @brief Whether @p name is acceptable as a NEW adapter name.
/// @return false for empty/whitespace-only, over the length cap, containing an ASCII control
///         byte or a double quote, containing '=' , or beginning with '-' or '/'.
/// @note The '=' rule is not cosmetic. The rename is issued as the single argument
///       `newname=<value>`; a value that itself contains '=' produces `newname=a=b`, which netsh
///       is free to split differently than intended. Leading '-' and '/' are rejected for the
///       same class of reason: netsh would read them as options rather than as a name. None of
///       this is shell quoting -- the arguments never go through a shell -- it is about what
///       netsh's OWN parser does with the token.
[[nodiscard]] bool isValidNewAdapterName(const QString& name);

/// @brief Resolve @p requested against the system's own adapter list, case-insensitively.
/// @return The authoritative system spelling, or an empty string when there is no exact match.
/// @note This is the anti-injection barrier: only a real, system-sourced name ever reaches
///       netsh, so a model-supplied or mistyped name is refused before any command runs. It
///       deliberately does NOT do prefix or fuzzy matching -- picking the "closest" adapter is
///       how you disable the wrong NIC.
[[nodiscard]] QString resolveAdapterName(const QStringList& available, const QString& requested);

/// @brief The netsh argument vector that brings @p adapter_name up or down.
/// @param adapter_name MUST already be a resolved, system-sourced name.
[[nodiscard]] QStringList adapterAdminStateArgs(const QString& adapter_name, bool enabled);

/// @brief The netsh argument vector that renames @p adapter_name to @p new_name.
/// @param adapter_name MUST already be a resolved, system-sourced name.
/// @param new_name MUST already have passed isValidNewAdapterName().
[[nodiscard]] QStringList adapterRenameArgs(const QString& adapter_name, const QString& new_name);

/// @brief Bring a network adapter up or down.
/// @param adapter_name An exact, system-sourced adapter name.
/// @return Success only when netsh ran and reported success. Requires elevation; a non-elevated
///         run fails HONESTLY rather than reporting a change that did not happen.
/// @warning Disabling the adapter carrying the current session drops connectivity. That is
///          reversible (enable it again) but not remotely, which is why the action that wraps
///          this is gated.
[[nodiscard]] AdapterAdminOutcome setAdapterEnabled(const QString& adapter_name, bool enabled);

/// @brief Rename a network adapter.
/// @param adapter_name An exact, system-sourced adapter name.
/// @param new_name The new name; rejected before netsh runs unless isValidNewAdapterName().
[[nodiscard]] AdapterAdminOutcome renameAdapter(const QString& adapter_name,
                                                const QString& new_name);

}  // namespace sak
