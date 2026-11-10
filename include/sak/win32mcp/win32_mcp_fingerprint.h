// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#ifndef SAK_WIN32MCP_WIN32_MCP_FINGERPRINT_H
#define SAK_WIN32MCP_WIN32_MCP_FINGERPRINT_H

#include <QByteArray>

namespace sak::win32mcp {

// Visual-fingerprint parameters. A capture is reduced to a fixed kFingerprintGrid x
// kFingerprintGrid grayscale grid so two fingerprints are always comparable regardless of window
// size. A per-cell gray delta over kFingerprintTolerance counts as changed; more than
// kFingerprintChangedRatio of the cells changed means the view changed.
constexpr int kFingerprintGrid = 16;
constexpr int kFingerprintTolerance = 16;
constexpr double kFingerprintChangedRatio = 0.02;

// Sample a kFingerprintGrid x kFingerprintGrid grayscale grid (nearest-neighbour) from a BGRA
// buffer and return the grid.size() == kFingerprintGrid*kFingerprintGrid fingerprint. Returns an
// EMPTY QByteArray when width/height are non-positive or the buffer is smaller than width*height*4
// (a degenerate/short capture), so a bad frame reads as "changed" rather than an out-of-bounds
// sample.
QByteArray fingerprint(const QByteArray& bgra, int width, int height);

// Fraction of grid cells whose gray value differs by more than kFingerprintTolerance. Returns 1.0
// (fully changed) when either fingerprint is empty or the two differ in size.
double fingerprintDiff(const QByteArray& a, const QByteArray& b);

// True when the two fingerprints differ by more than kFingerprintChangedRatio -- the decision
// behind compare_screenshots (changed?) and wait_for_idle (view still moving?).
bool fingerprintChanged(const QByteArray& a, const QByteArray& b);

}  // namespace sak::win32mcp

#endif  // SAK_WIN32MCP_WIN32_MCP_FINGERPRINT_H
