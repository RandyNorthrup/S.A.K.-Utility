#include "sak/elevation_manager.h"

#ifdef _WIN32

#include "sak/logger.h"
#include <windows.h>
#include <shellapi.h>
#include <sddl.h>
// Undefine Windows macros that conflict with Qt
#undef emit
#undef signals
#undef slots

#pragma comment(lib, "shell32.lib")

namespace sak {

bool ElevationManager::is_elevated() noexcept
{
    BOOL is_admin = FALSE;
    PSID administrators_group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt_authority = SECURITY_NT_AUTHORITY;

    if (AllocateAndInitializeSid(
            &nt_authority,
            2,
            SECURITY_BUILTIN_DOMAIN_RID,
            DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0,
            &administrators_group)) {
        
        CheckTokenMembership(nullptr, administrators_group, &is_admin);
        FreeSid(administrators_group);
    }

    return is_admin == TRUE;
}

bool ElevationManager::can_elevate() noexcept
{
    // On Windows Vista+, UAC is available
    OSVERSIONINFOEX osvi;
    ZeroMemory(&osvi, sizeof(OSVERSIONINFOEX));
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFOEX);
    osvi.dwMajorVersion = 6; // Windows Vista+

    DWORDLONG condition_mask = 0;
    VER_SET_CONDITION(condition_mask, VER_MAJORVERSION, VER_GREATER_EQUAL);

    return VerifyVersionInfo(&osvi, VER_MAJORVERSION, condition_mask) != FALSE;
}

bool ElevationManager::is_user_admin() noexcept
{
    return is_elevated();
}

auto ElevationManager::get_executable_path()
    -> std::expected<std::string, sak::error_code>
{
    char path[MAX_PATH];
    DWORD result = GetModuleFileNameA(nullptr, path, MAX_PATH);
    
    if (result == 0 || result == MAX_PATH) {
        DWORD error = GetLastError();
        sak::log_error("Failed to get executable path: error {}", error);
        return std::unexpected(sak::error_code::execution_failed);
    }

    return std::string(path);
}

std::string ElevationManager::get_command_line_args()
{
    LPWSTR cmd_line = GetCommandLineW();
    
    // Skip the executable name (first argument)
    int argc;
    LPWSTR* argv = CommandLineToArgvW(cmd_line, &argc);
    
    std::string args;
    if (argv && argc > 1) {
        for (int i = 1; i < argc; ++i) {
            if (i > 1) {
                args += " ";
            }
            
            // Convert wide string to narrow
            int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
            if (size > 0) {
                std::string arg(static_cast<size_t>(size - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), size, nullptr, nullptr);
                
                // Quote if contains spaces
                if (arg.find(' ') != std::string::npos) {
                    args += "\"" + arg + "\"";
                } else {
                    args += arg;
                }
            }
        }
    }
    
    if (argv) {
        LocalFree(argv);
    }
    
    return args;
}

auto ElevationManager::restart_elevated(bool wait_for_exit)
    -> std::expected<void, sak::error_code>
{
    if (is_elevated()) {
        sak::log_info("Already running with administrator privileges");
        return {};
    }

    auto exe_path_result = get_executable_path();
    if (!exe_path_result) {
        return std::unexpected(exe_path_result.error());
    }

    std::string args = get_command_line_args();
    
    sak::log_info("Restarting with elevation: {} {}", exe_path_result.value(), args);

    return execute_elevated(exe_path_result.value(), args, wait_for_exit);
}

auto ElevationManager::execute_elevated(
    const std::string& executable,
    const std::string& arguments,
    bool wait_for_exit)
    -> std::expected<void, sak::error_code>
{
    SHELLEXECUTEINFOA sei = { sizeof(SHELLEXECUTEINFOA) };
    sei.lpVerb = "runas";  // Request elevation
    sei.lpFile = executable.c_str();
    sei.lpParameters = arguments.empty() ? nullptr : arguments.c_str();
    sei.nShow = SW_NORMAL;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;

    if (!ShellExecuteExA(&sei)) {
        DWORD error = GetLastError();
        
        if (error == ERROR_CANCELLED) {
            sak::log_info("User cancelled elevation request");
            return std::unexpected(sak::error_code::operation_cancelled);
        }
        
        sak::log_error("Failed to execute with elevation: {}", 
                      get_elevation_error_message(error));
        return std::unexpected(sak::error_code::elevation_failed);
    }

    sak::log_info("Successfully launched elevated process");

    if (wait_for_exit && sei.hProcess) {
        sak::log_info("Waiting for elevated process to complete...");
        WaitForSingleObject(sei.hProcess, INFINITE);
        
        DWORD exit_code = 0;
        GetExitCodeProcess(sei.hProcess, &exit_code);
        CloseHandle(sei.hProcess);
        
        sak::log_info("Elevated process exited with code {}", exit_code);
    } else if (sei.hProcess) {
        CloseHandle(sei.hProcess);
    }

    return {};
}

std::string ElevationManager::get_elevation_error_message(unsigned long error_code)
{
    char* message_buffer = nullptr;
    
    DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message_buffer),
        0,
        nullptr
    );

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

} // namespace sak

#endif // _WIN32
