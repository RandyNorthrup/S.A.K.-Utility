// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file secure_memory.cpp
/// @brief Implementation of secure memory handling utilities

#include "sak/secure_memory.h"

#include <cstring>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")
#else
#include <sys/mman.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/random.h>
#endif
#ifdef __APPLE__
#include "sak/logger.h"

#include <QtGlobal>

#include <Security/Security.h>
#endif
#endif

namespace {

#ifndef _WIN32
bool readFromDevUrandom(void* buffer, std::size_t size) noexcept {
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        return false;
    }
    size_t bytes_read = fread(buffer, 1, size, urandom);
    fclose(urandom);
    return bytes_read == size;
}
#endif

}  // namespace

namespace sak {

bool generateSecureRandom(void* buffer, std::size_t size) noexcept {
    Q_ASSERT(buffer);
    if (buffer == nullptr || size == 0) {
        return false;
    }

#ifdef _WIN32
    if (size > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    NTSTATUS status = BCryptGenRandom(nullptr,
                                      static_cast<PUCHAR>(buffer),
                                      static_cast<ULONG>(size),
                                      BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return BCRYPT_SUCCESS(status);

#elif defined(__linux__)
    ssize_t result = getrandom(buffer, size, 0);
    if (result < 0) {
        return readFromDevUrandom(buffer, size);
    }
    return static_cast<std::size_t>(result) == size;

#elif defined(__APPLE__)
    int result = SecRandomCopyBytes(kSecRandomDefault, size, static_cast<uint8_t*>(buffer));
    if (result != errSecSuccess) {
        return readFromDevUrandom(buffer, size);
    }
    return true;

#else
    return readFromDevUrandom(buffer, size);
#endif
}

bool lockMemory(void* ptr, std::size_t size) noexcept {
    Q_ASSERT(ptr);
    if (ptr == nullptr || size == 0) {
        return false;
    }

#ifdef _WIN32
    // VirtualLock on Windows
    return VirtualLock(ptr, size) != 0;

#else
    // mlock on Unix
    return mlock(ptr, size) == 0;
#endif
}

bool unlockMemory(void* ptr, std::size_t size) noexcept {
    Q_ASSERT(ptr);
    if (ptr == nullptr || size == 0) {
        return false;
    }

#ifdef _WIN32
    // VirtualUnlock on Windows
    return VirtualUnlock(ptr, size) != 0;

#else
    // munlock on Unix
    return munlock(ptr, size) == 0;
#endif
}

}  // namespace sak
