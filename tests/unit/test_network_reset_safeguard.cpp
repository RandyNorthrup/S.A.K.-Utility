// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_reset_safeguard.cpp
/// @brief Reset Network's record, its reading of netsh, its put-back plan and its checks, against
///        the output the 2026-11-09 VM smoke and fix round S captured on a stock Windows 11 26200
///        (tests/fixtures/network_reset). Nothing here starts a process or touches an adapter.
///
/// Fixtures, all from the VirtualBox "Windows 11" VM restored to its snapshot (the evidence is
/// under E:/SAK-Utility-Test-Scratch/fixround-s/vm): netsh_int_*_reset_win11_26200.txt are
/// netsh's stdout byte for byte (each file ends in one line break, not two);
/// capture_vm_static_before.json is the product's capture script run as Technician;
/// capture_vm_static_after_reset.json is the same after both netsh resets and an adapter restart;
/// capture_vm_dhcp_before.json is the adapter on DHCP, with the DNS server VirtualBox NAT passed
/// through from the host's own network replaced by NAT's 10.0.2.3.

#include "sak/network_adapter_admin.h"
#include "sak/network_reset_safeguard.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <optional>

using sak::AdapterSettings;
using sak::NetshResetVerdict;
using sak::NetworkSettingsCapture;
using sak::StaticGateway;

namespace {

QString fixture(const QString& name) {
    QFile file(QDir(QStringLiteral(SAK_TEST_FIXTURE_DIR))
                   .filePath(QStringLiteral("network_reset/") + name));
    if (!file.open(QIODevice::ReadOnly)) {
        qFatal("missing fixture %s", qPrintable(name));
    }
    return QString::fromUtf8(file.readAll());
}

NetworkSettingsCapture capture(const QString& name) {
    auto parsed = sak::parseNetworkCapture(fixture(name));
    if (!parsed) {
        qFatal("fixture %s does not parse: %s", qPrintable(name), qPrintable(parsed.error()));
    }
    return *parsed;
}

AdapterSettings adapter(const QString& name, const QString& status) {
    AdapterSettings settings;
    settings.name = name;
    settings.status = status;
    settings.ipv4.present = true;
    settings.ipv6.present = true;
    return settings;
}

/// An adapter set up by hand, as the VM's Ethernet is in its snapshot.
AdapterSettings staticEthernet() {
    AdapterSettings settings = adapter(QStringLiteral("Ethernet"), QStringLiteral("Up"));
    settings.ipv4.addresses = {QStringLiteral("10.0.2.15/24")};
    settings.ipv4.static_addresses = {QStringLiteral("10.0.2.15/24")};
    settings.ipv4.gateways = {QStringLiteral("10.0.2.2")};
    // Windows chose its metric, as it had on the VM: registry 0, route metric 256.
    settings.ipv4.static_gateways = {StaticGateway{QStringLiteral("10.0.2.2"), 256, true}};
    settings.ipv4.dns_servers = {QStringLiteral("10.0.2.3")};
    settings.ipv4.static_dns_servers = {QStringLiteral("10.0.2.3")};
    return settings;
}

/// What the reset leaves of it: DHCP still off, a self-assigned address, no gateway.
AdapterSettings resetEthernet() {
    AdapterSettings settings = adapter(QStringLiteral("Ethernet"), QStringLiteral("Up"));
    settings.ipv4.addresses = {QStringLiteral("169.254.193.132/16")};
    settings.ipv4.dns_servers = {QStringLiteral("10.0.2.3")};
    settings.ipv4.static_dns_servers = {QStringLiteral("10.0.2.3")};
    return settings;
}

NetworkSettingsCapture captureOf(const QList<AdapterSettings>& adapters) {
    NetworkSettingsCapture result;
    result.adapters = adapters;
    return result;
}

QStringList joinedSteps(const QList<sak::ReapplyStep>& steps) {
    QStringList joined;
    for (const auto& step : steps) {
        joined << step.netsh_args.join(QLatin1Char(' '));
    }
    return joined;
}

/// A clock that moves only when the wait sleeps.
struct FakeClock {
    qint64 now{0};
    int sleeps{0};
};

}  // namespace

