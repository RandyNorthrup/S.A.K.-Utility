// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_adapter_admin.cpp
/// @brief Unit tests for the pure half of adapter administration: new-name validation,
///        resolution against the system's own adapter list, and the exact netsh argument
///        vectors. None of these touch an adapter, so they run identically on a machine with
///        no network hardware.

#include "sak/network_adapter_admin.h"
#include "sak/process_runner.h"

#include <QFileInfo>
#include <QtTest/QtTest>

class TestNetworkAdapterAdmin : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- isValidNewAdapterName -------------------------------------------
    void newName_acceptsOrdinaryNames();
    void newName_rejectsEmptyAndWhitespace();
    void newName_rejectsEqualsSign();
    void newName_rejectsQuoteAndControlBytes();
    void newName_rejectsOptionLookalikes();
    void newName_rejectsOverLengthCap();

    // -- resolveAdapterName ----------------------------------------------
    void resolve_returnsSystemSpellingCaseInsensitively();
    void resolve_refusesUnknownName();
    void resolve_refusesPartialMatch();
    void resolve_refusesEmptyRequest();

    // -- argument vectors -------------------------------------------------
    void stateArgs_enableAndDisableDifferOnlyInTheAdminToken();
    void stateArgs_passTheNameAsItsOwnArgument();
    void renameArgs_buildTheNewnameToken();

    // -- isDottedIpv4 -----------------------------------------------------
    void ipv4_acceptsCompleteDottedQuads();
    void ipv4_rejectsAbbreviatedInetAtonForms();
    void ipv4_rejectsPartialAndDecoratedForms();

    // -- IPv4 argument vectors --------------------------------------------
    void staticIpArgs_buildTheNamedTagForm();
    void gatewayRoute_isAutomaticUnlessAMetricIsGiven();
    void staticIp_setsTheAddressThenTheGatewayAsAnAutomaticRoute();
    void dhcpArgs_addressAndDnsDifferOnlyInTheSubcommand();
    void dnsArgs_registerTokenIsTheOnlyDialectDifference();
    void dnsArgs_secondariesStartAtIndexTwo();
    void ipv4Dialects_differOnlyInRegistration();

    // -- fail-closed guards, reachable with no adapter present -------------
    void staticIp_refusesMalformedAddresses();
    void staticDns_refusesEmptyListAndMalformedServers();
    void adapterOps_refuseAnEmptyAdapterName();

    // -- putting settings back after a network reset ------------------------
    void selfAssigned_isOnly169dot254();
    void ipv6Literal_refusesZonesPrefixesAndIpv4();
    void restoreGateway_pinsOnlyWhatWasPinned();
    void restoreBuilders_addAddressesGatewaysAndDnsPerFamily();
};

// ---------------------------------------------------------------------------
// isValidNewAdapterName
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::newName_acceptsOrdinaryNames() {
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Ethernet")));
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Wi-Fi")));       // interior hyphen is fine
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Ethernet 2")));  // spaces are legitimate
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Lab NIC (front port)")));
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("net_1.2")));
    // A name that is only just inside the cap must still be accepted, so the boundary is pinned
    // from both directions (the over-cap case is its own test).
    QVERIFY(sak::isValidNewAdapterName(QString(sak::kMaxAdapterNameLength, QLatin1Char('a'))));
}

void TestNetworkAdapterAdmin::newName_rejectsEmptyAndWhitespace() {
    QVERIFY(!sak::isValidNewAdapterName(QString()));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("")));
    // Whitespace-only is empty once trimmed. Accepting it would rename an adapter to a name
    // nobody can type or see.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("   ")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("\t\r\n")));
}

void TestNetworkAdapterAdmin::newName_rejectsEqualsSign() {
    // THE reason this validation exists. The rename is issued as the single argument
    // `newname=<value>`; a value containing '=' yields `newname=a=b`, which netsh's own parser
    // is free to split differently than intended. Nothing here is about shell quoting -- the
    // arguments never reach a shell.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("a=b")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("Ethernet=2")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("=leading")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("trailing=")));
    // Control: the same names without the '=' are accepted, so the refusals above are the
    // equals rule and not some other property of these strings.
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("ab")));
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Ethernet2")));
}

