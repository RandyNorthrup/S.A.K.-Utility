// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_secure_memory.cpp
/// @brief Unit tests for secure memory handling utilities

#include "sak/secure_memory.h"

#include <QtTest/QtTest>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
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

    // sizeof(T) > 1: clear() must wipe m_size * sizeof(T) bytes, and clear() is also the
    // destructor body. On an unsigned char buffer a byte count that dropped the sizeof(T)
    // factor is invisible; on an int buffer it would leave three quarters of the secret in
    // freed memory.
    sak::secure_buffer<int> wide(16);
    for (std::size_t i = 0; i < wide.size(); ++i) {
        wide[i] = 0x7F'7F'7F'7F;
    }
    wide.clear();
    for (std::size_t i = 0; i < wide.size(); ++i) {
        QCOMPARE(wide[i], 0);
    }
}

void SecureMemoryTests::secureBuffer_moveConstructor() {
    sak::secure_buffer<int> original(32);
    original[0] = 999;

    sak::secure_buffer<int> moved(std::move(original));
    QCOMPARE(moved.size(), std::size_t{32});
    QCOMPARE(moved[0], 999);
    QVERIFY(moved.data() != nullptr);

    // Only the destination half was asserted. The move constructor also zeroes the SOURCE's
    // size; without that, the moved-from buffer still reports 32 elements over a null pointer,
    // so span() hands out a 32-int span over nullptr.
    // cppcheck-suppress accessMoved
    const std::size_t source_size = original.size();
    // cppcheck-suppress accessMoved
    const int* source_data = original.data();
    QCOMPARE(source_size, std::size_t{0});
    QVERIFY(source_data == nullptr);
}

void SecureMemoryTests::secureBuffer_moveAssignment() {
    sak::secure_buffer<int> a(16);
    a[0] = 100;

    sak::secure_buffer<int> b(8);
    b = std::move(a);

    QCOMPARE(b.size(), std::size_t{16});
    QCOMPARE(b[0], 100);
    QVERIFY(b.data() != nullptr);

    // Move assignment zeroes the source's size too.
    // cppcheck-suppress accessMoved
    const std::size_t source_size = a.size();
    // cppcheck-suppress accessMoved
    const int* source_data = a.data();
    QCOMPARE(source_size, std::size_t{0});
    QVERIFY(source_data == nullptr);

    // Self-assignment is guarded: without the this != &other check the operator would clear()
    // its own storage and then zero its own size, destroying a live buffer.
    sak::secure_buffer<int>& alias = b;
    b = std::move(alias);
    // cppcheck-suppress accessMoved
    const std::size_t self_size = b.size();
    // cppcheck-suppress accessMoved
    const int self_head = b[0];
    QCOMPARE(self_size, std::size_t{16});
    QCOMPARE(self_head, 100);
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
    QVERIFY(s.data() == buf.data());
    QCOMPARE(s[0], 1);
    QCOMPARE(s[1], 2);
    QCOMPARE(s[2], 3);
    QCOMPARE(s[3], 4);

    // The span must ALIAS the buffer, not a copy of it: a write through the span has to be
    // visible through operator[].
    s[2] = 30;
    QCOMPARE(buf[2], 30);

    // The const overload has its own separate body and no caller anywhere in the tree; it must
    // cover exactly the same region.
    const sak::secure_buffer<int>& cbuf = buf;
    const std::span<const int> cs = cbuf.span();
    QCOMPARE(cs.size(), std::size_t{4});
    QVERIFY(cs.data() == buf.data());
    QCOMPARE(cs[3], 4);
}

// ============================================================================
// secure_allocator Tests
// ============================================================================

void SecureMemoryTests::secureAllocator_allocDeallocRoundTrip() {
    sak::secure_allocator<int> alloc;
    int* ptr = alloc.allocate(10);
    QVERIFY(ptr != nullptr);

    // The size-overflow guard is the ONLY rejection allocate() has, and it had no coverage: a
    // count whose byte size wraps must throw, never hand back the minimal operator new(0) block
    // that the caller then writes 2^62 elements into.
    const int* overflow_ptr = nullptr;
    bool threw = false;
    try {
        overflow_ptr = alloc.allocate(std::numeric_limits<std::size_t>::max() / sizeof(int) + 1);
    } catch (const std::bad_alloc&) {
        threw = true;
    }
    QVERIFY2(threw, "allocate() must throw bad_alloc when n * sizeof(T) would wrap");
    QVERIFY(overflow_ptr == nullptr);

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

    // secure_string exists only to force its characters onto the heap: the small-string buffer
    // inside std::basic_string is never handed to the allocator, so deallocate() never wipes
    // it. Weaken the default constructor to `= default` and a fresh string reports the 15-char
    // small-string capacity, so every short secret lives in storage deallocate() never sees.
    QCOMPARE(sak::kSecureStringMinCapacity, std::size_t{32});
    const sak::secure_string fresh;
    QVERIFY(fresh.empty());
    QVERIFY2(fresh.capacity() >= sak::kSecureStringMinCapacity,
             "default-constructed secure_string must reserve past the small-string buffer");
    QVERIFY(s.capacity() >= sak::kSecureStringMinCapacity);
}

