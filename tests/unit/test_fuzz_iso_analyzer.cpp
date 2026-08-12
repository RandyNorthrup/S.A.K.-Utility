// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_fuzz_iso_analyzer.cpp
/// @brief Mutation-fuzz of the ISO 9660 / El Torito / UDF volume-descriptor parser
///        (G14, iso_analyzer).
///
/// The app classifies downloaded install media (Windows vs Linux, bootable, editions) by
/// parsing the ISO's volume descriptors -- attacker-controlled bytes. That parsing read
/// from a QIODevice behind IsoAnalyzer::analyze(const QString&), which opened a QFile, so
/// it could not be driven with a raw buffer. A byte-in seam, IsoAnalyzer::analyzeDevice,
/// now does the QIODevice work and analyze() just wraps it around a QFile::open(),
/// behaviour unchanged. This harness feeds thousands of mutated images through a QBuffer
/// and asserts the parser never crashes or hangs on any bytes, and that it reports the
/// media size straight from the stream it read. The seeds are sized past the ISO 9660
/// system area so mutants reach the primary-volume-descriptor scan (LBA 16) and the
/// boot-catalog / UDF reads rather than bouncing off the minimum-size gate.

#include "sak/iso_analyzer.h"

#include "../fuzz/fuzz_harness.h"

#include <QBuffer>
#include <QByteArray>
#include <QString>
#include <QtTest/QtTest>

#include <vector>

namespace {

// ISO 9660 geometry (ECMA-119): 2 KiB logical sectors, a 16-sector system area, so the
// primary volume descriptor sits at LBA 16. A seed must clear that plus one sector to pass
// the analyzer's minimum-size gate and exercise the descriptor scan.
constexpr int kIsoSectorSize = 2048;
constexpr int kIsoSystemAreaSectors = 16;
constexpr int kIsoPvdOffset = kIsoSystemAreaSectors * kIsoSectorSize;
constexpr int kIsoSeedSectors = 4;
constexpr int kIsoSeedSize = kIsoPvdOffset + kIsoSeedSectors * kIsoSectorSize;
constexpr char kVdTypePrimary = 1;

// A minimally-valid ISO image: zeroed, with a primary volume descriptor (type byte + the
// "CD001" standard identifier) stamped at LBA 16 so the parser walks the field extraction.
QByteArray validishIsoSeed() {
    QByteArray image(kIsoSeedSize, '\0');
    image[kIsoPvdOffset] = kVdTypePrimary;
    image.replace(kIsoPvdOffset + 1, 5, QByteArray("CD001"));
    return image;
}

std::vector<QByteArray> isoCorpus() {
    return {
        QByteArray(),
        QByteArray("MZ short header, far too small for an ISO"),
        QByteArray(kIsoSeedSize, '\0'),    // passes the size gate, no CD001
        QByteArray(kIsoSeedSize, '\xFF'),  // all ones
        validishIsoSeed(),
    };
}

QString isoInvariant(const QByteArray& input) {
    QByteArray bytes = input;
    QBuffer buffer(&bytes);
    if (!buffer.open(QIODevice::ReadOnly)) {
        return QStringLiteral("could not open the in-memory buffer");
    }
    const sak::IsoInfo info = sak::IsoAnalyzer::analyzeDevice(buffer);
    // analyzeDevice sizes the media straight from the stream it parsed.
    if (info.file_size != static_cast<qint64>(bytes.size())) {
        return QStringLiteral("reported file_size %1 != stream size %2")
            .arg(info.file_size)
            .arg(bytes.size());
    }
    return {};  // no crash / no hang == invariant satisfied
}

QByteArray failureBanner(const sak::fuzz::FuzzOutcome& outcome) {
    const QString message =
        QStringLiteral("ISO analyzer fuzz failed after %1 inputs: %2\n  bytes (hex): %3")
            .arg(outcome.iterations_run)
            .arg(outcome.failure_detail, sak::fuzz::reproducerHex(outcome.failing_input));
    return message.toUtf8();
}

}  // namespace

class IsoAnalyzerFuzzTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void analyzeDeviceNeverCrashesOnAnyBytes() {
        const std::vector<QByteArray> corpus = isoCorpus();
        const sak::fuzz::FuzzOutcome outcome = sak::fuzz::run(
            corpus, isoInvariant, sak::fuzz::iterationsFromEnv(), sak::fuzz::seedFromEnv());
        if (!outcome.ok) {
            const QByteArray banner = failureBanner(outcome);
            QVERIFY2(false, banner.constData());
        }
        QVERIFY(outcome.iterations_run >= static_cast<int>(corpus.size()));
    }
};

QTEST_APPLESS_MAIN(IsoAnalyzerFuzzTests)
#include "test_fuzz_iso_analyzer.moc"
