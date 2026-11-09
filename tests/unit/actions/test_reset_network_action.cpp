// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_reset_network_action.cpp
/// @brief Unit tests for ResetNetworkAction pure seams (Codex B6-12/13):
///        the adapter-reset script must fail closed on a per-adapter error, and
///        a timed-out / non-zero step must count as a failure for the verdict.
///        Fix round S: the whole reset, run against faked processes that answer with what a stock
///        Windows 11 26200 VM printed (tests/fixtures/network_reset). No process starts.

#include "sak/actions/reset_network_action.h"
#include "sak/process_runner.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest/QtTest>

using sak::ResetNetworkAction;

namespace {

QString fixture(const QString& name) {
    QFile file(QDir(QStringLiteral(SAK_TEST_FIXTURE_DIR))
                   .filePath(QStringLiteral("network_reset/") + name));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", qPrintable(name));
    }
    return QString::fromUtf8(file.readAll());
}

sak::ProcessResult answer(int exit_code, const QString& out = {}) {
    sak::ProcessResult result;
    result.exit_code = exit_code;
    result.std_out = out;
    return result;
}

/// A machine whose processes are all faked. The adapters' state moves as the commands that change
/// it run: `netsh int ip reset` takes the settings typed in by hand away, and `set address
/// source=static` puts them back. Anything not modelled answers exit 99, so no real process starts.
struct FakeMachine {
    QString before;
    QString after_reset;
    QString after_put_back;
    QString state;
    bool record_fails{false};
    bool capture_fails_after_reset{false};
    bool reset_done{false};
    int put_back_exit{0};
    QStringList commands;

    [[nodiscard]] sak::ProcessResult capture() const {
        if (record_fails || (capture_fails_after_reset && reset_done)) {
            sak::ProcessResult result;
            result.timed_out = true;
            return result;
        }
        return answer(0, state);
    }

    sak::ProcessResult netshReset(const QString& line) {
        state = after_reset;
        reset_done = true;
        return answer(1,
                      fixture(line == QStringLiteral("int ip reset")
                                  ? QStringLiteral("netsh_int_ip_reset_win11_26200.txt")
                                  : QStringLiteral("netsh_int_ipv6_reset_win11_26200.txt")));
    }

    static sak::ProcessResult firewallExport(const QString& path) {
        QFile rules(path);
        if (rules.open(QIODevice::WriteOnly)) {
            rules.write("rules");
        }
        return answer(0, QStringLiteral("Ok."));
    }

    sak::ProcessResult putBack() {
        if (put_back_exit != 0) {
            return answer(put_back_exit, QStringLiteral("The object already exists."));
        }
        state = after_put_back;
        return answer(0);
    }

    sak::ProcessResult netsh(const QStringList& args) {
        const QString line = args.join(QLatin1Char(' '));
        if (line == QStringLiteral("winsock show catalog")) {
            return answer(0, QStringLiteral("Winsock Catalog Provider Entry\n"));
        }
        if (line == QStringLiteral("int ip reset") || line == QStringLiteral("int ipv6 reset")) {
            return netshReset(line);
        }
        if (args.size() == 3 && args.at(1) == QStringLiteral("export")) {
            return firewallExport(args.at(2));
        }
        if (line.startsWith(QStringLiteral("interface ipv4 set address")) &&
            line.contains(QStringLiteral("source=static"))) {
            return putBack();
        }
        return answer(0, QStringLiteral("Ok."));
    }

    sak::ProcessFaultInjector injector() {
        return [this](const QString& program, const QStringList& args) {
            const QString exe = QFileInfo(program).fileName().toLower();
            commands << exe + QLatin1Char(' ') + args.join(QLatin1Char(' '));
            if (exe == QStringLiteral("powershell.exe")) {
                const QString script = args.isEmpty() ? QString() : args.last();
                if (script.contains(QStringLiteral("sak_network_capture"))) {
                    return std::optional(capture());
                }
                const bool restart = script.contains(QStringLiteral("Restart-NetAdapter"));
                return std::optional(answer(restart ? 0 : 99));
            }
            if (exe == QStringLiteral("netsh.exe")) {
                return std::optional(netsh(args));
            }
            if (exe == QStringLiteral("ipconfig.exe") || exe == QStringLiteral("nbtstat.exe")) {
                return std::optional(answer(0));
            }
            return std::optional(answer(99, QStringLiteral("unexpected program")));
        };
    }

