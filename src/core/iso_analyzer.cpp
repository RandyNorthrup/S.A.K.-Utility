// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/iso_analyzer.h"

#include "sak/logger.h"

#include <QFile>
#include <QRegularExpression>

#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>

namespace {

// ============================================================================
// ISO 9660 Constants
// ============================================================================

constexpr qint64 kSectorSize = 2048;
constexpr qint64 kSystemAreaSectors = 16;
constexpr qint64 kPrimaryVolumeDescriptorOffset = kSystemAreaSectors * kSectorSize;

// Volume descriptor type codes
constexpr uint8_t kVdTypeBoot = 0;
constexpr uint8_t kVdTypePrimary = 1;
constexpr uint8_t kVdTypeTerminator = 255;

// Standard identifier in all volume descriptors
constexpr char kIso9660Magic[] = "CD001";
constexpr int kIso9660MagicLength = 5;

// El Torito boot record identifier
constexpr char kElToritoId[] = "EL TORITO SPECIFICATION";

// UDF identifiers
constexpr char kUdfBea[] = "BEA01";
constexpr char kUdfNsr02[] = "NSR02";
constexpr char kUdfNsr03[] = "NSR03";

// Primary Volume Descriptor field offsets (within the 2048-byte sector)
constexpr int kPvdSystemIdOffset = 8;
constexpr int kPvdSystemIdLength = 32;
constexpr int kPvdVolumeIdOffset = 40;
constexpr int kPvdVolumeIdLength = 32;
constexpr int kPvdVolumeSizeLsbOffset = 80;
constexpr int kPvdPublisherOffset = 318;
constexpr int kPvdPublisherLength = 128;
constexpr int kPvdPreparerOffset = 446;
constexpr int kPvdPreparerLength = 128;
constexpr int kPvdApplicationOffset = 574;
constexpr int kPvdApplicationLength = 128;
constexpr int kPvdCreationDateOffset = 813;

// El Torito boot catalog pointer offset within boot record
constexpr int kElToritoBootCatalogOffset = 71;

// Boot catalog entry boot media type masks
constexpr int kValidationEntryPlatformOffset = 1;  // Platform ID byte in the Validation Entry
constexpr uint8_t kBootIndicatorBootable = 0x88;   // Initial/Default Entry Boot Indicator
constexpr int kDefaultBootCatalogEntryOffset = 32;
constexpr int kCatalogEntrySize = 32;
constexpr int kFirstCatalogSectionEntry = 2;
constexpr int kMaxCatalogEntries = 16;
constexpr uint8_t kBootCatalogSectionHeader = 0x90;
constexpr uint8_t kBootCatalogFinalSectionHeader = 0x91;
constexpr uint8_t kBootCatalogPlatformX86 = 0x00;
constexpr uint8_t kBootCatalogPlatformEfi = 0xEF;

// ============================================================================
// Linux Distro Detection Patterns
// ============================================================================

struct DistroPattern {
    const char* volume_prefix;
    const char* distro_name;
};

constexpr DistroPattern kLinuxDistroPatterns[] = {
    {.volume_prefix = "Ubuntu", .distro_name = "Ubuntu"},
    {.volume_prefix = "UBUNTU", .distro_name = "Ubuntu"},
    {.volume_prefix = "Kubuntu", .distro_name = "Kubuntu"},
    {.volume_prefix = "Xubuntu", .distro_name = "Xubuntu"},
    {.volume_prefix = "Lubuntu", .distro_name = "Lubuntu"},
    {.volume_prefix = "Linux Mint", .distro_name = "Linux Mint"},
    {.volume_prefix = "linuxmint", .distro_name = "Linux Mint"},
    {.volume_prefix = "Fedora", .distro_name = "Fedora"},
    {.volume_prefix = "FEDORA", .distro_name = "Fedora"},
    {.volume_prefix = "Debian", .distro_name = "Debian"},
    {.volume_prefix = "DEBIAN", .distro_name = "Debian"},
    {.volume_prefix = "Arch", .distro_name = "Arch Linux"},
    {.volume_prefix = "ARCH", .distro_name = "Arch Linux"},
    {.volume_prefix = "Manjaro", .distro_name = "Manjaro"},
    {.volume_prefix = "openSUSE", .distro_name = "openSUSE"},
    {.volume_prefix = "OPENSUSE", .distro_name = "openSUSE"},
    {.volume_prefix = "CentOS", .distro_name = "CentOS"},
    {.volume_prefix = "CENTOS", .distro_name = "CentOS"},
    {.volume_prefix = "Rocky", .distro_name = "Rocky Linux"},
    {.volume_prefix = "AlmaLinux", .distro_name = "AlmaLinux"},
    {.volume_prefix = "Pop_OS", .distro_name = "Pop!_OS"},
    {.volume_prefix = "Pop!_OS", .distro_name = "Pop!_OS"},
    {.volume_prefix = "Kali", .distro_name = "Kali Linux"},
    {.volume_prefix = "KALI", .distro_name = "Kali Linux"},
    {.volume_prefix = "EndeavourOS", .distro_name = "EndeavourOS"},
    {.volume_prefix = "elementary", .distro_name = "elementary OS"},
    {.volume_prefix = "Zorin", .distro_name = "Zorin OS"},
    {.volume_prefix = "MX-Linux", .distro_name = "MX Linux"},
    {.volume_prefix = "MX_Linux", .distro_name = "MX Linux"},
    {.volume_prefix = "antiX", .distro_name = "antiX"},
    {.volume_prefix = "Tails", .distro_name = "Tails"},
    {.volume_prefix = "Solus", .distro_name = "Solus"},
    {.volume_prefix = "Void", .distro_name = "Void Linux"},
    {.volume_prefix = "Gentoo", .distro_name = "Gentoo"},
    {.volume_prefix = "Slackware", .distro_name = "Slackware"},
    {.volume_prefix = "NixOS", .distro_name = "NixOS"},
    {.volume_prefix = "Garuda", .distro_name = "Garuda Linux"},
    {.volume_prefix = "ArcoLinux", .distro_name = "ArcoLinux"},
    {.volume_prefix = "Nobara", .distro_name = "Nobara"},
    {.volume_prefix = "Parrot", .distro_name = "Parrot OS"},
    {.volume_prefix = "BlackArch", .distro_name = "BlackArch"},
    {.volume_prefix = "Artix", .distro_name = "Artix Linux"},
    {.volume_prefix = "LMDE", .distro_name = "LMDE"},
    {.volume_prefix = "Deepin", .distro_name = "Deepin"},
    {.volume_prefix = "Peppermint", .distro_name = "Peppermint OS"},
    {.volume_prefix = "KaOS", .distro_name = "KaOS"},
    {.volume_prefix = "Puppy", .distro_name = "Puppy Linux"},
    {.volume_prefix = "Alpine", .distro_name = "Alpine Linux"},
    {.volume_prefix = "Clear Linux", .distro_name = "Clear Linux"},
    {.volume_prefix = "SteamOS", .distro_name = "SteamOS"},
    {.volume_prefix = "Proxmox", .distro_name = "Proxmox VE"},
    {.volume_prefix = "TrueNAS", .distro_name = "TrueNAS"},
    {.volume_prefix = "pfSense", .distro_name = "pfSense"},
    {.volume_prefix = "OPNsense", .distro_name = "OPNsense"},
    {.volume_prefix = "Clonezilla", .distro_name = "Clonezilla"},
    {.volume_prefix = "GParted", .distro_name = "GParted Live"},
    {.volume_prefix = "SystemRescue", .distro_name = "SystemRescue"},
    {.volume_prefix = "Hiren", .distro_name = "Hiren's Boot"},
    {.volume_prefix = "Ventoy", .distro_name = "Ventoy"},
    {.volume_prefix = "Batocera", .distro_name = "Batocera"},
    {.volume_prefix = "ChimeraOS", .distro_name = "ChimeraOS"},
    {.volume_prefix = "Bazzite", .distro_name = "Bazzite"},
    {.volume_prefix = "Vanilla", .distro_name = "Vanilla OS"},
    {.volume_prefix = "BlendOS", .distro_name = "BlendOS"},
    {.volume_prefix = "Bodhi", .distro_name = "Bodhi Linux"},
    {.volume_prefix = "Q4OS", .distro_name = "Q4OS"},
    {.volume_prefix = "Sparky", .distro_name = "SparkyLinux"},
    {.volume_prefix = "BunsenLabs", .distro_name = "BunsenLabs"},
    {.volume_prefix = "Mageia", .distro_name = "Mageia"},
    {.volume_prefix = "PCLinuxOS", .distro_name = "PCLinuxOS"},
    {.volume_prefix = "Solaris", .distro_name = "Oracle Solaris"},
    {.volume_prefix = "FreeBSD", .distro_name = "FreeBSD"},
    {.volume_prefix = "OpenBSD", .distro_name = "OpenBSD"},
    {.volume_prefix = "NetBSD", .distro_name = "NetBSD"},
};

constexpr int kLinuxDistroPatternCount =
    static_cast<int>(sizeof(kLinuxDistroPatterns) / sizeof(kLinuxDistroPatterns[0]));

constexpr auto kVersionPattern = "(\\d{1,4}(?:\\.\\d{1,3}){0,2})";

QString extractVersionFromText(const QString& text);

struct BootCatalogFlags {
    bool has_legacy{false};
    bool has_efi{false};
};

bool containsAny(const QString& text, std::initializer_list<const char*> needles) {
    return std::ranges::any_of(needles, [&text](const char* needle) {
        return text.contains(QString::fromLatin1(needle));
    });
}

bool isBootCatalogSectionHeader(uint8_t header_id) {
    return header_id == kBootCatalogSectionHeader || header_id == kBootCatalogFinalSectionHeader;
}

bool readSectorAt(QIODevice& device, qint64 offset, std::array<char, kSectorSize>& sector) {
    if (!device.seek(offset)) {
        return false;
    }
    const qint64 bytes_read = device.read(sector.data(), kSectorSize);
    return bytes_read >= kSectorSize;
}

void applyDefaultBootEntry(const std::array<char, kSectorSize>& catalog, BootCatalogFlags& flags) {
    // Offset 32 is the Initial/Default Entry's Boot Indicator (0x88 = bootable), NOT a platform ID.
    // The platform is declared in the Validation Entry at offset 1; reading offset 32 as the media
    // type misclassified every EFI-only default entry (0x88 != 0xEF) as legacy BIOS.
    const auto boot_indicator = static_cast<uint8_t>(catalog[kDefaultBootCatalogEntryOffset]);
    if (boot_indicator != kBootIndicatorBootable) {
        return;
    }
    const auto platform_id = static_cast<uint8_t>(catalog[kValidationEntryPlatformOffset]);
    if (platform_id == kBootCatalogPlatformEfi) {
        flags.has_efi = true;
    } else {
        flags.has_legacy = true;
    }
}

void applyBootCatalogPlatform(uint8_t platform_id, BootCatalogFlags& flags) {
    if (platform_id == kBootCatalogPlatformEfi) {
        flags.has_efi = true;
    } else if (platform_id == kBootCatalogPlatformX86) {
        flags.has_legacy = true;
    }
}

void scanBootCatalogSections(const std::array<char, kSectorSize>& catalog,
                             BootCatalogFlags& flags) {
    for (int entry_index = kFirstCatalogSectionEntry; entry_index < kMaxCatalogEntries;
         ++entry_index) {
        const int entry_offset = entry_index * kCatalogEntrySize;
        if (entry_offset + kCatalogEntrySize > kSectorSize) {
            break;
        }

        const auto header_id = static_cast<uint8_t>(catalog[entry_offset]);
        if (isBootCatalogSectionHeader(header_id)) {
            applyBootCatalogPlatform(static_cast<uint8_t>(catalog[entry_offset + 1]), flags);
        }
    }
}

// A boot-record volume descriptor is present but its catalog is absent, unreadable,
// or carries no recognizable bootable entry: that is not proof of a bootable image, so
// it is reported distinctly from the recognized boot types below (read-only analyzer).
QString unknownBootType() {
    return QStringLiteral("Unknown/Invalid");
}

QString bootTypeFromFlags(const BootCatalogFlags& flags) {
    if (flags.has_efi && flags.has_legacy) {
        return QStringLiteral("UEFI + Legacy BIOS");
    }
    if (flags.has_efi) {
        return QStringLiteral("UEFI");
    }
    if (flags.has_legacy) {
        return QStringLiteral("Legacy BIOS");
    }
    // No bootable entry recognized in the catalog.
    return unknownBootType();
}

bool hasWindowsMetadata(const QString& label, const QString& app) {
    return containsAny(
               label,
               {"WIN", "CCCOMA", "J_CCSA", "SSS_X64", "CPBA_", "CCSA_", "CPRA_", "GSP1RM"}) ||
           containsAny(app, {"MICROSOFT", "CDIMAGE"});
}

QString windowsNameFromLabel(const QString& label) {
    if (containsAny(label, {"11", "W11"})) {
        return QStringLiteral("Windows 11");
    }
    if (containsAny(label, {"10", "W10"})) {
        return QStringLiteral("Windows 10");
    }
    if (containsAny(label, {"SERVER", "SRV"})) {
        return QStringLiteral("Windows Server");
    }
    if (label.contains("8.1")) {
        return QStringLiteral("Windows 8.1");
    }
    if (label.contains("8")) {
        return QStringLiteral("Windows 8");
    }
    if (containsAny(label, {"7", "GSP1RM"})) {
        return QStringLiteral("Windows 7");
    }
    return QStringLiteral("Windows");
}

QString windowsArchitectureFromLabel(const QString& label) {
    if (containsAny(label, {"X64", "AMD64"})) {
        return QStringLiteral("x64");
    }
    if (containsAny(label, {"X86", "I386"})) {
        return QStringLiteral("x86");
    }
    if (label.contains("ARM64")) {
        return QStringLiteral("ARM64");
    }
    return {};
}

void appendWindowsEditions(const QString& label, QStringList& editions) {
    if (label.contains("PRO")) {
        editions.append(QStringLiteral("Pro"));
    }
    if (containsAny(label, {"HOME", "CORE"})) {
        editions.append(QStringLiteral("Home"));
    }
    if (containsAny(label, {"EDU", "EDUCATION"})) {
        editions.append(QStringLiteral("Education"));
    }
    if (containsAny(label, {"ENT", "ENTERPRISE"})) {
        editions.append(QStringLiteral("Enterprise"));
    }
}

QString desktopEnvironmentFromLabel(const QString& label_upper) {
    if (label_upper.contains("GNOME")) {
        return QStringLiteral("GNOME");
    }
    if (containsAny(label_upper, {"KDE", "PLASMA"})) {
        return QStringLiteral("KDE Plasma");
    }
    if (label_upper.contains("XFCE")) {
        return QStringLiteral("XFCE");
    }
    if (label_upper.contains("CINNAMON")) {
        return QStringLiteral("Cinnamon");
    }
    if (label_upper.contains("MATE")) {
        return QStringLiteral("MATE");
    }
    if (label_upper.contains("BUDGIE")) {
        return QStringLiteral("Budgie");
    }
    if (containsAny(label_upper, {"LXQT", "LXDE"})) {
        return QStringLiteral("LXQt");
    }
    return {};
}

bool isLiveLinuxLabel(const QString& label_upper) {
    return containsAny(label_upper, {"LIVE", "DESKTOP", "LIVECD", "LIVEDVD"});
}

void applyLinuxDistroMatch(sak::IsoInfo& info,
                           const DistroPattern& pattern,
                           const QString& search_text,
                           const QString& label_upper) {
    info.os_family = QStringLiteral("Linux");
    info.distro_name = QString::fromLatin1(pattern.distro_name);
    info.os_name = info.distro_name;

    info.distro_version = extractVersionFromText(search_text);
    if (!info.distro_version.isEmpty()) {
        info.os_version = info.distro_version;
        info.os_name = QString("%1 %2").arg(info.distro_name, info.distro_version);
    }

    info.is_live = isLiveLinuxLabel(label_upper);
    info.desktop_env = desktopEnvironmentFromLabel(label_upper);
}

bool tryMatchLinuxDistro(sak::IsoInfo& info,
                         const QString& search_text,
                         const QString& label_upper) {
    for (int idx = 0; idx < kLinuxDistroPatternCount; ++idx) {
        const auto& pattern = kLinuxDistroPatterns[idx];
        if (!search_text.contains(QString::fromLatin1(pattern.volume_prefix),
                                  Qt::CaseInsensitive)) {
            continue;
        }

        applyLinuxDistroMatch(info, pattern, search_text, label_upper);
        return true;
    }
    return false;
}

bool hasGenericLinuxMetadata(const QString& label_upper, const QString& meta_upper) {
    return containsAny(label_upper, {"LINUX", "LIVECD", "RESCUE"}) ||
           containsAny(meta_upper, {"LINUX", "MKISOFS", "XORRISO", "GENISOIMAGE"});
}

void applyGenericLinuxMetadata(sak::IsoInfo& info, const QString& label) {
    info.os_family = QStringLiteral("Linux");
    info.os_name = label.isEmpty() ? QStringLiteral("Linux") : label;
    info.distro_version = extractVersionFromText(label);
    if (!info.distro_version.isEmpty()) {
        info.os_version = info.distro_version;
    }
}

// ============================================================================
// Version Extraction Helpers
// ============================================================================

/// Try to extract a version number (e.g. "24.04", "41", "12.8") from text
QString extractVersionFromText(const QString& text) {
    // Match patterns like 24.04.1, 24.04, 41, 12
    static const QRegularExpression version_rx(QString::fromLatin1(kVersionPattern));
    auto match = version_rx.match(text);
    if (match.hasMatch()) {
        return match.captured(1);
    }
    return {};
}

/// Try to extract architecture from text
QString extractArchFromText(const QString& text) {
    const QString lower = text.toLower();
    if (lower.contains("amd64") || lower.contains("x86_64") || lower.contains("x64")) {
        return QStringLiteral("x64");
    }
    if (lower.contains("arm64") || lower.contains("aarch64")) {
        return QStringLiteral("ARM64");
    }
    if (lower.contains("i386") || lower.contains("i686") || lower.contains("x86")) {
        return QStringLiteral("x86");
    }
    return {};
}

}  // namespace

