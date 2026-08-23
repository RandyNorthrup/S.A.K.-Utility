// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_uup_manifest_guard.cpp
/// @brief Mutation-fuzz of the UUP-dump aria2 manifest-entry guard (G14 parser sweep).
///
/// The UUP dump JSON API (api.uupdump.net) is a third-party service; its file records --
/// fileName, url, and sha1 -- are attacker-influenced strings that SAK serializes into an aria2
/// input manifest and hands to the downloader. UupDumpApi::isSafeAria2FileEntry() is the security
/// boundary that stops a malicious record from injecting an out=/uri directive, escaping the
/// download directory (../), aliasing a drive/ADS (C: / name:stream), opening a reserved DOS device
/// (CON, NUL, COM1...), or normalizing to a different on-disk name than the one integrity-checked
/// (trailing dot/space). UupDumpApi::isValidSha1() is the companion checksum gate.
///
/// This harness drives both guards over thousands of mutated field strings and, for EVERY input,
/// checks the verdict against an INDEPENDENT re-derivation of the safety rules (not a copy of the
/// guard's control flow -- a plain restatement of the property). It asserts:
///
///   1. No crash and no hang (a fault never returns the empty string; a hang trips the ctest
///      timeout with the reproducer recorded in fuzz_harness.h).
///   2. Determinism: the same field yields the same verdict.
///   3. The guard AGREES with the independent oracle. The security-critical direction is called out
///      separately: an ACCEPT of an oracle-unsafe field is an aria2-injection escape and fails the
///      run loudly ([[no-fallbacks-fail-closed]]).
///   4. isValidSha1() accepts exactly the 40-hex-character digests and nothing else.

#include "sak/uup_dump_api.h"

#include "../fuzz/fuzz_harness.h"

#include <QByteArray>
#include <QChar>
#include <QString>
#include <QStringList>
#include <QtTest/QtTest>

#include <vector>

