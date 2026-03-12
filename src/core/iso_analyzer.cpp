// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/iso_analyzer.h"

#include "sak/logger.h"

#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>

#include <array>
#include <cstring>

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

// ============================================================================
// Version Extraction Helpers
// ============================================================================

/// Try to extract a version number (e.g. "24.04", "41", "12.8") from text
QString extractVersionFromText(const QString& text) {
    // Match patterns like 24.04.1, 24.04, 41, 12
    static const QRegularExpression version_rx(R"((\d{1,4}(?:\.\d{1,3}){0,2}))");
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
    constexpr int kElToritoIdLength = 23;
    if (std::memcmp(sector.data() + 7, kElToritoId, kElToritoIdLength) != 0) {
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

    qint64 catalog_offset = static_cast<qint64>(catalog_lba) * kSectorSize;
    if (!device.seek(catalog_offset)) {
        return QStringLiteral("Legacy BIOS");
    }

    std::array<char, kSectorSize> catalog{};
    qint64 bytes_read = device.read(catalog.data(), kSectorSize);
    if (bytes_read < kSectorSize) {
        return QStringLiteral("Legacy BIOS");
    }

    bool has_legacy = false;
    bool has_efi = false;

    // Default entry at offset 32
    auto default_media = static_cast<uint8_t>(catalog[32]);
    if (default_media == kBootMediaEfi) {
        has_efi = true;
    } else {
        has_legacy = true;
    }

    // Scan section headers (each 32 bytes starting at offset 64)
    constexpr int kCatalogEntrySize = 32;
    constexpr int kMaxCatalogEntries = 16;
    for (int entry_index = 2; entry_index < kMaxCatalogEntries; ++entry_index) {
        int entry_offset = entry_index * kCatalogEntrySize;
        if (entry_offset + kCatalogEntrySize > kSectorSize) {
            break;
        }

        auto header_id = static_cast<uint8_t>(catalog[entry_offset]);
        // 0x90 = section header more follows, 0x91 = final section header
        if (header_id == 0x90 || header_id == 0x91) {
            auto platform_id = static_cast<uint8_t>(catalog[entry_offset + 1]);
            // Platform: 0 = 80x86, 1 = PowerPC, 2 = Mac, 0xEF = EFI
            if (platform_id == 0xEF) {
                has_efi = true;
            } else if (platform_id == 0x00) {
                has_legacy = true;
            }
        }
    }

    if (has_efi && has_legacy) {
        return QStringLiteral("UEFI + Legacy BIOS");
    }
    if (has_efi) {
        return QStringLiteral("UEFI");
    }
    return QStringLiteral("Legacy BIOS");
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

    // Common Windows ISO volume labels and patterns
    QString label = info.volume_label.toUpper();
    QString app = info.application.toUpper();

    bool is_windows = false;

    // Check volume label patterns
    if (label.contains("WIN") || label.contains("CCCOMA") || label.contains("J_CCSA") ||
        label.contains("SSS_X64") || label.contains("CPBA_") || label.contains("CCSA_") ||
        label.contains("CPRA_") || label.contains("GSP1RM")) {
        is_windows = true;
    }

    // Check application ID
    if (app.contains("MICROSOFT") || app.contains("CDIMAGE")) {
        is_windows = true;
    }

    if (!is_windows) {
        return;
    }

    info.os_family = QStringLiteral("Windows");

    // Try to determine specific Windows version from volume label
    if (label.contains("11") || label.contains("W11")) {
        info.os_name = QStringLiteral("Windows 11");
    } else if (label.contains("10") || label.contains("W10")) {
        info.os_name = QStringLiteral("Windows 10");
    } else if (label.contains("SERVER") || label.contains("SRV")) {
        info.os_name = QStringLiteral("Windows Server");
    } else if (label.contains("8.1")) {
        info.os_name = QStringLiteral("Windows 8.1");
    } else if (label.contains("8")) {
        info.os_name = QStringLiteral("Windows 8");
    } else if (label.contains("7") || label.contains("GSP1RM")) {
        info.os_name = QStringLiteral("Windows 7");
    } else {
        info.os_name = QStringLiteral("Windows");
    }

    // Determine architecture
    if (label.contains("X64") || label.contains("AMD64")) {
        info.architecture = QStringLiteral("x64");
    } else if (label.contains("X86") || label.contains("I386")) {
        info.architecture = QStringLiteral("x86");
    } else if (label.contains("ARM64")) {
        info.architecture = QStringLiteral("ARM64");
    }

    // Detect editions from volume label substrings
    if (label.contains("PRO")) {
        info.windows_editions.append(QStringLiteral("Pro"));
    }
    if (label.contains("HOME") || label.contains("CORE")) {
        info.windows_editions.append(QStringLiteral("Home"));
    }
    if (label.contains("EDU") || label.contains("EDUCATION")) {
        info.windows_editions.append(QStringLiteral("Education"));
    }
    if (label.contains("ENT") || label.contains("ENTERPRISE")) {
        info.windows_editions.append(QStringLiteral("Enterprise"));
    }
}

// ============================================================================
// Linux Identification
// ============================================================================

QString IsoAnalyzer::detectDesktopEnvironment(const QString& label_upper) {
    if (label_upper.contains("GNOME")) {
        return QStringLiteral("GNOME");
    }
    if (label_upper.contains("KDE") || label_upper.contains("PLASMA")) {
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
    if (label_upper.contains("LXQT") || label_upper.contains("LXDE")) {
        return QStringLiteral("LXQt");
    }
    return {};
}

void IsoAnalyzer::identifyLinux(IsoInfo& info) {
    Q_ASSERT(info.os_family.isEmpty());
    Q_ASSERT(info.distro_name.isEmpty());

    QString label = info.volume_label;
    QString label_upper = label.toUpper();

    // Build a combined search corpus from all metadata fields
    QString all_metadata = label + " " + info.publisher + " " + info.preparer + " " +
                           info.application;

    // Check against known distro patterns in label first, then all metadata
    auto tryMatchDistro = [&](const QString& search_text) -> bool {
        for (int idx = 0; idx < kLinuxDistroPatternCount; ++idx) {
            const auto& pattern = kLinuxDistroPatterns[idx];
            if (!search_text.contains(QString::fromLatin1(pattern.volume_prefix),
                                      Qt::CaseInsensitive)) {
                continue;
            }

            info.os_family = QStringLiteral("Linux");
            info.distro_name = QString::fromLatin1(pattern.distro_name);
            info.os_name = info.distro_name;

            info.distro_version = extractVersionFromText(search_text);
            if (!info.distro_version.isEmpty()) {
                info.os_version = info.distro_version;
                info.os_name = QString("%1 %2").arg(info.distro_name, info.distro_version);
            }

            if (label_upper.contains("LIVE") || label_upper.contains("DESKTOP") ||
                label_upper.contains("LIVECD") || label_upper.contains("LIVEDVD")) {
                info.is_live = true;
            }

            info.desktop_env = detectDesktopEnvironment(label_upper);
            return true;
        }
        return false;
    };

    // Try matching against volume label first
    if (tryMatchDistro(label)) {
        return;
    }
    // Then try broader metadata fields (publisher, preparer, application)
    if (tryMatchDistro(all_metadata)) {
        return;
    }

    // Generic Linux detection from any metadata field
    QString meta_upper = all_metadata.toUpper();
    if (label_upper.contains("LINUX") || label_upper.contains("LIVECD") ||
        label_upper.contains("RESCUE") || meta_upper.contains("LINUX") ||
        meta_upper.contains("MKISOFS") || meta_upper.contains("XORRISO") ||
        meta_upper.contains("GENISOIMAGE")) {
        info.os_family = QStringLiteral("Linux");
        info.os_name = label.isEmpty() ? QStringLiteral("Linux") : label;
        info.distro_version = extractVersionFromText(label);
        if (!info.distro_version.isEmpty()) {
            info.os_version = info.distro_version;
        }
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
    QString day = readFixedAscii(raw + 6, kFieldLength);
    QString hour = readFixedAscii(raw + 8, kFieldLength);
    QString minute = readFixedAscii(raw + 10, kFieldLength);
    QString second = readFixedAscii(raw + 12, kFieldLength);

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