class TestNetworkResetSafeguard : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- netsh's answer (S2) ----------------------------------------------
    void realIpResetIsDoneWithARestart();
    void realIpv6ResetIsDoneWithARestart();
    void lineEndingsDoNotMatter();
    void anotherFailureReasonIsAFailure();
    void noRestartNoticeWithExitOneIsAFailure();
    void unrecognisedOutputFailsUnlessExitZero();
    void timeoutIsAFailure();

    // -- the record (S1) --------------------------------------------------
    void realStaticCaptureReadsTheSettingsTypedIn();
    void realDhcpCaptureHasNothingTypedIn();
    void captureScriptAsksForEachRoutePrefixAlone();
    void captureWithoutItsMarkerIsRefused();
    void captureWithABadAddressIsRefused();
    void captureWithANamelessAdapterIsRefused();
    void aOneItemListWrittenAsTextIsRead();
    void backupFileRoundTrips();
    void backupRefusesAnEmptyFolder();

    // -- the confirmation (S1) --------------------------------------------
    void confirmationNamesEachAdapterSetUpByHand();
    void confirmationForDhcpOnlySaysItStays();
    void confirmationNamesTurnedOffAdapters();

    // -- putting back (S1) ------------------------------------------------
    void realResetIsPutBackWithOneSetAddress();
    void unknownStateAfterResetPutsEverythingBack();
    void nothingLostMeansNothingToDo();
    void dhcpTurnedOffIsTurnedBackOn();
    void dhcpIsLeftAloneWhenWindowsCouldNotBeRead();
    void extraAddressesGatewaysAndIpv6GoBack();
    void aPinnedGatewayMetricGoesBackPinned();
    void anAdapterGoneAfterTheResetIsAProblem();
    void anAddressAnotherProgramSetsIsNotPutBack();

    // -- checking (S1, S2) ------------------------------------------------
    void selfAssignedOnlyIsNotUsable();
    void restoredStaticIsUsable();
    void aStaticAdapterNeedsItsOwnAddressBack();
    void dhcpIsUsableEvenWhileWaiting();
    void onlyAdaptersThatWereUpAndOwnedAreChecked();

    // -- bounded wait (S2) ------------------------------------------------
    void waitStopsOnceSettled();
    void waitGivesUpAtItsBudget();
    void waitKeepsTheLastCaptureThatWorked();
    void waitStopsWhenCancelled();
};

// ---------------------------------------------------------------------------
// netsh's answer
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::realIpResetIsDoneWithARestart() {
    // Exit 1 on a stock, elevated Windows 11 26200: 33 items OK, one protected item refused, and
    // the restart notice. That is a reset that worked, with a restart to finish it.
    const auto reading = sak::readNetshReset(
        fixture(QStringLiteral("netsh_int_ip_reset_win11_26200.txt")), 1, false);
    QCOMPARE(reading.verdict, NetshResetVerdict::DoneRestartRequired);
    QCOMPARE(reading.items_reset, 33);
    QCOMPARE(reading.items_denied, 1);
    QVERIFY2(reading.detail.contains(QStringLiteral("1 protected item was not reset")),
             qPrintable(reading.detail));
}

void TestNetworkResetSafeguard::realIpv6ResetIsDoneWithARestart() {
    const auto reading = sak::readNetshReset(
        fixture(QStringLiteral("netsh_int_ipv6_reset_win11_26200.txt")), 1, false);
    QCOMPARE(reading.verdict, NetshResetVerdict::DoneRestartRequired);
    QCOMPARE(reading.items_denied, 1);
}

void TestNetworkResetSafeguard::lineEndingsDoNotMatter() {
    const QString crlf = fixture(QStringLiteral("netsh_int_ip_reset_win11_26200.txt"));
    QString lf = crlf;
    lf.remove(QLatin1Char('\r'));
    QCOMPARE(sak::readNetshReset(lf, 1, false).verdict, NetshResetVerdict::DoneRestartRequired);
    QCOMPARE(sak::readNetshReset(lf, 1, false).items_reset,
             sak::readNetshReset(crlf, 1, false).items_reset);
}

void TestNetworkResetSafeguard::anotherFailureReasonIsAFailure() {
    QString output = fixture(QStringLiteral("netsh_int_ip_reset_win11_26200.txt"));
    output.replace(QStringLiteral("Access is denied."),
                   QStringLiteral("The system cannot find the file specified."));
    const auto reading = sak::readNetshReset(output, 1, false);
    QCOMPARE(reading.verdict, NetshResetVerdict::Failed);
    QVERIFY(reading.detail.contains(QStringLiteral("cannot find the file")));
    // Even with exit 0, an item that failed for another reason is not a reset that worked.
    QCOMPARE(sak::readNetshReset(output, 0, false).verdict, NetshResetVerdict::Failed);
}

