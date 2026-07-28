// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file deadline_canceller.h
/// @brief Wall-clock deadline that cancels an otherwise-unbounded synchronous engine call.

#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <stop_token>
#include <thread>
#include <utility>

namespace sak {

/// Bounds an otherwise-unbounded synchronous engine call (a walk/scan/hash over a huge tree or
/// pathological input) with a wall-clock deadline. A background jthread invokes @p on_deadline once
/// timeout_ms elapses -- the callback does whatever cancels the engine (request_stop() on a
/// std::stop_source, or a worker's own cancel()). Call finish() the instant the engine returns so
/// the monitor dies promptly, then read fired() for an honest "timed out / partial" flag.
///
/// finish() and the deadline race on a SINGLE atomic state, so exactly one outcome wins: if the
/// engine completes at (or fractionally after) the deadline instant, finish() claims Done and the
/// monitor's compare-exchange fails, so fired() stays false and on_deadline never runs -- a run
/// that genuinely completed is never reported as a timeout (the older design set the fired flag
/// unconditionally on the deadline, over-reporting a just-completed run as partial).
///
/// The jthread is the LAST member, so on destruction it is stopped+joined FIRST -- before m_state
/// (which it reads) and before the caller's engine/stop_source go out of scope. Not thread-safe for
/// concurrent finish() calls; use from a single owning thread.
class DeadlineCanceller {
public:
    DeadlineCanceller(std::function<void()> on_deadline, int timeout_ms)
        : m_monitor([this, on_deadline = std::move(on_deadline), timeout_ms](std::stop_token st) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            while (!st.stop_requested() && m_state.load() == State::Running) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    State expected = State::Running;
                    if (m_state.compare_exchange_strong(expected, State::Fired)) {
                        on_deadline();  // we won the race -> a real timeout
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
        }) {}

    /// Call the instant the engine returns. Claims the "completed" outcome unless the deadline has
    /// already fired, then stops the monitor thread.
    void finish() {
        State expected = State::Running;
        m_state.compare_exchange_strong(expected, State::Done);
        m_monitor.request_stop();
    }

    /// True only if the deadline elapsed AND won the race against finish() (i.e. a real timeout).
    [[nodiscard]] bool fired() const { return m_state.load() == State::Fired; }

private:
    static constexpr int kPollMs = 50;
    enum class State {
        Running,
        Fired,
        Done
    };
    std::atomic<State> m_state{State::Running};
    std::jthread m_monitor;  // declared LAST -> stopped+joined FIRST on destruction
};

}  // namespace sak
