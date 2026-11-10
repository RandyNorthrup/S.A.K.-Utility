// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_dns_diagnostic_tool.cpp
/// @brief Unit tests for DnsDiagnosticTool static helpers

#include "sak/dns_diagnostic_tool.h"

#include <QtTest/QtTest>

using namespace sak;

class TestDnsDiagnosticTool : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();

    // -- wellKnownDnsServers ---------------------------------------
    void wellKnown_nonEmpty();
    void wellKnown_containsGoogle();
    void wellKnown_containsCloudflare();
    void wellKnown_pairsHaveNameAndIp();

    // -- supportedRecordTypes --------------------------------------
    void recordTypes_nonEmpty();
    void recordTypes_containsA();
    void recordTypes_containsAAAA();
    void recordTypes_containsMX();
    void recordTypes_containsCNAME();
    void recordTypes_noDuplicates();

    // -- B9-18 hardening seams -------------------------------------
    void isSupportedRecordType_acceptsKnownRejectsUnknown();
    void isNumericIpv4_acceptsValidRejectsNonNumeric();
    void answersEquivalent_orderInsensitive();

    // -- parseDnsClientCacheJson (G23-4 locale-neutral cache view) --
    void parseDnsCache_arrayOfRecords();
    void parseDnsCache_singleBareObject();
    void parseDnsCache_skipsNullDataAndEmptyName();
    void parseDnsCache_emptyInputIsEmptyCache();
    void parseDnsCache_ignoresMalformed();
};

// ===================================================================
// Construction
// ===================================================================

void TestDnsDiagnosticTool::construction_default() {
    DnsDiagnosticTool tool;
    QVERIFY(tool.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<DnsDiagnosticTool>);

    // The declared ctor takes an owner (dns_diagnostic_tool.h:28) and must forward it to QObject
    // so the tool is destroyed with its owner. A default-constructed tool cannot see the argument
    // at all, so construct WITH a parent and pin both the link and the ownership registration.
    // `owner` is declared first so `owned` is destroyed (and de-registered) before it.
    QObject owner;
    DnsDiagnosticTool owned(&owner);
    QCOMPARE(owned.parent(), &owner);
    QVERIFY(owner.children().contains(&owned));
}

void TestDnsDiagnosticTool::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<DnsDiagnosticTool>);
    QVERIFY(!std::is_move_constructible_v<DnsDiagnosticTool>);
}

// ===================================================================
// wellKnownDnsServers
// ===================================================================

void TestDnsDiagnosticTool::wellKnown_nonEmpty() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    QVERIFY(!servers.isEmpty());
    QCOMPARE(servers.size(), qsizetype(9));
}

void TestDnsDiagnosticTool::wellKnown_containsGoogle() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    // Google DNS is the fixed entry at index 1; pin both name and IP (the either-or scan passed
    // on a single field, masking a wrong-IP or wrong-name regression).
    QCOMPARE(servers.at(1), qMakePair(QStringLiteral("Google DNS"), QStringLiteral("8.8.8.8")));
}

void TestDnsDiagnosticTool::wellKnown_containsCloudflare() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    QCOMPARE(servers.at(3), qMakePair(QStringLiteral("Cloudflare"), QStringLiteral("1.1.1.1")));
}

void TestDnsDiagnosticTool::wellKnown_pairsHaveNameAndIp() {
    const auto servers = DnsDiagnosticTool::wellKnownDnsServers();
    // The whole list is a fixed literal; pin it exactly (subsumes name-non-empty, IP-non-empty,
    // and the empty-System-Default checks).
    const QVector<QPair<QString, QString>> expected = {
        {QStringLiteral("System Default"), QStringLiteral("")},
        {QStringLiteral("Google DNS"), QStringLiteral("8.8.8.8")},
        {QStringLiteral("Google DNS (Secondary)"), QStringLiteral("8.8.4.4")},
        {QStringLiteral("Cloudflare"), QStringLiteral("1.1.1.1")},
        {QStringLiteral("Cloudflare (Secondary)"), QStringLiteral("1.0.0.1")},
        {QStringLiteral("Quad9"), QStringLiteral("9.9.9.9")},
        {QStringLiteral("Quad9 (Secondary)"), QStringLiteral("149.112.112.112")},
        {QStringLiteral("OpenDNS"), QStringLiteral("208.67.222.222")},
        {QStringLiteral("OpenDNS (Secondary)"), QStringLiteral("208.67.220.220")}};
    QCOMPARE(servers, expected);
}

// ===================================================================
// supportedRecordTypes
// ===================================================================