namespace {

// A field known to be oracle-safe, used to hold two of the three fields constant while the third is
// fuzzed -- so each variant isolates one field's contribution to the verdict.
const QString kValidName = QStringLiteral("core_en-us.esd");
const QString kValidUrl = QStringLiteral("https://example.com/a.esd");
const QString kValidSha1 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709");

// C0 control characters (incl. NUL) and DEL -- any of these in a serialized field could truncate an
// aria2 manifest line or break out of the record. Mirrors uup_dump_api.cpp's own bounds.
bool oracleHasControlChar(const QString& s) {
    for (const QChar ch : s) {
        if (ch.unicode() < 0x20 || ch.unicode() == 0x7f) {
            return true;
        }
    }
    return false;
}

bool oracleIsReservedDosName(const QString& fileName) {
    const int dot = static_cast<int>(fileName.indexOf(QLatin1Char('.')));
    const QString base = (dot < 0 ? fileName : fileName.left(dot)).toUpper();
    static const QStringList reserved = {
        QStringLiteral("CON"),  QStringLiteral("PRN"),  QStringLiteral("AUX"),
        QStringLiteral("NUL"),  QStringLiteral("COM1"), QStringLiteral("COM2"),
        QStringLiteral("COM3"), QStringLiteral("COM4"), QStringLiteral("COM5"),
        QStringLiteral("COM6"), QStringLiteral("COM7"), QStringLiteral("COM8"),
        QStringLiteral("COM9"), QStringLiteral("LPT1"), QStringLiteral("LPT2"),
        QStringLiteral("LPT3"), QStringLiteral("LPT4"), QStringLiteral("LPT5"),
        QStringLiteral("LPT6"), QStringLiteral("LPT7"), QStringLiteral("LPT8"),
        QStringLiteral("LPT9")};
    return reserved.contains(base);
}

// True iff @p fileName is an unsafe aria2 out= name -- independent restatement of the property the
// guard must enforce, deliberately NOT structured like the guard's code.
bool oracleUnsafeName(const QString& fileName) {
    if (fileName.isEmpty()) {
        return true;
    }
    if (fileName.contains(QStringLiteral(".."))) {
        return true;
    }
    if (fileName.contains(QLatin1Char('/')) || fileName.contains(QLatin1Char('\\')) ||
        fileName.contains(QLatin1Char(':'))) {
        return true;
    }
    const QChar last = fileName.back();
    if (last == QLatin1Char('.') || last == QLatin1Char(' ')) {
        return true;
    }
    return oracleIsReservedDosName(fileName);
}

// The full entry property: a safe entry has a safe name and no control character in any field.
bool oracleSafeEntry(const UupDumpApi::FileInfo& info) {
    return !oracleUnsafeName(info.fileName) && !oracleHasControlChar(info.fileName) &&
           !oracleHasControlChar(info.url) && !oracleHasControlChar(info.sha1);
}

bool oracleValidSha1(const QString& s) {
    if (s.size() != 40) {
        return false;
    }
    for (const QChar ch : s) {
        const bool hex = (ch >= QLatin1Char('0') && ch <= QLatin1Char('9')) ||
                         (ch >= QLatin1Char('a') && ch <= QLatin1Char('f')) ||
                         (ch >= QLatin1Char('A') && ch <= QLatin1Char('F'));
        if (!hex) {
            return false;
        }
    }
    return true;
}

// Drive @p field through one FileInfo slot and confirm the guard matches the oracle. Returns "" on
// agreement, else a reason. @p slotName only labels the failure.
QString checkEntrySlot(const UupDumpApi::FileInfo& info, const char* slotName) {
    const bool got = UupDumpApi::isSafeAria2FileEntry(info);
    const bool again = UupDumpApi::isSafeAria2FileEntry(info);
    if (got != again) {
        return QStringLiteral("isSafeAria2FileEntry non-deterministic on %1 field")
            .arg(QLatin1String(slotName));
    }
    const bool oracle = oracleSafeEntry(info);
    // Security-critical direction first: an accept of an oracle-unsafe entry is an injection
    // escape.
    if (got && !oracle) {
        return QStringLiteral(
                   "ACCEPTED an oracle-unsafe %1 field (aria2 manifest injection escape)")
            .arg(QLatin1String(slotName));
    }
    if (got != oracle) {
        return QStringLiteral("guard/oracle disagree on %1 field").arg(QLatin1String(slotName));
    }
    return {};
}

QString manifestGuardInvariant(const QByteArray& input) {
    const QString s = QString::fromUtf8(input);

    // fileName slot: traversal, drive/ADS, reserved device, trailing dot/space, and control chars.
    {
        UupDumpApi::FileInfo info;
        info.fileName = s;
        info.url = kValidUrl;
        info.sha1 = kValidSha1;
        const QString reason = checkEntrySlot(info, "fileName");
        if (!reason.isEmpty()) {
            return reason;
        }
    }
    // url slot: only control chars matter to isSafeAria2FileEntry (scheme is a separate gate).
    {
        UupDumpApi::FileInfo info;
        info.fileName = kValidName;
        info.url = s;
        info.sha1 = kValidSha1;
        const QString reason = checkEntrySlot(info, "url");
        if (!reason.isEmpty()) {
            return reason;
        }
    }
    // sha1 slot: control chars here too (hex-shape is isValidSha1's job, checked below).
    {
        UupDumpApi::FileInfo info;
        info.fileName = kValidName;
        info.url = kValidUrl;
        info.sha1 = s;
        const QString reason = checkEntrySlot(info, "sha1");
        if (!reason.isEmpty()) {
            return reason;
        }
    }
    // isValidSha1: exactly-40-hex, independent oracle.
    {
        const bool got = UupDumpApi::isValidSha1(s);
        if (got != oracleValidSha1(s)) {
            return QStringLiteral("isValidSha1 disagrees with the 40-hex oracle");
        }
    }
    return {};
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("uup manifest guard fuzz failed after %1 inputs: %2\n  reproducer (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class UupManifestGuardFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void guardMatchesOracleAndNeverAcceptsUnsafe() {
        std::vector<QByteArray> corpus{
            QByteArrayLiteral("core_en-us.esd"),
            QByteArrayLiteral("good_file123.wim"),
            QByteArrayLiteral("../../etc/passwd"),
            QByteArrayLiteral("..\\..\\windows\\win.ini"),
            QByteArrayLiteral("CON"),
            QByteArrayLiteral("NUL.esd"),
            QByteArrayLiteral("COM1"),
            QByteArrayLiteral("lpt9.txt"),
            QByteArrayLiteral("C:/abs/path.esd"),
            QByteArrayLiteral("name:stream.esd"),
            QByteArrayLiteral("trailing.dot."),
            QByteArrayLiteral("trailing space "),
            QByteArrayLiteral("line\nbreak.esd"),
            QByteArrayLiteral("tab\there.esd"),
            QByteArray("\0nul-lead.esd", 13),
            QByteArrayLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"),
            QByteArrayLiteral("https://dl.delivery.mp.microsoft.com/x.esd"),
            QByteArray(),
        };
        const sak::fuzz::Target target = [](const QByteArray& input) {
            return manifestGuardInvariant(input);
        };
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, target, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        // Exact count on the all-pass path (any failure QVERIFY2(false)-returns above): run()
        // increments iterations_run once per seed plus once per mutation iteration. The old >=
        // bound would still pass if the mutation loop ran ZERO iterations.
        QCOMPARE(outcome.iterations_run,
                 static_cast<int>(corpus.size()) + sak::fuzz::iterationsFromEnv());
    }

    // Pin the accept/reject anchors the fuzz oracle is built around: a clean entry is accepted, and
    // each named attack class is rejected. If these drift, the oracle above is meaningless.
    void knownEntriesClassifyAsExpected() {
        const auto entry = [](const QString& name, const QString& url, const QString& sha1) {
            UupDumpApi::FileInfo info;
            info.fileName = name;
            info.url = url;
            info.sha1 = sha1;
            return info;
        };
        QVERIFY(UupDumpApi::isSafeAria2FileEntry(entry(kValidName, kValidUrl, kValidSha1)));
        QVERIFY(!UupDumpApi::isSafeAria2FileEntry(
            entry(QStringLiteral("../esc.esd"), kValidUrl, kValidSha1)));
        QVERIFY(
            !UupDumpApi::isSafeAria2FileEntry(entry(QStringLiteral("CON"), kValidUrl, kValidSha1)));
        QVERIFY(!UupDumpApi::isSafeAria2FileEntry(
            entry(QStringLiteral("a:b.esd"), kValidUrl, kValidSha1)));
        QVERIFY(!UupDumpApi::isSafeAria2FileEntry(
            entry(QStringLiteral("trail. "), kValidUrl, kValidSha1)));
        QVERIFY(!UupDumpApi::isSafeAria2FileEntry(
            entry(kValidName, QStringLiteral("http://x/\ninject"), kValidSha1)));
        QVERIFY(UupDumpApi::isValidSha1(kValidSha1));
        QVERIFY(!UupDumpApi::isValidSha1(QStringLiteral("nothex")));
    }
};

QTEST_GUILESS_MAIN(UupManifestGuardFuzzTests)
#include "test_fuzz_uup_manifest_guard.moc"
