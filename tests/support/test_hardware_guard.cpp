// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file test_hardware_guard.cpp
/// @brief Stops a test binary touching a real drive. See test_hardware_guard.h.
///
/// It works on the import tables of the test executable and Qt6Core, because everything that
/// reaches a drive from a test goes through one of the Win32 calls below: the product libraries
/// are static, so their calls import from the executable, and QFile and QProcess import from Qt.
/// Nothing here is compiled into the application.

#include "test_hardware_guard.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwctype>

#include <windows.h>

#include <crtdbg.h>
#include <shellapi.h>

namespace {

// A drive number at or above this cannot exist on any PC, and the flash suites use such numbers
// to prove the "no such drive" path. Below it, the number may be a real drive.
constexpr int kLowestAbsentDriveNumber = 32;
constexpr int kDecimalBase = 10;
constexpr int kMostOfACommandToPrint = 300;
// That much path, " access=0x00000000 share=0x00000000", and the terminator.
constexpr int kMostOfADetail = kMostOfACommandToPrint + 40;

// Text whose presence on a command line means the process would query a drive, scan or repair
// this Windows install, or change the network. Lower case; matched anywhere.
constexpr auto kBlockedCommandText = std::to_array<std::wstring_view>({
    L"smartctl",
    L"get-physicaldisk",
    L"get-disk",
    L"get-storagereliabilitycounter",
    L"get-partition",
    L"get-volume",
    L"repair-volume",
    L"optimize-volume",
    L"format-volume",
    L"initialize-disk",
    L"clear-disk",
    L"set-disk",
    L"new-partition",
    L"remove-partition",
    L"resize-partition",
    L"mount-diskimage",
    L"mount-vhd",
    L"win32_diskdrive",
    L"win32_diskpartition",
    L"win32_logicaldisktopartition",
    L"win32_volume",
    L"msft_disk",
    L"msft_physicaldisk",
    L"msft_partition",
    L"msft_volume",
    L"diskpart",
    L"chkdsk",
    L"fsutil",
    L"manage-bde",
    L"get-bitlockervolume",
    L"mountvol",
    L"physicaldrive",
    L"sak_utility.exe",
    L"clear-dnsclientcache",
    L"/flushdns",
    L"set-dnsclientserveraddress",
    L"netsh interface ip set",
    L"advfirewall firewall add",
    L"advfirewall firewall delete",
    L"advfirewall firewall set",
});

// The same, for short names that are only a command when they stand alone.
constexpr auto kBlockedCommandWords =
    std::to_array<std::wstring_view>({L"sfc", L"dism", L"defrag"});

// Device namespaces other than PhysicalDriveN and drive letters, which have their own rules.
constexpr auto kBlockedDeviceText = std::to_array<std::wstring_view>({
    L"volume{",
    L"harddisk",
    L"globalroot",
    L"cdrom",
    L"scsi",
});

[[nodiscard]] std::wstring lowered(std::wstring_view text) {
    std::wstring result(text);
    for (wchar_t& ch : result) {
        ch = static_cast<wchar_t>(std::towlower(ch));
    }
    return result;
}

[[nodiscard]] std::wstring normalizedPath(std::wstring_view path) {
    std::wstring result = lowered(path);
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
}

[[nodiscard]] bool isWordChar(wchar_t ch) {
    return std::iswalnum(ch) != 0 || ch == L'_' || ch == L'-';
}

[[nodiscard]] bool containsWord(std::wstring_view text, std::wstring_view word) {
    for (size_t at = text.find(word); at != std::wstring_view::npos; at = text.find(word, at + 1)) {
        const bool starts = at == 0 || !isWordChar(text[at - 1]);
        const size_t end = at + word.size();
        const bool ends = end == text.size() || !isWordChar(text[end]);
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] std::wstring deviceNamespaceTail(std::wstring_view lowered_path) {
    constexpr std::wstring_view kDot = L"\\\\.\\";
    constexpr std::wstring_view kQuestion = L"\\\\?\\";
    if (lowered_path.starts_with(kDot) || lowered_path.starts_with(kQuestion)) {
        return std::wstring(lowered_path.substr(kDot.size()));
    }
    return {};
}

[[nodiscard]] bool physicalDriveMayExist(std::wstring_view tail) {
    constexpr std::wstring_view kPrefix = L"physicaldrive";
    int number = 0;
    for (const wchar_t ch : tail.substr(kPrefix.size())) {
        if (std::iswdigit(ch) == 0 || number >= kLowestAbsentDriveNumber) {
            break;
        }
        number = number * kDecimalBase + (ch - L'0');
    }
    return number < kLowestAbsentDriveNumber;
}

[[nodiscard]] bool isOsDriveLetter(wchar_t letter) {
    std::array<wchar_t, 4> system_drive{};
    const DWORD length = GetEnvironmentVariableW(L"SystemDrive",
                                                 system_drive.data(),
                                                 static_cast<DWORD>(system_drive.size()));
    return length == 2 && std::towlower(system_drive[0]) == letter;
}

// True for "x:" and "x:\", the volume itself. "\\.\x:\folder\file" is an ordinary path.
[[nodiscard]] bool isVolumeAlias(std::wstring_view tail) {
    return (tail.size() == 2 || (tail.size() == 3 && tail[2] == L'\\')) && tail[1] == L':' &&
           std::iswalpha(tail[0]) != 0;
}

[[nodiscard]] bool isForeignVolumeAlias(std::wstring_view tail) {
    return isVolumeAlias(tail) && !isOsDriveLetter(tail[0]);
}

// One line per attempt, so a run's logs say exactly what its tests tried to reach.
void reportRefusal(const wchar_t* what, std::wstring_view detail) {
    std::fwprintf(stderr,
                  L"SAK TEST HARDWARE GUARD: refused to %ls: %.*ls\n",
                  what,
                  std::min(static_cast<int>(detail.size()), kMostOfADetail),
                  detail.data());
    std::fflush(stderr);
}

// What was asked for matters as much as where: access 0 only queries a drive, GENERIC_WRITE could
// change it. The numbers are the raw dwDesiredAccess and dwShareMode, so a log can be counted.
[[nodiscard]] bool refusesOpen(const wchar_t* path, DWORD access, DWORD share) {
    if (path == nullptr || (sak::test::blockedDevicePath(path).empty() &&
                            !sak::test::writesToSystemVolume(path, access))) {
        return false;
    }
    std::array<wchar_t, kMostOfADetail> detail{};
    const int written = std::swprintf(detail.data(),
                                      detail.size(),
                                      L"%.*ls access=0x%08lX share=0x%08lX",
                                      kMostOfACommandToPrint,
                                      path,
                                      access,
                                      share);
    reportRefusal(L"open a real drive or volume",
                  std::wstring_view(detail.data(), static_cast<size_t>(std::max(written, 0))));
    return true;
}

[[nodiscard]] bool refusesCommand(const wchar_t* application, const wchar_t* command_line) {
    for (const wchar_t* text : {application, command_line}) {
        if (text != nullptr && !sak::test::blockedCommand(text).empty()) {
            reportRefusal(L"start a process that reaches real hardware", text);
            return true;
        }
    }
    return false;
}

// A request for elevation is how the application reaches its helper, which reads raw disks. It also
// puts a prompt on the desktop of whoever is running the tests.
[[nodiscard]] bool refusesShellExecute(const SHELLEXECUTEINFOW* info) {
    if (info->lpVerb != nullptr && lowered(info->lpVerb) == L"runas") {
        reportRefusal(L"ask for elevation", info->lpFile != nullptr ? info->lpFile : L"");
        return true;
    }
    return refusesCommand(info->lpFile, info->lpParameters);
}

// What the import table held before the guard replaced it.
decltype(&CreateFileW) g_create_file_w = nullptr;
decltype(&CreateProcessW) g_create_process_w = nullptr;
decltype(&ShellExecuteExW) g_shell_execute_ex_w = nullptr;

// The rest of each signature is Windows' own and passes straight through, so it is deduced from the
// function the guard stands in for.
template <typename... Rest>
HANDLE WINAPI guardedCreateFileW(LPCWSTR name, DWORD access, DWORD share, Rest... rest) {
    if (refusesOpen(name, access, share)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return INVALID_HANDLE_VALUE;
    }
    return g_create_file_w(name, access, share, rest...);
}

template <typename... Rest>
BOOL WINAPI guardedCreateProcessW(LPCWSTR application, LPWSTR command_line, Rest... rest) {
    if (refusesCommand(application, command_line)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return g_create_process_w(application, command_line, rest...);
}

BOOL WINAPI guardedShellExecuteExW(SHELLEXECUTEINFOW* info) {
    if (refusesShellExecute(info)) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return g_shell_execute_ex_w(info);
}

void patchImport(const char* name, std::uintptr_t* slot) {
    if (std::strcmp(name, "CreateFileW") == 0) {
        g_create_file_w = reinterpret_cast<decltype(&CreateFileW)>(*slot);
        const decltype(&CreateFileW) guarded = guardedCreateFileW;
        sak::test::setImportSlot(slot, reinterpret_cast<std::uintptr_t>(guarded));
    } else if (std::strcmp(name, "CreateProcessW") == 0) {
        g_create_process_w = reinterpret_cast<decltype(&CreateProcessW)>(*slot);
        const decltype(&CreateProcessW) guarded = guardedCreateProcessW;
        sak::test::setImportSlot(slot, reinterpret_cast<std::uintptr_t>(guarded));
    } else if (std::strcmp(name, "ShellExecuteExW") == 0) {
        g_shell_execute_ex_w = reinterpret_cast<decltype(&ShellExecuteExW)>(*slot);
        sak::test::setImportSlot(slot, reinterpret_cast<std::uintptr_t>(&guardedShellExecuteExW));
    }
}

void visitImportsFrom(const char* base,
                      const IMAGE_IMPORT_DESCRIPTOR* descriptor,
                      sak::test::ImportVisitor visit) {
    if (descriptor->OriginalFirstThunk == 0) {
        return;
    }
    const auto* names =
        reinterpret_cast<const IMAGE_THUNK_DATA*>(base + descriptor->OriginalFirstThunk);
    auto* slots =
        reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<char*>(base) + descriptor->FirstThunk);
    for (; names->u1.AddressOfData != 0; ++names, ++slots) {
        if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) {
            continue;
        }
        const auto* by_name =
            reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
        visit(by_name->Name, reinterpret_cast<std::uintptr_t*>(&slots->u1.Function));
    }
}

// A crash box or an assert box waits for someone to close it, on a person's screen or on a runner
// with nobody there, and the test sits on it until it times out. A binary that cannot load at all
// never gets this far: that one is scripts/run_ctest_no_dialogs.ps1's.
void sendBoxesToTheLog() {
    SetErrorMode(GetErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                 SEM_NOOPENFILEERRORBOX);
#ifdef _DEBUG
    for (const int kind : {_CRT_ASSERT, _CRT_ERROR}) {
        _CrtSetReportMode(kind, _CRTDBG_MODE_FILE);
        _CrtSetReportFile(kind, _CRTDBG_FILE_STDERR);
    }
#endif
}

bool installGuard() {
    sendBoxesToTheLog();
    if (!sak::test::realHardwareAllowed()) {
        sak::test::visitImports(GetModuleHandleW(nullptr), patchImport);
        sak::test::visitImports(GetModuleHandleW(L"Qt6Core.dll"), patchImport);
        sak::test::visitImports(GetModuleHandleW(L"Qt6Cored.dll"), patchImport);
    }
    return true;
}

// Runs before main, so nothing in the binary can reach a drive ahead of it.
[[maybe_unused]] const bool g_guard_installed = installGuard();

}  // namespace

