// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

/// @file dedicated_threads.h
/// @brief Threads of the caller's own, for long CPU-bound jobs.
namespace sak {

/// Runs each job on a thread created for it alone, and joins them all.
///
/// Use this, not std::async, for CPU-bound work. MSVC's std::async runs its job on the process's
/// shared Windows thread pool, and that pool lets only one busy thread per logical CPU run at
/// once. Jobs that never wait fill it, and everything else that needs it waits until they end:
/// Qt reads a child process's output and learns that it exited through that pool, so a QProcess
/// started while the jobs spin does not return. A stress test whose clock ran such a process
/// every five seconds stopped only when the pool let a thread go, minutes late or never.
///
/// A job that throws does not end the process: its exception is kept, and joinAll() rethrows the
/// first one, as std::future::get() did.
class DedicatedThreads final {
public:
    DedicatedThreads() = default;
    /// Joins every thread. The caller tells its jobs to stop first; this waits for them.
    ~DedicatedThreads();

    DedicatedThreads(const DedicatedThreads&) = delete;
    DedicatedThreads& operator=(const DedicatedThreads&) = delete;
    DedicatedThreads(DedicatedThreads&&) = delete;
    DedicatedThreads& operator=(DedicatedThreads&&) = delete;

    /// Starts @p job on a new thread. Throws std::system_error when Windows cannot create one;
    /// the jobs already started keep running and are joined as usual.
    void launch(std::function<void()> job);

    /// Waits for every job, then rethrows the first exception a job threw, if any.
    void joinAll();

    /// Number of jobs started.
    [[nodiscard]] std::size_t size() const noexcept { return m_threads.size(); }

private:
    void joinEvery() noexcept;

    std::vector<std::thread> m_threads;
    std::mutex m_error_mutex;
    std::exception_ptr m_first_error;
};

}  // namespace sak
