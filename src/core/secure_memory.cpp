// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file secure_memory.cpp
/// @brief Implementation of secure memory handling utilities

#include "sak/secure_memory.h"

#include <cerrno>
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
// Only the generic non-Windows/Linux/Apple fallback branch uses this; Linux and
// Apple now fail closed on their platform CSPRNG instead of falling back here.
[[maybe_unused]] bool readFromDevUrandom(void* buffer, std::size_t size) noexcept {
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
    // There is deliberately no Q_ASSERT(buffer) here. The runtime rejection below
    // IS the contract -- it is what every caller and test relies on. An assert
    // would make the SAME input abort the process in a Debug build (qt_assert ->
    // qFatal -> abort -> __fastfail, exit 0xC0000409) while being handled cleanly
    // in Release, so the function would have two different behaviours.
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
    // Fail closed: getrandom is the only accepted CSPRNG here. Retry the transient
    // EINTR and drain partial reads (getrandom can return fewer than `size` bytes for
    // a large request), but surface any real failure as false -- never a silent
    // /dev/urandom fallback that would mask the getrandom error. [[no-fallbacks-fail-closed]]
    auto* out = static_cast<unsigned char*>(buffer);
    std::size_t filled = 0;
    while (filled < size) {
        const ssize_t result = getrandom(out + filled, size - filled, 0);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        filled += static_cast<std::size_t>(result);
    }
    return true;

#elif defined(__APPLE__)
    // Fail closed: no /dev/urandom fallback that would mask a real SecRandomCopyBytes
    // failure. [[no-fallbacks-fail-closed]]
    return SecRandomCopyBytes(kSecRandomDefault, size, static_cast<uint8_t*>(buffer)) ==
           errSecSuccess;

#else
    return readFromDevUrandom(buffer, size);
#endif
}

bool lockMemory(void* ptr, std::size_t size) noexcept {
    // No assert on ptr; the rejection below is the contract (see
    // generateSecureRandom).
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
    // No assert on ptr; see lockMemory.
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
