// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

#include <cstdint>

class QIODevice;

namespace sak {

/// @brief Detailed information extracted from an ISO/IMG file
struct IsoInfo {
    // ── Identity ────────────────────────────────────────────────
    QString os_name;       ///< e.g. "Windows 11 Pro" or "Ubuntu 24.04 LTS"
    QString os_version;    ///< e.g. "24H2" or "24.04"
    QString architecture;  ///< e.g. "x64", "ARM64"
    QString os_family;     ///< "Windows", "Linux", "Unknown"

    // ── ISO Metadata ────────────────────────────────────────────
    QString volume_label;   ///< Volume ID from primary volume descriptor
    QString publisher;      ///< Publisher from primary volume descriptor
    QString preparer;       ///< Data preparer from primary volume descriptor
    QString application;    ///< Application ID from primary volume descriptor
    QString creation_date;  ///< Volume creation date
    QString filesystem;     ///< "ISO 9660", "UDF", "ISO 9660 + UDF", "FAT32"

    // ── Boot Info ───────────────────────────────────────────────
    bool is_bootable{false};  ///< El Torito boot record found
    QString boot_type;        ///< "UEFI", "Legacy BIOS", "UEFI + Legacy"

    // ── Windows-Specific ────────────────────────────────────────
    QString windows_build;         ///< Build number, e.g. "26100"
    QStringList windows_editions;  ///< Editions in install.wim/esd

    // ── Linux-Specific ──────────────────────────────────────────
    QString distro_name;     ///< e.g. "Ubuntu", "Fedora", "Arch"
    QString distro_version;  ///< e.g. "24.04", "41"
    QString desktop_env;     ///< e.g. "GNOME", "KDE", "XFCE" (if detectable)
    bool is_live{false};     ///< Live-boot capable

    // ── Size ────────────────────────────────────────────────────
    qint64 file_size{0};      ///< File size on disk (bytes)
    uint64_t volume_size{0};  ///< Logical volume size from volume descriptor

    /// @brief Whether any meaningful info was extracted
    bool isValid() const { return !volume_label.isEmpty() || !os_name.isEmpty(); }
};

/// @brief Analyze an ISO/IMG file and extract build identity information
///
/// Reads ISO 9660 primary volume descriptors, El Torito boot records,
/// and heuristically identifies Windows and Linux distributions from
/// file listings and metadata embedded in the image.
///
/// Thread-Safety: Safe to call from any thread. Uses only local file I/O.
class IsoAnalyzer {
public:
    /// @brief Analyze the given image file
    /// @param file_path Path to .iso or .img file
    /// @return Extracted info (check isValid())
    static IsoInfo analyze(const QString& file_path);

private:
    /// @brief Read ISO 9660 primary volume descriptor at LBA 16
    static void readPrimaryVolumeDescriptor(QIODevice& device, IsoInfo& info);

    /// @brief Check for El Torito boot record at LBA 17
    static void readElToritoBootRecord(QIODevice& device, IsoInfo& info);

    /// @brief Classify boot catalog entries into boot type string
    static QString classifyBootCatalog(QIODevice& device, uint32_t catalog_lba);

    /// @brief Check for UDF filesystem markers
    static void detectUdf(QIODevice& device, IsoInfo& info);

    /// @brief Identify Windows from volume label and known patterns
    static void identifyWindows(IsoInfo& info);

    /// @brief Identify Linux distributions from volume label patterns
    static void identifyLinux(IsoInfo& info);

    /// @brief Detect desktop environment from ISO label
    static QString detectDesktopEnvironment(const QString& label_upper);

    /// @brief Parse a date-time string from ISO 9660 format (17 bytes)
    static QString parseIsoDateTime(const char* raw);

    /// @brief Read a trimmed ASCII string from a fixed-width field
    static QString readFixedAscii(const char* data, int length);
};

}  // namespace sak