namespace sak {

// ============================================================================
// Public API
// ============================================================================

IsoInfo IsoAnalyzer::analyze(const QString& file_path) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        logWarning("IsoAnalyzer: Could not open file: " + file_path.toStdString());
        return {};
    }
    // Parse from the handle we actually opened. analyzeDevice sizes the media from the
    // device itself, not a separate QFileInfo stat of the path: a second path resolution
    // can race a reparse/target swap and size a file we never read.
    return analyzeDevice(file);
}

IsoInfo IsoAnalyzer::analyzeDevice(QIODevice& device) {
    IsoInfo info;
    info.file_size = device.size();

    // Need at least system area + one sector for any ISO 9660.
    constexpr qint64 kMinIsoSize = kPrimaryVolumeDescriptorOffset + kSectorSize;
    if (info.file_size < kMinIsoSize) {
        logInfo("IsoAnalyzer: stream too small for ISO 9660");
        return info;
    }

    readPrimaryVolumeDescriptor(device, info);
    readElToritoBootRecord(device, info);
    detectUdf(device, info);

    // Set filesystem type based on what we found
    if (info.filesystem.isEmpty()) {
        if (!info.volume_label.isEmpty()) {
            info.filesystem = QStringLiteral("ISO 9660");
        }
    }

    // Identify OS
    identifyWindows(info);
    if (info.os_family.isEmpty()) {
        identifyLinux(info);
    }

    // Extract architecture from volume label if not yet determined
    if (info.architecture.isEmpty()) {
        info.architecture = extractArchFromText(info.volume_label);
    }

    return info;
}

