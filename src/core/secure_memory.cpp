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
#include <Security/Security.h>
#endif
#endif

namespace sak {

bool generate_secure_random(void* buffer, std::size_t size) noexcept {
    if (buffer == nullptr || size == 0) {
        return false;
    }
    
#ifdef _WIN32
    // Guard against size_t → ULONG truncation
    if (size > static_cast<std::size_t>(MAXDWORD)) {
        return false;
    }
    
    // Use BCryptGenRandom (modern Windows crypto API)
    NTSTATUS status = BCryptGenRandom(
        nullptr,  // Use default provider
        static_cast<PUCHAR>(buffer),
        static_cast<ULONG>(size),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    
    return BCRYPT_SUCCESS(status);
    
#elif defined(__linux__)
    // Use getrandom syscall (Linux 3.17+)
    ssize_t result = getrandom(buffer, size, 0);
    if (result < 0) {
        // Fallback to /dev/urandom
        FILE* urandom = fopen("/dev/urandom", "rb");
        if (!urandom) {
            return false;
        }
        
        size_t bytes_read = fread(buffer, 1, size, urandom);
        fclose(urandom);
        return bytes_read == size;
    }
    return static_cast<std::size_t>(result) == size;
    
#elif defined(__APPLE__)
    // Use Security Framework (macOS/iOS)
    int result = SecRandomCopyBytes(
        kSecRandomDefault,
        size,
        static_cast<uint8_t*>(buffer));
    
    if (result != errSecSuccess) {
        // Fallback to /dev/urandom
        FILE* urandom = fopen("/dev/urandom", "rb");
        if (!urandom) {
            return false;
        }
        
        size_t bytes_read = fread(buffer, 1, size, urandom);
        fclose(urandom);
        return bytes_read == size;
    }
    return true;
    
#else
    // Generic Unix fallback - /dev/urandom
    FILE* urandom = fopen("/dev/urandom", "rb");
    if (!urandom) {
        return false;
    }
    
    size_t bytes_read = fread(buffer, 1, size, urandom);
    fclose(urandom);
    return bytes_read == size;
#endif
}

bool lock_memory(void* ptr, std::size_t size) noexcept {
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

bool unlock_memory(void* ptr, std::size_t size) noexcept {
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

} // namespace sak
