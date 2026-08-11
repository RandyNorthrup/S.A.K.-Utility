// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevation_manager.cpp
/// @brief Implements Windows UAC elevation detection and privilege management

#include "sak/elevation_manager.h"

#ifdef _WIN32

#include "sak/logger.h"

#include <windows.h>

#include <sddl.h>
#include <shellapi.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots

#pragma comment(lib, "shell32.lib")

namespace sak {

namespace {
constexpr BYTE kAdministratorsSidSubAuthorityCount = 2;
constexpr DWORD kWindowsVistaMajorVersion = 6;

// Append one argument using the exact quoting rules CommandLineToArgvW reverses, so the
// elevated relaunch receives byte-identical arguments. Backslashes that precede a quote (or
// the closing quote) must be doubled; otherwise a trailing backslash escapes the closing quote
// and the argument is corrupted or the next argument is absorbed. An empty argument becomes an
// explicit empty quoted token instead of vanishing.
void appendQuotedArg(std::wstring& out, const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        out += arg;
        return;
    }
    out += L'\"';
    for (auto it = arg.begin();; ++it) {
        size_t slashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++slashes;
        }
        if (it == arg.end()) {
            out.append(slashes + slashes, L'\\');
            break;
        }
        if (*it == L'\"') {
            out.append(slashes + slashes + 1, L'\\');
        } else {
            out.append(slashes, L'\\');
        }
        out += *it;
    }
    out += L'\"';
}

// Wait for an elevated child to finish and map its outcome to a result. Fails closed on a
// missing handle, a failed wait, an unreadable exit code, or a non-zero exit code, so a caller
// that asked to wait is never told an operation succeeded that we could not actually observe.
std::expected<void, sak::error_code> waitForElevatedExit(HANDLE process) {
    if (!process) {
        sak::logError("Elevated launch returned no process handle; cannot confirm completion");
        return std::unexpected(sak::error_code::execution_failed);
    }
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) {
        sak::logError("Failed to wait for elevated process: error {}", GetLastError());
        return std::unexpected(sak::error_code::execution_failed);
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(process, &exit_code)) {
        sak::logError("Failed to read elevated process exit code: error {}", GetLastError());
        return std::unexpected(sak::error_code::execution_failed);
    }
    if (exit_code != 0) {
        sak::logError("Elevated process failed with exit code {}", exit_code);
        return std::unexpected(sak::error_code::execution_failed);
    }
    return {};
}
}  // namespace

std::wstring ElevationManager::serializeArgsForRelaunch(int argc, wchar_t* const* argv) {
    std::wstring args;
    for (int i = 1; argv && i < argc; ++i) {
        if (i > 1) {
            args += L' ';
        }
        appendQuotedArg(args, std::wstring(argv[i]));
    }
    return args;
}

bool ElevationManager::isElevated() noexcept {
    BOOL is_admin = FALSE;
    PSID administrators_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(&nt_authority,
                                 kAdministratorsSidSubAuthorityCount,
                                 SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 0,
                                 &administrators_group)) {
        CheckTokenMembership(nullptr, administrators_group, &is_admin);
        FreeSid(administrators_group);
    }

    return is_admin == TRUE;
}

bool ElevationManager::canElevate() noexcept {
    // On Windows Vista+, UAC is available
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    osvi.dwMajorVersion = kWindowsVistaMajorVersion;

    DWORDLONG condition_mask = 0;
    VER_SET_CONDITION(condition_mask, VER_MAJORVERSION, VER_GREATER_EQUAL);

    return VerifyVersionInfo(&osvi, VER_MAJORVERSION, condition_mask) != FALSE;
}

auto ElevationManager::get_executable_path() -> std::expected<std::wstring, sak::error_code> {
    wchar_t path[MAX_PATH];
    DWORD const result = GetModuleFileNameW(nullptr, path, MAX_PATH);

    if (result == 0 || result == MAX_PATH) {
        DWORD const error = GetLastError();
        sak::logError("Failed to get executable path: error {}", error);
        return std::unexpected(sak::error_code::execution_failed);
    }

    return std::wstring(path);
}

