// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_network_adapter_admin.cpp
/// @brief Unit tests for the pure half of adapter administration: new-name validation,
///        resolution against the system's own adapter list, and the exact netsh argument
///        vectors. None of these touch an adapter, so they run identically on a machine with
///        no network hardware.

#include "sak/network_adapter_admin.h"

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

QTEST_MAIN(TestNetworkAdapterAdmin)
#include "test_network_adapter_admin.moc"
