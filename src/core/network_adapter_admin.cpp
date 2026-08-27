// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_admin.cpp
/// @brief Every network-adapter mutation, through a System32-qualified netsh (or ipconfig).

#include "sak/network_adapter_admin.h"

#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/process_runner.h"

namespace sak {

namespace {

/// The first printable ASCII codepoint. Anything below it is a C0 control character.
constexpr char16_t kFirstPrintableAscii = 0x20;

/// DEL. Not a C0 control, but equally not something that belongs in an adapter name.
constexpr char16_t kDeleteCodepoint = 0x7F;

/// The preference position netsh gives the primary DNS server. Secondaries start after it.
constexpr int kPrimaryDnsIndex = 1;

/// An IPv4 address has exactly four octets. Not three, and not one -- see isDottedIpv4.
constexpr int kIpv4OctetCount = 4;

/// The largest value a single octet can hold, and the most digits one may be written with.
constexpr int kMaxOctetValue = 255;
constexpr int kMaxOctetDigits = 3;

/// One octet of a dotted IPv4 address: 1-3 ASCII digits, value 0-255, and no leading zero unless
/// the octet IS zero. The leading-zero rule is not cosmetic: the C `inet_aton` family reads a
/// leading zero as OCTAL, so "010.1.1.1" means 8.1.1.1 to some parsers and 10.1.1.1 to others.
/// Refusing the ambiguous spelling outright means every consumer of a validated address agrees on
/// what it says.
bool isIpv4Octet(QStringView octet) {
    if (octet.isEmpty() || octet.size() > kMaxOctetDigits) {
        return false;
    }
    for (const QChar ch : octet) {
        // Deliberately ASCII-only: QChar::isDigit() also accepts non-ASCII decimal digits, which
        // QString::toInt would then convert, letting a non-ASCII spelling of an address through.
        if (ch < QLatin1Char('0') || ch > QLatin1Char('9')) {
            return false;
        }
    }
    if (octet.size() > 1 && octet.front() == QLatin1Char('0')) {
        return false;
    }
    return octet.toInt() <= kMaxOctetValue;
}

}  // namespace

bool isDottedIpv4(const QString& value) {
    // Hand-parsed rather than delegated to QHostAddress, which accepts the abbreviated inet_aton
    // forms: it reads "192.168.1" as 192.168.0.1 and "8.8.4" as 8.8.0.4. Four files carried a copy
    // of that check, each with a comment claiming it accepted only a complete address, and none of
    // them did. A caller that types or a model that emits a three-part address would have had a
    // DIFFERENT address configured than the one it asked for, silently.
    const QList<QStringView> octets = QStringView{value}.split(QLatin1Char('.'));
    if (octets.size() != kIpv4OctetCount) {
        return false;
    }
    for (const QStringView octet : octets) {
        if (!isIpv4Octet(octet)) {
            return false;
        }
    }
    return true;
}

bool isValidNewAdapterName(const QString& name) {
    const QString trimmed = name.trimmed();
    if (trimmed.isEmpty() || trimmed.size() > kMaxAdapterNameLength) {
        return false;
    }
    // Reject the token shapes netsh's own parser would read as something other than a name, and
    // any control byte (which would also corrupt a log line carrying the name).
    if (trimmed.startsWith(QLatin1Char('-')) || trimmed.startsWith(QLatin1Char('/'))) {
        return false;
    }
    for (const QChar ch : trimmed) {
        if (ch.unicode() < kFirstPrintableAscii || ch.unicode() == kDeleteCodepoint) {
            return false;
        }
        if (ch == QLatin1Char('=') || ch == QLatin1Char('"')) {
            return false;
        }
    }
    return true;
}

QString resolveAdapterName(const QStringList& available, const QString& requested) {
    const QString wanted = requested.trimmed();
    if (wanted.isEmpty()) {
        return {};
    }
    for (const QString& candidate : available) {
        if (candidate.compare(wanted, Qt::CaseInsensitive) == 0) {
            return candidate;  // the system's own spelling, not the caller's
        }
    }
    return {};
}

QStringList adapterAdminStateArgs(const QString& adapter_name, bool enabled) {
    return {QStringLiteral("interface"),
            QStringLiteral("set"),
            QStringLiteral("interface"),
            adapter_name,
            enabled ? QStringLiteral("admin=ENABLED") : QStringLiteral("admin=DISABLED")};
}

QStringList adapterRenameArgs(const QString& adapter_name, const QString& new_name) {
    return {QStringLiteral("interface"),
            QStringLiteral("set"),
            QStringLiteral("interface"),
            adapter_name,
            QStringLiteral("newname=") + new_name};
}

namespace {

/// The text netsh printed, preferring stderr and falling back to stdout.
QString netshDetail(const ProcessResult& result) {
    const QString err = result.std_err.trimmed();
    return err.isEmpty() ? result.std_out.trimmed() : err;
}

/// True when a completed netsh run should be treated as a failure.
///
/// netsh is not dependable about exit codes: it prints an error and can still exit 0. Before these
/// paths were unified this tree carried BOTH rules -- EthernetConfigManager treated a run as failed
/// on `!succeeded() || output contains "error"`, while the diagnostic panel looked at the exit code
/// alone -- so the same netsh outcome was classified two different ways depending on which button
/// the technician pressed. This is the union, i.e. the stricter of the two: adopting it means an
/// operation netsh reports an error for is no longer shown as success anywhere.
///
/// The text half is deliberately a supplement to the exit code, never a substitute for it. netsh
/// localizes its messages, so on a non-English Windows only the exit code will fire -- which is
/// exactly why the exit code is checked first and unconditionally.
bool netshReportedFailure(const ProcessResult& result) {
    if (!result.succeeded()) {
        return true;
    }
    return netshDetail(result).contains(QStringLiteral("error"), Qt::CaseInsensitive);
}

/// Run one netsh command. System32-qualified, never the bare name: CreateProcess searches the
/// current directory ahead of System32, and these commands are privileged, so an unresolvable path
/// is a FAILED operation rather than a PATH-found binary.
AdapterAdminOutcome runAdapterNetsh(const QStringList& args, const QString& success_message) {
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        sak::logError("Cannot resolve the System32 netsh.exe path; adapter administration aborted");
        return {false,
                QStringLiteral("Cannot resolve the System32 netsh.exe path; refusing to run a "
                               "privileged adapter command from an unverified location")};
    }