    [[nodiscard]] qsizetype indexOf(const QString& fragment) const {
        for (qsizetype i = 0; i < commands.size(); ++i) {
            if (commands.at(i).contains(fragment)) {
                return i;
            }
        }
        return -1;
    }
};

FakeMachine staticMachine() {
    FakeMachine machine;
    machine.before = fixture(QStringLiteral("capture_vm_static_before.json"));
    machine.after_reset = fixture(QStringLiteral("capture_vm_static_after_reset.json"));
    machine.after_put_back = machine.before;
    machine.state = machine.before;
    return machine;
}

sak::ResetNetworkOptions quickOptions(const QString& folder) {
    sak::ResetNetworkOptions options;
    options.backup_directory = folder;
    options.capture_timeout_ms = 1000;
    options.settle_budget_ms = 60;
    options.settle_poll_ms = 20;
    return options;
}

}  // namespace

class ResetNetworkActionTests : public QObject {
    Q_OBJECT

    using Action = ResetNetworkAction;

private Q_SLOTS:
    // B6-12: Restart-NetAdapter must be forced terminating and exit non-zero on error.
    void buildAdapterResetScript_failsClosed();
    // B6-13: the outcome must treat a timed-out / non-zero step as a failure.
    void stepFailed_truthTable();

    // Fix round S: the whole reset against the VM's real answers.
    void staticAdapterIsPutBackAndTheResetSucceedsWithARestart();
    void noRecordMeansNoReset();
    void dhcpAdapterNeedsNothingPutBack();
    void adaptersStillRestartingIsARestartPendingNotAFailure();
    void aStaticAdapterLeftWithoutItsAddressIsAFailure();
};

void ResetNetworkActionTests::buildAdapterResetScript_failsClosed() {
    const QString script = Action::buildAdapterResetScript();

    // Non-terminating errors are promoted to terminating and caught. The preference must be in
    // force BEFORE the (unguarded) Get-NetAdapter enumeration -- that enumeration carries no
    // -ErrorAction of its own, so the top-level preference alone makes an enumeration failure
    // terminating.
    const qsizetype pref_at = script.indexOf(QStringLiteral("$ErrorActionPreference = 'Stop'"));
    const qsizetype enum_at = script.indexOf(QStringLiteral("Get-NetAdapter"));
    QVERIFY(pref_at >= 0);
    QVERIFY(enum_at > pref_at);

    // The guard flags must be bound to the Restart-NetAdapter invocation ITSELF: moving
    // -ErrorAction Stop onto the Get-NetAdapter enumeration, or dropping -Confirm:$false, leaves
    // the per-adapter restart non-terminating / able to block on a confirmation prompt, while a
    // script-wide substring check stays green.
    const QStringList script_lines = script.split(QLatin1Char('\n'));
    QString restart_line;
    for (const QString& line : script_lines) {
        if (line.contains(QStringLiteral("Restart-NetAdapter"))) {
            QVERIFY2(restart_line.isEmpty(), "expected exactly one Restart-NetAdapter invocation");
            restart_line = line;
        }
    }
    QVERIFY(!restart_line.isEmpty());
    QVERIFY(restart_line.contains(QStringLiteral("-Name $_.Name")));
    QVERIFY(restart_line.contains(QStringLiteral("-Confirm:$false")));
    QVERIFY(restart_line.contains(QStringLiteral("-ErrorAction Stop")));

    // The catch arm must do more than exist and exit 1: it must write the exception message to
    // stderr, because the caller logs std_err and that is the ONLY cause the operator ever sees
    // behind "exit 1".
    const qsizetype catch_at = script.indexOf(QStringLiteral("} catch {"));
    QVERIFY(catch_at >= 0);
    const QString catch_arm = script.mid(catch_at);
    QVERIFY(catch_arm.contains(QStringLiteral("Write-Error $_.Exception.Message")));
    QVERIFY(catch_arm.contains(QStringLiteral("exit 1")));
    // A failure exits non-zero so the caller records it (vs. the old exit-0 pipeline).
    // Pin WHICH arm exits WHICH code: the sole `exit 1` lives inside the catch arm and
    // the sole `exit 0` is the clean path that precedes it. Swapping the two codes in
    // reset_network_action.cpp:46/:49 turns both ordering checks red.
    const qsizetype catch_pos = script.indexOf(QStringLiteral("} catch {"));
    QVERIFY(catch_pos > 0);
    QVERIFY(script.count(QStringLiteral("exit 0")) == 1);
    QVERIFY(script.count(QStringLiteral("exit 1")) == 1);
    QVERIFY(script.indexOf(QStringLiteral("exit 0")) < catch_pos);  // success arm exits 0
    QVERIFY(script.indexOf(QStringLiteral("exit 1")) > catch_pos);  // failure arm exits 1
    // Still actually restarts the up adapters -- and ONLY the up ones. The filter is
    // load-bearing because of the -ErrorAction Stop above: an unfiltered Get-NetAdapter throws on
    // the first Disabled/Disconnected adapter, so the catch exits 1 on a healthy machine AND the
    // remaining real adapters are never restarted.
    QVERIFY(script.contains(QStringLiteral("Restart-NetAdapter -Name $_.Name")));
    QVERIFY(script.contains(
        QStringLiteral("Get-NetAdapter | Where-Object {$_.Status -eq 'Up'} | ForEach-Object {")));
}

