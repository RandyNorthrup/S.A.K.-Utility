// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_uup_dump_api.cpp
/// @brief Unit tests for UupDumpApi static helpers and channel mappings

#include "sak/uup_dump_api.h"

#include <QtTest/QtTest>

class TestUupDumpApi : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // -- Construction ----------------------------------------------
    void construction_default();
    void construction_nonCopyable();

    // -- channelToRing ---------------------------------------------
    void channelToRing_retail();
    void channelToRing_releasePreview();
    void channelToRing_beta();
    void channelToRing_dev();
    void channelToRing_canary();
    void channelToRing_allNonEmpty();

    // -- channelToDisplayName --------------------------------------
    void channelToDisplayName_retail();
    void channelToDisplayName_dev();
    void channelToDisplayName_allNonEmpty();

    // -- allChannels -----------------------------------------------
    void allChannels_nonEmpty();
    void allChannels_containsRetail();
    void allChannels_noDuplicates();

    // -- BuildInfo defaults ----------------------------------------
    void buildInfo_defaults();

    // -- FileInfo defaults -----------------------------------------
    void fileInfo_defaults();

    // -- SHA-1 validation (B10-20) ---------------------------------
    void isValidSha1_acceptsWellFormed();
    void isValidSha1_rejectsMalformed();

    // -- isSafeAria2FileEntry (aria2 manifest injection guard) -----
    // fileName/url/sha1 are serialized into an attacker-influenced aria2 input
    // manifest; each slot pins one rejection predicate (or a scan boundary) with
    // a positive (accept) and a negative (reject) case.
    void isSafeAria2FileEntry_acceptsNormalEntry();
    void isSafeAria2FileEntry_rejectsPathTraversal();
    void isSafeAria2FileEntry_rejectsPathSeparators();
    void isSafeAria2FileEntry_rejectsDriveOrAds();
    void isSafeAria2FileEntry_rejectsTrailingDotOrSpace();
    void isSafeAria2FileEntry_rejectsReservedDeviceName();
    void isSafeAria2FileEntry_rejectsControlCharInFileName();
    void isSafeAria2FileEntry_rejectsControlCharInUrl();
    void isSafeAria2FileEntry_rejectsControlCharInSha1();
    void isSafeAria2FileEntry_controlCharBoundary();
};

// ===================================================================
// Construction
// ===================================================================

void TestUupDumpApi::construction_default() {
    UupDumpApi api;
    QVERIFY(api.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
}

void TestUupDumpApi::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
    QVERIFY(!std::is_move_constructible_v<UupDumpApi>);
}

// ===================================================================
// channelToRing
// ===================================================================

void TestUupDumpApi::channelToRing_retail() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Retail);
    QVERIFY(!ring.isEmpty());
}

void TestUupDumpApi::channelToRing_releasePreview() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::ReleasePreview);
    QVERIFY(!ring.isEmpty());
}

void TestUupDumpApi::channelToRing_beta() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Beta);
    QVERIFY(!ring.isEmpty());
}

void TestUupDumpApi::channelToRing_dev() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Dev);
    QVERIFY(!ring.isEmpty());
}

void TestUupDumpApi::channelToRing_canary() {
    const auto ring = UupDumpApi::channelToRing(UupDumpApi::ReleaseChannel::Canary);
    QVERIFY(!ring.isEmpty());
}

void TestUupDumpApi::channelToRing_allNonEmpty() {
    for (const auto channel : UupDumpApi::allChannels()) {
        const auto ring = UupDumpApi::channelToRing(channel);
        QVERIFY2(!ring.isEmpty(), qPrintable(QString("channelToRing returned empty for channel")));
    }
}

// ===================================================================
// channelToDisplayName
// ===================================================================

void TestUupDumpApi::channelToDisplayName_retail() {
    const auto name = UupDumpApi::channelToDisplayName(UupDumpApi::ReleaseChannel::Retail);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("Retail", Qt::CaseInsensitive) ||
            name.contains("Stable", Qt::CaseInsensitive) ||
            name.contains("Release", Qt::CaseInsensitive));
}

