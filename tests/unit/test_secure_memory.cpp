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

    // secure_memory_guard is the scope-exit half of the same wiping contract and has neither a
    // caller nor a test anywhere in the tree, so nothing pins its destructor at all. Both
    // factories must wipe the FULL m_size * sizeof(T) byte extent: with the sizeof(T) factor
    // dropped an 8-int region keeps six of its eight secrets.
    std::array<int, 8> guarded_span{};
    std::fill(guarded_span.begin(), guarded_span.end(), 0x5A'5A'5A'5A);
    { auto span_guard = sak::makeSecureGuard(std::span<int>(guarded_span)); }
    for (auto val : guarded_span) {
        QCOMPARE(val, 0);
    }

    std::array<int, 8> guarded_ptr{};
    std::fill(guarded_ptr.begin(), guarded_ptr.end(), 0x5A'5A'5A'5A);
    { auto ptr_guard = sak::makeSecureGuard(guarded_ptr.data(), guarded_ptr.size()); }
    for (auto val : guarded_ptr) {
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
// generateSecureRandom Tests
// ============================================================================

namespace {

// The std::span overload of generateSecureRandom has its own body and no caller anywhere in the
// tree: it must request size_bytes(), not size(). With the sizeof(T) factor dropped, a 16-int
// span gets only its first four elements randomized and the tail keeps whatever secret the
// caller left there. The sentinel is re-laid every round, so an element the generator never
// touches is byte-identical to it in EVERY round and the verdict is deterministic.
void verifySpanOverloadRandomizesEveryElement() {
    std::array<bool, 16> span_written{};
    for (int span_round = 0; span_round < 8; ++span_round) {
        std::array<int, 16> wide_draw{};
        std::fill(wide_draw.begin(), wide_draw.end(), 0x0D'0D'0D'0D);
        QVERIFY(sak::generateSecureRandom(std::span<int>(wide_draw)));
        for (std::size_t i = 0; i < wide_draw.size(); ++i) {
            if (wide_draw[i] != 0x0D'0D'0D'0D) {
                span_written[i] = true;
            }
        }
    }
    for (std::size_t i = 0; i < span_written.size(); ++i) {
        QVERIFY2(
            span_written[i],
            qPrintable(
                QStringLiteral("span element %1 was never randomized").arg(static_cast<int>(i))));
    }
}

}  // namespace

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

    verifySpanOverloadRandomizesEveryElement();
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

    // The size-overflow guard is the second of secureCompare's two refusals and has no coverage:
    // only the length-mismatch arm is ever reached. A count whose byte size wraps must be refused
    // OUTRIGHT -- with the guard gone the wrapped byte count is 0, the compare loop never runs and
    // two spans are reported EQUAL without a single byte being read. Equal sizes are used so the
    // length arm at the top cannot be what produces the refusal. No element is dereferenced on
    // either path.
    const std::size_t wrapping_count = std::numeric_limits<std::size_t>::max() / sizeof(int) + 1;
    QVERIFY2(!sak::secureCompare(std::span<const int>(wide_a.data(), wrapping_count),
                                 std::span<const int>(wide_c.data(), wrapping_count)),
             "secureCompare must refuse a length whose byte count would wrap");
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

    // Every inequality above differs at index 0, so the loop bound is never exercised: a compare
    // that looked at only the first byte -- or that stopped one byte short of the end -- returns
    // the same three verdicts. Pin both ends of the bound with pairs that differ ONLY at the last
    // byte and ONLY at an interior byte.
    QVERIFY(!sak::secureCompare(std::string_view("hello"), std::string_view("hellO")));
    QVERIFY(!sak::secureCompare(std::string_view("hello"), std::string_view("heLlo")));
    QVERIFY(!sak::secureCompare(std::string_view("short"), std::string_view("longer_string")));
}

// ============================================================================
// locked_memory Tests
// ============================================================================

void SecureMemoryTests::lockedMemory_lockUnlock() {
    std::array<unsigned char, 4096> buffer{};
    sak::locked_memory lock(buffer.data(), buffer.size());

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
