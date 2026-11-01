// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file aligned_buffer.h
/// @brief RAII wrappers for aligned and virtual memory allocation
///
/// Provides Win32 memory allocation for performance-sensitive code paths
/// (disk I/O with FILE_FLAG_NO_BUFFERING, memory bandwidth tests).

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdlib>

#include <Windows.h>

namespace sak {

constexpr size_t kDefaultDirectIoAlignmentBytes = 4096;

/// @brief Sector-aligned buffer for direct I/O
///
/// Uses _aligned_malloc / _aligned_free.
/// Required for FILE_FLAG_NO_BUFFERING disk operations.
class AlignedBuffer {
public:
    explicit AlignedBuffer(size_t size, size_t alignment = kDefaultDirectIoAlignmentBytes)
        : m_size(size) {
        m_data = static_cast<uint8_t*>(_aligned_malloc(size, alignment));
    }

    ~AlignedBuffer() {
        if (m_data) {
            _aligned_free(m_data);
        }
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
/// Uses VirtualAlloc(MEM_COMMIT | MEM_RESERVE). Intended for memory
/// bandwidth benchmarks where large contiguous committed regions are needed.
class VirtualBuffer {
public:
    explicit VirtualBuffer(size_t size) : m_size(size) {
        m_data = VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    }

    ~VirtualBuffer() {
        if (m_data) {
            VirtualFree(m_data, 0, MEM_RELEASE);
        }
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
