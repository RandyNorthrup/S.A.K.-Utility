// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file deadline_canceller.h
/// @brief Wall-clock deadline that cancels an otherwise-unbounded synchronous engine call.

#pragma once

#include <atomic>
#include <chrono>
#include <exception>
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
    /// Outcome of invoking the cancellation callback after the deadline wins. Kept separate from
    /// fired(): the deadline still elapsed even when the callback itself failed.
    enum class CallbackFailure {
        None,
        MissingCallback,
        StandardException,
        UnknownException
    };

    DeadlineCanceller(std::function<void()> on_deadline, int timeout_ms)
        : m_monitor([this, on_deadline = std::move(on_deadline), timeout_ms](std::stop_token st) {
            const auto deadline = std::chrono::steady_clock::now() +
                                  std::chrono::milliseconds(timeout_ms);
            while (!st.stop_requested() && m_state.load() == State::Running) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    State expected = State::Running;
                    if (m_state.compare_exchange_strong(expected, State::Fired)) {
                        // We won the race -> a real timeout. Nothing may escape this monitor thread
                        // into std::terminate. Record every failure class in an atomic observation
                        // seam so the owning thread can report it after finish() joins the monitor.
                        if (!on_deadline) {
                            m_callback_failure.store(CallbackFailure::MissingCallback);
                            return;
                        }
                        try {
                            on_deadline();
                        } catch (const std::exception&) {
                            m_callback_failure.store(CallbackFailure::StandardException);
                        } catch (...) {
                            // A std::function target may throw a non-standard value. This final
                            // process-safety boundary records that distinct failure; its test
                            // throws an int so removal would terminate instead of producing
                            // evidence.
                            m_callback_failure.store(CallbackFailure::UnknownException);
                        }
                    }
                    return;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
            }
        }) {}

    /// Call the instant the engine returns. Claims the "completed" outcome unless the deadline has
    /// already fired, then stops and joins the monitor thread so callbackFailure() is final.
    void finish() {
        State expected = State::Running;
        m_state.compare_exchange_strong(expected, State::Done);
        m_monitor.request_stop();
        if (m_monitor.joinable()) {
            m_monitor.join();
        }
    }

    /// True only if the deadline elapsed AND won the race against finish() (i.e. a real timeout).
    [[nodiscard]] bool fired() const noexcept { return m_state.load() == State::Fired; }

    /// Callback failure observed after a real timeout. finish() joins the monitor first, so this
    /// value is final when queried afterward. Atomic reads remain safe before finish().
    [[nodiscard]] CallbackFailure callbackFailure() const noexcept {
        return m_callback_failure.load();
    }

private:
    static constexpr int kPollMs = 50;
    enum class State {
        Running,
        Fired,
        Done
    };
    std::atomic<State> m_state{State::Running};
    std::atomic<CallbackFailure> m_callback_failure{CallbackFailure::None};
    std::jthread m_monitor;  // declared LAST -> stopped+joined FIRST on destruction
};

}  // namespace sak
