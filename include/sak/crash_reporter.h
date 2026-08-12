// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file crash_reporter.h
/// @brief Process-wide unhandled-exception crash reporter (minidump + summary).

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace sak {

/// @brief Writes a crash artifact when the process dies of an unhandled Windows
/// structured exception.
///
/// An access violation, stack overflow, or similar SEH fault is invisible to a
/// C++ try/catch, so before this the application simply vanished on a technician's
/// machine (R5-G23-2). Once installed, an unhandled exception writes a minidump
/// (.dmp) plus a human-readable summary (.txt) into a crash directory, then lets
/// the process terminate. Windows-only; install() is a no-op elsewhere.
///
/// The filter itself lives in the .cpp and touches only the pure helpers below;
/// those carry no global state and are unit-tested directly (the dump write
/// itself needs a real crash and is covered by manual/soak testing).
class CrashReporter {
public:
    /// Install the unhandled-exception filter, directing artifacts to @p crash_dir
    /// (created if absent). Idempotent: a second call is a no-op returning false.
    /// Returns false if the directory cannot be created. Call once, early in
    /// startup, after the crash directory's parent is known.
    static bool install(const std::filesystem::path& crash_dir);

    /// Whether install() has succeeded in this process.
    [[nodiscard]] static bool isInstalled();

    // --- Pure helpers (no global state; unit-tested) -------------------------

    /// Broken-down local time for the artifact name (a plain value, so the pure
    /// naming helper does not depend on any OS time type).
    struct Clock {
        int year{};
        int month{};
        int day{};
        int hour{};
        int minute{};
        int second{};
    };

    /// Deterministic artifact basename (no extension) for a crash at @p pid and
    /// @p at. Format: sak_crash_<pid>_<YYYYMMDD_HHMMSS>.
    [[nodiscard]] static std::wstring crashFileStem(std::uint32_t pid, const Clock& at);

    /// Short symbolic name for a Windows SEH exception code (e.g. 0xC0000005 ->
    /// "ACCESS_VIOLATION"); "UNKNOWN" for an unrecognized code.
    [[nodiscard]] static const char* exceptionCodeName(std::uint32_t code);

    /// Body of the .txt crash summary, assembled from the captured fields.
    [[nodiscard]] static std::string formatSummary(std::uint32_t code,
                                                   const void* address,
                                                   std::uint32_t pid,
                                                   std::uint32_t thread_id,
                                                   const std::string& iso_timestamp);
};

}  // namespace sak
