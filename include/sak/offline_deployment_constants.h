// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file offline_deployment_constants.h
/// @brief Centralized constants for the offline deployment package system
///
/// All timeouts, limits, API endpoints, and format tokens for the
/// offline deployment feature should be defined here.

#pragma once

#include <cstdint>

namespace sak::offline {

// ============================================================================
// NuGet v2 API
// ============================================================================

constexpr auto kNuGetBaseUrl = "https://community.chocolatey.org/api/v2/";
constexpr auto kNuGetSearchPath = "Search()";
constexpr auto kNuGetPackagePath = "package/";
constexpr auto kNuGetFindByIdPath = "FindPackagesById()";
constexpr auto kNuGetPackagesPath = "Packages";

constexpr int kApiRequestTimeoutMs = 30'000;
constexpr int kApiMaxRetries = 3;
constexpr int kApiRetryDelayBaseMs = 2000;
constexpr int kSearchMaxResults = 50;
constexpr int kSearchResultsDefault = 30;

// ============================================================================
// Binary downloads
// ============================================================================

constexpr int kDownloadTimeoutMs = 300'000;
constexpr int kDownloadMaxRetries = 3;
constexpr int kDownloadRetryDelayBaseMs = 3000;
constexpr int kMaxConcurrentDownloads = 3;
constexpr int64_t kMaxBinarySizeBytes = 4LL * 1024 * 1024 * 1024;
constexpr int kDownloadBufferSize = 65'536;
constexpr int kDownloadProgressIntervalMs = 250;

// ============================================================================
// Internalization
// ============================================================================

constexpr int kMaxPackagesPerBuild = 200;
constexpr int kPackTimeoutMs = 60'000;
constexpr int kInstallTimeoutPerPackageMs = 600'000;
constexpr int kMaxDependencyDepth = 10;

// ============================================================================
// Checksums
// ============================================================================

constexpr auto kDefaultChecksumAlgorithm = "sha256";
constexpr int kChecksumBlockSize = 65'536;

// ============================================================================
// Deployment manifest
// ============================================================================

constexpr auto kManifestVersion = "1.0";
constexpr auto kManifestFilename = "manifest.json";
constexpr auto kPackagesSubdir = "packages";
constexpr auto kInstallersSubdir = "installers";
constexpr auto kLogsSubdir = "logs";
constexpr auto kReadmeFilename = "README.txt";

// ============================================================================
// Package list presets
// ============================================================================

constexpr int kMaxPackageListEntries = 500;
constexpr int kMaxPackageListNameLength = 100;

// ============================================================================
// Compile-time invariants
// ============================================================================

static_assert(kApiMaxRetries > 0, "Must allow at least one API attempt");
static_assert(kDownloadMaxRetries > 0, "Must allow at least one download attempt");
static_assert(kMaxConcurrentDownloads >= 1, "Must allow at least one concurrent download");
static_assert(kMaxConcurrentDownloads <= 8, "Too many concurrent downloads");
static_assert(kMaxDependencyDepth >= 1, "Must allow at least one dependency level");
static_assert(kMaxPackagesPerBuild > 0, "Must allow at least one package per build");

}  // namespace sak::offline