void TestNetworkResetSafeguard::noRestartNoticeWithExitOneIsAFailure() {
    QString output = fixture(QStringLiteral("netsh_int_ip_reset_win11_26200.txt"));
    output.remove(QStringLiteral("Restart the computer to complete this action."));
    QCOMPARE(sak::readNetshReset(output, 1, false).verdict, NetshResetVerdict::Failed);
}

void TestNetworkResetSafeguard::unrecognisedOutputFailsUnlessExitZero() {
    // Another language: nothing is recognised, so only the exit code can decide.
    const QString german = QStringLiteral(
        "Zuruecksetzen von Global, OK!\r\nZuruecksetzen von , Fehler.\r\nZugriff verweigert\r\n"
        "Starten Sie den Computer neu, um diesen Vorgang abzuschliessen.\r\n");
    const auto failed = sak::readNetshReset(german, 1, false);
    QCOMPARE(failed.verdict, NetshResetVerdict::Failed);
    QVERIFY(failed.detail.contains(QStringLiteral("not recognised")));
    QCOMPARE(sak::readNetshReset(german, 0, false).verdict, NetshResetVerdict::Done);
    QCOMPARE(sak::readNetshReset(QString(), 1, false).verdict, NetshResetVerdict::Failed);
}

void TestNetworkResetSafeguard::timeoutIsAFailure() {
    const auto reading =
        sak::readNetshReset(fixture(QStringLiteral("netsh_int_ip_reset_win11_26200.txt")), 0, true);
    QCOMPARE(reading.verdict, NetshResetVerdict::Failed);
    QCOMPARE(reading.detail, QStringLiteral("timed out"));
}

// ---------------------------------------------------------------------------
// The record
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::realStaticCaptureReadsTheSettingsTypedIn() {
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_static_before.json"));
    const AdapterSettings* ethernet = before.find(QStringLiteral("ethernet"));
    QVERIFY(ethernet != nullptr);
    QVERIFY(ethernet->isUp());
    QVERIFY(!ethernet->ipv4.dhcp);
    QCOMPARE(ethernet->ipv4.static_addresses, QStringList{QStringLiteral("10.0.2.15/24")});
    QCOMPARE(ethernet->ipv4.static_gateways.size(), 1);
    QCOMPARE(ethernet->ipv4.static_gateways.first().next_hop, QStringLiteral("10.0.2.2"));
    QVERIFY(ethernet->ipv4.static_gateways.first().automatic);
    QCOMPARE(ethernet->ipv4.static_dns_servers, QStringList{QStringLiteral("10.0.2.3")});
    QVERIFY(ethernet->hasStaticSettings());
    // The router-advertised IPv6 addresses are there, and none of them counts as typed in.
    QVERIFY(!ethernet->ipv6.addresses.isEmpty());
    QVERIFY(ethernet->ipv6.static_addresses.isEmpty());
    QVERIFY(!ethernet->ipv6.hasStaticSettings());
}

void TestNetworkResetSafeguard::realDhcpCaptureHasNothingTypedIn() {
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_dhcp_before.json"));
    const AdapterSettings* ethernet = before.find(QStringLiteral("Ethernet"));
    QVERIFY(ethernet != nullptr);
    QVERIFY(ethernet->ipv4.dhcp);
    QVERIFY(ethernet->ipv4.static_addresses.isEmpty());
    QVERIFY(ethernet->ipv4.static_dns_servers.isEmpty());
    QVERIFY(!ethernet->hasStaticSettings());
}

void TestNetworkResetSafeguard::captureScriptAsksForEachRoutePrefixAlone() {
    // Given both prefixes, Get-NetRoute answers ObjectNotFound for the one with no route, and under
    // Stop that threw the other's answer away: the VM's static gateway went unrecorded, so a reset
    // would have put the address back without it.
    const QStringList lines = sak::networkCaptureScript().split(QLatin1Char('\n'));
    int route_queries = 0;
    for (const QString& line : lines) {
        if (!line.contains(QStringLiteral("Get-NetRoute"))) {
            continue;
        }
        ++route_queries;
        QVERIFY2(line.count(QStringLiteral("/0'")) == 1, qPrintable(line));
    }
    QCOMPARE(route_queries, 4);
    // Read-only: nothing in it sets, adds, removes or resets anything.
    for (const QString& verb : {QStringLiteral("Set-"),
                                QStringLiteral("New-Net"),
                                QStringLiteral("Remove-"),
                                QStringLiteral("netsh"),
                                QStringLiteral("Restart-")}) {
        QVERIFY2(!sak::networkCaptureScript().contains(verb), qPrintable(verb));
    }
}