    const ProcessResult result = sak::runProcess(netsh_exe, args, sak::kTimerNetshWaitMs);
    if (result.timed_out) {
        return {false, QStringLiteral("The netsh adapter command timed out")};
    }
    if (netshReportedFailure(result)) {
        const QString detail = netshDetail(result);
        return {false,
                QStringLiteral("netsh refused the adapter command (changing adapter state needs "
                               "administrator rights; run S.A.K. elevated)%1")
                    .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail)};
    }
    return {true, success_message};
}

/// Run one ipconfig lease command. Same System32 anti-hijack rule as netsh.
AdapterAdminOutcome runLeaseIpconfig(const QStringList& args,
                                     int timeout_ms,
                                     const QString& success_message,
                                     const QString& failure_prefix) {
    const QString ipconfig_exe = sak::system32Path(QStringLiteral("ipconfig.exe"));
    if (ipconfig_exe.isEmpty()) {
        sak::logError("Cannot resolve the System32 ipconfig.exe path; DHCP lease command aborted");
        return {false,
                QStringLiteral("Cannot resolve the System32 ipconfig.exe path; refusing to run a "
                               "lease command from an unverified location")};
    }
    const ProcessResult result = sak::runProcess(ipconfig_exe, args, timeout_ms);
    if (result.timed_out) {
        return {false, QStringLiteral("The ipconfig lease command timed out")};
    }
    if (!result.succeeded()) {
        const QString detail = netshDetail(result);
        return {false,
                failure_prefix + (detail.isEmpty() ? QString() : QStringLiteral(": ") + detail)};
    }
    return {true, success_message};
}

}  // namespace

QString staticIpv4ConfigurationProblem(const QString& address,
                                       const QString& mask,
                                       const QString& gateway) {
    // Each field names ITSELF in the refusal. A single generic message would leave an operator
    // guessing which of three fields to correct.
    if (!isDottedIpv4(address)) {
        return QStringLiteral("'%1' is not a valid IPv4 address").arg(address);
    }
    if (!isDottedIpv4(mask)) {
        return QStringLiteral("'%1' is not a valid dotted IPv4 subnet mask").arg(mask);
    }
    if (!gateway.isEmpty() && !isDottedIpv4(gateway)) {
        return QStringLiteral("'%1' is not a valid IPv4 gateway address").arg(gateway);
    }
    return {};
}

QString staticDnsProblem(const QStringList& servers) {
    if (servers.isEmpty()) {
        return QStringLiteral("No DNS servers were supplied");
    }
    // The WHOLE list is checked before anything is applied. Validating lazily would let a bad
    // third entry stop the sequence after the primary had already replaced the adapter's resolver.
    for (const QString& server : servers) {
        if (!isDottedIpv4(server)) {
            return QStringLiteral("'%1' is not a valid IPv4 DNS server address").arg(server);
        }
    }
    return {};
}

AdapterAdminOutcome setAdapterEnabled(const QString& adapter_name, bool enabled) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, QStringLiteral("No adapter name was supplied")};
    }
    return runAdapterNetsh(adapterAdminStateArgs(adapter_name, enabled),
                           enabled ? QStringLiteral("Enabled adapter '%1'").arg(adapter_name)
                                   : QStringLiteral("Disabled adapter '%1'").arg(adapter_name));
}