// ============================================================================
// ISO 9660 Primary Volume Descriptor
// ============================================================================

void IsoAnalyzer::readPrimaryVolumeDescriptor(QIODevice& device, IsoInfo& info) {
    // Private helper: analyze() is the only caller and reaches it only after its own
    // QFile::open(QIODevice::ReadOnly) succeeded.
    Q_ASSERT(device.isOpen());
    Q_ASSERT(device.isReadable());

    // Scan volume descriptor set starting at LBA 16
    constexpr int kMaxDescriptors = 16;
    for (int descriptor_index = 0; descriptor_index < kMaxDescriptors; ++descriptor_index) {
        const qint64 offset = kPrimaryVolumeDescriptorOffset + (descriptor_index * kSectorSize);

        if (!device.seek(offset)) {
            return;
        }

        std::array<char, kSectorSize> sector{};
        const qint64 bytes_read = device.read(sector.data(), kSectorSize);
        if (bytes_read < kSectorSize) {
            return;
        }

        // Check standard identifier "CD001"
        if (std::memcmp(sector.data() + 1, kIso9660Magic, kIso9660MagicLength) != 0) {
            return;  // Not an ISO 9660 volume descriptor
        }

        auto descriptor_type = static_cast<uint8_t>(sector[0]);

        if (descriptor_type == kVdTypeTerminator) {
            return;  // End of volume descriptor set
        }

        if (descriptor_type != kVdTypePrimary) {
            continue;  // Skip boot records and supplementary -- we handle boot separately
        }

        // Extract fields from Primary Volume Descriptor
        info.volume_label = readFixedAscii(sector.data() + kPvdVolumeIdOffset, kPvdVolumeIdLength);
        info.publisher = readFixedAscii(sector.data() + kPvdPublisherOffset, kPvdPublisherLength);
        info.preparer = readFixedAscii(sector.data() + kPvdPreparerOffset, kPvdPreparerLength);
        info.application = readFixedAscii(sector.data() + kPvdApplicationOffset,
                                          kPvdApplicationLength);
        info.creation_date = parseIsoDateTime(sector.data() + kPvdCreationDateOffset);

        // Volume space size (little-endian 32-bit at offset 80)
        uint32_t volume_blocks = 0;
        std::memcpy(&volume_blocks, sector.data() + kPvdVolumeSizeLsbOffset, sizeof(volume_blocks));
        info.volume_size = static_cast<uint64_t>(volume_blocks) * kSectorSize;

        return;  // Found the PVD, done
    }
}

