// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/iso_analyzer.h"

#include "sak/logger.h"

#include <QFile>
#include <QFileInfo>
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
constexpr uint8_t kBootMediaEfi = 0xEF;
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
    {"Ubuntu", "Ubuntu"},
    {"UBUNTU", "Ubuntu"},
    {"Kubuntu", "Kubuntu"},
    {"Xubuntu", "Xubuntu"},
    {"Lubuntu", "Lubuntu"},
    {"Linux Mint", "Linux Mint"},
    {"linuxmint", "Linux Mint"},
    {"Fedora", "Fedora"},
    {"FEDORA", "Fedora"},
    {"Debian", "Debian"},
    {"DEBIAN", "Debian"},
    {"Arch", "Arch Linux"},
    {"ARCH", "Arch Linux"},
    {"Manjaro", "Manjaro"},
    {"openSUSE", "openSUSE"},
    {"OPENSUSE", "openSUSE"},
    {"CentOS", "CentOS"},
    {"CENTOS", "CentOS"},
    {"Rocky", "Rocky Linux"},
    {"AlmaLinux", "AlmaLinux"},
    {"Pop_OS", "Pop!_OS"},
    {"Pop!_OS", "Pop!_OS"},
    {"Kali", "Kali Linux"},
    {"KALI", "Kali Linux"},
    {"EndeavourOS", "EndeavourOS"},
    {"elementary", "elementary OS"},
    {"Zorin", "Zorin OS"},
    {"MX-Linux", "MX Linux"},
    {"MX_Linux", "MX Linux"},
    {"antiX", "antiX"},
    {"Tails", "Tails"},
    {"Solus", "Solus"},
    {"Void", "Void Linux"},
    {"Gentoo", "Gentoo"},
    {"Slackware", "Slackware"},
    {"NixOS", "NixOS"},
    {"Garuda", "Garuda Linux"},
    {"ArcoLinux", "ArcoLinux"},
    {"Nobara", "Nobara"},
    {"Parrot", "Parrot OS"},
    {"BlackArch", "BlackArch"},
    {"Artix", "Artix Linux"},
    {"LMDE", "LMDE"},
    {"Deepin", "Deepin"},
    {"Peppermint", "Peppermint OS"},
    {"KaOS", "KaOS"},
    {"Puppy", "Puppy Linux"},
    {"Alpine", "Alpine Linux"},
    {"Clear Linux", "Clear Linux"},
    {"SteamOS", "SteamOS"},
    {"Proxmox", "Proxmox VE"},
    {"TrueNAS", "TrueNAS"},
    {"pfSense", "pfSense"},
    {"OPNsense", "OPNsense"},
    {"Clonezilla", "Clonezilla"},
    {"GParted", "GParted Live"},
    {"SystemRescue", "SystemRescue"},
    {"Hiren", "Hiren's Boot"},
    {"Ventoy", "Ventoy"},
    {"Batocera", "Batocera"},
    {"ChimeraOS", "ChimeraOS"},
    {"Bazzite", "Bazzite"},
    {"Vanilla", "Vanilla OS"},
    {"BlendOS", "BlendOS"},
    {"Bodhi", "Bodhi Linux"},
    {"Q4OS", "Q4OS"},
    {"Sparky", "SparkyLinux"},
    {"BunsenLabs", "BunsenLabs"},
    {"Mageia", "Mageia"},
    {"PCLinuxOS", "PCLinuxOS"},
    {"Solaris", "Oracle Solaris"},
    {"FreeBSD", "FreeBSD"},
    {"OpenBSD", "OpenBSD"},
    {"NetBSD", "NetBSD"},
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
    return std::any_of(needles.begin(), needles.end(), [&text](const char* needle) {
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
    const auto default_media = static_cast<uint8_t>(catalog[kDefaultBootCatalogEntryOffset]);
    if (default_media == kBootMediaEfi) {
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

QString bootTypeFromFlags(const BootCatalogFlags& flags) {
    if (flags.has_efi && flags.has_legacy) {
        return QStringLiteral("UEFI + Legacy BIOS");
    }
    if (flags.has_efi) {
        return QStringLiteral("UEFI");
    }
    return QStringLiteral("Legacy BIOS");
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
    QString lower = text.toLower();
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
    Q_ASSERT(!file_path.isEmpty());
    Q_ASSERT(QFileInfo::exists(file_path));

    IsoInfo info;
    info.file_size = QFileInfo(file_path).size();

    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        logWarning("IsoAnalyzer: Could not open file: " + file_path.toStdString());
        return info;
    }

    // Need at least system area + one sector for any ISO 9660
    constexpr qint64 kMinIsoSize = kPrimaryVolumeDescriptorOffset + kSectorSize;
    if (info.file_size < kMinIsoSize) {
        logInfo("IsoAnalyzer: File too small for ISO 9660: " + file_path.toStdString());
        return info;
    }

    readPrimaryVolumeDescriptor(file, info);
    readElToritoBootRecord(file, info);
    detectUdf(file, info);

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
    Q_ASSERT(device.isOpen());
    Q_ASSERT(device.isReadable());

    // Scan volume descriptor set starting at LBA 16
    constexpr int kMaxDescriptors = 16;
    for (int descriptor_index = 0; descriptor_index < kMaxDescriptors; ++descriptor_index) {
        qint64 offset = kPrimaryVolumeDescriptorOffset + (descriptor_index * kSectorSize);

        if (!device.seek(offset)) {
            return;
        }

        std::array<char, kSectorSize> sector{};
        qint64 bytes_read = device.read(sector.data(), kSectorSize);
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
    Q_ASSERT(device.isOpen());

    // Boot record is at LBA 17 (sector 17)
    constexpr qint64 kBootRecordOffset = 17 * kSectorSize;
    if (!device.seek(kBootRecordOffset)) {
        return;
    }

    std::array<char, kSectorSize> sector{};
    qint64 bytes_read = device.read(sector.data(), kSectorSize);
    if (bytes_read < kSectorSize) {
        return;
    }

    // Check for boot record volume descriptor
    if (static_cast<uint8_t>(sector[0]) != kVdTypeBoot) {
        return;
    }
    if (std::memcmp(sector.data() + 1, kIso9660Magic, kIso9660MagicLength) != 0) {
        return;
    }

    // Check El Torito identifier at bytes 7-29
    constexpr int kElToritoIdentifierOffset = 7;
    constexpr int kElToritoIdLength = 23;
    if (std::memcmp(sector.data() + kElToritoIdentifierOffset, kElToritoId, kElToritoIdLength) !=
        0) {
        return;
    }

    info.is_bootable = true;

    // Read boot catalog pointer (LE 32-bit at offset 71)
    uint32_t catalog_lba = 0;
    std::memcpy(&catalog_lba, sector.data() + kElToritoBootCatalogOffset, sizeof(catalog_lba));

    if (catalog_lba == 0) {
        info.boot_type = QStringLiteral("Legacy BIOS");
        return;
    }

    // Read and classify boot catalog entries
    info.boot_type = classifyBootCatalog(device, catalog_lba);
}

// ============================================================================
// Boot Catalog Classification
// ============================================================================

QString IsoAnalyzer::classifyBootCatalog(QIODevice& device, uint32_t catalog_lba) {
    Q_ASSERT(device.isOpen());
    Q_ASSERT(catalog_lba > 0);

    std::array<char, kSectorSize> catalog{};
    const qint64 catalog_offset = static_cast<qint64>(catalog_lba) * kSectorSize;
    if (!readSectorAt(device, catalog_offset, catalog)) {
        return QStringLiteral("Legacy BIOS");
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
    Q_ASSERT(device.isOpen());

    // UDF uses Extended Area Descriptor at sector 16+n
    // Look for BEA01 and NSR02/NSR03 identifiers in sectors after ISO descriptors
    constexpr int kUdfSearchStart = 16;
    constexpr int kUdfSearchEnd = 48;

    for (int sector_index = kUdfSearchStart; sector_index < kUdfSearchEnd; ++sector_index) {
        qint64 offset = static_cast<qint64>(sector_index) * kSectorSize;
        if (!device.seek(offset)) {
            return;
        }

        std::array<char, kSectorSize> sector{};
        qint64 bytes_read = device.read(sector.data(), kSectorSize);
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

void IsoAnalyzer::identifyWindows(IsoInfo& info) {
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

QString IsoAnalyzer::detectDesktopEnvironment(const QString& label_upper) {
    return desktopEnvironmentFromLabel(label_upper);
}

void IsoAnalyzer::identifyLinux(IsoInfo& info) {
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
    QString year = readFixedAscii(raw, kYearLength);
    QString month = readFixedAscii(raw + kYearLength, kFieldLength);
    QString day = readFixedAscii(raw + kIsoDateDayOffset, kFieldLength);
    QString hour = readFixedAscii(raw + kIsoDateHourOffset, kFieldLength);
    QString minute = readFixedAscii(raw + kIsoDateMinuteOffset, kFieldLength);
    QString second = readFixedAscii(raw + kIsoDateSecondOffset, kFieldLength);

    return QString("%1-%2-%3 %4:%5:%6").arg(year, month, day, hour, minute, second);
}

QString IsoAnalyzer::readFixedAscii(const char* data, int length) {
    Q_ASSERT(data);
    Q_ASSERT(length > 0);

    // Copy the fixed-width field and trim trailing spaces/nulls
    QString result = QString::fromLatin1(data, length).trimmed();

    // Remove trailing null characters that trimmed() might miss
    while (!result.isEmpty() && result.back() == QChar('\0')) {
        result.chop(1);
    }

    return result;
}

}  // namespace sak
