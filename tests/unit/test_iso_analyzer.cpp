// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_iso_analyzer.cpp
/// @brief Unit tests for IsoAnalyzer classification helpers.

#include "sak/iso_analyzer.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <algorithm>

using namespace sak;

namespace {
bool nameContainsAny(const QString& name, const QStringList& tokens) {
    return std::any_of(tokens.cbegin(), tokens.cend(), [&name](const QString& token) {
        return name.contains(token);
    });
}

// ============================================================================
// Synthetic ISO 9660 images
//
// Every other test in this file hands IsoAnalyzer a pre-filled IsoInfo, so until
// now the ISO 9660 READER -- analyze(), which is what actually looks at an
// untrusted image on disk -- ran only under the env-gated corpus test below, i.e.
// never in CI. That reader decides isWindowsInstallISO(), which routes the Image
// Flasher between two different destructive writers, so it is exercised here
// against real bytes.
//
// The images are built rather than committed: a few dozen kilobytes of structure
// with nothing in it, so a binary fixture would add opaque blobs to the repo for
// bytes that are clearer as code. The env-gated corpus test stays -- it covers
// real-world images, which these cannot.
// ============================================================================

constexpr int kSectorSize = 2048;
constexpr int kSystemAreaSectors = 16;
constexpr int kPvdSector = 16;
constexpr int kBootRecordSector = 17;
constexpr int kTerminatorSector = 18;
constexpr int kBootCatalogSector = 19;
constexpr int kSyntheticSectors = 20;

// PVD field offsets, from ECMA-119 section 8.4.
constexpr int kPvdSystemIdOffset = 8;
constexpr int kPvdVolumeIdOffset = 40;
constexpr int kPvdVolumeSizeLsbOffset = 80;
constexpr int kPvdPublisherOffset = 318;
constexpr int kPvdPreparerOffset = 446;
constexpr int kPvdApplicationOffset = 574;
constexpr int kPvdCreationDateOffset = 813;

// El Torito boot record layout.
constexpr int kElToritoIdOffset = 7;
constexpr int kElToritoCatalogLbaOffset = 71;
constexpr int kCatalogDefaultEntryOffset = 32;
constexpr uint8_t kBootIndicatorBootable = 0x88;
constexpr uint8_t kPlatformEfi = 0xEF;
constexpr uint8_t kPlatformX86 = 0x00;

/// @brief Write @p text at @p offset, space-padded to @p length (ISO 9660 pads
///        its a-characters/d-characters fields with spaces, not NULs).
void writePadded(QByteArray& image, int offset, const char* text, int length) {
    const QByteArray value(text);
    for (int i = 0; i < length; ++i) {
        image[offset + i] = (i < value.size()) ? value.at(i) : ' ';
    }
}

void writeLe32(QByteArray& image, int offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        image[offset + i] = static_cast<char>((value >> (8 * i)) & 0xFF);
    }
}

struct SyntheticIsoSpec {
    const char* volume_label = "";
    const char* publisher = "";
    const char* preparer = "";
    const char* application = "";
    bool with_boot_record = false;
    uint8_t boot_platform = kPlatformX86;
    bool boot_entry_bootable = true;
};