void TestNetworkResetSafeguard::captureWithoutItsMarkerIsRefused() {
    QVERIFY(!sak::parseNetworkCapture(QStringLiteral("{\"adapters\":[]}")).has_value());
    QVERIFY(!sak::parseNetworkCapture(QStringLiteral("not json")).has_value());
    QVERIFY(!sak::parseNetworkCapture(QString()).has_value());
    QVERIFY(sak::parseNetworkCapture(QStringLiteral("{\"sak_network_capture\":1,\"adapters\":[]}"))
                .has_value());
}

void TestNetworkResetSafeguard::captureWithABadAddressIsRefused() {
    QString json = fixture(QStringLiteral("capture_vm_static_before.json"));
    json.replace(QStringLiteral("\"10.0.2.15/24\""), QStringLiteral("\"10.0.2/24\""));
    const auto parsed = sak::parseNetworkCapture(json);
    QVERIFY(!parsed.has_value());
    QVERIFY2(parsed.error().contains(QStringLiteral("10.0.2/24")), qPrintable(parsed.error()));
    QString gateway = fixture(QStringLiteral("capture_vm_static_before.json"));
    gateway.replace(QStringLiteral("\"next_hop\":\"10.0.2.2\""),
                    QStringLiteral("\"next_hop\":\"gw\""));
    QVERIFY(!sak::parseNetworkCapture(gateway).has_value());
}

void TestNetworkResetSafeguard::captureWithANamelessAdapterIsRefused() {
    QString json = fixture(QStringLiteral("capture_vm_static_before.json"));
    json.replace(QStringLiteral("\"name\":\"Ethernet\""), QStringLiteral("\"name\":\" \""));
    QVERIFY(!sak::parseNetworkCapture(json).has_value());
}

void TestNetworkResetSafeguard::aOneItemListWrittenAsTextIsRead() {
    const QString json = QStringLiteral(
        "{\"sak_network_capture\":1,\"adapters\":[{\"name\":\"Ethernet\",\"status\":\"Up\","
        "\"ipv4\":{\"present\":true,\"dhcp\":false,\"addresses\":\"10.0.0.5/8\","
        "\"static_addresses\":\"10.0.0.5/8\",\"gateways\":null,\"static_gateways\":"
        "{\"next_hop\":\"10.0.0.1\",\"metric\":0},\"dns_servers\":[],\"static_dns_servers\":"
        "\"10.0.0.1\"},\"ipv6\":{\"present\":false}}]}");
    const auto parsed = sak::parseNetworkCapture(json);
    QVERIFY2(parsed.has_value(), qPrintable(parsed ? QString() : parsed.error()));
    const AdapterSettings& ethernet = parsed->adapters.first();
    QCOMPARE(ethernet.ipv4.static_addresses, QStringList{QStringLiteral("10.0.0.5/8")});
    QCOMPARE(ethernet.ipv4.static_gateways.first().next_hop, QStringLiteral("10.0.0.1"));
    QCOMPARE(ethernet.ipv4.static_dns_servers, QStringList{QStringLiteral("10.0.0.1")});
}

void TestNetworkResetSafeguard::backupFileRoundTrips() {
    QTemporaryDir folder;
    QVERIFY(folder.isValid());
    NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_static_before.json"));
    before.captured_at = QStringLiteral("2026-11-09T12:00:00");
    before.computer_name = QStringLiteral("VM");
    const auto saved = sak::saveNetworkCapture(before, folder.path());
    QVERIFY2(saved.has_value(), qPrintable(saved ? QString() : saved.error()));
    QVERIFY(
        QFileInfo(*saved).fileName().startsWith(QStringLiteral("network-settings-before-reset-")));
    QFile file(*saved);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto reread = sak::parseNetworkCapture(QString::fromUtf8(file.readAll()));
    QVERIFY2(reread.has_value(), qPrintable(reread ? QString() : reread.error()));
    QCOMPARE(reread->adapters.size(), before.adapters.size());
    const AdapterSettings* ethernet = reread->find(QStringLiteral("Ethernet"));
    QVERIFY(ethernet != nullptr);
    QCOMPARE(ethernet->ipv4.static_gateways,
             before.find(QStringLiteral("Ethernet"))->ipv4.static_gateways);
    QCOMPARE(ethernet->ipv4.static_dns_servers, QStringList{QStringLiteral("10.0.2.3")});
}

void TestNetworkResetSafeguard::backupRefusesAnEmptyFolder() {
    QVERIFY(!sak::saveNetworkCapture(captureOf({staticEthernet()}), QString()).has_value());
}

