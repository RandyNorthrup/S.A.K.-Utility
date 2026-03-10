// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_secure_memory.cpp
/// @brief Unit tests for secure memory handling utilities

#include "sak/secure_memory.h"

#include <QtTest/QtTest>

#include <array>
#include <cstring>
#include <vector>

class SecureMemoryTests : public QObject {
    Q_OBJECT

private Q_SLOTS:
    // secure_wiper
    void wipe_zerosMemory();
    void wipe_spanOverload();
    void wipe_nullIsNoop();
    void wipe_zeroSizeIsNoop();

    // secure_buffer
    void secureBuffer_allocAndAccess();
    void secureBuffer_clear();
    void secureBuffer_moveConstructor();
    void secureBuffer_moveAssignment();
    void secureBuffer_emptyCheck();
    void secureBuffer_spanAccess();

    // secure_allocator
    void secureAllocator_allocDeallocRoundTrip();
    void secureAllocator_stringUsage();

    // generateSecureRandom
    void secureRandom_fillsBuffer();
    void secureRandom_differentEachCall();
    void secureRandom_nullBuffer();
    void secureRandom_zeroSize();

    // secureCompare
    void secureCompare_equal();
    void secureCompare_notEqual();
    void secureCompare_differentLengths();
    void secureCompare_stringView();

