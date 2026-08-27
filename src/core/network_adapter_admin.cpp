// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_adapter_admin.cpp
/// @brief Enable/disable/rename a network adapter through a System32-qualified netsh.

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

}  // namespace

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

/// Run one netsh adapter-administration command. System32-qualified, never the bare name:
/// CreateProcess searches the current directory ahead of System32, and these commands are
/// privileged, so an unresolvable path is a FAILED operation rather than a PATH-found binary.
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
    if (!result.succeeded()) {
        const QString detail = result.std_err.trimmed().isEmpty() ? result.std_out.trimmed()
                                                                  : result.std_err.trimmed();
        return {false,
                QStringLiteral("netsh refused the adapter command (changing adapter state needs "
                               "administrator rights; run S.A.K. elevated)%1")
                    .arg(detail.isEmpty() ? QString() : QStringLiteral(": ") + detail)};
    }
    return {true, success_message};
}

}  // namespace

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

}  // namespace sak