void TestUupDumpApi::channelToDisplayName_dev() {
    const auto name = UupDumpApi::channelToDisplayName(UupDumpApi::ReleaseChannel::Dev);
    QVERIFY(!name.isEmpty());
    QVERIFY(name.contains("Dev", Qt::CaseInsensitive));
}

void TestUupDumpApi::channelToDisplayName_allNonEmpty() {
    for (const auto channel : UupDumpApi::allChannels()) {
        const auto name = UupDumpApi::channelToDisplayName(channel);
        QVERIFY2(!name.isEmpty(), "channelToDisplayName returned empty for a channel");
    }
}

// ===================================================================
// allChannels
// ===================================================================

void TestUupDumpApi::allChannels_nonEmpty() {
    const auto channels = UupDumpApi::allChannels();
    QVERIFY(!channels.isEmpty());
    QVERIFY(channels.size() >= 5);
}

void TestUupDumpApi::allChannels_containsRetail() {
    const auto channels = UupDumpApi::allChannels();
    QVERIFY(channels.contains(UupDumpApi::ReleaseChannel::Retail));
}

void TestUupDumpApi::allChannels_noDuplicates() {
    const auto channels = UupDumpApi::allChannels();
    QSet<int> unique;
    for (const auto channel : channels) {
        unique.insert(static_cast<int>(channel));
    }
    QCOMPARE(unique.size(), channels.size());
}

// ===================================================================
// BuildInfo / FileInfo defaults
// ===================================================================

void TestUupDumpApi::buildInfo_defaults() {
    UupDumpApi::BuildInfo info;
    QVERIFY(info.uuid.isEmpty());
    QVERIFY(info.title.isEmpty());
    QVERIFY(info.build.isEmpty());
    QVERIFY(info.arch.isEmpty());
    QCOMPARE(info.created, static_cast<qint64>(0));
}

void TestUupDumpApi::fileInfo_defaults() {
    UupDumpApi::FileInfo info;
    QVERIFY(info.fileName.isEmpty());
    QVERIFY(info.sha1.isEmpty());
    QCOMPARE(info.size, static_cast<qint64>(0));
    QVERIFY(info.url.isEmpty());
}

// ===================================================================
// SHA-1 validation (B10-20)
// ===================================================================

void TestUupDumpApi::isValidSha1_acceptsWellFormed() {
    // 40 hex chars, lower/upper/mixed case.
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709")));
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("DA39A3EE5E6B4B0D3255BFEF95601890AFD80709")));
    QVERIFY(UupDumpApi::isValidSha1(QStringLiteral("Da39A3ee5e6B4b0d3255bFEF95601890afd80709")));
}

void TestUupDumpApi::isValidSha1_rejectsMalformed() {
    QVERIFY(!UupDumpApi::isValidSha1(QString()));                        // empty
    QVERIFY(!UupDumpApi::isValidSha1(QStringLiteral("da39a3ee")));       // too short
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709aa")));  // too long
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd8070g")));    // non-hex 'g'
    QVERIFY(!UupDumpApi::isValidSha1(
        QStringLiteral("da39a3ee5e6b4b0d3255 fef95601890afd80709")));    // embedded space
}

// ===================================================================
// isSafeAria2FileEntry (aria2 manifest injection guard)
// ===================================================================

namespace {

// A fully safe file entry: every field is accepted by the guard. Individual
// slots mutate one field to a boundary/attack value and re-check.
UupDumpApi::FileInfo safeBaselineEntry() {
    UupDumpApi::FileInfo info;
    info.fileName = QStringLiteral("core_en-us.esd");
    info.url = QStringLiteral("https://amd64.uup.microsoft.com/core_en-us.esd");
    info.sha1 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709");
    info.size = 12'345;
    return info;
}

}  // namespace

