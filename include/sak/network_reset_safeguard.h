// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_reset_safeguard.h
/// @brief What Reset Network records before it resets the stack, how it reads netsh's answer, how
///        it puts settings typed in by hand back, and how it checks the adapters afterwards.
///
/// The 2026-11-09 VM smoke (E:\SAK-Utility-Test-Scratch\vm-smoke\repro) found two defects in the
/// reset:
///
///   * `netsh int ip reset` clears an adapter's static IPv4 setup. DHCP stays off, so the adapter
///     comes back with only a self-assigned 169.254.x.x address, no gateway and no working DNS --
///     before and after the reboot. Nothing warned the technician, and nothing was saved.
///   * The same command exits 1 on a stock, elevated Windows 11 26200: about thirty items print
///     "OK!", one protected item prints "failed. Access is denied.", and the output ends "Restart
///     the computer to complete this action". The app counted that as a failed step, and a verify
///     probe that timed out while the adapters restarted as another.
///
/// Everything here that can be decided without touching the machine is a pure function, so the
/// rules are tested against the real output captured on the VM. The only functions that run
/// anything are captureNetworkSettings (read-only PowerShell) and saveNetworkCapture (one file).
/// Every netsh command that CHANGES an adapter is still built and run by network_adapter_admin.h.

#pragma once

#include "sak/process_runner.h"

#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <expected>
#include <functional>
#include <optional>

namespace sak {

/// @brief A default gateway typed in by hand, with the route metric Windows stores for it.
struct StaticGateway {
    QString next_hop;
    int metric{0};
    /// Windows chooses the metric (route print "Default"; the interface's DefaultGatewayMetric 0
    /// beside a route metric that is not 0). It goes back with no metric, so it stays automatic.
    bool automatic{false};

    bool operator==(const StaticGateway&) const = default;
};

/// @brief One IP family (IPv4 or IPv6) on one adapter.
struct IpFamilySettings {
    /// Windows has this family bound to the adapter (Get-NetIPInterface lists it).
    bool present{false};
    /// Get-NetIPInterface reports Dhcp "Enabled".
    bool dhcp{false};
    /// Every address on the adapter now, as "address/prefix" (zone index removed).
    QStringList addresses;
    /// The addresses typed in by hand: the persistent store, which is what a reset clears.
    QStringList static_addresses;
    /// Every default-route next hop in use now.
    QStringList gateways;
    /// The default gateways typed in by hand (persistent store).
    QList<StaticGateway> static_gateways;
    /// The DNS servers in use now.
    QStringList dns_servers;
    /// The DNS servers typed in by hand (the interface's registry NameServer value).
    QStringList static_dns_servers;

    [[nodiscard]] bool hasStaticSettings() const;
};

/// @brief One network adapter as Get-NetAdapter reports it.
struct AdapterSettings {
    QString name;
    QString description;
    /// Get-NetAdapter's Status: "Up", "Disconnected", "Disabled", ...
    QString status;
    IpFamilySettings ipv4;
    IpFamilySettings ipv6;

    [[nodiscard]] bool isUp() const;
    /// A disabled adapter has no IP interface, so Windows reports none of its settings.
    [[nodiscard]] bool isDisabled() const;
    [[nodiscard]] bool hasStaticSettings() const;
};

/// @brief Every adapter's settings at one moment.
struct NetworkSettingsCapture {
    QList<AdapterSettings> adapters;
    QString captured_at;
    QString computer_name;