auto ElevationManager::get_command_line_args() -> std::expected<std::wstring, sak::error_code> {
    LPWSTR cmd_line = GetCommandLineW();

    // Skip the executable name (first argument)
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc);

    if (!argv) {
        // A genuine parse failure is NOT the same as "no arguments": fail closed instead of
        // relaunching elevated with whatever arguments this process was started with dropped.
        sak::logError("CommandLineToArgvW failed: error {}", GetLastError());
        return std::unexpected(sak::error_code::execution_failed);
    }

    if (argc <= 1) {
        LocalFree(argv);
        return std::wstring{};  // legitimately no arguments
    }

    std::wstring args = serializeArgsForRelaunch(argc, argv);
    LocalFree(argv);
    return args;
}

auto ElevationManager::restartElevated(bool wait_for_exit) -> std::expected<void, sak::error_code> {
    if (isElevated()) {
        sak::logInfo("Already running with administrator privileges");
        return {};
    }

    auto exe_path_result = get_executable_path();
    if (!exe_path_result) {
        return std::unexpected(exe_path_result.error());
    }

    auto args_result = get_command_line_args();
    if (!args_result) {
        return std::unexpected(args_result.error());
    }
    const std::wstring& args = args_result.value();

    // Log using narrow-string conversion for log output
    const int logLen = WideCharToMultiByte(
        CP_UTF8, 0, exe_path_result.value().c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string logPath(logLen > 0 ? static_cast<size_t>(logLen - 1) : 0, '\0');
    if (logLen > 0) {
        WideCharToMultiByte(CP_UTF8,
                            0,
                            exe_path_result.value().c_str(),
                            -1,
                            logPath.data(),
                            logLen,
                            nullptr,
                            nullptr);
    }
    sak::logInfo("Restarting with elevation: {}", logPath);

    return executeElevated(exe_path_result.value(), args, wait_for_exit);
}

auto ElevationManager::executeElevated(const std::wstring& executable,
                                       const std::wstring& arguments,
                                       bool wait_for_exit) -> std::expected<void, sak::error_code> {
    SHELLEXECUTEINFOW sei = {sizeof(SHELLEXECUTEINFOW)};
    sei.lpVerb = L"runas";  // Request elevation
    sei.lpFile = executable.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.nShow = SW_NORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExW(&sei)) {
        DWORD const error = GetLastError();

        if (error == ERROR_CANCELLED) {
            sak::logInfo("User cancelled elevation request");
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        sak::logError("Failed to execute with elevation: {}", getElevationErrorMessage(error));
        return std::unexpected(sak::error_code::elevation_failed);
    }

    sak::logInfo("Successfully launched elevated process");

    if (wait_for_exit) {
        sak::logInfo("Waiting for elevated process to complete...");
        auto wait_result = waitForElevatedExit(sei.hProcess);
        if (sei.hProcess) {
            CloseHandle(sei.hProcess);
        }
        if (!wait_result) {
            return std::unexpected(wait_result.error());
        }
        sak::logInfo("Elevated process completed successfully");
    } else if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }

    return {};
}

std::string ElevationManager::getElevationErrorMessage(unsigned long error_code) {
    char* message_buffer = nullptr;

    DWORD const size = FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                          FORMAT_MESSAGE_IGNORE_INSERTS,
                                      nullptr,
                                      error_code,
                                      MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                      reinterpret_cast<LPSTR>(&message_buffer),
                                      0,
                                      nullptr);

    std::string message;
    if (size > 0 && message_buffer) {
        message = message_buffer;
        // Remove trailing newlines
        while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
            message.pop_back();
        }
        LocalFree(message_buffer);
    } else {
        message = "Error code: " + std::to_string(error_code);
    }

    return message;
}

}  // namespace sak

#endif  // _WIN32