void TestUupDumpApi::isSafeAria2FileEntry_acceptsNormalEntry() {
    // Ordinary CDN file names with underscores, hyphens, and dots are accepted.
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(safeBaselineEntry()));

    auto e = safeBaselineEntry();
    e.fileName = QStringLiteral("professional_x64-26100.1.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));

    e.fileName = QStringLiteral("UUPDump-metadata.txt");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsPathTraversal() {
    auto e = safeBaselineEntry();
    // ".." anywhere is traversal -- even with no separator. Pins the ".." predicate
    // in isolation so dropping it cannot be masked by the '/' or '\\' checks.
    e.fileName = QStringLiteral("foo..esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("../evil.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("..\\evil.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsPathSeparators() {
    auto e = safeBaselineEntry();
    // '/' only (no ".."), isolates the forward-slash predicate.
    e.fileName = QStringLiteral("sub/core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // '\\' only, isolates the backslash predicate.
    e.fileName = QStringLiteral("sub\\core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Absolute POSIX and UNC forms remain rejected via the same separators.
    e.fileName = QStringLiteral("/etc/passwd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("\\\\server\\share");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsDriveOrAds() {
    auto e = safeBaselineEntry();
    // ':' only (no separator) -> NTFS alternate data stream. Isolates the ':' predicate.
    e.fileName = QStringLiteral("core.esd:zone");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Drive-absolute form.
    e.fileName = QStringLiteral("C:core.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsTrailingDotOrSpace() {
    auto e = safeBaselineEntry();
    // Windows trims a trailing dot -> on-disk name differs from the integrity-checked
    // name. Isolates the trailing-dot predicate (no "..", no separator, base not reserved).
    e.fileName = QStringLiteral("core.esd.");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // Windows trims a trailing space likewise. Isolates the trailing-space predicate.
    e.fileName = QStringLiteral("core.esd ");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsReservedDeviceName() {
    auto e = safeBaselineEntry();
    // A reserved DOS device name, with or without extension, any case: aria2 out=con
    // would open the console device instead of creating a file.
    e.fileName = QStringLiteral("CON.esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("nul");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("COM1.txt");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInFileName() {
    auto e = safeBaselineEntry();
    // A C0 control char in the name could truncate the aria2 line or break the record.
    e.fileName = QStringLiteral("core") + QChar(u'\n') + QStringLiteral(".esd");        // LF
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(u'\r') + QStringLiteral(".esd");        // CR
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(u'\t') + QStringLiteral(".esd");        // TAB
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    e.fileName = QStringLiteral("core") + QChar(char16_t(0)) + QStringLiteral(".esd");  // NUL
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInUrl() {
    auto e = safeBaselineEntry();
    // CRLF in the URL could break the aria2 line and inject an out=/uri directive.
    e.url = QStringLiteral("https://amd64.uup.microsoft.com/x") + QChar(u'\r') + QChar(u'\n') +
            QStringLiteral(" out=/etc/passwd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_rejectsControlCharInSha1() {
    auto e = safeBaselineEntry();
    // A control char in the checksum field is likewise serialized -> must be rejected.
    e.sha1 = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd8") + QChar(u'\n') +
             QStringLiteral("0709");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

void TestUupDumpApi::isSafeAria2FileEntry_controlCharBoundary() {
    auto e = safeBaselineEntry();
    // 0x20 (space) is the first printable code unit -> an INTERNAL space is accepted.
    // Pins the '< 0x20' lower boundary: relaxing '<' to '<=' would reject this.
    e.fileName = QStringLiteral("core update.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    // 0x7e ('~') sits just below DEL and is printable -> accepted.
    e.fileName = QStringLiteral("core~1.esd");
    QVERIFY(UupDumpApi::isSafeAria2FileEntry(e));
    // 0x1f (unit separator, < 0x20) -> rejected.
    e.fileName = QStringLiteral("core") + QChar(char16_t(0x1f)) + QStringLiteral(".esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
    // 0x7f (DEL) -> rejected. Pins the '== 0x7f' predicate specifically (the '< 0x20'
    // test does not cover it).
    e.fileName = QStringLiteral("core") + QChar(char16_t(0x7f)) + QStringLiteral(".esd");
    QVERIFY(!UupDumpApi::isSafeAria2FileEntry(e));
}

QTEST_MAIN(TestUupDumpApi)
#include "test_uup_dump_api.moc"
