// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file thread_pool_probe.h
/// @brief Helpers for tests that check CPU-bound work leaves Windows' shared thread pool free.
///
/// MSVC's std::async runs its job on the process's shared Windows thread pool, which lets only
/// one busy thread per logical CPU run at once. Spinning jobs there fill it, and Qt -- which
/// reads a child process's output and learns that it exited through that pool -- then cannot
/// finish a QProcess. These helpers keep such a test light on the machine running it: the
/// process is held to two cores while it spins, and the probe waits a bounded time.

#include <QElapsedTimer>

#include <Windows.h>

namespace sak::test {

/// Holds this test process to two logical CPUs for its lifetime. A test that has to spin one
/// thread per CPU then loads two cores, not the whole machine; the thread pool still counts every
/// spinning thread as busy, so what it measures does not change.
class TwoCoreAffinity {
public:
    TwoCoreAffinity() {
        DWORD_PTR system_mask = 0;
        if (GetProcessAffinityMask(GetCurrentProcess(), &m_saved, &system_mask) == 0) {
            m_saved = 0;
            return;
        }
        DWORD_PTR two = 0;
        int taken = 0;
        for (DWORD_PTR bit = 1; bit != 0 && taken < 2; bit <<= 1) {
            if ((m_saved & bit) != 0) {
                two |= bit;
                ++taken;
            }
        }
        if (two != 0) {
            SetProcessAffinityMask(GetCurrentProcess(), two);
        }
    }
    ~TwoCoreAffinity() {
        if (m_saved != 0) {
            SetProcessAffinityMask(GetCurrentProcess(), m_saved);
        }
    }
    TwoCoreAffinity(const TwoCoreAffinity&) = delete;
    TwoCoreAffinity& operator=(const TwoCoreAffinity&) = delete;
    TwoCoreAffinity(TwoCoreAffinity&&) = delete;
    TwoCoreAffinity& operator=(TwoCoreAffinity&&) = delete;

private:
    DWORD_PTR m_saved{0};
};

/// Milliseconds until a job handed to the process's shared thread pool starts, or -1 when it has
/// not started within @p timeout_ms. When it has not, the job is still queued and will set its
/// event whenever the pool gets to it, so that one event handle is left open for it rather than
/// closed underneath it: a failing test leaks one handle, never a handle the job then misuses.
inline qint64 threadPoolStartLatencyMs(int timeout_ms) {
    HANDLE started = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (started == nullptr) {
        return -1;
    }
    QElapsedTimer timer;
    timer.start();
    const auto signal_started = [](PTP_CALLBACK_INSTANCE, PVOID context) {
        SetEvent(static_cast<HANDLE>(context));
    };
    if (TrySubmitThreadpoolCallback(signal_started, started, nullptr) == 0) {
        CloseHandle(started);
        return -1;
    }
    if (WaitForSingleObject(started, static_cast<DWORD>(timeout_ms)) != WAIT_OBJECT_0) {
        return -1;  // still queued: the handle stays open for it (see above)
    }
    const qint64 latency = timer.elapsed();
    CloseHandle(started);
    return latency;
}

}  // namespace sak::test
