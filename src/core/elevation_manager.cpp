// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file elevation_manager.cpp
/// @brief Implements Windows UAC elevation detection and privilege management

#include "sak/elevation_manager.h"

#include "sak/logger.h"
#include "sak/no_follow_fs.h"
#include "sak/process_runner.h"
#include "sak/program_integrity.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#include <array>
#include <string_view>

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
// How long Run as Admin keeps the program files held for the elevated copy: it verifies every one
// of them itself before it reports them held, which reads the whole program folder (a USB 2 stick
// reads about 30 MB/s).
constexpr DWORD kElevatedReadyWaitMs = 180'000;

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

}  // namespace

std::wstring ElevationManager::serializeArgsForRelaunch(int argc, wchar_t* const* argv) {
    std::wstring args;
    for (int i = 1; (argv != nullptr) && i < argc; ++i) {
        if (i > 1) {
            args += L' ';
        }
        appendQuotedArg(args, std::wstring(argv[i]));
    }
    return args;
}

std::wstring ElevationManager::relaunchArguments(const std::wstring& current_args) {
    std::wstring args = current_args;
    if (!args.empty()) {
        args += L' ';
    }
    const std::string_view flag(kElevatedRelaunchArgument);
    args.append(flag.begin(), flag.end());
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
                                 &administrators_group) != 0) {
        // A failed membership query is not an answer: fail closed (not elevated) explicitly.
        if (CheckTokenMembership(nullptr, administrators_group, &is_admin) == 0) {
            is_admin = FALSE;
        }
        FreeSid(administrators_group);
    }

    return is_admin == TRUE;
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

    if (argv == nullptr) {
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

auto ElevationManager::restartElevated() -> std::expected<void, sak::error_code> {
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
    // Security brief 4c/4d: every program file passes program_integrity's policy and stays held
    // until the elevated copy holds them itself, so nothing planted or swapped on the stick in
    // between runs as administrator.
    auto hold = program_integrity::ProgramHold::verify(QCoreApplication::applicationDirPath());
    if (!hold) {
        program_integrity::reportRefusal(hold.error());
        return std::unexpected(hold.error().file.isEmpty()
                                   ? sak::error_code::program_check_failed
                                   : sak::error_code::untrusted_program_file);
    }
    const std::wstring ready_name =
        (QStringLiteral("Local\\SAK-ElevatedReady-") + no_follow::randomToken()).toStdWString();
    const no_follow::UniqueHandle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    if (!ready.valid()) {
        sak::logError("Failed to create the elevated handshake event: {}", GetLastError());
        return std::unexpected(sak::error_code::elevation_failed);
    }

    // The relaunch is told it is one: it waits for this instance to exit instead of bringing
    // it forward as a second launch would (SingleInstanceGuard, owner request 2026-10-31).
    const std::string_view ready_flag(kElevatedReadyArgument);
    const std::wstring args = relaunchArguments(args_result.value()) + L' ' +
                              std::wstring(ready_flag.begin(), ready_flag.end()) + ready_name;

    sak::logInfo("Restarting with elevation: {}",
                 QString::fromStdWString(exe_path_result.value()).toStdString());

    const auto launched = launchElevated(exe_path_result.value(), args, SW_NORMAL);
    if (!launched) {
        return std::unexpected(launched.error());
    }
    const no_follow::UniqueHandle process(*launched);
    // Hold the program files until the elevated copy holds them too (it sets the event), or it
    // has exited, or the bound runs out; then this instance quits and lets go.
    const std::array<HANDLE, 2> waits = {ready.get(), process.get()};
    const DWORD count = process.valid() ? 2 : 1;
    if (WaitForMultipleObjects(count, waits.data(), FALSE, kElevatedReadyWaitMs) == WAIT_TIMEOUT) {
        sak::logWarning("The elevated copy did not report its program files held in time");
    }
    return {};
}

sak::error_code ElevationManager::elevationLaunchError(unsigned long win32_error) noexcept {
    return win32_error == ERROR_CANCELLED ? sak::error_code::elevation_denied
                                          : sak::error_code::elevation_failed;
}

auto ElevationManager::launchElevated(const std::wstring& executable,
                                      const std::wstring& arguments,
                                      int show_command) -> std::expected<void*, sak::error_code> {
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(SHELLEXECUTEINFOW);
    sei.lpVerb = L"runas";  // Request elevation
    sei.lpFile = executable.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.nShow = show_command;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    // Security brief 4c: an elevated process starts in System32, never in a folder another
    // account may write to (the stick, Downloads): its current directory is searched for DLLs
    // and, by cmd.exe and CreateProcess, for programs.
    const QString cmd = system32Path(QStringLiteral("cmd.exe"));
    const std::wstring directory =
        QDir::toNativeSeparators(QFileInfo(cmd).absolutePath()).toStdWString();
    sei.lpDirectory = cmd.isEmpty() ? nullptr : directory.c_str();

    if (ShellExecuteExW(&sei) == 0) {
        DWORD const error = GetLastError();
        const sak::error_code mapped = elevationLaunchError(error);
        if (mapped == sak::error_code::elevation_denied) {
            sak::logInfo("User declined the UAC prompt");
        } else {
            sak::logError("Failed to launch with elevation: {}", getElevationErrorMessage(error));
        }
        return std::unexpected(mapped);
    }
    sak::logInfo("Launched elevated process");
    return sei.hProcess;
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
    if (size > 0 && (message_buffer != nullptr)) {
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
