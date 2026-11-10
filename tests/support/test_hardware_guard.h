// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_guard.h
/// @brief What every native test binary may not reach on the machine it runs on.
///
/// CMake links test_hardware_guard.cpp into every native test target. Unless the environment
/// sets SAK_TEST_ALLOW_REAL_HARDWARE=1, which only the CI workflows do, anything in the binary,
/// product code or Qt alike, that tries to open a real drive or volume, or to start a process that
/// would query a drive, scan or repair Windows, change the network or the firewall, or run the
/// application, or to ask for elevation, is refused: the call fails with access denied and one SAK
/// TEST HARDWARE GUARD line goes to stderr. A refused open's line ends with the access mask and
/// share mode it asked for, so a run's logs show whether anything asked to write.
/// To the code it looks like a PC where that is not allowed, which it has to handle anyway, so a
/// panel that lists drives as it starts carries on with none. A developer's PC has drives on it
/// that a test has no business touching.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sak::test {

/// Called with each function a module imports by name, and the import table slot holding its
/// address. Windows-only test plumbing, shared with what else stands in for an imported call.
using ImportVisitor = void (*)(const char* name, std::uintptr_t* slot);

/// Walks @p module's import table, an HMODULE. Does nothing for a null module.
void visitImports(const void* module, ImportVisitor visit);

/// Writes @p replacement into an import table slot, which Windows maps read-only.
void setImportSlot(std::uintptr_t* slot, std::uintptr_t replacement);

/// True when the machine has said a test may reach real hardware: a CI runner, not a person's PC.
[[nodiscard]] bool realHardwareAllowed();

/// What @p path would open that the guard protects, or empty when it is an ordinary file.
[[nodiscard]] std::wstring blockedDevicePath(std::wstring_view path);

/// True when opening @p path with @p access would let a test write to the volume Windows runs from.
/// A query of that volume is left alone, because the application's own safety checks make one.
[[nodiscard]] bool writesToSystemVolume(std::wstring_view path, unsigned long access);

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