// ============================================================================
// El Torito Boot Record
// ============================================================================

void IsoAnalyzer::readElToritoBootRecord(QIODevice& device, IsoInfo& info) {
    // Private helper: analyze() only calls this after its QFile::open() succeeded.
    Q_ASSERT(device.isOpen());

    // The Boot Record volume descriptor is NOT always at LBA 17: it lives
    // somewhere in the VD sequence that starts at LBA 16 and ends at a type-255
    // terminator, and a supplementary VD before it shifts it later. Scan the set
    // (bounded by kMaxDescriptors) for the boot record instead of a fixed LBA.
    constexpr int kElToritoIdentifierOffset = 7;
    constexpr int kElToritoIdLength = 23;
    constexpr int kMaxDescriptors = 16;

    for (int descriptor_index = 0; descriptor_index < kMaxDescriptors; ++descriptor_index) {
        const qint64 offset = kPrimaryVolumeDescriptorOffset + (descriptor_index * kSectorSize);
        if (!device.seek(offset)) {
            return;
        }
        std::array<char, kSectorSize> sector{};
        if (device.read(sector.data(), kSectorSize) < kSectorSize) {
            return;
        }
        if (std::memcmp(sector.data() + 1, kIso9660Magic, kIso9660MagicLength) != 0) {
            return;  // not an ISO 9660 volume descriptor
        }
        const auto descriptor_type = static_cast<uint8_t>(sector[0]);
        if (descriptor_type == kVdTypeTerminator) {
            return;  // end of the descriptor set: no boot record
        }
        if (descriptor_type != kVdTypeBoot || std::memcmp(sector.data() + kElToritoIdentifierOffset,
                                                          kElToritoId,
                                                          kElToritoIdLength) != 0) {
            continue;  // some other VD, or a boot record that is not El Torito
        }

        // A boot-record VD alone does not make an image bootable: El Torito requires a
        // boot catalog with at least one valid entry. Read the pointer and classify; a
        // zero/unreadable/malformed catalog stays non-bootable and reports Unknown/Invalid.
        uint32_t catalog_lba = 0;
        std::memcpy(&catalog_lba, sector.data() + kElToritoBootCatalogOffset, sizeof(catalog_lba));
        info.boot_type = (catalog_lba == 0) ? unknownBootType()
                                            : classifyBootCatalog(device, catalog_lba);
        info.is_bootable = (info.boot_type != unknownBootType());
        return;
    }
}