void ResetNetworkActionTests::stepFailed_truthTable() {
    QVERIFY(!Action::stepFailed(false, 0));  // clean success
    QVERIFY(Action::stepFailed(true, 0));    // timed out (even with exit 0)
    QVERIFY(Action::stepFailed(false, 1));   // non-zero exit
    QVERIFY(Action::stepFailed(true, 5));    // both
    // NEGATIVE exit codes are the ones that matter most and the only positive value tested was
    // 1: -1 is the fail-closed sentinel of a ProcessResult that never ran, and 0xC0000005
    // arrives as a negative int for a crashed child. A guard written `exit_code > 0` treats both
    // as success and reports a network reset that never happened.
    QVERIFY(Action::stepFailed(false, -1));
    QVERIFY(Action::stepFailed(false, -1'073'741'819));
}

void ResetNetworkActionTests::staticAdapterIsPutBackAndTheResetSucceedsWithARestart() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FakeMachine machine = staticMachine();
    const sak::ScopedProcessFaultInjector injector(machine.injector());
    ResetNetworkAction action(quickOptions(folder.path()));
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY2(result.success, qPrintable(result.message + QLatin1Char('\n') + result.log));
    // Both netsh resets exit 1 on this Windows; each is read as done, with a restart to finish.
    QVERIFY2(result.message.contains(QStringLiteral("REBOOT REQUIRED")),
             qPrintable(result.message));
    QVERIFY(result.message.contains(QStringLiteral("settings typed in by hand were put back")));
    QVERIFY2(result.log.contains(QStringLiteral(
                 "Put back:\n  Ethernet: IPv4 10.0.2.15/24\n  Ethernet: gateway 10.0.2.2")),
             qPrintable(result.log));
    QVERIFY(result.log.contains(
        QStringLiteral("Verification:\n  Ethernet: 10.0.2.15/24, typed in by hand")));
    QVERIFY(result.log.contains(QStringLiteral("TCP/IP reset: 1 protected item was not reset")));
    QVERIFY(!result.log.contains(QStringLiteral("Errors:")));
    // The record is written before anything changes, and the result points at it.
    QVERIFY(QFileInfo(result.output_path).isFile());
    QCOMPARE(QFileInfo(result.output_path).absolutePath(), QDir(folder.path()).absolutePath());
    const qsizetype first_capture = machine.indexOf(QStringLiteral("sak_network_capture"));
    const qsizetype reset_at = machine.indexOf(QStringLiteral("netsh.exe int ip reset"));
    const qsizetype restart_at = machine.indexOf(QStringLiteral("Restart-NetAdapter"));
    const qsizetype put_back_at = machine.indexOf(
        QStringLiteral("netsh.exe interface ipv4 set address name=Ethernet source=static"));
    QVERIFY(first_capture >= 0 && first_capture < reset_at);
    QVERIFY2(put_back_at > restart_at && restart_at > reset_at,
             qPrintable(machine.commands.join(QLatin1Char('\n'))));
}

void ResetNetworkActionTests::noRecordMeansNoReset() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FakeMachine machine = staticMachine();
    machine.record_fails = true;
    const sak::ScopedProcessFaultInjector injector(machine.injector());
    ResetNetworkAction action(quickOptions(folder.path()));
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY(!result.success);
    QCOMPARE(result.message,
             QStringLiteral(
                 "Network reset aborted: the adapters' settings could not be recorded first"));
    QCOMPARE(machine.indexOf(QStringLiteral("netsh.exe")), -1);
    QVERIFY(QDir(folder.path()).isEmpty());
}

