// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevation_tier.h
/// @brief Elevation tier classification for per-task privilege escalation

#pragma once

#include <cstdint>
#include <string_view>

namespace sak {

/// @brief Classifies a feature's elevation requirement
///
/// - Standard:  The feature works fully without admin privileges.
/// - Elevated:  The feature always requires admin (Windows API enforced).
/// - Mixed:     Most sub-operations work unelevated; specific sub-tasks
///              require admin and will trigger a UAC prompt on demand.
enum class ElevationTier : uint8_t {
    Standard = 0,  ///< Never needs admin
    Elevated = 1,  ///< Always needs admin
    Mixed = 2,     ///< Some sub-operations need admin
};

/// @brief Convert an ElevationTier to its display name
[[nodiscard]] constexpr std::string_view to_string(ElevationTier tier) noexcept {
    switch (tier) {
    case ElevationTier::Standard:
        return "Standard";
    case ElevationTier::Elevated:
        return "Elevated";
    case ElevationTier::Mixed:
        return "Mixed";
    }
    return "Unknown";
}

/// @brief Identifies a specific feature or task in the application
enum class FeatureId : uint16_t {
    // File Management (100-199)
    AdvancedSearch = 100,
    FileOrganizer = 101,
    DuplicateFinder = 102,

    // Email Tools (200-299)
    EmailInspector = 200,

    // Quick Actions — Standard (300-349)
    GenerateSystemReport = 300,
    ScreenshotSettings = 301,
    OptimizePowerSettings = 302,

    // Quick Actions — Elevated (350-399)
    BackupBitlockerKeys = 350,
    VerifySystemFiles = 351,
    CheckDiskErrors = 352,
    ResetNetworkStack = 353,

    // Image Flasher (400-499)
    FlashUsbDrive = 400,
    UupIsoConversion = 401,

    // Backup & Restore (500-599)
    BackupCurrentUser = 500,
    BackupCrossUser = 501,
    RestoreToUserPaths = 502,
    RestoreWithAcls = 503,

    // App Management (600-699)
    AppScanning = 600,
    ChocolateyBrowse = 601,
    ChocolateyInstall = 602,
    ChocolateyUninstall = 603,
    AdvancedUninstall = 604,

    // Network Management (700-799)
    NetworkDiagnostics = 700,
    WifiAnalyzer = 701,
    ActiveConnections = 702,
    FirewallAuditRead = 703,
    FirewallAuditModify = 704,
    PortScanner = 705,
    BandwidthTest = 706,
    InternetSpeedTest = 707,
    NetworkShareBrowser = 708,
    DnsLookup = 709,
    DnsCacheFlush = 710,
    ModifyAdapterConfig = 711,

    // Hardware & Diagnostics (800-899)
    HardwareInventory = 800,
    CpuThermalData = 801,
    StressTest = 802,