namespace sak::test {

void visitImports(const void* module, ImportVisitor visit) {
    if (module == nullptr) {
        return;
    }
    const auto* base = static_cast<const char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& imports =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress == 0) {
        return;
    }
    const auto* descriptor =
        reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        visitImportsFrom(base, descriptor, visit);
    }
}

void setImportSlot(std::uintptr_t* slot, std::uintptr_t replacement) {
    DWORD previous = 0;
    if (VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &previous) != 0) {
        *slot = replacement;
        VirtualProtect(slot, sizeof(*slot), previous, &previous);
    }
}

bool realHardwareAllowed() {
    std::array<wchar_t, 2> value{};
    const DWORD length = GetEnvironmentVariableW(L"SAK_TEST_ALLOW_REAL_HARDWARE",
                                                 value.data(),
                                                 static_cast<DWORD>(value.size()));
    return length == 1 && value[0] == L'1';
}

std::wstring blockedDevicePath(std::wstring_view path) {
    const std::wstring normal = normalizedPath(path);
    const std::wstring tail = deviceNamespaceTail(normal);
    if (tail.empty()) {
        return {};
    }
    if (tail.starts_with(L"physicaldrive")) {
        return physicalDriveMayExist(tail) ? normal : std::wstring{};
    }
    if (isForeignVolumeAlias(tail)) {
        return normal;
    }
    for (const std::wstring_view text : kBlockedDeviceText) {
        if (tail.find(text) != std::wstring::npos) {
            return normal;
        }
    }
    return {};
}

bool writesToSystemVolume(std::wstring_view path, unsigned long access) {
    constexpr DWORD kWriteAccess = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | FILE_WRITE_DATA |
                                   FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES |
                                   DELETE | WRITE_DAC | WRITE_OWNER;
    if ((access & kWriteAccess) == 0) {
        return false;
    }
    const std::wstring tail = deviceNamespaceTail(normalizedPath(path));
    return !tail.empty() && isVolumeAlias(tail) && isOsDriveLetter(tail[0]);
}

std::wstring blockedCommand(std::wstring_view command_line) {
    const std::wstring normal = lowered(command_line);
    for (const std::wstring_view text : kBlockedCommandText) {
        if (normal.find(text) != std::wstring::npos) {
            return std::wstring(text);
        }
    }
    for (const std::wstring_view word : kBlockedCommandWords) {
        if (containsWord(normal, word)) {
            return std::wstring(word);
        }
    }
    return {};
}

}  // namespace sak::test