// ============================================================================
// Boot Catalog Classification
// ============================================================================

QString IsoAnalyzer::classifyBootCatalog(QIODevice& device, uint32_t catalog_lba) {
    // Private helper: the device came open from analyze(), and the sole caller
    // (readElToritoBootRecord) routes catalog_lba == 0 to Unknown/Invalid without calling here.
    Q_ASSERT(device.isOpen());
    Q_ASSERT(catalog_lba > 0);

    std::array<char, kSectorSize> catalog{};
    const qint64 catalog_offset = static_cast<qint64>(catalog_lba) * kSectorSize;
    if (!readSectorAt(device, catalog_offset, catalog)) {
        return unknownBootType();  // catalog pointer past EOF or unreadable: not bootable
    }

    BootCatalogFlags flags;
    applyDefaultBootEntry(catalog, flags);
    scanBootCatalogSections(catalog, flags);
    return bootTypeFromFlags(flags);
}

// ============================================================================
// UDF Detection
// ============================================================================

void IsoAnalyzer::detectUdf(QIODevice& device, IsoInfo& info) {
    // Private helper: analyze() only calls this after its QFile::open() succeeded.
    Q_ASSERT(device.isOpen());

    // UDF uses Extended Area Descriptor at sector 16+n
    // Look for BEA01 and NSR02/NSR03 identifiers in sectors after ISO descriptors
    constexpr int kUdfSearchStart = 16;
    constexpr int kUdfSearchEnd = 48;

    for (int sector_index = kUdfSearchStart; sector_index < kUdfSearchEnd; ++sector_index) {
        const qint64 offset = static_cast<qint64>(sector_index) * kSectorSize;
        if (!device.seek(offset)) {
            return;
        }

        std::array<char, kSectorSize> sector{};
        const qint64 bytes_read = device.read(sector.data(), kSectorSize);
        if (bytes_read < kSectorSize) {
            return;
        }

        // Check for UDF identifiers at offset 1 (after type byte)
        if (std::memcmp(sector.data() + 1, kUdfBea, kIso9660MagicLength) == 0 ||
            std::memcmp(sector.data() + 1, kUdfNsr02, kIso9660MagicLength) == 0 ||
            std::memcmp(sector.data() + 1, kUdfNsr03, kIso9660MagicLength) == 0) {
            if (info.filesystem.isEmpty() || info.filesystem == "ISO 9660") {
                info.filesystem = info.volume_label.isEmpty() ? QStringLiteral("UDF")
                                                              : QStringLiteral("ISO 9660 + UDF");
            }
            return;
        }
    }
}