// ============================================================================
// generateSecureRandom Tests
// ============================================================================

void SecureMemoryTests::secureRandom_fillsBuffer() {
    std::array<unsigned char, 32> buffer{};
    bool result = sak::generateSecureRandom(buffer.data(), buffer.size());
    QVERIFY(result);

    // Every requested byte must actually be written. The check below fails only if the ENTIRE
    // buffer is still zero, so a generator that filled a prefix and left the tail untouched
    // passes it. Redraw and require each index to change at least once; for a real CSPRNG the
    // chance a given index repeats the same value across 64 further draws is (1/256)^64.
    std::array<bool, 32> varied{};
    for (int round = 0; round < 64; ++round) {
        std::array<unsigned char, 32> next{};
        QVERIFY(sak::generateSecureRandom(next.data(), next.size()));
        for (std::size_t i = 0; i < next.size(); ++i) {
            if (next[i] != buffer[i]) {
                varied[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < varied.size(); ++i) {
        QVERIFY2(varied[i],
                 qPrintable(QStringLiteral("byte %1 was never written").arg(static_cast<int>(i))));
    }

    // The size argument bounds the write in the other direction too: a 16-byte request must not
    // touch bytes 16..31.
    std::array<unsigned char, 32> bounded{};
    std::fill(bounded.begin(), bounded.end(), static_cast<unsigned char>(0xCD));
    QVERIFY(sak::generateSecureRandom(bounded.data(), 16));
    for (std::size_t i = 16; i < bounded.size(); ++i) {
        QCOMPARE(bounded[i], static_cast<unsigned char>(0xCD));
    }

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

    // sizeof(T) > 1: the compared byte count is size() * sizeof(T), and every span test in this
    // file uses unsigned char, so the multiplication is never exercised. A count that dropped
    // the factor would look at only the first 8 of these 32 bytes and report two different
    // 8-int buffers as equal.
    std::array<int, 8> wide_a = {1, 2, 3, 4, 5, 6, 7, 8};
    std::array<int, 8> wide_b = {1, 2, 3, 4, 5, 6, 7, 9};
    std::array<int, 8> wide_c = {1, 2, 3, 4, 5, 6, 7, 8};
    QVERIFY(!sak::secureCompare(std::span<const int>(wide_a), std::span<const int>(wide_b)));
    QVERIFY(sak::secureCompare(std::span<const int>(wide_a), std::span<const int>(wide_c)));
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

    // Whether the real 4096-byte region locks depends on the OS working-set quota, so that one
    // value stays unpinned -- but it was the slot's ONLY value, leaving this a pure no-crash
    // test. The rejections below do not depend on the machine.
    bool locked = lock.isLocked();
    Q_UNUSED(locked);  // State depends on OS quota

    // A null pointer or a zero-length region is refused before any OS call. Each rejection is
    // probed on its own arm -- the null cases pass a real length, the zero-length cases pass a
    // real pointer -- so neither can be satisfied by the other guard.
    QVERIFY(!sak::lockMemory(nullptr, buffer.size()));
    QVERIFY(!sak::lockMemory(buffer.data(), 0));
    QVERIFY(!sak::unlockMemory(nullptr, buffer.size()));
    QVERIFY(!sak::unlockMemory(buffer.data(), 0));

    // locked_memory must report unlocked for a region it never locked, so its destructor never
    // calls VirtualUnlock on memory it does not own.
    sak::locked_memory null_lock(nullptr, buffer.size());
    QVERIFY(!null_lock.isLocked());
    sak::locked_memory zero_lock(buffer.data(), 0);
    QVERIFY(!zero_lock.isLocked());

    // Destructor should handle unlock without crashing
}

QTEST_GUILESS_MAIN(SecureMemoryTests)
#include "test_secure_memory.moc"