void TestDnsDiagnosticTool::recordTypes_nonEmpty() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(!types.isEmpty());
    QCOMPARE(types.size(), qsizetype(9));
}

void TestDnsDiagnosticTool::recordTypes_containsA() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("A"));
}

void TestDnsDiagnosticTool::recordTypes_containsAAAA() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("AAAA"));
}

void TestDnsDiagnosticTool::recordTypes_containsMX() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("MX"));
}

void TestDnsDiagnosticTool::recordTypes_containsCNAME() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QVERIFY(types.contains("CNAME"));
}

void TestDnsDiagnosticTool::recordTypes_noDuplicates() {
    const auto types = DnsDiagnosticTool::supportedRecordTypes();
    QSet<QString> unique_types(types.begin(), types.end());
    QCOMPARE(unique_types.size(), types.size());
}

// ===================================================================
// B9-18 hardening seams
// ===================================================================

void TestDnsDiagnosticTool::isSupportedRecordType_acceptsKnownRejectsUnknown() {
    QVERIFY(DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("A")));
    QVERIFY(DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("a")));  // case-insensitive
    QVERIFY(DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("AAAA")));
    QVERIFY(DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("PTR")));
    // A typo must NOT silently become an A query.
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("AA")));
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("XYZ")));
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QString()));
    // The acceptance guard is the ONLY normalization boundary: mapRecordType
    // (dns_diagnostic_tool.cpp:130) upper-cases and nothing else, then falls through to
    // DNS_TYPE_A (:136). Anything this guard normalizes away that the mapper does not
    // clears the gate at :294 and silently issues an A query labelled with the caller's
    // raw string -- the R5-P10-31 coercion bug. Pin case folding as the only normalization.
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral(" MX ")));
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("MX ")));
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral(" A")));
    QVERIFY(!DnsDiagnosticTool::isSupportedRecordType(QStringLiteral("\tPTR\n")));
}

void TestDnsDiagnosticTool::isNumericIpv4_acceptsValidRejectsNonNumeric() {
    QVERIFY(DnsDiagnosticTool::isNumericIpv4(QStringLiteral("1.2.3.4")));
    QVERIFY(DnsDiagnosticTool::isNumericIpv4(QStringLiteral("255.255.255.255")));
    QVERIFY(DnsDiagnosticTool::isNumericIpv4(QStringLiteral("0.0.0.0")));
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("256.1.1.1")));  // octet > 255
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("a.b.c.d")));    // non-numeric
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("1.2.3")));      // too few
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("1.2.3.4.5")));  // too many
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("1..3.4")));     // empty octet
    QVERIFY(!DnsDiagnosticTool::isNumericIpv4(QStringLiteral("1.2.3.-1")));   // negative
}

void TestDnsDiagnosticTool::answersEquivalent_orderInsensitive() {
    const QVector<QString> a = {QStringLiteral("1.1.1.1"), QStringLiteral("2.2.2.2")};
    const QVector<QString> reordered = {QStringLiteral("2.2.2.2"), QStringLiteral("1.1.1.1")};
    const QVector<QString> different = {QStringLiteral("1.1.1.1"), QStringLiteral("3.3.3.3")};
    const QVector<QString> shorter = {QStringLiteral("1.1.1.1")};

    QVERIFY(DnsDiagnosticTool::answersEquivalent(a, reordered));  // round-robin still agrees
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(a, different));
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(a, shorter));
    // Multiset, not set: a hijacking/misconfigured resolver that flattens a CNAME chain into a
    // repeated A record must NOT compare equivalent to the single-record answer, and two same-size
    // answers with the same members but different duplicate counts must disagree. A de-duplicating
    // QSet compare collapses both cases (the size guard alone only catches the first).
    const QVector<QString> duplicated = {QStringLiteral("1.1.1.1"), QStringLiteral("1.1.1.1")};
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(duplicated, shorter));
    const QVector<QString> twoOnes = {QStringLiteral("1.1.1.1"),
                                      QStringLiteral("1.1.1.1"),
                                      QStringLiteral("2.2.2.2")};
    const QVector<QString> twoTwos = {QStringLiteral("1.1.1.1"),
                                      QStringLiteral("2.2.2.2"),
                                      QStringLiteral("2.2.2.2")};
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(twoOnes, twoTwos));
    // ...while a duplicate-carrying answer still agrees with its own round-robin reordering.
    const QVector<QString> twoOnesReordered = {QStringLiteral("2.2.2.2"),
                                               QStringLiteral("1.1.1.1"),
                                               QStringLiteral("1.1.1.1")};
    QVERIFY(DnsDiagnosticTool::answersEquivalent(twoOnes, twoOnesReordered));
    // Two resolvers that both legitimately return an EMPTY answer set agree; the first success
    // sets the baseline even when empty (dns_diagnostic_tool.cpp:352-357) and every later success
    // is compared against it, so a fail-closed empty check here would fabricate a disagreement.
    const QVector<QString> empty;
    QVERIFY(DnsDiagnosticTool::answersEquivalent(empty, empty));
    // An empty answer set is still NOT equivalent to a populated one (size guard, both directions).
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(empty, a));
    QVERIFY(!DnsDiagnosticTool::answersEquivalent(a, empty));
}

