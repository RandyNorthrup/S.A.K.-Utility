// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/error_codes.h"

#include <expected>
#include <string>

namespace sak {

/**
 * @brief Windows UAC elevation helper
 *
 * Provides utilities for checking and requesting administrator privileges
 * on Windows systems using UAC (User Account Control).
 * Uses wide-char (W) Win32 APIs internally to support Unicode paths.
 *
 * Thread-Safety: Can be used from any thread
 */
class ElevationManager {
public:
    /**
     * @brief Check if current process is running as administrator
     * @return True if running with admin privileges
     */
    [[nodiscard]] static bool isElevated() noexcept;

    /**
     * @brief Start this executable again with administrator privileges (the caller then quits)
     * @return Success or error code; a declined UAC prompt is elevation_denied
     */
    static auto restartElevated() -> std::expected<void, sak::error_code>;

    /// @brief The one UAC request: ShellExecuteExW "runas" of @p executable with @p arguments
    ///        (already quoted), shown with @p show_command (SW_*). Returns the process handle,
    ///        which the caller owns and closes. Used by restartElevated and by ElevationBroker to
    ///        start sak_elevated_helper.exe.
    static auto launchElevated(const std::wstring& executable,
                               const std::wstring& arguments,
                               int show_command) -> std::expected<void*, sak::error_code>;

    /// @brief The result for a failed elevated launch's Win32 error: ERROR_CANCELLED (the user
    ///        declined the UAC prompt) is elevation_denied, anything else elevation_failed. Pure.
    [[nodiscard]] static sak::error_code elevationLaunchError(unsigned long win32_error) noexcept;

    /// The argument an elevated relaunch of the application carries. The new instance waits for
    /// the one that launched it to exit (sak::SingleInstanceGuard) instead of bringing that
    /// closing window forward and quitting as a second launch would.
    static constexpr const char* kElevatedRelaunchArgument = "--elevated-relaunch";

    /// @brief The arguments an elevated relaunch is started with: this process's own arguments
    ///        (@p current_args, already serialized) followed by kElevatedRelaunchArgument. Pure.
    [[nodiscard]] static std::wstring relaunchArguments(const std::wstring& current_args);

    /// @brief Whether a failed restartElevated() deserves an error dialog. Declining the UAC
    ///        prompt (ERROR_CANCELLED -> elevation_denied) is the user's answer, not a
    ///        failure: it used to raise "Elevation Failed".
    [[nodiscard]] static bool restartFailureNeedsNotice(sak::error_code error) noexcept {
        // A refused program file was already shown, naming the file (program_integrity).
        return error != sak::error_code::elevation_denied &&
               error != sak::error_code::untrusted_program_file;
    }

    /// The argument that names the event an elevated relaunch sets once it holds its own program
    /// files, so the instance that launched it can let go of its hold (security brief 4c).
    static constexpr const char* kElevatedReadyArgument = "--elevated-ready=";

    /**
     * @brief Get elevation error message
     * @param error_code Windows error code
     * @return Human-readable error message
     */
    [[nodiscard]] static std::string getElevationErrorMessage(unsigned long error_code);

private:
    /**
     * @brief Get current executable path (wide-char for Unicode support)
     * @return Executable path or error
     */
    [[nodiscard]] static auto get_executable_path() -> std::expected<std::wstring, sak::error_code>;

    /**
     * @brief Get command line arguments (wide-char for Unicode support)
     * @return Serialized arguments (empty when there are legitimately none), or an error when
     *         the command line could not be parsed -- so a genuine parse failure fails closed
     *         instead of relaunching elevated with the arguments silently dropped.
     */
    [[nodiscard]] static auto get_command_line_args()
        -> std::expected<std::wstring, sak::error_code>;

    /**
     * @brief Serialize argv[1..argc-1] into one command-line string using the
     *        CommandLineToArgvW quoting rules (exposed for testing).
     * @param argc Argument count as returned by CommandLineToArgvW
     * @param argv Argument vector as returned by CommandLineToArgvW
     * @return Round-trip-safe command line (argv[0] excluded)
     */
    [[nodiscard]] static std::wstring serializeArgsForRelaunch(int argc, wchar_t* const* argv);
};

}  // namespace sak
