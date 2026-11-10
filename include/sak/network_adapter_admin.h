// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_admin.h
/// @brief Every network-adapter mutation this program performs, and the exact command each one
///        issues. This is the single place a netsh adapter command is constructed or run.
///
/// R5-IDX-19c: enable/disable/rename were the last adapter operations a technician could perform
/// from the GUI that the assistant could not perform at all. The registered network ids covered
/// DHCP, static IP and DNS, but nothing brought an adapter up or down or renamed one, so the
/// "assistant drives the same code the buttons drive" claim was false for them in the strongest
/// possible way -- there was no headless code to drive.
///
/// R5-IDX-19b: per the owner's ruling, the GUI does not build or run commands -- it points at this
/// header. `src/gui/network_diagnostic_panel.cpp` previously spelled out its own netsh argument
/// vectors for static IP, DNS and DHCP and ran them itself, and those vectors did not match the
/// ones `EthernetConfigManager` issues for the same operations. Two independent implementations of
/// "reconfigure this machine's networking", already disagreeing, is the defect; one implementation
/// with the disagreements named as parameters is the fix. See AdapterIpv4Dialect.
///
/// Everything that can be decided WITHOUT touching the machine is a pure function here, so the
/// interesting parts (name validation, resolution against the system's own list, and the exact
/// argument vector handed to netsh) are unit-testable with no adapter present.

#pragma once

#include <QString>
#include <QStringList>

#include <optional>

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

// ---------------------------------------------------------------------------
// IPv4 configuration
//
// The two call sites that reconfigure IPv4 -- the restore wizard / assistant path through
// EthernetConfigManager, and the technician's diagnostic panel -- issued DIFFERENT netsh commands
// for the same operation. Both are unified onto the builders below, and the one place they still
// differ is a named policy rather than a fork:
//
//   * register. The restore path passes `register=primary` (register under the primary DNS suffix
//     only); the panel omitted it and took netsh's own default, which netsh's help does not state.
//
// They also differed in the gateway's metric, and both were wrong (fix round S, measured on the
// Windows 11 VM): the panel's `set address ... gateway=` with no `gwmetric` pins the route metric
// at 1, and the restore path's `gwmetric=0` pins it at 0, where Windows' own adapter dialog leaves
// it automatic (route print "Default", effective 281 against 26 and 25 on that VM). On a PC with
// more than one adapter that metric decides which carries the default route. Only an address set
// with `gateway=none` and the gateway then added as a route with no metric leaves it automatic, so
// both paths now do that.
// ---------------------------------------------------------------------------

/// @brief True when @p value is a complete dotted IPv4 quad and nothing else.
/// @note Four octets exactly, each 0-255, written in plain ASCII decimal with no leading zero. No
///       CIDR suffix, no surrounding whitespace, no trailing junk, and none of the abbreviated
///       inet_aton forms -- "192.168.1" is REFUSED rather than read as 192.168.0.1. This is the
///       whole point of the check: it guarantees the token shape that reaches netsh, so a value a
///       technician typed or a model emitted cannot be silently reinterpreted into a different
///       address than the one requested. The argument vectors are shell-free (each value is one
///       argv token, so there is no flag or shell injection); this is defence in depth on that.
[[nodiscard]] bool isDottedIpv4(const QString& value);

/// @brief Why a static IPv4 configuration would be refused, or an empty string when it is usable.
/// @param gateway Optional; an empty gateway is legitimate and is not a problem.
/// @note Pure, and separated from setAdapterStaticIpv4 on purpose: everything decidable WITHOUT
///       touching the machine must be testable without touching the machine. A test that reached
///       the executor to check a refusal would run a live netsh apply the moment the refusal
///       regressed -- the exact accident this split prevents.
[[nodiscard]] QString staticIpv4ConfigurationProblem(const QString& address,
                                                     const QString& mask,
                                                     const QString& gateway);

/// @brief Why a DNS server list would be refused, or an empty string when it is usable.
[[nodiscard]] QString staticDnsProblem(const QStringList& servers);