AdapterAdminOutcome renameAdapter(const QString& adapter_name, const QString& new_name) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, QStringLiteral("No adapter name was supplied")};
    }
    // Validate BEFORE building the argument vector, so a rejected name never reaches netsh at
    // all rather than being handed over and refused there.
    if (!isValidNewAdapterName(new_name)) {
        return {false,
                QStringLiteral("'%1' is not a usable adapter name: it must be 1-%2 characters, "
                               "must not contain '=', a double quote or a control character, and "
                               "must not begin with '-' or '/'")
                    .arg(new_name)
                    .arg(kMaxAdapterNameLength)};
    }
    const QString trimmed_new_name = new_name.trimmed();
    if (trimmed_new_name.compare(adapter_name, Qt::CaseInsensitive) == 0) {
        return {false, QStringLiteral("Adapter '%1' already has that name").arg(adapter_name)};
    }
    return runAdapterNetsh(
        adapterRenameArgs(adapter_name, trimmed_new_name),
        QStringLiteral("Renamed adapter '%1' to '%2'").arg(adapter_name, trimmed_new_name));
}

// ---------------------------------------------------------------------------
// IPv4 configuration
// ---------------------------------------------------------------------------

QStringList adapterStaticIpv4Args(const QString& adapter_name,
                                  const QString& address,
                                  const QString& mask,
                                  const QString& gateway,
                                  GatewayMetric gateway_metric) {
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("ipv4"),
                        QStringLiteral("set"),
                        QStringLiteral("address"),
                        QStringLiteral("name=") + adapter_name,
                        QStringLiteral("source=static"),
                        QStringLiteral("address=") + address,
                        QStringLiteral("mask=") + mask};
    if (gateway.isEmpty()) {
        // No gateway means no gwmetric: netsh documents gwmetric as settable only alongside a
        // gateway, so emitting it alone would be rejected rather than merely ignored.
        return args;
    }
    args << QStringLiteral("gateway=") + gateway;
    if (gateway_metric == GatewayMetric::PinnedZero) {
        args << QStringLiteral("gwmetric=0");
    }
    return args;
}

QStringList adapterDhcpAddressArgs(const QString& adapter_name) {
    return {QStringLiteral("interface"),
            QStringLiteral("ipv4"),
            QStringLiteral("set"),
            QStringLiteral("address"),
            QStringLiteral("name=") + adapter_name,
            QStringLiteral("source=dhcp")};
}

QStringList adapterDhcpDnsArgs(const QString& adapter_name) {
    return {QStringLiteral("interface"),
            QStringLiteral("ipv4"),
            QStringLiteral("set"),
            QStringLiteral("dnsservers"),
            QStringLiteral("name=") + adapter_name,
            QStringLiteral("source=dhcp")};
}

QStringList adapterStaticPrimaryDnsArgs(const QString& adapter_name,
                                        const QString& address,
                                        DnsRegistration registration) {
    QStringList args = {QStringLiteral("interface"),
                        QStringLiteral("ipv4"),
                        QStringLiteral("set"),
                        QStringLiteral("dnsservers"),
                        QStringLiteral("name=") + adapter_name,
                        QStringLiteral("source=static"),
                        QStringLiteral("address=") + address};
    if (registration == DnsRegistration::PrimarySuffixOnly) {
        args << QStringLiteral("register=primary");
    }
    return args;
}

QStringList adapterAdditionalDnsArgs(const QString& adapter_name,
                                     const QString& address,
                                     int index) {
    return {QStringLiteral("interface"),
            QStringLiteral("ipv4"),
            QStringLiteral("add"),
            QStringLiteral("dnsservers"),
            QStringLiteral("name=") + adapter_name,
            QStringLiteral("address=") + address,
            QStringLiteral("index=") + QString::number(index)};
}

AdapterAdminOutcome setAdapterStaticIpv4(const QString& adapter_name,
                                         const QString& address,
                                         const QString& mask,
                                         const QString& gateway,
                                         AdapterIpv4Dialect dialect) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, QStringLiteral("No adapter name was supplied")};
    }
    const QString problem = staticIpv4ConfigurationProblem(address, mask, gateway);
    if (!problem.isEmpty()) {
        return {false, problem};
    }
    return runAdapterNetsh(
        adapterStaticIpv4Args(adapter_name, address, mask, gateway, dialect.gateway_metric),
        QStringLiteral("Set a static IPv4 configuration on '%1'").arg(adapter_name));
}

