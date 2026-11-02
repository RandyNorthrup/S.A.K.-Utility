// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file secure_memory.cpp
/// @brief Implementation of secure memory handling utilities

#include "sak/secure_memory.h"

#include <windows.h>

namespace sak {

bool unlockMemory(void* ptr, std::size_t size) noexcept {
    // There is deliberately no Q_ASSERT(ptr) here: the rejection below is the contract, and an
    // assert would make the same input abort in a Debug build and be refused in Release.
    if (ptr == nullptr || size == 0) {
        return false;
    }

    return VirtualUnlock(ptr, size) != 0;
}

}  // namespace sak
