// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file network_constants.h
/// @brief Centralized network, port, and transfer constants for the SAK Utility
///
/// All port numbers, transfer limits, aria2c configuration values, and network
/// buffer sizes should reference these constants.

#pragma once

#include <cstdint>

namespace sak {

// ============================================================================
// Network ports
// ============================================================================

constexpr uint16_t kPortDiscovery = 54'321;  ///< Peer discovery / broadcast
constexpr uint16_t kPortControl = 54'322;    ///< Control channel
constexpr uint16_t kPortData = 54'323;       ///< Data transfer channel
constexpr uint16_t kPortRangeMin = 1024;     ///< Lowest assignable port
constexpr uint16_t kPortRangeMax = 65'535;   ///< Highest assignable port

// ============================================================================
// Buffer sizes
// ============================================================================

constexpr int64_t kBufferAlignment = 4096;                     ///< Sector-aligned buffer
constexpr int64_t kBufferChunkDefault = 65'536;                ///< Default chunk (64 KB)
constexpr int64_t kBufferRead128K = 131'072;                   ///< 128 KB read buffer
constexpr int64_t kBufferRead1MB = 1'048'576;                  ///< 1 MB streaming buffer
constexpr int64_t kBufferMax5MB = 5 * 1'048'576;               ///< 5 MB cap (ISO chunk)
constexpr int64_t kDatagramMax = 65'536;                       ///< Max UDP datagram
constexpr int64_t kMinFreeSpace64MB = 64 * 1'048'576;          ///< 64 MB minimum free space
constexpr int64_t kMinFreeSpace100MB = 100 * 1'048'576;        ///< 100 MB minimum free space
constexpr int64_t kMinFreeSpace256MB = 256 * 1'048'576;        ///< 256 MB minimum free space
constexpr int64_t kMinFreeSpace512MB = 512 * 1'048'576;        ///< 512 MB minimum free space
constexpr int64_t kWarnFreeSpaceGB = 16LL * 1024 * 1'048'576;  ///< 16 GB ISO warning

// ============================================================================
// Transfer / aria2c configuration
// ============================================================================

constexpr int kAria2TimeoutSec = 60;             ///< aria2c --timeout
constexpr int kAria2TimeoutLongSec = 120;        ///< aria2c extended timeout
constexpr int kAria2ConnectTimeoutSec = 10;      ///< aria2c --connect-timeout (fast)
constexpr int kAria2ConnectTimeoutLongSec = 30;  ///< aria2c --connect-timeout (slow)
constexpr int kAria2RetryWaitSec = 3;            ///< aria2c --retry-wait (fast)
constexpr int kAria2RetryWaitLongSec = 5;        ///< aria2c --retry-wait (long)
constexpr int kAria2MaxTries = 5;                ///< aria2c --max-tries (normal)
constexpr int kAria2MaxTriesHigh = 10;           ///< aria2c --max-tries (resilient)
constexpr int kAria2MaxConnsPerServer = 16;      ///< aria2c --max-connection-per-server
constexpr int kAria2Split = 16;                  ///< aria2c --split
constexpr int kAria2SingleConn = 1;              ///< Single connection mode
constexpr int kAria2SingleSplit = 1;             ///< No splitting

// Max concurrent transfers
constexpr int kMaxConcurrentTransfers = 2;  ///< File transfer concurrency
constexpr int kMaxConcurrentScrape = 10;    ///< Web scrape concurrency

}  // namespace sak