void ResetNetworkActionTests::dhcpAdapterNeedsNothingPutBack() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FakeMachine machine;
    machine.before = fixture(QStringLiteral("capture_vm_dhcp_before.json"));
    machine.after_reset = machine.before;
    machine.after_put_back = machine.before;
    machine.state = machine.before;
    const sak::ScopedProcessFaultInjector injector(machine.injector());
    ResetNetworkAction action(quickOptions(folder.path()));
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY2(result.success, qPrintable(result.message + QLatin1Char('\n') + result.log));
    QVERIFY(!result.message.contains(QStringLiteral("put back")));
    QVERIFY(result.message.contains(QStringLiteral("REBOOT REQUIRED")));
    QCOMPARE(machine.indexOf(QStringLiteral("interface ipv4 set")), -1);
    QVERIFY2(result.log.contains(
                 QStringLiteral("Verification:\n  Ethernet: DHCP on, 10.0.2.15/24")),
             qPrintable(result.log));
}

void ResetNetworkActionTests::adaptersStillRestartingIsARestartPendingNotAFailure() {
    // The VM's verify probe timed out while the adapters restarted, and was counted as an issue.
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FakeMachine machine = staticMachine();
    machine.capture_fails_after_reset = true;
    const sak::ScopedProcessFaultInjector injector(machine.injector());
    ResetNetworkAction action(quickOptions(folder.path()));
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY2(result.success, qPrintable(result.message + QLatin1Char('\n') + result.log));
    QVERIFY(result.message.contains(QStringLiteral("REBOOT REQUIRED")));
    QVERIFY2(result.warning.contains(QStringLiteral("still restarting the adapters")),
             qPrintable(result.warning));
    QVERIFY(result.log.contains(QStringLiteral("not checked: the adapters were still restarting")));
    // Windows could not be read, so what was typed in by hand went back in full.
    QVERIFY(machine.indexOf(
                QStringLiteral("interface ipv4 set address name=Ethernet source=static")) >= 0);
    QVERIFY(machine.indexOf(
                QStringLiteral("interface ipv4 set dnsservers name=Ethernet source=static")) >= 0);
}

void ResetNetworkActionTests::aStaticAdapterLeftWithoutItsAddressIsAFailure() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    FakeMachine machine = staticMachine();
    machine.put_back_exit = 1;
    const sak::ScopedProcessFaultInjector injector(machine.injector());
    ResetNetworkAction action(quickOptions(folder.path()));
    action.execute();
    const auto& result = action.lastExecutionResult();
    QVERIFY(!result.success);
    QVERIFY2(result.message.startsWith(QStringLiteral("Network reset completed with 2 issues")),
             qPrintable(result.message));
    // What the check printed and the verdict come from the same reading.
    QVERIFY2(result.log.contains(QStringLiteral("Verification:\n  Ethernet: no usable IPv4 address "
                                                "(only the self-assigned 169.254.")),
             qPrintable(result.log));
    QVERIFY(
        result.log.contains(QStringLiteral("Could not put back IPv4 10.0.2.15/24 on Ethernet")));
    QVERIFY(result.log.contains(result.output_path));
}

QTEST_GUILESS_MAIN(ResetNetworkActionTests)
#include "test_reset_network_action.moc"
