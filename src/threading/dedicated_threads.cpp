// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file dedicated_threads.cpp
/// @brief Threads of the caller's own, for long CPU-bound jobs

#include "sak/dedicated_threads.h"

#include <utility>

namespace sak {

DedicatedThreads::~DedicatedThreads() {
    joinEvery();
}

void DedicatedThreads::launch(std::function<void()> job) {
    m_threads.emplace_back([this, job = std::move(job)]() {
        try {
            job();
        } catch (...) {
            const std::lock_guard lock(m_error_mutex);
            if (!m_first_error) {
                m_first_error = std::current_exception();
            }
        }
    });
}

void DedicatedThreads::joinAll() {
    joinEvery();
    std::exception_ptr error;
    {
        const std::lock_guard lock(m_error_mutex);
        error = std::exchange(m_first_error, nullptr);
    }
    if (error) {
        std::rethrow_exception(error);
    }
}

void DedicatedThreads::joinEvery() noexcept {
    for (std::thread& thread : m_threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
}

}  // namespace sak