// ============================================================================
// Windows Identification
// ============================================================================

bool IsoAnalyzer::isWindowsInstallMedia(const IsoInfo& info) {
    // os_family and windows_editions are set only when the volume label /
    // application ID match Windows install patterns, so this is content-metadata
    // based, unlike a filename check that trips on Linux "server" images.
    return info.os_family == QLatin1String("Windows") || !info.windows_editions.isEmpty();
}

void IsoAnalyzer::identifyWindows(IsoInfo& info) {
    // Private helper: analyze() default-constructs the IsoInfo, and the three readers that run
    // before this one (PVD, El Torito, UDF) never write os_family or os_name.
    Q_ASSERT(info.os_family.isEmpty());
    Q_ASSERT(info.os_name.isEmpty());

    const QString label = info.volume_label.toUpper();
    const QString app = info.application.toUpper();
    if (!hasWindowsMetadata(label, app)) {
        return;
    }

    info.os_family = QStringLiteral("Windows");
    info.os_name = windowsNameFromLabel(label);
    info.architecture = windowsArchitectureFromLabel(label);
    appendWindowsEditions(label, info.windows_editions);
}

// ============================================================================
// Linux Identification
// ============================================================================

void IsoAnalyzer::identifyLinux(IsoInfo& info) {
    // Private helper: analyze() calls this only inside if (info.os_family.isEmpty()), and
    // distro_name is written nowhere but this function's own helpers.
    Q_ASSERT(info.os_family.isEmpty());
    Q_ASSERT(info.distro_name.isEmpty());

    const QString label = info.volume_label;
    const QString label_upper = label.toUpper();
    const QString all_metadata = label + " " + info.publisher + " " + info.preparer + " " +
                                 info.application;

    if (tryMatchLinuxDistro(info, label, label_upper)) {
        return;
    }
    if (tryMatchLinuxDistro(info, all_metadata, label_upper)) {
        return;
    }

    const QString meta_upper = all_metadata.toUpper();
    if (hasGenericLinuxMetadata(label_upper, meta_upper)) {
        applyGenericLinuxMetadata(info, label);
    }
}

