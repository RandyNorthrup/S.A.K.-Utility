// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file secure_memory.h
/// @brief Secure memory handling utilities for sensitive data
/// @details Provides RAII-based secure memory management following security best practices

#pragma once

#include <QtGlobal>

#include <cstddef>
#include <span>
#include <type_traits>

#include <intrin.h>

namespace sak {

/// @brief Secure memory wiper - ensures memory is zeroed before deallocation
/// @details Uses volatile pointer to prevent compiler optimization of zeroing
class secure_wiper {
public:
    /// @brief Securely wipe memory region
    /// @param ptr Pointer to memory to wipe
    /// @param size Size of memory region in bytes
    static void wipe(void* ptr, std::size_t size) noexcept {
        if (ptr == nullptr || size == 0) {
            return;
        }

        // Use volatile to prevent compiler from optimizing away the zeroing
        volatile unsigned char* vptr = static_cast<volatile unsigned char*>(ptr);

        // Zero the memory
        for (std::size_t i = 0; i < size; ++i) {
            vptr[i] = 0;
        }

        // Memory barrier to ensure writes complete
        _ReadWriteBarrier();
    }

    /// @brief Securely wipe std::span
    /// @tparam T Element type
    /// @param data Span to wipe
    template <typename T>
    static void wipe(std::span<T> data) noexcept {
        // Overwriting the object representation of a non-trivial T is undefined behavior; the
        // byte-wise zeroing here is well-defined only for a trivially-copyable element type.
        static_assert(std::is_trivially_copyable_v<T>,
                      "secure_wiper::wipe requires a trivially-copyable element type");
        wipe(data.data(), data.size_bytes());
    }
};

}  // namespace sak