// ---------------------------------------------------------------------------
// The confirmation
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::confirmationNamesEachAdapterSetUpByHand() {
    const QString detail =
        sak::resetConfirmationDetail(capture(QStringLiteral("capture_vm_static_before.json")));
    QVERIFY2(detail.contains(
                 QStringLiteral("Ethernet: IPv4 10.0.2.15/24, gateway 10.0.2.2, DNS 10.0.2.3")),
             qPrintable(detail));
    QVERIFY(detail.contains(QStringLiteral("The reset clears settings typed in by hand")));
    QVERIFY(detail.contains(QStringLiteral("puts them back after the reset")));
}

void TestNetworkResetSafeguard::confirmationForDhcpOnlySaysItStays() {
    const QString detail =
        sak::resetConfirmationDetail(capture(QStringLiteral("capture_vm_dhcp_before.json")));
    QVERIFY2(!detail.contains(QStringLiteral("typed in by hand")), qPrintable(detail));
    QVERIFY(detail.contains(QStringLiteral("(DHCP) keep doing so")));
}

void TestNetworkResetSafeguard::confirmationNamesTurnedOffAdapters() {
    AdapterSettings off = adapter(QStringLiteral("Ethernet 2"), QStringLiteral("Disabled"));
    off.ipv4.present = false;
    off.ipv6.present = false;
    const QString detail = sak::resetConfirmationDetail(captureOf({staticEthernet(), off}));
    QVERIFY2(detail.contains(QStringLiteral("Turned off: Ethernet 2.")), qPrintable(detail));
    QVERIFY(!detail.contains(QStringLiteral("Ethernet 2:")));
}

// ---------------------------------------------------------------------------
// Putting back
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::realResetIsPutBackWithOneSetAddress() {
    // The real VM states: before (static), and after `netsh int ip reset` plus an adapter restart
    // (DHCP still off, only 169.254.x.x, no gateway; the static DNS survived). The address goes
    // back with no gateway, and the gateway as a route with no metric: Windows chose its metric
    // before, and only that form leaves it Windows' choice (given none, `set address` pins 1).
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_static_before.json"));
    const NetworkSettingsCapture after =
        capture(QStringLiteral("capture_vm_static_after_reset.json"));
    QVERIFY(before.find(QStringLiteral("Ethernet"))->ipv4.static_gateways.first().automatic);
    QStringList problems;
    const auto steps = sak::planSettingsReapply(before, after, &problems);
    QVERIFY(problems.isEmpty());
    QCOMPARE(joinedSteps(steps),
             (QStringList{QStringLiteral("interface ipv4 set address name=Ethernet source=static "
                                         "address=10.0.2.15 mask=255.255.255.0 gateway=none"),
                          QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 "
                                         "interface=Ethernet nexthop=10.0.2.2")}));
    QCOMPARE(steps.first().adapter, QStringLiteral("Ethernet"));
    QCOMPARE(steps.first().description, QStringLiteral("IPv4 10.0.2.15/24"));
    QCOMPARE(steps.last().description, QStringLiteral("gateway 10.0.2.2"));
}

void TestNetworkResetSafeguard::unknownStateAfterResetPutsEverythingBack() {
    const auto steps =
        sak::planSettingsReapply(captureOf({staticEthernet()}), std::nullopt, nullptr);
    QCOMPARE(joinedSteps(steps),
             (QStringList{QStringLiteral("interface ipv4 set address name=Ethernet source=static "
                                         "address=10.0.2.15 mask=255.255.255.0 gateway=none"),
                          QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 "
                                         "interface=Ethernet nexthop=10.0.2.2"),
                          QStringLiteral("interface ipv4 set dnsservers name=Ethernet "
                                         "source=static address=10.0.2.3")}));
}

void TestNetworkResetSafeguard::nothingLostMeansNothingToDo() {
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_static_before.json"));
    QVERIFY(sak::planSettingsReapply(before, before, nullptr).isEmpty());
    const NetworkSettingsCapture dhcp = capture(QStringLiteral("capture_vm_dhcp_before.json"));
    QVERIFY(sak::planSettingsReapply(dhcp, dhcp, nullptr).isEmpty());
}

void TestNetworkResetSafeguard::dhcpTurnedOffIsTurnedBackOn() {
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_dhcp_before.json"));
    NetworkSettingsCapture after = before;
    after.adapters.first().ipv4.dhcp = false;
    for (AdapterSettings& item : after.adapters) {
        item.ipv4.dhcp = false;
    }
    const auto steps = sak::planSettingsReapply(before, after, nullptr);
    QVERIFY(joinedSteps(steps).contains(
        QStringLiteral("interface ipv4 set address name=Ethernet source=dhcp")));
}