/// @brief Build a minimal but genuine ISO 9660 image: a zeroed 16-sector system
///        area, a Primary Volume Descriptor, optionally an El Torito boot record
///        plus its catalog, and a terminator.
QByteArray buildSyntheticIso(const SyntheticIsoSpec& spec) {
    QByteArray image(kSyntheticSectors * kSectorSize, '\0');

    const int pvd = kPvdSector * kSectorSize;
    image[pvd] = 1;      // Primary Volume Descriptor
    writePadded(image, pvd + 1, "CD001", 5);
    image[pvd + 6] = 1;  // descriptor version
    writePadded(image, pvd + kPvdSystemIdOffset, "", 32);
    writePadded(image, pvd + kPvdVolumeIdOffset, spec.volume_label, 32);
    writeLe32(image, pvd + kPvdVolumeSizeLsbOffset, kSyntheticSectors);
    writePadded(image, pvd + kPvdPublisherOffset, spec.publisher, 128);
    writePadded(image, pvd + kPvdPreparerOffset, spec.preparer, 128);
    writePadded(image, pvd + kPvdApplicationOffset, spec.application, 128);
    writePadded(image, pvd + kPvdCreationDateOffset, "2026010203040500", 16);

    if (spec.with_boot_record) {
        const int boot = kBootRecordSector * kSectorSize;
        image[boot] = 0;  // Boot Record
        writePadded(image, boot + 1, "CD001", 5);
        image[boot + 6] = 1;
        writePadded(image, boot + kElToritoIdOffset, "EL TORITO SPECIFICATION", 23);
        writeLe32(image, boot + kElToritoCatalogLbaOffset, kBootCatalogSector);

        const int catalog = kBootCatalogSector * kSectorSize;
        image[catalog] = 0x01;  // Validation Entry header
        image[catalog + 1] = static_cast<char>(spec.boot_platform);
        image[catalog + kCatalogDefaultEntryOffset] =
            spec.boot_entry_bootable ? static_cast<char>(kBootIndicatorBootable) : 0;
    }

    const int terminator = kTerminatorSector * kSectorSize;
    image[terminator] = static_cast<char>(0xFF);
    writePadded(image, terminator + 1, "CD001", 5);
    image[terminator + 6] = 1;

    return image;
}

}  // namespace

class IsoAnalyzerTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void windowsFamilyIsInstallMedia();
    void windowsEditionsImplyInstallMedia();
    void linuxServerIsoIsNotWindows();
    void unknownOrEmptyIsNotWindows();

    // analyze() over real bytes
    void analyzeReadsPrimaryVolumeDescriptorFields();
    void analyzeClassifiesSyntheticWindowsMediaFromLabel();
    void analyzeClassifiesSyntheticLinuxMediaAsNotWindows();
    void analyzeReadsElToritoBootTypeFromCatalog();
    void analyzeRejectsATruncatedImage();
    void analyzeRejectsBytesThatAreNotIso9660();
    void analyzeStopsAtTheDescriptorSetTerminator();
    void analyzeAcceptsImageExactlyAtMinimumSize();
    void analyzeStopsScanAtFirstNonDescriptorSector();
    void analyzeIgnoresShortReadOfLaterDescriptor();

    void realIsosClassifyByContent();

private:
    /// @brief Write @p image to a file under m_dir and return its path.
    QString writeImage(const QString& name, const QByteArray& image);

    QTemporaryDir m_dir;
};

QString IsoAnalyzerTests::writeImage(const QString& name, const QByteArray& image) {
    const QString path = m_dir.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return QString();
    }
    const qint64 written = file.write(image);
    file.close();
    return written == image.size() ? path : QString();
}

// os_family == "Windows" (from the volume label / application ID) routes to the
// Windows install USB path.
void IsoAnalyzerTests::windowsFamilyIsInstallMedia() {
    IsoInfo info;
    info.os_family = QStringLiteral("Windows");
    QVERIFY(IsoAnalyzer::isWindowsInstallMedia(info));
}

// Editions parsed from install.wim/esd metadata are a Windows-media signal even
// if os_family were somehow unset.
void IsoAnalyzerTests::windowsEditionsImplyInstallMedia() {
    IsoInfo info;
    info.windows_editions = {QStringLiteral("Windows 11 Pro")};
    QVERIFY(IsoAnalyzer::isWindowsInstallMedia(info));
}

// P12-09: a Linux server ISO (family "Linux") must NOT be treated as Windows --
// the old filename heuristic matched "server" and mis-routed it.
void IsoAnalyzerTests::linuxServerIsoIsNotWindows() {
    IsoInfo info;
    info.os_family = QStringLiteral("Linux");
    info.distro_name = QStringLiteral("Ubuntu");
    info.volume_label = QStringLiteral("Ubuntu-Server 24.04.1 LTS amd64");
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

void IsoAnalyzerTests::unknownOrEmptyIsNotWindows() {
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(IsoInfo{}));
    IsoInfo unknown;
    unknown.os_family = QStringLiteral("Unknown");
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(unknown));
}

// ============================================================================
// analyze() over real bytes
// ============================================================================

