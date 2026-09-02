// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file secure_memory.cpp
/// @brief Implementation of secure memory handling utilities

#include "sak/secure_memory.h"

#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

namespace sak {

bool generateSecureRandom(void* buffer, std::size_t size) noexcept {
    // There is deliberately no Q_ASSERT(buffer) here. The runtime rejection below
    // IS the contract -- it is what every caller and test relies on. An assert
    // would make the SAME input abort the process in a Debug build (qt_assert ->
    // qFatal -> abort -> __fastfail, exit 0xC0000409) while being handled cleanly
    // in Release, so the function would have two different behaviours.
    if (buffer == nullptr || size == 0) {
        return false;
    }

    if (size > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    NTSTATUS const status = BCryptGenRandom(nullptr,
                                            static_cast<PUCHAR>(buffer),
                                            static_cast<ULONG>(size),
                                            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);
}

bool lockMemory(void* ptr, std::size_t size) noexcept {
    // No assert on ptr; the rejection below is the contract (see
    // generateSecureRandom).
    if (ptr == nullptr || size == 0) {
        return false;
    }

    return VirtualLock(ptr, size) != 0;
}

bool unlockMemory(void* ptr, std::size_t size) noexcept {
    // No assert on ptr; see lockMemory.
    if (ptr == nullptr || size == 0) {
        return false;
    }

    return VirtualUnlock(ptr, size) != 0;
}

}  // namespace sak
