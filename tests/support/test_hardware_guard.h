// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_guard.h
/// @brief What every native test binary may not reach on the machine it runs on.
///
/// CMake links test_hardware_guard.cpp into every native test target. Unless the environment
/// sets SAK_TEST_ALLOW_REAL_HARDWARE=1, which only the CI workflows do, anything in the binary,
/// product code or Qt alike, that tries to open a real drive or volume, or to start a process that
/// would query a drive, scan or repair Windows, change the network, or run the application, or to
/// ask for elevation, is refused: the call fails with access denied and one SAK TEST HARDWARE GUARD
/// line goes to stderr.
/// To the code it looks like a PC where that is not allowed, which it has to handle anyway, so a
/// panel that lists drives as it starts carries on with none. A developer's PC has drives on it
/// that a test has no business touching.

#pragma once

#include <string>
#include <string_view>

namespace sak::test {

/// True when the machine has said a test may reach real hardware: a CI runner, not a person's PC.
[[nodiscard]] bool realHardwareAllowed();

/// What @p path would open that the guard protects, or empty when it is an ordinary file.
[[nodiscard]] std::wstring blockedDevicePath(std::wstring_view path);

/// What @p command_line would reach that the guard protects, or empty when it is harmless.
[[nodiscard]] std::wstring blockedCommand(std::wstring_view command_line);

}  // namespace sak::test

/// The first line of a test that needs the real answer and has nothing else to prove it with. It
/// runs in CI, which has no drive anyone cares about, and skips everywhere else. Include
/// <QtTest/QtTest> first.
#define SAK_SKIP_UNLESS_REAL_HARDWARE_ALLOWED()                                 \
    do {                                                                        \
        if (!sak::test::realHardwareAllowed()) {                                \
            QSKIP("Reaches this PC's real drives or the application: CI only"); \
        }                                                                       \
    } while (false)