/// @brief Which DNS suffix a static primary-DNS apply registers the connection under.
enum class DnsRegistration {
    /// Omit `register=` and take netsh's own default.
    NetshDefault,
    /// Pass `register=primary`.
    PrimarySuffixOnly,
};

/// @brief The netsh dialect a caller wants for IPv4 operations.
/// @note Two named instances exist (kRestoreIpv4Dialect, kTechnicianIpv4Dialect) so no caller
///       invents a third combination by accident.
struct AdapterIpv4Dialect {
    DnsRegistration dns_registration{DnsRegistration::NetshDefault};
};

/// @brief What EthernetConfigManager's restore path has always issued (primary-only registration).
///        Used by the assistant actions and the profile restore wizard.
inline constexpr AdapterIpv4Dialect kRestoreIpv4Dialect{DnsRegistration::PrimarySuffixOnly};

/// @brief What the diagnostic panel has always issued (netsh's default registration). Used when a
///        technician types a configuration into the panel.
inline constexpr AdapterIpv4Dialect kTechnicianIpv4Dialect{DnsRegistration::NetshDefault};

/// @brief The netsh argument vector that assigns a static IPv4 address, with no gateway
///        (`gateway=none`, which also removes any the adapter had). A gateway goes on afterwards
///        as a route (adapterDefaultGatewayArgs), which is how its metric stays automatic.
/// @param adapter_name MUST already be a resolved, system-sourced name.
/// @note Uses the named-tag form throughout (`name=`, `address=`, ...).
[[nodiscard]] QStringList adapterStaticIpv4Args(const QString& adapter_name,
                                                const QString& address,
                                                const QString& mask);

/// @brief The netsh argument vector that returns an adapter's IPv4 address to DHCP.
[[nodiscard]] QStringList adapterDhcpAddressArgs(const QString& adapter_name);

/// @brief The netsh argument vector that returns an adapter's DNS servers to DHCP.
[[nodiscard]] QStringList adapterDhcpDnsArgs(const QString& adapter_name);

/// @brief The netsh argument vector that sets the primary static DNS server.
/// @note This REPLACES the adapter's whole DNS list; secondaries are added afterwards.
[[nodiscard]] QStringList adapterStaticPrimaryDnsArgs(const QString& adapter_name,
                                                      const QString& address,
                                                      DnsRegistration registration);

/// @brief The netsh argument vector that appends one more static DNS server.
/// @param index 1-based preference position; the primary occupies 1, so secondaries start at 2.
[[nodiscard]] QStringList adapterAdditionalDnsArgs(const QString& adapter_name,
                                                   const QString& address,
                                                   int index);

/// @brief Assign a static IPv4 configuration to an adapter: the address, then the gateway as a
///        route whose metric Windows chooses, as its own adapter dialog does.
/// @param adapter_name An exact, system-sourced adapter name.
/// @param gateway Optional. Every non-empty address argument must be a dotted IPv4 quad; anything
///        else is refused before netsh runs.
/// @return Success only when both steps succeeded. A gateway that could not be added after the
///         address was set is a failure that says the address is set and the gateway is not.
[[nodiscard]] AdapterAdminOutcome setAdapterStaticIpv4(const QString& adapter_name,
                                                       const QString& address,
                                                       const QString& mask,
                                                       const QString& gateway,
                                                       AdapterIpv4Dialect dialect);

/// @brief The outcome of a static-DNS apply, which is more than one netsh call.
struct DnsApplyOutcome {
    /// True only when EVERY step succeeded.
    bool succeeded{false};

    /// True once the primary server is LIVE. The primary step replaces the adapter's whole DNS
    /// list, so a caller must be told this even when a later secondary step failed -- otherwise a
    /// partial apply reads as "nothing happened" when the machine's resolver has already changed.
    bool primary_applied{false};

    /// Human-readable outcome; never empty on failure.
    QString message;
};

