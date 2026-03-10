// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file aligned_buffer.h
/// @brief RAII wrappers for aligned and virtual memory allocation
///
/// Provides platform-abstracted memory allocation for performance-sensitive
/// code paths (disk I/O with FILE_FLAG_NO_BUFFERING, memory bandwidth tests).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#ifdef SAK_PLATFORM_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace sak {

/// @brief Sector-aligned buffer for direct I/O
///
/// Uses _aligned_malloc on Windows, std::aligned_alloc elsewhere.
/// Required for FILE_FLAG_NO_BUFFERING disk operations.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t size, size_t alignment = 4096) : m_size(size) {
#ifdef SAK_PLATFORM_WINDOWS
        m_data = static_cast<uint8_t*>(_aligned_malloc(size, alignment));
#else
        m_data = static_cast<uint8_t*>(std::aligned_alloc(alignment, size));
#endif
    }

    ~AlignedBuffer() {
#ifdef SAK_PLATFORM_WINDOWS
        if (m_data) {
            _aligned_free(m_data);
        }
#else
        if (m_data) {
            std::free(m_data);
        }
#endif
    }

    AlignedBuffer(const AlignedBuffer&) = delete;
    AlignedBuffer& operator=(const AlignedBuffer&) = delete;
    AlignedBuffer(AlignedBuffer&&) = delete;
    AlignedBuffer& operator=(AlignedBuffer&&) = delete;

    [[nodiscard]] uint8_t* data() const { return m_data; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] bool valid() const { return m_data != nullptr; }

private:
    uint8_t* m_data{nullptr};
    size_t m_size{0};
};

/// @brief Large committed buffer via VirtualAlloc
///
/// Uses VirtualAlloc(MEM_COMMIT | MEM_RESERVE) on Windows, std::malloc
/// elsewhere. Intended for memory bandwidth benchmarks where large
/// contiguous committed regions are needed.
class VirtualBuffer {
public:
    explicit VirtualBuffer(size_t size) : m_size(size) {
#ifdef SAK_PLATFORM_WINDOWS
        m_data = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
        m_data = std::malloc(size);
#endif
    }

    ~VirtualBuffer() {
#ifdef SAK_PLATFORM_WINDOWS
        if (m_data) {
            VirtualFree(m_data, 0, MEM_RELEASE);
        }
#else
        if (m_data) {
            std::free(m_data);
        }
#endif
    }

    VirtualBuffer(const VirtualBuffer&) = delete;
    VirtualBuffer& operator=(const VirtualBuffer&) = delete;
    VirtualBuffer(VirtualBuffer&&) = delete;
    VirtualBuffer& operator=(VirtualBuffer&&) = delete;

    [[nodiscard]] void* data() const { return m_data; }
    [[nodiscard]] size_t size() const { return m_size; }
    [[nodiscard]] bool valid() const { return m_data != nullptr; }

    template <typename T>
    [[nodiscard]] T* as() const {
        return static_cast<T*>(m_data);
    }

private:
    void* m_data{nullptr};
    size_t m_size{0};
};

}  // namespace sak