void TestNetworkAdapterAdmin::newName_rejectsQuoteAndControlBytes() {
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("say \"hi\"")));
    // A control byte would also corrupt any log line carrying the name, and a NUL in particular
    // makes the whole file read as binary to grep-based tooling.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("Ether\x01net")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("Ether\x7Fnet")));
    QVERIFY(!sak::isValidNewAdapterName(QString(QChar(QChar::Null))));
    // An interior newline is a control byte too -- and trimming only removes the outer ones.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("Ether\nnet")));
}

void TestNetworkAdapterAdmin::newName_rejectsOptionLookalikes() {
    // netsh would read a leading '-' or '/' as an option rather than as a name.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("-Ethernet")));
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("/Ethernet")));
    // Leading whitespace does not launder it: the check runs on the TRIMMED value, so the
    // hyphen is still leading.
    QVERIFY(!sak::isValidNewAdapterName(QStringLiteral("  -Ethernet")));
    // A hyphen or slash that is not leading is ordinary text.
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("Wi-Fi")));
    QVERIFY(sak::isValidNewAdapterName(QStringLiteral("lab/2")));
}

void TestNetworkAdapterAdmin::newName_rejectsOverLengthCap() {
    QVERIFY(!sak::isValidNewAdapterName(QString(sak::kMaxAdapterNameLength + 1, QLatin1Char('a'))));
}

// ---------------------------------------------------------------------------
// resolveAdapterName
// ---------------------------------------------------------------------------

namespace {

const QStringList kAdapters = {QStringLiteral("Ethernet"),
                               QStringLiteral("Wi-Fi"),
                               QStringLiteral("Ethernet 2")};

}  // namespace

void TestNetworkAdapterAdmin::resolve_returnsSystemSpellingCaseInsensitively() {
    // The SYSTEM's spelling comes back, not the caller's -- so the exact string netsh is given
    // is one Windows itself reported.
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("ethernet")),
             QStringLiteral("Ethernet"));
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("WI-FI")), QStringLiteral("Wi-Fi"));
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("Ethernet 2")),
             QStringLiteral("Ethernet 2"));
    // Surrounding whitespace in the request is formatting, not part of the name.
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("  Ethernet  ")),
             QStringLiteral("Ethernet"));
}

void TestNetworkAdapterAdmin::resolve_refusesUnknownName() {
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("Bluetooth")), QString());
    QCOMPARE(sak::resolveAdapterName({}, QStringLiteral("Ethernet")), QString());
}

void TestNetworkAdapterAdmin::resolve_refusesPartialMatch() {
    // No prefix, substring or fuzzy matching. Picking the "closest" adapter is how the wrong NIC
    // gets disabled: "Ethernet" must never resolve to "Ethernet 2", and a request for "Ether"
    // must resolve to nothing at all even though exactly one adapter starts with it.
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("Ether")), QString());
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("Ethernet 2")),
             QStringLiteral("Ethernet 2"));
    QCOMPARE(sak::resolveAdapterName({QStringLiteral("Ethernet 2")}, QStringLiteral("Ethernet")),
             QString());
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("Wi")), QString());
}

void TestNetworkAdapterAdmin::resolve_refusesEmptyRequest() {
    QCOMPARE(sak::resolveAdapterName(kAdapters, QString()), QString());
    QCOMPARE(sak::resolveAdapterName(kAdapters, QStringLiteral("   ")), QString());
}

// ---------------------------------------------------------------------------
// Argument vectors
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::stateArgs_enableAndDisableDifferOnlyInTheAdminToken() {
    const QStringList enable = sak::adapterAdminStateArgs(QStringLiteral("Ethernet"), true);
    const QStringList disable = sak::adapterAdminStateArgs(QStringLiteral("Ethernet"), false);

    QCOMPARE(enable,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("set"),
                          QStringLiteral("interface"),
                          QStringLiteral("Ethernet"),
                          QStringLiteral("admin=ENABLED")}));
    QCOMPARE(disable,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("set"),
                          QStringLiteral("interface"),
                          QStringLiteral("Ethernet"),
                          QStringLiteral("admin=DISABLED")}));
    // The two must differ in exactly one place. A refactor that swapped the sense of the flag
    // would keep both vectors well-formed and disable an adapter the operator asked to enable.
    QCOMPARE(enable.size(), disable.size());
    QCOMPARE(enable.mid(0, 4), disable.mid(0, 4));
    QVERIFY(enable.last() != disable.last());
}