    /// The adapter called @p name (case-insensitive), or nullptr.
    [[nodiscard]] const AdapterSettings* find(const QString& name) const;
};

// ---------------------------------------------------------------------------
// Capture
// ---------------------------------------------------------------------------

/// @brief The read-only PowerShell that lists every adapter's settings as JSON.
/// @note Language-neutral: it reads cmdlet enums, the persistent store and the registry, never
///       the translated text of ipconfig or netsh.
[[nodiscard]] QString networkCaptureScript();

/// @brief Parse what networkCaptureScript printed.
/// @return The capture, or why it cannot be trusted. Fails closed: output without the script's
///         marker, an adapter with no name, or an address that is not an address is refused,
///         because this capture is what the reset later writes back onto the adapters.
[[nodiscard]] std::expected<NetworkSettingsCapture, QString> parseNetworkCapture(
    const QString& json);

/// @brief Run networkCaptureScript and parse it. Read-only; needs no elevation.
[[nodiscard]] std::expected<NetworkSettingsCapture, QString> captureNetworkSettings(
    int timeout_ms, const CancelCheck& should_cancel = {});

/// @brief The capture as the JSON written to the backup file.
[[nodiscard]] QJsonObject networkCaptureToJson(const NetworkSettingsCapture& capture);

/// @brief Where Reset Network keeps the settings it records: data/network-backups beside the
///        program (or the packaged app's data folder), so a technician finds it with the app.
[[nodiscard]] QString networkBackupDirectory();

/// @brief Write @p capture as "network-settings-before-reset-<time>.json" in @p directory.
/// @return The file written, or why it could not be.
[[nodiscard]] std::expected<QString, QString> saveNetworkCapture(
    const NetworkSettingsCapture& capture, const QString& directory);

/// @brief "IPv4 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3" -- the settings typed in by hand.
[[nodiscard]] QString describeStaticSettings(const AdapterSettings& adapter);

/// @brief What the Reset Network confirmation adds about @p capture: each adapter with settings
///        typed in by hand and what happens to them, and each disabled adapter, whose settings
///        Windows does not report. Empty when there is nothing to say.
[[nodiscard]] QString resetConfirmationDetail(const NetworkSettingsCapture& capture);

// ---------------------------------------------------------------------------
// Reading `netsh int ip reset` / `netsh int ipv6 reset`
// ---------------------------------------------------------------------------

enum class NetshResetVerdict {
    /// Every item reset.
    Done,
    /// Every item reset or was refused as protected ("Access is denied."), and netsh said a restart
    /// completes the reset. Windows does this on a stock install; it is not a failure.
    DoneRestartRequired,
    /// Anything else: a timeout, an item that failed for another reason, or output not recognised
    /// together with a non-zero exit.
    Failed,
};

struct NetshResetReading {
    NetshResetVerdict verdict{NetshResetVerdict::Failed};
    int items_reset{0};
    int items_denied{0};
    /// Why it failed, or what to tell the technician about denied items. Empty on a clean Done.
    QString detail;
};

/// @brief Read one netsh reset's output and exit code.
/// @note The text netsh prints is English on an English Windows only. On another language the
///       lines are not recognised, so a non-zero exit stays a failure (fail closed) and the caller
///       shows netsh's own output; an exit of 0 is still read as done.
[[nodiscard]] NetshResetReading readNetshReset(const QString& output,
                                               int exit_code,
                                               bool timed_out);

// ---------------------------------------------------------------------------
// Putting settings back
// ---------------------------------------------------------------------------

/// @brief One netsh command that puts part of an adapter's settings back.
struct ReapplyStep {
    QString adapter;
    /// What it puts back, for the log: "IPv4 10.0.2.15/24 with gateway 10.0.2.2".
    QString description;
    QStringList netsh_args;
};

/// @brief The commands that put @p before's hand-typed settings back after the reset, given what
///        the adapters look like now.
/// @param after The adapters after the reset, or nullopt when Windows could not be read: every
///        hand-typed setting is then put back in full, and DHCP adapters are left alone (the reset
///        does not turn DHCP off, and asking netsh to turn it on again where it is already on is a
///        refused command).
/// @param problems Out: adapters that cannot be put back (gone, or disabled during the reset).
[[nodiscard]] QList<ReapplyStep> planSettingsReapply(
    const NetworkSettingsCapture& before,
    const std::optional<NetworkSettingsCapture>& after,
    QStringList* problems);

/// @brief How one adapter that was up before the reset reads afterwards.
struct AdapterCheck {
    QString adapter;
    bool usable{false};
    /// "10.0.2.15/24, set by hand" or "no usable IPv4 address: only 169.254.193.132".
    QString detail;
};

/// @brief Check every adapter that was up before the reset: it must have DHCP on or an IPv4
///        address that is not self-assigned (169.254.x.x), and every IPv4 address typed in by hand
///        must be back. Adapters that were not up, or had no IPv4, are not checked.
[[nodiscard]] QList<AdapterCheck> checkAdaptersAfterReset(const NetworkSettingsCapture& before,
                                                          const NetworkSettingsCapture& after);

[[nodiscard]] bool allAdaptersUsable(const QList<AdapterCheck>& checks);

// ---------------------------------------------------------------------------
// Waiting for the adapters to settle
// ---------------------------------------------------------------------------

/// @brief The clock and capture a bounded wait uses; real ones in production, fakes in tests.
struct SettleProbe {
    std::function<std::expected<NetworkSettingsCapture, QString>()> capture;
    std::function<qint64()> now_ms;
    std::function<void(int)> sleep_ms;
    std::function<bool()> cancelled;
};

struct SettleResult {
    /// The last capture that worked, if any did.
    std::optional<NetworkSettingsCapture> capture;
    /// @p settled returned true for it before the budget ran out.
    bool settled{false};
    /// Why the last attempt failed, when none worked.
    QString last_error;
    int attempts{0};
};

/// @brief Capture until @p settled says yes, or @p budget_ms runs out, polling every @p poll_ms.
/// @note Always makes at least one attempt. A capture that fails while Windows restarts the
///       adapters is not an error here: it is what settling looks like.
[[nodiscard]] SettleResult waitForSettledNetwork(
    const SettleProbe& probe,
    const std::function<bool(const NetworkSettingsCapture&)>& settled,
    int budget_ms,
    int poll_ms);

}  // namespace sak