void TestNetworkResetSafeguard::dhcpIsLeftAloneWhenWindowsCouldNotBeRead() {
    // netsh refuses "turn DHCP on" where it is already on, and the reset does not turn it off.
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_dhcp_before.json"));
    QVERIFY(sak::planSettingsReapply(before, std::nullopt, nullptr).isEmpty());
}

void TestNetworkResetSafeguard::extraAddressesGatewaysAndIpv6GoBack() {
    AdapterSettings before = staticEthernet();
    before.ipv4.static_addresses << QStringLiteral("10.0.2.16/24");
    before.ipv4.static_gateways << StaticGateway{QStringLiteral("10.0.2.4"), 50};
    before.ipv6.static_addresses = {QStringLiteral("fd00::5/64")};
    before.ipv6.static_gateways = {StaticGateway{QStringLiteral("fd00::1"), 256}};
    before.ipv6.static_dns_servers = {QStringLiteral("fd00::53"), QStringLiteral("fd00::54")};
    const auto steps = joinedSteps(
        sak::planSettingsReapply(captureOf({before}), captureOf({resetEthernet()}), nullptr));
    QCOMPARE(steps,
             (QStringList{
                 QStringLiteral("interface ipv4 set address name=Ethernet source=static "
                                "address=10.0.2.15 mask=255.255.255.0 gateway=none"),
                 QStringLiteral("interface ipv4 add address name=Ethernet address=10.0.2.16 "
                                "mask=255.255.255.0"),
                 QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 interface=Ethernet "
                                "nexthop=10.0.2.2"),
                 QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 interface=Ethernet "
                                "nexthop=10.0.2.4 metric=50"),
                 QStringLiteral("interface ipv6 add address interface=Ethernet address=fd00::5/64"),
                 QStringLiteral("interface ipv6 add route prefix=::/0 interface=Ethernet "
                                "nexthop=fd00::1 metric=256"),
                 QStringLiteral("interface ipv6 set dnsservers name=Ethernet source=static "
                                "address=fd00::53"),
                 QStringLiteral("interface ipv6 add dnsservers name=Ethernet address=fd00::54 "
                                "index=2")}));
    // When the first address survived, only what is missing goes back.
    AdapterSettings partial = resetEthernet();
    partial.ipv4.addresses = {QStringLiteral("10.0.2.15/24")};
    partial.ipv4.static_addresses = {QStringLiteral("10.0.2.15/24")};
    const auto missing =
        joinedSteps(sak::planSettingsReapply(captureOf({before}), captureOf({partial}), nullptr));
    QCOMPARE(missing.first(),
             QStringLiteral("interface ipv4 add address name=Ethernet address=10.0.2.16 "
                            "mask=255.255.255.0"));
    QVERIFY(
        missing.contains(QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 "
                                        "interface=Ethernet nexthop=10.0.2.2")));
}

void TestNetworkResetSafeguard::aPinnedGatewayMetricGoesBackPinned() {
    AdapterSettings before = staticEthernet();
    before.ipv4.static_gateways = {StaticGateway{QStringLiteral("10.0.2.2"), 10, false}};
    QCOMPARE(joinedSteps(sak::planSettingsReapply(
                 captureOf({before}), captureOf({resetEthernet()}), nullptr)),
             (QStringList{QStringLiteral("interface ipv4 set address name=Ethernet source=static "
                                         "address=10.0.2.15 mask=255.255.255.0 gateway=none"),
                          QStringLiteral("interface ipv4 add route prefix=0.0.0.0/0 "
                                         "interface=Ethernet nexthop=10.0.2.2 metric=10")}));
}

void TestNetworkResetSafeguard::anAdapterGoneAfterTheResetIsAProblem() {
    QStringList problems;
    const auto steps =
        sak::planSettingsReapply(captureOf({staticEthernet()}), captureOf({}), &problems);
    QVERIFY(steps.isEmpty());
    QCOMPARE(problems.size(), 1);
    QVERIFY2(problems.first().contains(QStringLiteral("Ethernet is gone after the reset")),
             qPrintable(problems.first()));
    QVERIFY(problems.first().contains(QStringLiteral("IPv4 10.0.2.15/24")));
}