void TestNetworkAdapterAdmin::stateArgs_passTheNameAsItsOwnArgument() {
    // The name is one argv element, so spaces in it are not a quoting problem and never need
    // escaping. If it were ever concatenated into a single string this assertion would fail.
    const QStringList args = sak::adapterAdminStateArgs(QStringLiteral("Ethernet 2"), true);
    QCOMPARE(args.size(), 5);
    QCOMPARE(args.at(3), QStringLiteral("Ethernet 2"));
}

void TestNetworkAdapterAdmin::renameArgs_buildTheNewnameToken() {
    const QStringList args = sak::adapterRenameArgs(QStringLiteral("Ethernet"),
                                                    QStringLiteral("Lab NIC"));
    QCOMPARE(args,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("set"),
                          QStringLiteral("interface"),
                          QStringLiteral("Ethernet"),
                          QStringLiteral("newname=Lab NIC")}));
    // The OLD name stays its own argument and the new one rides in the newname= token; swapping
    // them would rename a different adapter.
    QCOMPARE(args.at(3), QStringLiteral("Ethernet"));
    QVERIFY(args.last().startsWith(QStringLiteral("newname=")));
}

// ---------------------------------------------------------------------------
// isDottedIpv4 -- the single validator four files used to carry a copy of
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::ipv4_acceptsCompleteDottedQuads() {
    QVERIFY(sak::isDottedIpv4(QStringLiteral("192.168.1.50")));
    QVERIFY(sak::isDottedIpv4(QStringLiteral("255.255.255.0")));  // a mask is the same shape
    QVERIFY(sak::isDottedIpv4(QStringLiteral("0.0.0.0")));
    QVERIFY(sak::isDottedIpv4(QStringLiteral("8.8.8.8")));
}

void TestNetworkAdapterAdmin::ipv4_rejectsAbbreviatedInetAtonForms() {
    // THE bug this validator was rewritten for. Four files delegated to QHostAddress::setAddress
    // and each claimed in a comment to accept only a complete address; none did. QHostAddress
    // implements the inet_aton family, so it accepts a three-, two- or one-part address and
    // EXPANDS it: "192.168.1" becomes 192.168.0.1. A technician who typed that, or a model that
    // emitted it, would have had a different address configured than the one requested, with no
    // error anywhere.
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("8.8.4")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("127.1")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("10")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.50.60")));  // five parts, not four
    // A leading zero is read as OCTAL by the inet_aton family, so "010.1.1.1" means 8.1.1.1 to
    // some parsers and 10.1.1.1 to others. The ambiguous spelling is refused outright so every
    // consumer of a validated address agrees on what it says.
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("010.1.1.1")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.01.1")));
    // A bare "0" octet is not a leading zero and stays legal.
    QVERIFY(sak::isDottedIpv4(QStringLiteral("10.0.0.1")));
}

void TestNetworkAdapterAdmin::ipv4_rejectsPartialAndDecoratedForms() {
    QVERIFY(!sak::isDottedIpv4(QString()));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.256")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral(".168.1.1")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.-1")));
    // Non-ASCII decimal digits: QChar::isDigit() accepts these and QString::toInt() converts them,
    // so a digit-class check alone would let an address through in a script the operator cannot
    // read back. These are Arabic-Indic digits for 1.2.3.4.
    QVERIFY(!sak::isDottedIpv4(QString::fromUtf8("\xd9\xa1.\xd9\xa2.\xd9\xa3.\xd9\xa4")));
    // A CIDR suffix is the one that matters most: the capture path reads prefix lengths, and a
    // value like this reaching `address=` would be handed to netsh as an address it cannot parse.
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.50/24")));
    // Surrounding whitespace is not silently trimmed away into a valid address.
    QVERIFY(!sak::isDottedIpv4(QStringLiteral(" 192.168.1.50")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("192.168.1.50 ")));
    // Not an IPv4 address, however resolvable it might be.
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("localhost")));
    QVERIFY(!sak::isDottedIpv4(QStringLiteral("::1")));
}

