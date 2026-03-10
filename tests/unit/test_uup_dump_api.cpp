// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_uup_dump_api.cpp
/// @brief Unit tests for UupDumpApi static helpers and channel mappings

#include "sak/uup_dump_api.h"

#include <QtTest/QtTest>

class TestUupDumpApi : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // ── Construction ──────────────────────────────────────────────
    void construction_default();
    void construction_nonCopyable();

    // ── channelToRing ─────────────────────────────────────────────
    void channelToRing_retail();
    void channelToRing_releasePreview();
    void channelToRing_beta();
    void channelToRing_dev();
    void channelToRing_canary();
    void channelToRing_allNonEmpty();

    // ── channelToDisplayName ──────────────────────────────────────
    void channelToDisplayName_retail();
    void channelToDisplayName_dev();
    void channelToDisplayName_allNonEmpty();

    // ── allChannels ───────────────────────────────────────────────
    void allChannels_nonEmpty();
    void allChannels_containsRetail();
    void allChannels_noDuplicates();

    // ── BuildInfo defaults ────────────────────────────────────────
    void buildInfo_defaults();

    // ── FileInfo defaults ─────────────────────────────────────────
    void fileInfo_defaults();
};

// ═══════════════════════════════════════════════════════════════════
// Construction
// ═══════════════════════════════════════════════════════════════════

void TestUupDumpApi::construction_default() {
    UupDumpApi api;
    QVERIFY(api.parent() == nullptr);
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
}

void TestUupDumpApi::construction_nonCopyable() {
    QVERIFY(!std::is_copy_constructible_v<UupDumpApi>);
    QVERIFY(!std::is_move_constructible_v<UupDumpApi>);
}

// ═══════════════════════════════════════════════════════════════════
// channelToRing
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// channelToDisplayName
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// allChannels
// ═══════════════════════════════════════════════════════════════════

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

// ═══════════════════════════════════════════════════════════════════
// BuildInfo / FileInfo defaults
// ═══════════════════════════════════════════════════════════════════

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

QTEST_MAIN(TestUupDumpApi)
#include "test_uup_dump_api.moc"