    // Permissions (900-999)
    PermissionStripInherit = 900,
    PermissionTakeOwnership = 901,
    PermissionSetAcl = 902,
};

/// @brief Metadata about a feature's elevation requirement
struct FeatureElevation {
    FeatureId id;
    ElevationTier tier;
    std::string_view name;
    std::string_view reason;  ///< Why elevation is needed (empty for Standard)
};

/// @brief Complete table of feature elevation requirements
///
/// Every user-facing feature must appear in this table. The table is sorted
/// by FeatureId for binary-search lookup.
inline constexpr FeatureElevation kFeatureElevationTable[] = {
    // File Management — Standard
    {FeatureId::AdvancedSearch, ElevationTier::Standard, "Advanced Search", ""},
    {FeatureId::FileOrganizer, ElevationTier::Standard, "File Organizer", ""},
    {FeatureId::DuplicateFinder, ElevationTier::Standard, "Duplicate Finder", ""},

    // Email Tools — Standard
    {FeatureId::EmailInspector, ElevationTier::Standard, "Email Inspector", ""},

    // Quick Actions — Standard
    {FeatureId::GenerateSystemReport, ElevationTier::Standard, "Generate System Report", ""},
    {FeatureId::ScreenshotSettings, ElevationTier::Standard, "Screenshot Settings", ""},
    {FeatureId::OptimizePowerSettings, ElevationTier::Standard, "Optimize Power Settings", ""},

    // Quick Actions — Elevated
    {FeatureId::BackupBitlockerKeys,
     ElevationTier::Elevated,
     "Backup BitLocker Keys",
     "BitLocker WMI namespace requires administrator access"},
    {FeatureId::VerifySystemFiles,
     ElevationTier::Elevated,
     "Verify System Files",
     "SFC and DISM write to protected system directories"},
    {FeatureId::CheckDiskErrors,
     ElevationTier::Elevated,
     "Check Disk Errors",
     "Disk checking requires filesystem-level access"},
    {FeatureId::ResetNetworkStack,
     ElevationTier::Elevated,
     "Reset Network Stack",
     "netsh reset commands require administrator privileges"},

    // Image Flasher — Elevated
    {FeatureId::FlashUsbDrive,
     ElevationTier::Elevated,
     "Flash USB Drive",
     "Raw disk I/O requires administrator privileges"},
    {FeatureId::UupIsoConversion,
     ElevationTier::Elevated,
     "UUP ISO Conversion",
     "DISM AppX provisioning requires administrator privileges"},

    // Backup & Restore — Mixed
    {FeatureId::BackupCurrentUser, ElevationTier::Standard, "Backup Current User", ""},
    {FeatureId::BackupCrossUser,
     ElevationTier::Elevated,
     "Backup Other User Profiles",
     "SE_BACKUP_NAME privilege required for cross-user file access"},
    {FeatureId::RestoreToUserPaths, ElevationTier::Standard, "Restore to User Paths", ""},
    {FeatureId::RestoreWithAcls,
     ElevationTier::Elevated,
     "Restore with ACLs",
     "SE_RESTORE_NAME and SE_TAKE_OWNERSHIP_NAME required"},

    // App Management — Mixed
    {FeatureId::AppScanning, ElevationTier::Standard, "Application Scanner", ""},
    {FeatureId::ChocolateyBrowse, ElevationTier::Standard, "Browse Chocolatey Packages", ""},
    {FeatureId::ChocolateyInstall,
     ElevationTier::Elevated,
     "Install Chocolatey Packages",
     "Package installation modifies system directories and registry"},
    {FeatureId::ChocolateyUninstall,
     ElevationTier::Elevated,
     "Uninstall Chocolatey Packages",
     "Package removal modifies system directories and registry"},
    {FeatureId::AdvancedUninstall,
     ElevationTier::Elevated,
     "Advanced Uninstall",
     "System application removal requires administrator privileges"},

    // Network Management — mostly Standard
    {FeatureId::NetworkDiagnostics, ElevationTier::Standard, "Network Diagnostics", ""},
    {FeatureId::WifiAnalyzer, ElevationTier::Standard, "WiFi Analyzer", ""},
    {FeatureId::ActiveConnections, ElevationTier::Standard, "Active Connections Monitor", ""},
    {FeatureId::FirewallAuditRead, ElevationTier::Standard, "Firewall Rule Auditor (Read)", ""},
    {FeatureId::FirewallAuditModify,
     ElevationTier::Elevated,
     "Firewall Rule Modification",
     "Modifying firewall rules requires administrator privileges"},
    {FeatureId::PortScanner, ElevationTier::Standard, "Port Scanner", ""},
    {FeatureId::BandwidthTest, ElevationTier::Standard, "LAN Bandwidth Test", ""},
    {FeatureId::InternetSpeedTest, ElevationTier::Standard, "Internet Speed Test", ""},
    {FeatureId::NetworkShareBrowser, ElevationTier::Standard, "Network Share Browser", ""},
    {FeatureId::DnsLookup, ElevationTier::Standard, "DNS Lookup", ""},
    {FeatureId::DnsCacheFlush,
     ElevationTier::Elevated,
     "Flush DNS Cache",
     "ipconfig /flushdns requires administrator privileges"},
    {FeatureId::ModifyAdapterConfig,
     ElevationTier::Elevated,
     "Modify Adapter Configuration",
     "Changing IP/DNS settings requires administrator privileges"},

    // Hardware & Diagnostics — Mixed
    {FeatureId::HardwareInventory, ElevationTier::Standard, "Hardware Inventory", ""},
    {FeatureId::CpuThermalData,
     ElevationTier::Elevated,
     "CPU Thermal Data",
     "root/WMI thermal namespace requires administrator access"},
    {FeatureId::StressTest, ElevationTier::Standard, "Stress Test", ""},

    // Permissions — Elevated
    {FeatureId::PermissionStripInherit,
     ElevationTier::Mixed,
     "Strip/Inherit Permissions",
     "System files may require elevation; user files do not"},
    {FeatureId::PermissionTakeOwnership,
     ElevationTier::Elevated,
     "Take File Ownership",
     "SE_TAKE_OWNERSHIP_NAME privilege required"},
    {FeatureId::PermissionSetAcl,
     ElevationTier::Mixed,
     "Set File ACL",
     "System files may require elevation; user files do not"},
};

/// @brief Total number of registered features
inline constexpr size_t kFeatureCount = sizeof(kFeatureElevationTable) /
                                        sizeof(kFeatureElevationTable[0]);

/// @brief Look up the elevation tier for a given feature
/// @param id Feature identifier
/// @return Pointer to the FeatureElevation entry, or nullptr if not found
[[nodiscard]] constexpr const FeatureElevation* findFeatureElevation(FeatureId id) noexcept {
    for (const auto& entry : kFeatureElevationTable) {
        if (entry.id == id) {
            return &entry;
        }
    }
    return nullptr;
}

/// @brief Check whether a feature requires admin privileges
/// @param id Feature identifier
/// @return true if the feature is Elevated or Mixed tier
[[nodiscard]] constexpr bool featureNeedsElevation(FeatureId id) noexcept {
    const auto* entry = findFeatureElevation(id);
    if (!entry) {
        return false;
    }
    return entry->tier != ElevationTier::Standard;
}

// -- Compile-Time Invariants -------------------------------------------------

static_assert(sizeof(ElevationTier) == 1, "ElevationTier must be 1 byte");
static_assert(sizeof(FeatureId) == 2, "FeatureId must be 2 bytes");
static_assert(kFeatureCount > 0, "Feature elevation table must not be empty");

}  // namespace sak