// ---------------------------------------------------------------------------
// IPv4 argument vectors
//
// These pin the EXACT vector, not a property of it. Both the diagnostic panel and the
// restore/assistant path now enter through these builders, so a change here changes what every
// caller sends to netsh -- which is the point of unifying them, and the reason the assertion has
// to be exact rather than a "contains" check.
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::staticIpArgs_buildTheNamedTagForm() {
    // The address alone, and no gateway: `gateway=none` clears any the adapter had. The gateway
    // goes on afterwards as a route, the only form that leaves its metric automatic (fix round S,
    // VM).
    const QStringList args = sak::adapterStaticIpv4Args(QStringLiteral("Ethernet 2"),
                                                        QStringLiteral("192.168.1.50"),
                                                        QStringLiteral("255.255.255.0"));
    QCOMPARE(args,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("set"),
                          QStringLiteral("address"),
                          QStringLiteral("name=Ethernet 2"),
                          QStringLiteral("source=static"),
                          QStringLiteral("address=192.168.1.50"),
                          QStringLiteral("mask=255.255.255.0"),
                          QStringLiteral("gateway=none")}));
    for (const QString& arg : args) {
        QVERIFY(!arg.startsWith(QStringLiteral("gwmetric")));
    }
}

void TestNetworkAdapterAdmin::gatewayRoute_isAutomaticUnlessAMetricIsGiven() {
    // Measured on the Windows 11 VM: `set address ... gateway=` with no gwmetric pins metric 1 and
    // `gwmetric=0` pins 0; a route added with no metric reads "Default" in route print, as Windows'
    // own adapter dialog leaves it. `metric=0` is a pinned 0, not automatic.
    const QStringList automatic = sak::adapterDefaultGatewayArgs(
        sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.0.1"), std::nullopt);
    QCOMPARE(automatic,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("add"),
                          QStringLiteral("route"),
                          QStringLiteral("prefix=0.0.0.0/0"),
                          QStringLiteral("interface=Ethernet"),
                          QStringLiteral("nexthop=10.0.0.1")}));
    const QStringList zero = sak::adapterDefaultGatewayArgs(
        sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.0.1"), 0);
    QCOMPARE(zero.last(), QStringLiteral("metric=0"));
}

void TestNetworkAdapterAdmin::staticIp_setsTheAddressThenTheGatewayAsAnAutomaticRoute() {
    // What Set Static IP and Restore Settings really send: recorded, not run.
    QList<QStringList> calls;
    const sak::ScopedProcessFaultInjector injector(
        [&calls](const QString& program,
                 const QStringList& args) -> std::optional<sak::ProcessResult> {
            calls << (QStringList{QFileInfo(program).fileName().toLower()} + args);
            sak::ProcessResult ok;
            ok.exit_code = 0;
            ok.std_out = QStringLiteral("Ok.");
            return ok;
        });
    const auto outcome = sak::setAdapterStaticIpv4(QStringLiteral("Ethernet"),
                                                   QStringLiteral("10.0.2.15"),
                                                   QStringLiteral("255.255.255.0"),
                                                   QStringLiteral("10.0.2.2"),
                                                   sak::kTechnicianIpv4Dialect);
    QVERIFY2(outcome.succeeded, qPrintable(outcome.message));
    QCOMPARE(calls.size(), 2);
    QCOMPARE(calls.at(0).join(QLatin1Char(' ')),
             QStringLiteral("netsh.exe interface ipv4 set address name=Ethernet source=static "
                            "address=10.0.2.15 mask=255.255.255.0 gateway=none"));
    QCOMPARE(calls.at(1).join(QLatin1Char(' ')),
             QStringLiteral(
                 "netsh.exe interface ipv4 add route prefix=0.0.0.0/0 interface=Ethernet "
                 "nexthop=10.0.2.2"));
    // The restore path sends the same two commands; its dialect changes DNS registration only.
    calls.clear();
    QVERIFY(sak::setAdapterStaticIpv4(QStringLiteral("Ethernet"),
                                      QStringLiteral("10.0.2.15"),
                                      QStringLiteral("255.255.255.0"),
                                      QStringLiteral("10.0.2.2"),
                                      sak::kRestoreIpv4Dialect)
                .succeeded);
    QCOMPARE(calls.size(), 2);
    QVERIFY(!calls.at(1).join(QLatin1Char(' ')).contains(QStringLiteral("metric")));
    // No gateway, no route.
    calls.clear();
    QVERIFY(sak::setAdapterStaticIpv4(QStringLiteral("Ethernet"),
                                      QStringLiteral("10.0.2.15"),
                                      QStringLiteral("255.255.255.0"),
                                      QString(),
                                      sak::kTechnicianIpv4Dialect)
                .succeeded);
    QCOMPARE(calls.size(), 1);
}