void TestNetworkResetSafeguard::anAddressAnotherProgramSetsIsNotPutBack() {
    // Hyper-V's Default Switch: DHCP off and an address, but only in the active store.
    AdapterSettings hyperv = adapter(QStringLiteral("vEthernet (Default Switch)"),
                                     QStringLiteral("Up"));
    hyperv.ipv4.addresses = {QStringLiteral("172.20.48.1/20")};
    AdapterSettings gone = hyperv;
    gone.ipv4.addresses.clear();
    QVERIFY(sak::planSettingsReapply(captureOf({hyperv}), captureOf({gone}), nullptr).isEmpty());
    QVERIFY(sak::checkAdaptersAfterReset(captureOf({hyperv}), captureOf({gone})).isEmpty());
}

// ---------------------------------------------------------------------------
// Checking
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::selfAssignedOnlyIsNotUsable() {
    const auto checks =
        sak::checkAdaptersAfterReset(capture(QStringLiteral("capture_vm_static_before.json")),
                                     capture(QStringLiteral("capture_vm_static_after_reset.json")));
    QCOMPARE(checks.size(), 1);
    QVERIFY(!checks.first().usable);
    QVERIFY(!sak::allAdaptersUsable(checks));
    QVERIFY2(checks.first().detail.contains(QStringLiteral("only the self-assigned 169.254.")),
             qPrintable(checks.first().detail));
    QVERIFY(checks.first().detail.contains(
        QStringLiteral("10.0.2.15/24 typed in by hand is not back")));
}

void TestNetworkResetSafeguard::restoredStaticIsUsable() {
    const auto checks = sak::checkAdaptersAfterReset(captureOf({staticEthernet()}),
                                                     captureOf({staticEthernet()}));
    QCOMPARE(checks.size(), 1);
    QVERIFY(checks.first().usable);
    QCOMPARE(checks.first().detail, QStringLiteral("10.0.2.15/24, typed in by hand"));
    QVERIFY(sak::allAdaptersUsable(checks));
}

void TestNetworkResetSafeguard::aStaticAdapterNeedsItsOwnAddressBack() {
    // Each half on its own is a failure. Back in the store, but Windows is using only a
    // self-assigned address (another device took it, say):
    AdapterSettings stored_only = resetEthernet();
    stored_only.ipv4.static_addresses = {QStringLiteral("10.0.2.15/24")};
    const auto stored = sak::checkAdaptersAfterReset(captureOf({staticEthernet()}),
                                                     captureOf({stored_only}));
    QCOMPARE(stored.size(), 1);
    QVERIFY2(!stored.first().usable, qPrintable(stored.first().detail));
    // A usable address, but not the one typed in by hand:
    AdapterSettings other = resetEthernet();
    other.ipv4.addresses = {QStringLiteral("10.0.2.99/24")};
    const auto replaced = sak::checkAdaptersAfterReset(captureOf({staticEthernet()}),
                                                       captureOf({other}));
    QCOMPARE(replaced.size(), 1);
    QVERIFY2(!replaced.first().usable, qPrintable(replaced.first().detail));
    QVERIFY(replaced.first().detail.contains(
        QStringLiteral("10.0.2.15/24 typed in by hand is not back")));
}

void TestNetworkResetSafeguard::dhcpIsUsableEvenWhileWaiting() {
    const NetworkSettingsCapture before = capture(QStringLiteral("capture_vm_dhcp_before.json"));
    NetworkSettingsCapture waiting = before;
    for (AdapterSettings& item : waiting.adapters) {
        item.ipv4.addresses = {QStringLiteral("169.254.7.7/16")};
    }
    const auto checks = sak::checkAdaptersAfterReset(before, waiting);
    QCOMPARE(checks.size(), 1);
    QVERIFY(checks.first().usable);
    QCOMPARE(checks.first().detail, QStringLiteral("DHCP on, waiting for an address"));
    NetworkSettingsCapture off = before;
    for (AdapterSettings& item : off.adapters) {
        item.ipv4.dhcp = false;
    }
    QVERIFY(!sak::allAdaptersUsable(sak::checkAdaptersAfterReset(before, off)));
}

void TestNetworkResetSafeguard::onlyAdaptersThatWereUpAndOwnedAreChecked() {
    AdapterSettings unplugged = staticEthernet();
    unplugged.name = QStringLiteral("Ethernet 3");
    unplugged.status = QStringLiteral("Disconnected");
    AdapterSettings wifi = adapter(QStringLiteral("Wi-Fi"), QStringLiteral("Up"));
    wifi.ipv4.dhcp = true;
    const auto checks = sak::checkAdaptersAfterReset(captureOf({unplugged, wifi}),
                                                     captureOf({resetEthernet(), wifi}));
    QCOMPARE(checks.size(), 1);
    QCOMPARE(checks.first().adapter, QStringLiteral("Wi-Fi"));
    const auto missing = sak::checkAdaptersAfterReset(captureOf({wifi}), captureOf({}));
    QCOMPARE(missing.size(), 1);
    QVERIFY(!missing.first().usable);
    QCOMPARE(missing.first().detail, QStringLiteral("not found after the reset"));
}