// Every PVD string field is space-padded, so a reader that does not trim reports
// a label with 20 trailing spaces -- which then fails every containsAny() match
// the classification depends on.
void IsoAnalyzerTests::analyzeReadsPrimaryVolumeDescriptorFields() {
    QVERIFY(m_dir.isValid());
    const QString path = writeImage("fields.iso",
                                    buildSyntheticIso({.volume_label = "SAK_TEST_VOL",
                                                       .publisher = "SAK PUBLISHER",
                                                       .preparer = "SAK PREPARER",
                                                       .application = "SAK APPLICATION"}));
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QCOMPARE(info.volume_label, QStringLiteral("SAK_TEST_VOL"));
    QCOMPARE(info.publisher, QStringLiteral("SAK PUBLISHER"));
    QCOMPARE(info.preparer, QStringLiteral("SAK PREPARER"));
    QCOMPARE(info.application, QStringLiteral("SAK APPLICATION"));
    QCOMPARE(info.filesystem, QStringLiteral("ISO 9660"));
    QCOMPARE(info.volume_size, static_cast<uint64_t>(kSyntheticSectors) * kSectorSize);
    QCOMPARE(info.creation_date, QStringLiteral("2026-01-02 03:04:05"));  // parsed from the PVD
    QVERIFY(!info.is_bootable);  // no El Torito boot record in this image
}

// The Windows routing decision, made from bytes rather than from a hand-built
// IsoInfo. A miss here sends Windows install media down the raw-image writer,
// which produces an unbootable USB.
void IsoAnalyzerTests::analyzeClassifiesSyntheticWindowsMediaFromLabel() {
    QVERIFY(m_dir.isValid());
    const QString path =
        writeImage("windows.iso",
                   buildSyntheticIso({.volume_label = "CCCOMA_X64FRE_EN-US_DV9",
                                      .application = "CDIMAGE 2.56 (01/01/2026 TM)"}));
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QCOMPARE(info.os_family, QStringLiteral("Windows"));
    QCOMPARE(info.architecture, QStringLiteral("x64"));  // from the X64 label token
    QCOMPARE(info.os_name, QStringLiteral("Windows"));   // no 11/10/server token -> default
    QVERIFY(IsoAnalyzer::isWindowsInstallMedia(info));
}