/// @brief Set an adapter's static IPv4 DNS servers, primary first.
/// @param servers At least one; each must be a dotted IPv4 quad. Duplicates are NOT removed here,
///        because netsh rejects a duplicate `add` and the caller's list is its own to canonicalize.
[[nodiscard]] DnsApplyOutcome setAdapterStaticDns(const QString& adapter_name,
                                                  const QStringList& servers,
                                                  DnsRegistration registration);

/// @brief The outcome of switching an adapter to DHCP, which is an address step plus a DNS step.
struct DhcpApplyOutcome {
    /// True only when both steps succeeded.
    bool succeeded{false};

    /// True when DNS was switched to automatic as well. False leaves DNS pinned to the previous
    /// static servers, which is a genuinely partial result the caller must surface.
    bool dns_switched{false};

    /// Human-readable outcome; never empty on failure.
    QString message;
};

/// @brief Switch an adapter's IPv4 address AND DNS to automatic (DHCP).
[[nodiscard]] DhcpApplyOutcome setAdapterDhcpMode(const QString& adapter_name);

// ---------------------------------------------------------------------------
// Putting settings back after a network reset
//
// Reset Network records every adapter's settings first and puts the ones typed in by hand back
// afterwards (network_reset_safeguard.h). It restores what was there, so it passes the gateway
// metric Windows had stored rather than either dialect's policy, and it can put back more than
// one address, more than one gateway and IPv6 settings, which the single-adapter restore above
// never carried.
// ---------------------------------------------------------------------------

/// @brief True when @p value is the address an IPv4 adapter gives itself when nothing else
///        configures it (169.254.0.0/16, "self-assigned" or APIPA). It cannot reach other
///        networks. False for anything that is not a dotted IPv4 quad.
[[nodiscard]] bool isSelfAssignedIpv4(const QString& value);

/// @brief True when @p value is a plain IPv6 address: no zone index, no prefix, no brackets.
[[nodiscard]] bool isIpv6Literal(const QString& value);

/// @brief The IP family a command addresses.
enum class IpFamily {
    V4,
    V6,
};

/// @brief `add address` for one more static IPv4 address beside the first.
[[nodiscard]] QStringList adapterAdditionalIpv4AddressArgs(const QString& adapter_name,
                                                           const QString& address,
                                                           const QString& mask);

/// @brief `add address` for a static IPv6 address, "address/prefix".
[[nodiscard]] QStringList adapterIpv6AddressArgs(const QString& adapter_name,
                                                 const QString& address_with_prefix);

/// @brief `add route` for a default gateway (0.0.0.0/0 or ::/0) through @p next_hop.
/// @param metric The route metric to pin, or nullopt for none: Windows then chooses it, and
///        `route print` shows "Default". `metric=0` pins 0, it does not mean automatic.
[[nodiscard]] QStringList adapterDefaultGatewayArgs(IpFamily family,
                                                    const QString& adapter_name,
                                                    const QString& next_hop,
                                                    std::optional<int> metric);

/// @brief `set dnsservers source=static` for @p family's primary server, netsh's own registration.
[[nodiscard]] QStringList adapterFamilyPrimaryDnsArgs(IpFamily family,
                                                      const QString& adapter_name,
                                                      const QString& address);

/// @brief `add dnsservers` for @p family's server at 1-based @p index (2 and up).
[[nodiscard]] QStringList adapterFamilyAdditionalDnsArgs(IpFamily family,
                                                         const QString& adapter_name,
                                                         const QString& address,
                                                         int index);

/// @brief Run one argument vector built by a builder in this header, through the System32 netsh,
///        with the same failure rule as every other adapter command here.
/// @param netsh_args MUST come from a builder above, with a resolved, system-sourced name.
[[nodiscard]] AdapterAdminOutcome runAdapterCommand(const QStringList& netsh_args,
                                                    const QString& success_message);

/// @brief Release the adapter's current DHCP lease (`ipconfig /release`).
[[nodiscard]] AdapterAdminOutcome releaseAdapterDhcpLease(const QString& adapter_name);

/// @brief Renew the adapter's DHCP lease (`ipconfig /renew`).
[[nodiscard]] AdapterAdminOutcome renewAdapterDhcpLease(const QString& adapter_name);

}  // namespace sak