void TestNetworkAdapterAdmin::dhcpArgs_addressAndDnsDifferOnlyInTheSubcommand() {
    const QStringList address = sak::adapterDhcpAddressArgs(QStringLiteral("Ethernet"));
    const QStringList dns = sak::adapterDhcpDnsArgs(QStringLiteral("Ethernet"));

    QCOMPARE(address,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("set"),
                          QStringLiteral("address"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("source=dhcp")}));
    QCOMPARE(dns,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("set"),
                          QStringLiteral("dnsservers"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("source=dhcp")}));
    // Switching only the address to DHCP while leaving DNS static is a real half-configured state,
    // so the two vectors must stay distinct in exactly the subcommand and nowhere else.
    QCOMPARE(address.size(), dns.size());
    QVERIFY(address.at(3) != dns.at(3));
}

void TestNetworkAdapterAdmin::dnsArgs_registerTokenIsTheOnlyDialectDifference() {
    const QStringList plain = sak::adapterStaticPrimaryDnsArgs(QStringLiteral("Ethernet"),
                                                               QStringLiteral("8.8.8.8"),
                                                               sak::DnsRegistration::NetshDefault);
    const QStringList registered =
        sak::adapterStaticPrimaryDnsArgs(QStringLiteral("Ethernet"),
                                         QStringLiteral("8.8.8.8"),
                                         sak::DnsRegistration::PrimarySuffixOnly);

    QCOMPARE(plain,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("set"),
                          QStringLiteral("dnsservers"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("source=static"),
                          QStringLiteral("address=8.8.8.8")}));
    QCOMPARE(registered.size(), plain.size() + 1);
    QCOMPARE(registered.mid(0, plain.size()), plain);
    QCOMPARE(registered.last(), QStringLiteral("register=primary"));
}

void TestNetworkAdapterAdmin::dnsArgs_secondariesStartAtIndexTwo() {
    const QStringList second =
        sak::adapterAdditionalDnsArgs(QStringLiteral("Ethernet"), QStringLiteral("8.8.4.4"), 2);
    QCOMPARE(second,
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("add"),
                          QStringLiteral("dnsservers"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("address=8.8.4.4"),
                          QStringLiteral("index=2")}));
    // `add`, never `set`: a second `set` would REPLACE the whole list and silently drop the
    // primary that was just applied.
    QCOMPARE(second.at(2), QStringLiteral("add"));
    QCOMPARE(sak::adapterAdditionalDnsArgs(QStringLiteral("Ethernet"), QStringLiteral("1.1.1.1"), 3)
                 .last(),
             QStringLiteral("index=3"));
}

void TestNetworkAdapterAdmin::ipv4Dialects_differOnlyInRegistration() {
    // The shipped dialects are the whole record of how the panel and the restore path disagree. The
    // gateway metric is no longer one of them: both leave it automatic.
    QVERIFY(sak::kRestoreIpv4Dialect.dns_registration !=
            sak::kTechnicianIpv4Dialect.dns_registration);
    QCOMPARE(sak::kRestoreIpv4Dialect.dns_registration, sak::DnsRegistration::PrimarySuffixOnly);
    QCOMPARE(sak::kTechnicianIpv4Dialect.dns_registration, sak::DnsRegistration::NetshDefault);
}