// P12-09 end to end: the old filename heuristic matched "server" and routed Linux
// server images into the Windows installer path. Asserted from the image's own
// bytes, not from a filename and not from a pre-filled struct.
void IsoAnalyzerTests::analyzeClassifiesSyntheticLinuxMediaAsNotWindows() {
    QVERIFY(m_dir.isValid());
    const QString path = writeImage("ubuntu-server.iso",
                                    buildSyntheticIso({.volume_label = "Ubuntu-Server 24.04.1 LTS "
                                                                       "amd64"}));
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QCOMPARE(info.os_family, QStringLiteral("Linux"));
    QCOMPARE(info.distro_name, QStringLiteral("Ubuntu"));
    QCOMPARE(info.distro_version, QStringLiteral("24.04.1"));  // regex over the label
    QCOMPARE(info.os_name, QStringLiteral("Ubuntu 24.04.1"));  // name + version composite
    QCOMPARE(info.architecture, QStringLiteral("x64"));        // amd64 label token -> x64
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

// The platform lives in the Validation Entry at catalog offset 1; the byte at
// offset 32 is the Default Entry's boot INDICATOR. Reading offset 32 as a
// platform id reported every EFI-only image as Legacy BIOS.
void IsoAnalyzerTests::analyzeReadsElToritoBootTypeFromCatalog() {
    QVERIFY(m_dir.isValid());

    const QString efi = writeImage("efi.iso",
                                   buildSyntheticIso({.volume_label = "SAK_EFI",
                                                      .with_boot_record = true,
                                                      .boot_platform = kPlatformEfi}));
    QVERIFY(!efi.isEmpty());
    const IsoInfo efi_info = IsoAnalyzer::analyze(efi);
    QVERIFY(efi_info.is_bootable);
    QCOMPARE(efi_info.boot_type, QStringLiteral("UEFI"));

    const QString bios = writeImage("bios.iso",
                                    buildSyntheticIso({.volume_label = "SAK_BIOS",
                                                       .with_boot_record = true,
                                                       .boot_platform = kPlatformX86}));
    QVERIFY(!bios.isEmpty());
    const IsoInfo bios_info = IsoAnalyzer::analyze(bios);
    QVERIFY(bios_info.is_bootable);
    QCOMPARE(bios_info.boot_type, QStringLiteral("Legacy BIOS"));
}

// A file too short to hold a system area plus one descriptor must come back
// empty, not partially parsed off the end of the buffer.
void IsoAnalyzerTests::analyzeRejectsATruncatedImage() {
    QVERIFY(m_dir.isValid());
    QByteArray truncated = buildSyntheticIso({.volume_label = "SAK_TRUNC"});
    truncated.truncate(kSystemAreaSectors * kSectorSize + 32);
    const QString path = writeImage("truncated.iso", truncated);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QVERIFY(info.volume_label.isEmpty());
    QVERIFY(info.os_family.isEmpty());
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

// Non-ISO content of a plausible size must not be classified as anything. This is
// the fail-closed case that matters most: the flasher asks this question about a
// file the user picked, and an accidental "Windows" answer starts a diskpart run
// against a drive.
void IsoAnalyzerTests::analyzeRejectsBytesThatAreNotIso9660() {
    QVERIFY(m_dir.isValid());
    QByteArray junk(kSyntheticSectors * kSectorSize, '\0');
    // Fill with a repeating pattern that happens to contain "WIN", so a reader
    // that scanned for text instead of parsing the descriptor would trip.
    for (int i = 0; i < junk.size(); ++i) {
        junk[i] = "WINDOWS11"[i % 9];
    }
    const QString path = writeImage("junk.iso", junk);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QVERIFY(info.volume_label.isEmpty());
    QVERIFY(info.filesystem.isEmpty());
    QVERIFY(!info.is_bootable);
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

// A type-255 terminator ends the descriptor set. Anything after it is not part of
// the volume and must not be read -- including a boot record that would otherwise
// make the image look bootable.
void IsoAnalyzerTests::analyzeStopsAtTheDescriptorSetTerminator() {
    QVERIFY(m_dir.isValid());
    QByteArray image = buildSyntheticIso({.volume_label = "SAK_TERM", .with_boot_record = true});
    // Move the terminator in front of the boot record: PVD, terminator, boot record.
    const int boot = kBootRecordSector * kSectorSize;
    const int terminator = kTerminatorSector * kSectorSize;
    const QByteArray boot_sector = image.mid(boot, kSectorSize);
    const QByteArray terminator_sector = image.mid(terminator, kSectorSize);
    image.replace(boot, kSectorSize, terminator_sector);
    image.replace(terminator, kSectorSize, boot_sector);
    const QString path = writeImage("terminated.iso", image);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QCOMPARE(info.volume_label, QStringLiteral("SAK_TERM"));
    QVERIFY2(!info.is_bootable, "a boot record after the terminator is outside the descriptor set");
}

// The minimum-size guard rejects an image too small to hold the 16-sector system
// area plus one full descriptor sector. The smallest LEGAL image is exactly that
// size (kMinIsoSize) and must be ACCEPTED: a bound that slips from '<' to '<=' would
// reject a valid minimal ISO and report nothing. The existing truncated/full-size
// tests both sit away from the boundary, so only an at-boundary image pins it.
void IsoAnalyzerTests::analyzeAcceptsImageExactlyAtMinimumSize() {
    QVERIFY(m_dir.isValid());
    QByteArray image = buildSyntheticIso({.volume_label = "SAK_MIN_ISO"});
    // Exactly system area + one sector -- kMinIsoSize in the analyzer.
    const int min_size = (kPvdSector + 1) * kSectorSize;
    image.truncate(min_size);
    QVERIFY(image.size() == min_size);
    const QString path = writeImage("minsize.iso", image);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QCOMPARE(info.volume_label, QStringLiteral("SAK_MIN_ISO"));
    QCOMPARE(info.filesystem, QStringLiteral("ISO 9660"));
}

// Before decoding any field, the reader confirms the scanned sector carries the
// "CD001" standard identifier; a sector without it ends the descriptor set and the
// scan must stop fail-closed. If that 'return' became a 'continue', the reader would
// walk past the bad sector and mis-decode a PVD from unrelated later data -- so LBA
// 16 is blanked (no identifier) while a real, labelled PVD sits at LBA 17, and
// analyze() must surface nothing.
void IsoAnalyzerTests::analyzeStopsScanAtFirstNonDescriptorSector() {
    QVERIFY(m_dir.isValid());
    QByteArray image = buildSyntheticIso({.volume_label = "SNEAKY_PVD"});
    const int lba16 = kPvdSector * kSectorSize;
    const int lba17 = (kPvdSector + 1) * kSectorSize;
    // Relocate the real PVD to LBA 17, then blank LBA 16. LBA 16 stays a full sector
    // (no short read), so the reader reaches the identifier check and must stop here.
    image.replace(lba17, kSectorSize, image.mid(lba16, kSectorSize));
    image.replace(lba16, kSectorSize, QByteArray(kSectorSize, '\0'));
    const QString path = writeImage("non-descriptor.iso", image);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QVERIFY(info.volume_label.isEmpty());
    QVERIFY(info.filesystem.isEmpty());
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

// The per-descriptor short-read guard: a descriptor sector cut short (fewer than
// kSectorSize bytes left in the file) must be rejected, never decoded from a
// partially filled buffer. LBA 16 is a supplementary descriptor the reader skips;
// LBA 17 is a PVD truncated mid-sector -- its first bytes (including the volume-label
// field at offset 40) survive, so a reader that ignored the short read would surface
// a bogus label. analyze() must report nothing from the short read.
void IsoAnalyzerTests::analyzeIgnoresShortReadOfLaterDescriptor() {
    QVERIFY(m_dir.isValid());
    QByteArray image = buildSyntheticIso({.volume_label = "TRUNC_LATE"});
    const int lba16 = kPvdSector * kSectorSize;
    const int lba17 = (kPvdSector + 1) * kSectorSize;
    // Relocate the real PVD to LBA 17.
    image.replace(lba17, kSectorSize, image.mid(lba16, kSectorSize));
    // LBA 16 becomes a Supplementary Volume Descriptor (type 2) so the reader skips
    // it and advances to the PVD at LBA 17.
    QByteArray supplementary(kSectorSize, '\0');
    supplementary[0] = 2;
    writePadded(supplementary, 1, "CD001", 5);
    supplementary[6] = 1;
    image.replace(lba16, kSectorSize, supplementary);
    // Cut the PVD sector short: keep only its first 200 bytes.
    image.truncate(lba17 + 200);
    const QString path = writeImage("short-descriptor.iso", image);
    QVERIFY(!path.isEmpty());

    const IsoInfo info = IsoAnalyzer::analyze(path);
    QVERIFY(info.volume_label.isEmpty());
    QVERIFY(!info.is_bootable);
    QVERIFY(!IsoAnalyzer::isWindowsInstallMedia(info));
}

// Live end-to-end check against real images: set SAK_TEST_ISO_DIR to a folder of
// .iso files. Skipped in CI (env unset). Each Linux image must classify as
// not-Windows and each Windows image as Windows -- from content, not filename.
void IsoAnalyzerTests::realIsosClassifyByContent() {
    const QByteArray env = qgetenv("SAK_TEST_ISO_DIR");
    if (env.isEmpty()) {
        QSKIP("SAK_TEST_ISO_DIR not set; skipping real-ISO classification");
    }
    const QFileInfoList isos =
        QDir(QString::fromLocal8Bit(env)).entryInfoList({QStringLiteral("*.iso")}, QDir::Files);
    if (isos.isEmpty()) {
        QSKIP("no .iso files in SAK_TEST_ISO_DIR");
    }
    for (const QFileInfo& file : isos) {
        const IsoInfo info = IsoAnalyzer::analyze(file.absoluteFilePath());
        const bool is_windows = IsoAnalyzer::isWindowsInstallMedia(info);
        qInfo().noquote() << file.fileName() << "os_family=" << info.os_family
                          << "editions=" << info.windows_editions.size()
                          << "-> windows=" << is_windows;
        const QString lname = file.fileName().toLower();
        if (nameContainsAny(lname, {"arch", "ubuntu", "debian", "fedora", "mint", "linux"})) {
            QVERIFY2(!is_windows,
                     qPrintable("Linux ISO misclassified as Windows: " + file.fileName()));
        }
        if (nameContainsAny(lname, {"win11", "win10", "windows"})) {
            QVERIFY2(is_windows, qPrintable("Windows ISO not detected: " + file.fileName()));
        }
    }
}

QTEST_MAIN(IsoAnalyzerTests)
#include "test_iso_analyzer.moc"
