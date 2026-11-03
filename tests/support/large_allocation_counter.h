// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file large_allocation_counter.h
/// @brief Counts what Qt allocates above a size, so a test can show a loop does not allocate.
///
/// A timing test of this would be flaky, and a large allocation costs little until it is touched.
/// Counting is exact. It stands in for malloc and realloc in Qt6Core's import table, the same way
/// test_hardware_guard.cpp stands in for CreateFileW, and puts them back when it goes out of scope.
/// Only one may exist at a time.

#pragma once

#include "test_hardware_guard.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include <windows.h>

namespace sak::test {

inline constexpr std::size_t kLargeAllocationBytes = 1024 * 1024;
/// More than a file's one buffer, far fewer than the lines a test reads.
inline constexpr int kFewLargeAllocations = 4;

class LargeAllocationCounter {
public:
    LargeAllocationCounter() {
        s_count = 0;
        forEachQtModule(install);
    }
    ~LargeAllocationCounter() { forEachQtModule(remove); }

    LargeAllocationCounter(const LargeAllocationCounter&) = delete;
    LargeAllocationCounter& operator=(const LargeAllocationCounter&) = delete;
    LargeAllocationCounter(LargeAllocationCounter&&) = delete;
    LargeAllocationCounter& operator=(LargeAllocationCounter&&) = delete;

    [[nodiscard]] static int count() { return s_count.load(); }

private:
    using Malloc = void* (*)(std::size_t);
    using Realloc = void* (*)(void*, std::size_t);

    static void note(std::size_t size) {
        if (size >= kLargeAllocationBytes) {
            s_count.fetch_add(1);
        }
    }
    static void* countingMalloc(std::size_t size) {
        note(size);
        return s_malloc(size);
    }
    static void* countingRealloc(void* block, std::size_t size) {
        note(size);
        return s_realloc(block, size);
    }
    static void install(const char* name, std::uintptr_t* slot) {
        if (std::strcmp(name, "malloc") == 0) {
            s_malloc = reinterpret_cast<Malloc>(*slot);
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(&countingMalloc));
        } else if (std::strcmp(name, "realloc") == 0) {
            s_realloc = reinterpret_cast<Realloc>(*slot);
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(&countingRealloc));
        }
    }
    static void remove(const char* name, std::uintptr_t* slot) {
        if (std::strcmp(name, "malloc") == 0) {
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(s_malloc));
        } else if (std::strcmp(name, "realloc") == 0) {
            setImportSlot(slot, reinterpret_cast<std::uintptr_t>(s_realloc));
        }
    }
    static void forEachQtModule(ImportVisitor visit) {
        visitImports(GetModuleHandleW(L"Qt6Core.dll"), visit);
        visitImports(GetModuleHandleW(L"Qt6Cored.dll"), visit);
    }

    static inline std::atomic<int> s_count{0};
    static inline Malloc s_malloc = nullptr;
    static inline Realloc s_realloc = nullptr;
};

}  // namespace sak::test
