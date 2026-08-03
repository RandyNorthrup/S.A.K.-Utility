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

// Hard cap on a dependency-feed (FindPackagesById) response before it is handed
// to the DOM parser. The feed for one id is a small metadata document; 32 MiB is
// generous while bounding a hostile/oversized endpoint (parity with the
// package-internalization version fetch).
constexpr int64_t kMaxFeedResponseBytes = 32LL * 1024 * 1024;

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

/// @brief Hard cap on a deployment manifest.json read from disk on the install
/// side. 200 packages of metadata is a few dozen KiB, so 8 MiB is generous while
/// still bounding a hostile/corrupt manifest before it is read wholesale into
/// memory (parity with the build side's 32 MiB feed-read cap).
constexpr long long kMaxManifestBytes = 8LL * 1024 * 1024;

// ============================================================================
// Checksums
// ============================================================================

constexpr auto kDefaultChecksumAlgorithm = "sha256";
constexpr int kChecksumBlockSize = 65'536;

// ============================================================================
// Install exit codes
// ============================================================================

// Chocolatey / MSI exit codes that all mean the install SUCCEEDED: 0 = success;
// 1641 = reboot has been initiated; 3010 = a reboot is required to finish. A
// bundle install must treat 1641/3010 as success (reboot pending), not failure.
constexpr int kExitSuccess = 0;
constexpr int kExitRebootInitiated = 1641;
constexpr int kExitRebootRequired = 3010;

// ============================================================================
// Deployment manifest
// ============================================================================

constexpr auto kManifestVersion = "1.0";
constexpr auto kManifestFilename = "manifest.json";
constexpr auto kPackagesSubdir = "packages";
constexpr auto kInstallersSubdir = "installers";
constexpr auto kLogsSubdir = "logs";
constexpr auto kReadmeFilename = "README.txt";

// Ownership stamp dropped into a build's <output>/_work directory so a later
// recursive cleanup only ever deletes a work tree this app created (a
// pre-existing FOREIGN _work is refused, never wiped).
constexpr auto kWorkDirOwnershipMarker = ".sak_offline_work";

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
