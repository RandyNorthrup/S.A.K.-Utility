// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/win32_mcp_fingerprint.h"

#include <QtTest/QtTest>

using sak::win32mcp::fingerprint;
using sak::win32mcp::fingerprintChanged;
using sak::win32mcp::fingerprintDiff;
using sak::win32mcp::kFingerprintGrid;

namespace {

constexpr int kCells = kFingerprintGrid * kFingerprintGrid;

// A width*height BGRA buffer filled with one gray level (B=G=R=level).
QByteArray solidBgra(int width, int height, unsigned char level) {
    QByteArray buf(static_cast<qsizetype>(width) * height * 4, 0);
    for (qsizetype i = 0; i + 3 < buf.size(); i += 4) {
        buf[i] = static_cast<char>(level);
        buf[i + 1] = static_cast<char>(level);
        buf[i + 2] = static_cast<char>(level);
    }
    return buf;
}

// A synthetic fingerprint of kCells bytes all `base`, with the first `nDifferent` cells bumped by
// `delta` -- so a diff against an all-`base` fingerprint sees exactly nDifferent changed cells.
QByteArray fpWith(unsigned char base, int nDifferent, int delta) {
    QByteArray fp(kCells, static_cast<char>(base));
    for (int i = 0; i < nDifferent && i < kCells; ++i) {
        fp[i] = static_cast<char>(static_cast<unsigned char>(base + delta));
    }
    return fp;
}

}  // namespace

class Win32McpFingerprintTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void fingerprintOfValidBufferIsGridSized();
    void identicalFingerprintsDiffZero();
    void emptyOrShortBufferReturnsEmpty();
    void degenerateFingerprintReadsAsChanged();
    void sizeMismatchIsFullyChanged();
    void toleranceBoundaryAtSixteen();
    void changedRatioBoundaryAtFiveVsSixCells();
    void signedGrayRoundTripsThroughUnsigned();
};

void Win32McpFingerprintTests::fingerprintOfValidBufferIsGridSized() {
    const QByteArray fp = fingerprint(solidBgra(40, 30, 128), 40, 30);
    // Every pixel is B=G=R=128, so each of the kCells grid cells is gray 128; pin the full content
    // (subsumes the size check) so a broken sampler (wrong channel / never-run fill) is caught.
    QCOMPARE(fp, QByteArray(kCells, static_cast<char>(128)));
}

void Win32McpFingerprintTests::identicalFingerprintsDiffZero() {
    const QByteArray fp = fingerprint(solidBgra(64, 64, 200), 64, 64);
    QCOMPARE(fingerprintDiff(fp, fp), 0.0);
    QVERIFY(!fingerprintChanged(fp, fp));
}

void Win32McpFingerprintTests::emptyOrShortBufferReturnsEmpty() {
    QVERIFY(fingerprint(QByteArray(), 10, 10).isEmpty());          // no buffer
    QVERIFY(fingerprint(solidBgra(4, 4, 0), 10, 10).isEmpty());    // buffer smaller than w*h*4
    QVERIFY(fingerprint(solidBgra(10, 10, 0), 0, 10).isEmpty());   // non-positive width
    QVERIFY(fingerprint(solidBgra(10, 10, 0), 10, -1).isEmpty());  // non-positive height
}

void Win32McpFingerprintTests::degenerateFingerprintReadsAsChanged() {
    const QByteArray good = fingerprint(solidBgra(20, 20, 100), 20, 20);
    QCOMPARE(fingerprintDiff(QByteArray(), good), 1.0);  // empty baseline -> fully changed
    QVERIFY(fingerprintChanged(QByteArray(), good));
}

void Win32McpFingerprintTests::sizeMismatchIsFullyChanged() {
    QCOMPARE(fingerprintDiff(QByteArray(kCells, 0), QByteArray(kCells - 1, 0)), 1.0);
}

void Win32McpFingerprintTests::toleranceBoundaryAtSixteen() {
    // A per-cell delta of exactly 16 is NOT over tolerance (16) -> not counted; 17 is counted.
    QCOMPARE(fingerprintDiff(QByteArray(kCells, static_cast<char>(100)), fpWith(100, 1, 16)), 0.0);
    QCOMPARE(fingerprintDiff(QByteArray(kCells, static_cast<char>(100)), fpWith(100, 1, 17)),
             1.0 / kCells);
}

void Win32McpFingerprintTests::changedRatioBoundaryAtFiveVsSixCells() {
    // 5/256 = 0.0195 <= 0.02 -> not changed; 6/256 = 0.0234 > 0.02 -> changed.
    const QByteArray base(kCells, static_cast<char>(100));
    QVERIFY(!fingerprintChanged(base, fpWith(100, 5, 40)));
    QVERIFY(fingerprintChanged(base, fpWith(100, 6, 40)));
}

void Win32McpFingerprintTests::signedGrayRoundTripsThroughUnsigned() {
    // High gray values are stored as (signed) char; the diff must read them back as unsigned, so a
    // 250-vs-250 comparison is 0 and 250-vs-233 (delta 17) counts as changed.
    QCOMPARE(fingerprintDiff(QByteArray(kCells, static_cast<char>(250)),
                             QByteArray(kCells, static_cast<char>(250))),
             0.0);
    QCOMPARE(fingerprintDiff(QByteArray(kCells, static_cast<char>(250)), fpWith(250, 1, -17)),
             1.0 / kCells);
}

QTEST_GUILESS_MAIN(Win32McpFingerprintTests)
#include "test_win32_mcp_fingerprint.moc"