DnsApplyOutcome setAdapterStaticDns(const QString& adapter_name,
                                    const QStringList& servers,
                                    DnsRegistration registration) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, false, QStringLiteral("No adapter name was supplied")};
    }
    const QString problem = staticDnsProblem(servers);
    if (!problem.isEmpty()) {
        return {false, false, problem};
    }

    const AdapterAdminOutcome primary =
        runAdapterNetsh(adapterStaticPrimaryDnsArgs(adapter_name, servers.first(), registration),
                        QStringLiteral("Set DNS servers on '%1'").arg(adapter_name));
    if (!primary.succeeded) {
        return {false,
                false,
                QStringLiteral("Failed to set the primary DNS server on '%1'. %2")
                    .arg(adapter_name, primary.message)};
    }

    // The primary is LIVE from here on: the `set dnsservers source=static` above replaced the
    // adapter's ENTIRE DNS list. Every later return therefore reports primary_applied = true, so a
    // failed secondary is surfaced as a partial apply rather than as "nothing happened".
    for (int i = 1; i < servers.size(); ++i) {
        const AdapterAdminOutcome extra = runAdapterNetsh(
            adapterAdditionalDnsArgs(adapter_name, servers.at(i), kPrimaryDnsIndex + i), QString());
        if (!extra.succeeded) {
            return {false,
                    true,
                    QStringLiteral("Partially configured DNS on '%1': primary %2 is live, but "
                                   "adding %3 failed. %4")
                        .arg(adapter_name, servers.first(), servers.at(i), extra.message)};
        }
    }
    return {true, true, primary.message};
}

DhcpApplyOutcome setAdapterDhcpMode(const QString& adapter_name) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, false, QStringLiteral("No adapter name was supplied")};
    }

    const AdapterAdminOutcome address =
        runAdapterNetsh(adapterDhcpAddressArgs(adapter_name),
                        QStringLiteral("Set '%1' to automatic (DHCP)").arg(adapter_name));
    if (!address.succeeded) {
        return {
            false,
            false,
            QStringLiteral("Failed to enable DHCP on '%1'. %2").arg(adapter_name, address.message)};
    }

    // The DNS-to-automatic step gets its OWN failure rule rather than the shared one. netsh reports
    // the benign "DNS is already automatic" no-op as a NON-ZERO exit with a message that never
    // contains "error", so the shared rule would false-fail a restore that has already reached the
    // target state. Treat it as failed only on a genuine netsh error, or when the command could not
    // run at all (non-success AND no output) -- both of those leave DNS pinned to the previous
    // static servers, which is a real partial result.
    const QString netsh_exe = sak::system32Path(QStringLiteral("netsh.exe"));
    if (netsh_exe.isEmpty()) {
        return {false,
                false,
                QStringLiteral("Set '%1' IPv4 to automatic (DHCP), but the System32 netsh.exe path "
                               "could not be resolved for the DNS step")
                    .arg(adapter_name)};
    }
    const ProcessResult dns =
        sak::runProcess(netsh_exe, adapterDhcpDnsArgs(adapter_name), sak::kTimerNetshWaitMs);
    const QString dns_detail = netshDetail(dns);
    const bool dns_failed = dns.timed_out ||
                            dns_detail.contains(QStringLiteral("error"), Qt::CaseInsensitive) ||
                            (!dns.succeeded() && dns_detail.isEmpty());
    if (dns_failed) {
        return {false,
                false,
                QStringLiteral("Set '%1' IPv4 to automatic (DHCP), but DNS could NOT be set to "
                               "automatic -- it is still pinned to the previous servers%2")
                    .arg(adapter_name,
                         dns_detail.isEmpty() ? QString() : QStringLiteral(": ") + dns_detail)};
    }
    return {true,
            true,
            QStringLiteral("Set '%1' to automatic (DHCP), including DNS").arg(adapter_name)};
}

AdapterAdminOutcome releaseAdapterDhcpLease(const QString& adapter_name) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, QStringLiteral("No adapter name was supplied")};
    }
    constexpr int kReleaseTimeoutMs = 10'000;
    return runLeaseIpconfig(
        {QStringLiteral("/release"), adapter_name},
        kReleaseTimeoutMs,
        QStringLiteral("Released the DHCP lease on '%1'").arg(adapter_name),
        QStringLiteral("ipconfig could not release the DHCP lease on '%1'").arg(adapter_name));
}

AdapterAdminOutcome renewAdapterDhcpLease(const QString& adapter_name) {
    if (adapter_name.trimmed().isEmpty()) {
        return {false, QStringLiteral("No adapter name was supplied")};
    }
    // A renew waits on a DHCP server round trip, so it gets a longer budget than a release.
    constexpr int kRenewTimeoutMs = 30'000;
    return runLeaseIpconfig(
        {QStringLiteral("/renew"), adapter_name},
        kRenewTimeoutMs,
        QStringLiteral("Renewed the DHCP lease on '%1'").arg(adapter_name),
        QStringLiteral("ipconfig could not renew the DHCP lease on '%1'").arg(adapter_name));
}

}  // namespace sak