// ===================================================================
// parseDnsClientCacheJson -- the locale-neutral DNS cache view (G23-4)
// ===================================================================

void TestDnsDiagnosticTool::parseDnsCache_arrayOfRecords() {
    const QString json = QStringLiteral(R"([{"Name":"a.example","Data":"1.2.3.4"},)"
                                        R"({"Name":"b.example","Data":"5.6.7.8"}])");
    const auto entries = DnsDiagnosticTool::parseDnsClientCacheJson(json);
    QCOMPARE(entries.size(), qsizetype(2));
    QCOMPARE(entries.at(0).first, QStringLiteral("a.example"));
    QCOMPARE(entries.at(0).second, QStringLiteral("1.2.3.4"));
    QCOMPARE(entries.at(1).first, QStringLiteral("b.example"));
    QCOMPARE(entries.at(1).second, QStringLiteral("5.6.7.8"));
}

void TestDnsDiagnosticTool::parseDnsCache_singleBareObject() {
    // ConvertTo-Json emits a bare object, not a one-element array, for a single record.
    const auto entries = DnsDiagnosticTool::parseDnsClientCacheJson(
        QStringLiteral(R"({"Name":"solo.example","Data":"9.9.9.9"})"));
    QCOMPARE(entries.size(), qsizetype(1));
    QCOMPARE(entries.at(0).first, QStringLiteral("solo.example"));
    QCOMPARE(entries.at(0).second, QStringLiteral("9.9.9.9"));
}

void TestDnsDiagnosticTool::parseDnsCache_skipsNullDataAndEmptyName() {
    // A real Get-DnsClientCache dump mixes resolved records with negative-cache / pending entries
    // that carry a null Data, an empty Data, or an empty Name; only the resolved (name, address)
    // pair is useful. Each skip arm gets a row that ONLY it can reject, so no guard is masked by
    // a sibling (the old fixture's single nameless row also had null Data, so the name.isEmpty()
    // arm never decided anything and the empty-address arm was never reached at all):
    //   row 2 -- empty Name WITH a real address -> only name.isEmpty() can drop it
    //   row 3 -- named record with empty-string Data -> only address.isEmpty() can drop it
    //   row 4 -- named record with null Data -> only !data.isString() can drop it
    const QString json = QStringLiteral(R"([{"Name":"good.example","Data":"1.1.1.1"},)"
                                        R"({"Name":"","Data":"2.2.2.2"},)"
                                        R"({"Name":"blank.example","Data":""},)"
                                        R"({"Name":"pending.example","Data":null},)"
                                        R"({"Name":"","Data":null}])");
    const auto entries = DnsDiagnosticTool::parseDnsClientCacheJson(json);
    const QVector<QPair<QString, QString>> expected = {
        {QStringLiteral("good.example"), QStringLiteral("1.1.1.1")}};
    QCOMPARE(entries, expected);
}

void TestDnsDiagnosticTool::parseDnsCache_emptyInputIsEmptyCache() {
    // An empty A,AAAA cache serializes as empty output / JSON null / an empty array; each is an
    // empty cache, NOT an error (the process exit code is the failure signal, checked separately).
    QVERIFY(DnsDiagnosticTool::parseDnsClientCacheJson(QString()).isEmpty());
    QVERIFY(DnsDiagnosticTool::parseDnsClientCacheJson(QStringLiteral("null")).isEmpty());
    QVERIFY(DnsDiagnosticTool::parseDnsClientCacheJson(QStringLiteral("[]")).isEmpty());
}

void TestDnsDiagnosticTool::parseDnsCache_ignoresMalformed() {
    QVERIFY(
        DnsDiagnosticTool::parseDnsClientCacheJson(QStringLiteral("not json at all")).isEmpty());
    QVERIFY(DnsDiagnosticTool::parseDnsClientCacheJson(QStringLiteral(R"({"Name":)")).isEmpty());
}

QTEST_MAIN(TestDnsDiagnosticTool)
#include "test_dns_diagnostic_tool.moc"
