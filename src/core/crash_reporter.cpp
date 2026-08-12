// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/crash_reporter.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <format>

#if defined(_WIN32)
// clang-format off
#include <windows.h>
#include <dbghelp.h>
// clang-format on
#endif

namespace sak {

namespace {

#if defined(_WIN32)
// Set once by install() before any crash can occur, then only read by the filter.
std::wstring g_crash_dir;
std::atomic<bool> g_installed{false};

// Create @p path with CREATE_ALWAYS and write @p bytes; best-effort in a faulting
// process, so every failure is swallowed (there is nothing to fall back to).
void writeFileBestEffort(const std::wstring& path, const void* bytes, DWORD size) {
    HANDLE file = CreateFileW(
        path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written = 0;
    WriteFile(file, bytes, size, &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI sakUnhandledExceptionFilter(EXCEPTION_POINTERS* info) {
    // A fault inside the writer must not recurse into the writer again.
    static LONG in_handler = 0;
    if (InterlockedExchange(&in_handler, 1) != 0) {
        return EXCEPTION_EXECUTE_HANDLER;
    }

    SYSTEMTIME st{};
    GetLocalTime(&st);
    const DWORD pid = GetCurrentProcessId();
    const DWORD tid = GetCurrentThreadId();
    const std::wstring stem = CrashReporter::crashFileStem(pid,
                                                           {.year = st.wYear,
                                                            .month = st.wMonth,
                                                            .day = st.wDay,
                                                            .hour = st.wHour,
                                                            .minute = st.wMinute,
                                                            .second = st.wSecond});

    // Minidump.
    const std::wstring dmp_path = g_crash_dir + L"\\" + stem + L".dmp";
    HANDLE dump = CreateFileW(
        dmp_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = tid;
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), pid, dump, MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(dump);
    }

    // Human-readable summary next to the dump.
    const DWORD code = (info != nullptr && info->ExceptionRecord != nullptr)
                           ? info->ExceptionRecord->ExceptionCode
                           : 0;
    const void* addr = (info != nullptr && info->ExceptionRecord != nullptr)
                           ? info->ExceptionRecord->ExceptionAddress
                           : nullptr;
    const std::string iso = std::format("{:04d}-{:02d}-{:02d}T{:02d}:{:02d}:{:02d}",
                                        static_cast<int>(st.wYear),
                                        static_cast<int>(st.wMonth),
                                        static_cast<int>(st.wDay),
                                        static_cast<int>(st.wHour),
                                        static_cast<int>(st.wMinute),
                                        static_cast<int>(st.wSecond));
    const std::string summary = CrashReporter::formatSummary(code, addr, pid, tid, iso);
    const std::wstring txt_path = g_crash_dir + L"\\" + stem + L".txt";
    writeFileBestEffort(txt_path, summary.data(), static_cast<DWORD>(summary.size()));

    // Let the process terminate; a technician now has an artifact to send.
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif  // _WIN32

}  // namespace

bool CrashReporter::install(const std::filesystem::path& crash_dir) {
#if defined(_WIN32)
    if (g_installed.exchange(true)) {
        return false;  // already installed; do not re-register the filter
    }
    std::error_code ec;
    std::filesystem::create_directories(crash_dir, ec);
    if (ec) {
        g_installed.store(false);
        return false;  // fail closed: no writable crash dir means no reporter
    }
    g_crash_dir = crash_dir.wstring();
    SetUnhandledExceptionFilter(&sakUnhandledExceptionFilter);
    return true;
#else
    (void)crash_dir;
    return false;
#endif
}

bool CrashReporter::isInstalled() {
#if defined(_WIN32)
    return g_installed.load();
#else
    return false;
#endif
}

std::wstring CrashReporter::crashFileStem(std::uint32_t pid, const Clock& at) {
    // "sak_crash_" + up to 10 pid digits + "_YYYYMMDD_HHMMSS" fits well under this.
    constexpr std::size_t kStemBufferChars = 64;
    std::array<wchar_t, kStemBufferChars> buf{};
    std::swprintf(buf.data(),
                  buf.size(),
                  L"sak_crash_%u_%04d%02d%02d_%02d%02d%02d",
                  pid,
                  at.year,
                  at.month,
                  at.day,
                  at.hour,
                  at.minute,
                  at.second);
    return {buf.data()};
}

namespace {
// Windows SEH exception codes (from winnt.h). Named so the switch below carries no bare magic
// literals; kept as local constants because exceptionCodeName is compiled cross-platform and the
// EXCEPTION_* macros exist only under <windows.h>.
constexpr std::uint32_t kExceptionAccessViolation = 0xC0'00'00'05;
constexpr std::uint32_t kExceptionStackOverflow = 0xC0'00'00'FD;
constexpr std::uint32_t kExceptionIllegalInstruction = 0xC0'00'00'1D;
constexpr std::uint32_t kExceptionIntDivideByZero = 0xC0'00'00'94;
constexpr std::uint32_t kExceptionPrivilegedInstruction = 0xC0'00'00'96;
constexpr std::uint32_t kExceptionNoncontinuable = 0xC0'00'00'25;
constexpr std::uint32_t kExceptionBreakpoint = 0x80'00'00'03;
constexpr std::uint32_t kExceptionCppThrow = 0xE0'6D'73'63;  // MSVC C++ throw ('msc' | 0xE0000000)
}  // namespace

const char* CrashReporter::exceptionCodeName(std::uint32_t code) {
    switch (code) {
    case kExceptionAccessViolation:
        return "ACCESS_VIOLATION";
    case kExceptionStackOverflow:
        return "STACK_OVERFLOW";
    case kExceptionIllegalInstruction:
        return "ILLEGAL_INSTRUCTION";
    case kExceptionIntDivideByZero:
        return "INTEGER_DIVIDE_BY_ZERO";
    case kExceptionPrivilegedInstruction:
        return "PRIVILEGED_INSTRUCTION";
    case kExceptionNoncontinuable:
        return "NONCONTINUABLE_EXCEPTION";
    case kExceptionBreakpoint:
        return "BREAKPOINT";
    case kExceptionCppThrow:
        return "CPP_EXCEPTION";
    default:
        return "UNKNOWN";
    }
}

std::string CrashReporter::formatSummary(std::uint32_t code,
                                         const void* address,
                                         std::uint32_t pid,
                                         std::uint32_t thread_id,
                                         const std::string& iso_timestamp) {
    return std::format(
        "SAK-Utility crash report\r\n"
        "time: {}\r\n"
        "process id: {}\r\n"
        "thread id: {}\r\n"
        "exception code: 0x{:08X} ({})\r\n"
        "fault address: 0x{:016X}\r\n",
        iso_timestamp,
        pid,
        thread_id,
        code,
        CrashReporter::exceptionCodeName(code),
        reinterpret_cast<std::uintptr_t>(address));
}

}  // namespace sak