// ---------------------------------------------------------------------------
// Fail-closed guards
//
// These reach the executors, but every case below is refused BEFORE a process is launched, so
// they run on a machine with no adapters and never mutate anything. A regression that let a
// malformed value through would show up here as a different message, not as a passing test.
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::staticIp_refusesMalformedAddresses() {
    // The PURE validator, never the executor. Asserting a refusal by calling setAdapterStaticIpv4
    // would run a live netsh apply against a real adapter the moment the refusal regressed -- and
    // that is not hypothetical: the first version of this test did exactly that, because the
    // validator it trusted accepted "192.168.1".
    const QString bad_ip = sak::staticIpv4ConfigurationProblem(QStringLiteral("192.168.1"),
                                                               QStringLiteral("255.255.255.0"),
                                                               QString());
    QVERIFY(bad_ip.contains(QStringLiteral("not a valid IPv4 address")));

    const QString bad_mask = sak::staticIpv4ConfigurationProblem(QStringLiteral("192.168.1.50"),
                                                                 QStringLiteral("not-a-mask"),
                                                                 QString());
    QVERIFY(bad_mask.contains(QStringLiteral("subnet mask")));

    const QString bad_gateway =
        sak::staticIpv4ConfigurationProblem(QStringLiteral("192.168.1.50"),
                                            QStringLiteral("255.255.255.0"),
                                            QStringLiteral("192.168.1.999"));
    QVERIFY(bad_gateway.contains(QStringLiteral("gateway")));

    // Each refusal names WHICH field was wrong. A single generic message would leave an operator
    // guessing which of three fields to correct.
    QVERIFY(bad_ip != bad_mask);
    QVERIFY(bad_mask != bad_gateway);

    // A well-formed configuration is NOT refused -- otherwise the assertions above would pass just
    // as happily against a validator that rejected everything.
    QCOMPARE(sak::staticIpv4ConfigurationProblem(QStringLiteral("192.168.1.50"),
                                                 QStringLiteral("255.255.255.0"),
                                                 QStringLiteral("192.168.1.1")),
             QString());
    // An empty gateway is legitimate, not a malformed one.
    QCOMPARE(sak::staticIpv4ConfigurationProblem(
                 QStringLiteral("192.168.1.50"), QStringLiteral("255.255.255.0"), QString()),
             QString());
}

void TestNetworkAdapterAdmin::staticDns_refusesEmptyListAndMalformedServers() {
    QVERIFY(!sak::staticDnsProblem({}).isEmpty());

    // A malformed SECONDARY is refused before the PRIMARY would be applied. Validating the whole
    // list up front is what keeps a bad later entry from leaving the resolver half-rewritten: the
    // primary step REPLACES the adapter's entire DNS list, so there is no undo once it has run.
    const QString bad_secondary =
        sak::staticDnsProblem({QStringLiteral("8.8.8.8"), QStringLiteral("8.8.4")});
    QVERIFY(bad_secondary.contains(QStringLiteral("8.8.4")));

    QCOMPARE(sak::staticDnsProblem({QStringLiteral("8.8.8.8"), QStringLiteral("8.8.4.4")}),
             QString());
}

void TestNetworkAdapterAdmin::adapterOps_refuseAnEmptyAdapterName() {
    // Every entry point refuses a nameless adapter rather than letting netsh decide what an empty
    // name means.
    QVERIFY(!sak::setAdapterEnabled(QStringLiteral("  "), true).succeeded);
    QVERIFY(!sak::renameAdapter(QString(), QStringLiteral("Lab NIC")).succeeded);
    QVERIFY(!sak::setAdapterStaticIpv4(QString(),
                                       QStringLiteral("192.168.1.50"),
                                       QStringLiteral("255.255.255.0"),
                                       QString(),
                                       sak::kTechnicianIpv4Dialect)
                 .succeeded);
    QVERIFY(!sak::setAdapterStaticDns(
                 QString(), {QStringLiteral("8.8.8.8")}, sak::DnsRegistration::NetshDefault)
                 .succeeded);
    QVERIFY(!sak::setAdapterDhcpMode(QStringLiteral("   ")).succeeded);
    QVERIFY(!sak::releaseAdapterDhcpLease(QString()).succeeded);
    QVERIFY(!sak::renewAdapterDhcpLease(QString()).succeeded);
}

// ---------------------------------------------------------------------------
// Putting settings back after a network reset
// ---------------------------------------------------------------------------

void TestNetworkAdapterAdmin::selfAssigned_isOnly169dot254() {
    QVERIFY(sak::isSelfAssignedIpv4(QStringLiteral("169.254.193.132")));
    QVERIFY(sak::isSelfAssignedIpv4(QStringLiteral("169.254.0.1")));
    QVERIFY(!sak::isSelfAssignedIpv4(QStringLiteral("10.0.2.15")));
    QVERIFY(!sak::isSelfAssignedIpv4(QStringLiteral("169.25.1.1")));
    QVERIFY(!sak::isSelfAssignedIpv4(QStringLiteral("169.2540.1.1")));
    QVERIFY(!sak::isSelfAssignedIpv4(QStringLiteral("169.254.1")));
    QVERIFY(!sak::isSelfAssignedIpv4(QStringLiteral("169.254.193.132/16")));
}

