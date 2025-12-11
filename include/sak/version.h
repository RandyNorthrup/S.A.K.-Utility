/// @file version.h
/// @brief Centralized version information for S.A.K. Utility

#pragma once

#define SAK_VERSION_MAJOR 0
#define SAK_VERSION_MINOR 5
#define SAK_VERSION_PATCH 0

#define SAK_VERSION_STRING "0.5.0"
#define SAK_VERSION_STRING_SHORT "0.5"

// Build date macros
#define SAK_BUILD_DATE __DATE__
#define SAK_BUILD_TIME __TIME__

// Product information
#define SAK_PRODUCT_NAME "S.A.K. Utility"
#define SAK_PRODUCT_FULL_NAME "Swiss Army Knife Utility"
#define SAK_PRODUCT_DESCRIPTION "PC Technician's Toolkit - Windows PC Migration & Backup Tool"
#define SAK_COPYRIGHT "Copyright (C) 2025 Randy Northrup"
#define SAK_LICENSE "GNU General Public License v2.0"

// Organization information
#define SAK_ORGANIZATION_NAME "SAK"
#define SAK_ORGANIZATION_DOMAIN "sak-utility.local"

namespace sak {

/// @brief Get the full version string
/// @return Version string in format "0.5.0"
constexpr const char* get_version() noexcept {
    return SAK_VERSION_STRING;
}

/// @brief Get the short version string
/// @return Version string in format "0.5"
constexpr const char* get_version_short() noexcept {
    return SAK_VERSION_STRING_SHORT;
}

/// @brief Get the major version number
/// @return Major version (0)
constexpr int get_version_major() noexcept {
    return SAK_VERSION_MAJOR;
}

/// @brief Get the minor version number
/// @return Minor version (5)
constexpr int get_version_minor() noexcept {
    return SAK_VERSION_MINOR;
}

/// @brief Get the patch version number
/// @return Patch version (0)
constexpr int get_version_patch() noexcept {
    return SAK_VERSION_PATCH;
}

/// @brief Get the product name
/// @return Product name
constexpr const char* get_product_name() noexcept {
    return SAK_PRODUCT_NAME;
}

/// @brief Get the full product name
/// @return Full product name
constexpr const char* get_product_full_name() noexcept {
    return SAK_PRODUCT_FULL_NAME;
}

/// @brief Get the build date
/// @return Build date string
constexpr const char* get_build_date() noexcept {
    return SAK_BUILD_DATE;
}

/// @brief Get the build time
/// @return Build time string
constexpr const char* get_build_time() noexcept {
    return SAK_BUILD_TIME;
}

} // namespace sak