// ============================================================================
// Utility Functions
// ============================================================================

QString IsoAnalyzer::parseIsoDateTime(const char* raw) {
    // Private helper: the sole caller passes an interior pointer into its own
    // std::array<char, kSectorSize> sector buffer, which is never null.
    Q_ASSERT(raw);

    // ISO 9660 date format: 17 bytes
    // "YYYYMMDDHHMMSScc" + timezone offset byte
    // Digits are ASCII characters, not binary
    constexpr int kDateFieldLength = 16;
    constexpr int kIsoDateDayOffset = 6;
    constexpr int kIsoDateHourOffset = 8;
    constexpr int kIsoDateMinuteOffset = 10;
    constexpr int kIsoDateSecondOffset = 12;

    // Check if date is all zeros or spaces (means not set)
    bool all_blank = true;
    for (int byte_index = 0; byte_index < kDateFieldLength; ++byte_index) {
        if (raw[byte_index] != '0' && raw[byte_index] != ' ' && raw[byte_index] != '\0') {
            all_blank = false;
            break;
        }
    }

    if (all_blank) {
        return {};
    }

    // Parse: YYYY-MM-DD HH:MM:SS
    constexpr int kYearLength = 4;
    constexpr int kFieldLength = 2;
    const QString year = readFixedAscii(raw, kYearLength);
    const QString month = readFixedAscii(raw + kYearLength, kFieldLength);
    const QString day = readFixedAscii(raw + kIsoDateDayOffset, kFieldLength);
    const QString hour = readFixedAscii(raw + kIsoDateHourOffset, kFieldLength);
    const QString minute = readFixedAscii(raw + kIsoDateMinuteOffset, kFieldLength);
    const QString second = readFixedAscii(raw + kIsoDateSecondOffset, kFieldLength);

    return QString("%1-%2-%3 %4:%5:%6").arg(year, month, day, hour, minute, second);
}

QString IsoAnalyzer::readFixedAscii(const char* data, int length) {
    // Private helper: every caller (readPrimaryVolumeDescriptor, parseIsoDateTime) passes an
    // interior pointer into a fixed sector buffer plus a positive compile-time field length.
    Q_ASSERT(data);
    Q_ASSERT(length > 0);

    // Fail closed if the private contract is ever violated: QString::fromLatin1(data, <0)
    // reinterprets a negative length as "read until NUL", which would run off the fixed
    // sector field. Callers pass compile-time-constant positive lengths, so this is
    // defensive only, but it removes the out-of-bounds read the header's raw-pointer
    // signature cannot otherwise preclude.
    if (data == nullptr || length <= 0) {
        return {};
    }

    // Copy the fixed-width field and trim trailing spaces/nulls
    QString result = QString::fromLatin1(data, length).trimmed();

    // Remove trailing null characters that trimmed() might miss
    while (!result.isEmpty() && result.back() == QChar('\0')) {
        result.chop(1);
    }

    return result;
}

}  // namespace sak