    // locked_memory
    void lockedMemory_lockUnlock();
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

// ============================================================================
// secure_buffer Tests
// ============================================================================

void SecureMemoryTests::secureBuffer_allocAndAccess() {
    sak::secure_buffer<unsigned char> buf(128);
    QCOMPARE(buf.size(), std::size_t{128});
    QVERIFY(buf.data() != nullptr);
    QVERIFY(!buf.empty());

    // Write and read back
    buf[0] = 0xFF;
    buf[127] = 0xAB;
    QCOMPARE(buf[0], static_cast<unsigned char>(0xFF));
    QCOMPARE(buf[127], static_cast<unsigned char>(0xAB));
}

void SecureMemoryTests::secureBuffer_clear() {
    sak::secure_buffer<unsigned char> buf(64);
    for (std::size_t i = 0; i < buf.size(); ++i) {
        buf[i] = static_cast<unsigned char>(i);
    }

    buf.clear();

    for (std::size_t i = 0; i < buf.size(); ++i) {
        QCOMPARE(buf[i], static_cast<unsigned char>(0));
    }
}

void SecureMemoryTests::secureBuffer_moveConstructor() {
    sak::secure_buffer<int> original(32);
    original[0] = 999;

    sak::secure_buffer<int> moved(std::move(original));
    QCOMPARE(moved.size(), std::size_t{32});
    QCOMPARE(moved[0], 999);
    QVERIFY(original.empty());  // NOLINT: testing moved-from state
}

void SecureMemoryTests::secureBuffer_moveAssignment() {
    sak::secure_buffer<int> a(16);
    a[0] = 100;

    sak::secure_buffer<int> b(8);
    b = std::move(a);

    QCOMPARE(b.size(), std::size_t{16});
    QCOMPARE(b[0], 100);
    QVERIFY(a.empty());  // NOLINT: testing moved-from state
}

void SecureMemoryTests::secureBuffer_emptyCheck() {
    sak::secure_buffer<char> buf(0);
    QVERIFY(buf.empty());
    QCOMPARE(buf.size(), std::size_t{0});
}

void SecureMemoryTests::secureBuffer_spanAccess() {
    sak::secure_buffer<int> buf(4);
    buf[0] = 1;
    buf[1] = 2;
    buf[2] = 3;
    buf[3] = 4;

    auto s = buf.span();
    QCOMPARE(s.size(), std::size_t{4});
    QCOMPARE(s[0], 1);
    QCOMPARE(s[3], 4);
}

// ============================================================================
// secure_allocator Tests
// ============================================================================

void SecureMemoryTests::secureAllocator_allocDeallocRoundTrip() {
    sak::secure_allocator<int> alloc;
    int* ptr = alloc.allocate(10);
    QVERIFY(ptr != nullptr);

    // Write values
    for (int i = 0; i < 10; ++i) {
        ptr[i] = i * 100;
    }
    QCOMPARE(ptr[5], 500);

    // Deallocate (should wipe and free)
    alloc.deallocate(ptr, 10);
}

void SecureMemoryTests::secureAllocator_stringUsage() {
    // secure_string should work as a std::string replacement
    sak::secure_string s;
    s = "sensitive_password_123";
    QVERIFY(!s.empty());
    QCOMPARE(s.length(), std::size_t{22});
    QVERIFY(s == "sensitive_password_123");
}

// ============================================================================
// generateSecureRandom Tests
// ============================================================================

void SecureMemoryTests::secureRandom_fillsBuffer() {
    std::array<unsigned char, 32> buffer{};
    bool result = sak::generateSecureRandom(buffer.data(), buffer.size());
    QVERIFY(result);

    // Extremely unlikely all 32 bytes are still zero
    bool allZero = true;
    for (auto byte : buffer) {
        if (byte != 0) {
            allZero = false;
            break;
        }
    }
    QVERIFY2(!allZero, "Random buffer should not be all zeros");
}

void SecureMemoryTests::secureRandom_differentEachCall() {
    std::array<unsigned char, 32> a{}, b{};
    QVERIFY(sak::generateSecureRandom(a.data(), a.size()));
    QVERIFY(sak::generateSecureRandom(b.data(), b.size()));
    QVERIFY(a != b);
}

void SecureMemoryTests::secureRandom_nullBuffer() {
    bool result = sak::generateSecureRandom(nullptr, 32);
    QVERIFY(!result);
}

void SecureMemoryTests::secureRandom_zeroSize() {
    unsigned char dummy = 0xAA;
    bool result = sak::generateSecureRandom(&dummy, 0);
    QVERIFY(!result);
}

// ============================================================================
// secureCompare Tests
// ============================================================================

void SecureMemoryTests::secureCompare_equal() {
    std::array<unsigned char, 8> a = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<unsigned char, 8> b = {1, 2, 3, 4, 5, 6, 7, 8};
    QVERIFY(
        sak::secureCompare(std::span<const unsigned char>(a), std::span<const unsigned char>(b)));
}

void SecureMemoryTests::secureCompare_notEqual() {
    std::array<unsigned char, 4> a = {1, 2, 3, 4};
    std::array<unsigned char, 4> b = {1, 2, 3, 5};
    QVERIFY(
        !sak::secureCompare(std::span<const unsigned char>(a), std::span<const unsigned char>(b)));
}

void SecureMemoryTests::secureCompare_differentLengths() {
    std::array<unsigned char, 3> a = {1, 2, 3};
    std::array<unsigned char, 4> b = {1, 2, 3, 4};
    QVERIFY(
        !sak::secureCompare(std::span<const unsigned char>(a), std::span<const unsigned char>(b)));
}

void SecureMemoryTests::secureCompare_stringView() {
    QVERIFY(sak::secureCompare(std::string_view("hello"), std::string_view("hello")));
    QVERIFY(!sak::secureCompare(std::string_view("hello"), std::string_view("world")));
    QVERIFY(!sak::secureCompare(std::string_view("short"), std::string_view("longer_string")));
}

// ============================================================================
// locked_memory Tests
// ============================================================================

void SecureMemoryTests::lockedMemory_lockUnlock() {
    std::array<unsigned char, 4096> buffer{};
    sak::locked_memory lock(buffer.data(), buffer.size());

    // Lock may or may not succeed depending on working set quota
    // We just verify it doesn't crash and reports state consistently
    bool locked = lock.isLocked();
    Q_UNUSED(locked);  // State depends on OS quota

    // Destructor should handle unlock without crashing
}

QTEST_GUILESS_MAIN(SecureMemoryTests)
#include "test_secure_memory.moc"
