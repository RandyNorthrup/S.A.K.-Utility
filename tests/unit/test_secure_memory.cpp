// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_secure_memory.cpp
/// @brief Unit tests for secure memory handling utilities

#include "sak/secure_memory.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <array>

class SecureMemoryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // secure_wiper
    void wipe_zerosMemory();
    void wipe_spanOverload();
    void wipe_nullIsNoop();
    void wipe_zeroSizeIsNoop();
};

// ============================================================================
// secure_wiper Tests
// ============================================================================

void SecureMemoryTests::wipe_zerosMemory() {
    std::array<unsigned char, 64> buffer{};
    std::fill(buffer.begin(), buffer.end(), 0xAA);

    sak::secure_wiper::wipe(buffer.data(), buffer.size());

    for (auto byte : buffer) {
        QCOMPARE(byte, static_cast<unsigned char>(0));
    }
}

void SecureMemoryTests::wipe_spanOverload() {
    std::array<int, 16> data{};
    std::fill(data.begin(), data.end(), 42);

    sak::secure_wiper::wipe(std::span<int>(data));

    for (auto val : data) {
        QCOMPARE(val, 0);
    }
}

void SecureMemoryTests::wipe_nullIsNoop() {
    // Should not crash
    sak::secure_wiper::wipe(nullptr, 0);
    sak::secure_wiper::wipe(nullptr, 100);
}

void SecureMemoryTests::wipe_zeroSizeIsNoop() {
    int dummy = 42;
    sak::secure_wiper::wipe(&dummy, 0);
    // Value should remain unchanged (wipe with size 0 is a no-op)
    QCOMPARE(dummy, 42);
}

QTEST_GUILESS_MAIN(SecureMemoryTests)
#include "test_secure_memory.moc"