void TestNetworkAdapterAdmin::ipv6Literal_refusesZonesPrefixesAndIpv4() {
    QVERIFY(sak::isIpv6Literal(QStringLiteral("fd17:625c:f037:2:bf9a:5448:a14:9b3b")));
    QVERIFY(sak::isIpv6Literal(QStringLiteral("fe80::2")));
    QVERIFY(sak::isIpv6Literal(QStringLiteral("::")));
    QVERIFY(!sak::isIpv6Literal(QStringLiteral("fe80::2%14")));
    QVERIFY(!sak::isIpv6Literal(QStringLiteral("fd00::5/64")));
    QVERIFY(!sak::isIpv6Literal(QStringLiteral("10.0.2.15")));
    QVERIFY(!sak::isIpv6Literal(QStringLiteral("fd00::g")));
    QVERIFY(!sak::isIpv6Literal(QString()));
}

void TestNetworkAdapterAdmin::restoreGateway_pinsOnlyWhatWasPinned() {
    // A reset puts back the metric a gateway had: a pinned one pinned, an automatic one with none.
    const QStringList pinned = sak::adapterDefaultGatewayArgs(
        sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.2.2"), 256);
    QCOMPARE(pinned.last(), QStringLiteral("metric=256"));
    const QStringList automatic = sak::adapterDefaultGatewayArgs(
        sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.2.2"), std::nullopt);
    QCOMPARE(automatic.last(), QStringLiteral("nexthop=10.0.2.2"));
}

void TestNetworkAdapterAdmin::restoreBuilders_addAddressesGatewaysAndDnsPerFamily() {
    QCOMPARE(sak::adapterAdditionalIpv4AddressArgs(QStringLiteral("Ethernet"),
                                                   QStringLiteral("10.0.2.16"),
                                                   QStringLiteral("255.255.255.0")),
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("add"),
                          QStringLiteral("address"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("address=10.0.2.16"),
                          QStringLiteral("mask=255.255.255.0")}));
    QCOMPARE(sak::adapterIpv6AddressArgs(QStringLiteral("Ethernet"), QStringLiteral("fd00::5/64")),
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv6"),
                          QStringLiteral("add"),
                          QStringLiteral("address"),
                          QStringLiteral("interface=Ethernet"),
                          QStringLiteral("address=fd00::5/64")}));
    QCOMPARE(sak::adapterDefaultGatewayArgs(
                 sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.2.3"), 10),
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("add"),
                          QStringLiteral("route"),
                          QStringLiteral("prefix=0.0.0.0/0"),
                          QStringLiteral("interface=Ethernet"),
                          QStringLiteral("nexthop=10.0.2.3"),
                          QStringLiteral("metric=10")}));
    const QStringList v6_route = sak::adapterDefaultGatewayArgs(
        sak::IpFamily::V6, QStringLiteral("Ethernet"), QStringLiteral("fd00::1"), 256);
    QCOMPARE(v6_route.at(1), QStringLiteral("ipv6"));
    QCOMPARE(v6_route.at(4), QStringLiteral("prefix=::/0"));
    QCOMPARE(sak::adapterFamilyPrimaryDnsArgs(
                 sak::IpFamily::V6, QStringLiteral("Ethernet"), QStringLiteral("fd00::53")),
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv6"),
                          QStringLiteral("set"),
                          QStringLiteral("dnsservers"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("source=static"),
                          QStringLiteral("address=fd00::53")}));
    QCOMPARE(sak::adapterFamilyAdditionalDnsArgs(
                 sak::IpFamily::V4, QStringLiteral("Ethernet"), QStringLiteral("10.0.2.4"), 2),
             (QStringList{QStringLiteral("interface"),
                          QStringLiteral("ipv4"),
                          QStringLiteral("add"),
                          QStringLiteral("dnsservers"),
                          QStringLiteral("name=Ethernet"),
                          QStringLiteral("address=10.0.2.4"),
                          QStringLiteral("index=2")}));
    QVERIFY(!sak::runAdapterCommand({}, QStringLiteral("nothing")).succeeded);
}

QTEST_MAIN(TestNetworkAdapterAdmin)
#include "test_network_adapter_admin.moc"