// ---------------------------------------------------------------------------
// Bounded wait
// ---------------------------------------------------------------------------

void TestNetworkResetSafeguard::waitStopsOnceSettled() {
    FakeClock clock;
    int calls = 0;
    const sak::SettleProbe probe{
        .capture = [&calls]() -> std::expected<NetworkSettingsCapture, QString> {
            ++calls;
            if (calls < 3) {
                return std::unexpected(QStringLiteral("Reading the adapters' settings timed out"));
            }
            return captureOf({staticEthernet()});
        },
        .now_ms = [&clock]() { return clock.now; },
        .sleep_ms =
            [&clock](int ms) {
                clock.now += ms;
                ++clock.sleeps;
            },
        .cancelled = []() { return false; }};
    const auto result = sak::waitForSettledNetwork(
        probe, [](const NetworkSettingsCapture&) { return true; }, 90'000, 3000);
    QVERIFY(result.settled);
    QCOMPARE(result.attempts, 3);
    QCOMPARE(clock.sleeps, 2);
    QVERIFY(result.capture.has_value());
    QVERIFY(result.last_error.isEmpty());
}

void TestNetworkResetSafeguard::waitGivesUpAtItsBudget() {
    FakeClock clock;
    const sak::SettleProbe probe{.capture = []() -> std::expected<NetworkSettingsCapture, QString> {
                                     return std::unexpected(QStringLiteral(
                                         "Reading the adapters' settings timed out"));
                                 },
                                 .now_ms = [&clock]() { return clock.now; },
                                 .sleep_ms =
                                     [&clock](int ms) {
                                         clock.now += ms;
                                         ++clock.sleeps;
                                     },
                                 .cancelled = []() { return false; }};
    const auto result = sak::waitForSettledNetwork(
        probe, [](const NetworkSettingsCapture&) { return true; }, 9000, 3000);
    QVERIFY(!result.settled);
    QVERIFY(!result.capture.has_value());
    QCOMPARE(result.last_error, QStringLiteral("Reading the adapters' settings timed out"));
    // Attempts at 0, 3, 6 and 9 s; another sleep would end past the budget, so it stops there.
    QCOMPARE(result.attempts, 4);
    QVERIFY(clock.now <= 9000);
}

void TestNetworkResetSafeguard::waitKeepsTheLastCaptureThatWorked() {
    FakeClock clock;
    const sak::SettleProbe probe{.capture = []() -> std::expected<NetworkSettingsCapture, QString> {
                                     return captureOf({resetEthernet()});
                                 },
                                 .now_ms = [&clock]() { return clock.now; },
                                 .sleep_ms = [&clock](int ms) { clock.now += ms; },
                                 .cancelled = []() { return false; }};
    const NetworkSettingsCapture before = captureOf({staticEthernet()});
    const auto result = sak::waitForSettledNetwork(
        probe,
        [&before](const NetworkSettingsCapture& after) {
            return sak::allAdaptersUsable(sak::checkAdaptersAfterReset(before, after));
        },
        6000,
        3000);
    QVERIFY(!result.settled);
    QVERIFY(result.capture.has_value());
    QCOMPARE(result.capture->adapters.first().ipv4.addresses,
             QStringList{QStringLiteral("169.254.193.132/16")});
}

void TestNetworkResetSafeguard::waitStopsWhenCancelled() {
    FakeClock clock;
    const sak::SettleProbe probe{.capture = []() -> std::expected<NetworkSettingsCapture, QString> {
                                     return std::unexpected(QStringLiteral("busy"));
                                 },
                                 .now_ms = [&clock]() { return clock.now; },
                                 .sleep_ms = [&clock](int ms) { clock.now += ms; },
                                 .cancelled = []() { return true; }};
    const auto result = sak::waitForSettledNetwork(
        probe, [](const NetworkSettingsCapture&) { return true; }, 90'000, 3000);
    QCOMPARE(result.attempts, 1);
}

QTEST_GUILESS_MAIN(TestNetworkResetSafeguard)
#include "test_network_reset_safeguard.moc"
