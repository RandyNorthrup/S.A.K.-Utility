// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <QPointer>
#include <QProcess>
#include <QString>

#include <array>
#include <cstddef>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include <windows.h>

class WindowsProcessJobTests;
class AiMcpStdioSessionTests;
class AiMcpSessionPoolTests;

namespace sak {

inline constexpr UINT kWindowsProcessJobFailureExitCode = 197;

struct WindowsJobAccounting {
    DWORD total_processes{0};
    DWORD active_processes{0};
};

/// Bounded direct-reader mode for separate stdout/stderr channels. Qt's default
/// readyRead slots copy private pipe bytes into a second, uncapped QProcess buffer;
/// this mode disconnects those slots before child creation. Callers with limits
/// must read the private readers directly, never QProcess::readAllStandardOutput.
struct WindowsJobPipeLimits {
    qint64 stdout_bytes{0};
    qint64 stderr_bytes{0};
};

/// Owns one unnamed kill-on-close Windows Job and its one-shot QProcess JOB_LIST launch state.
/// Native operations are nonblocking; callers choose worker-thread or event-driven wait policy.
class WindowsProcessJob final {
public:
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsProcessJob>, QString> create();

    WindowsProcessJob(const WindowsProcessJob&) = delete;
    WindowsProcessJob& operator=(const WindowsProcessJob&) = delete;
    WindowsProcessJob(WindowsProcessJob&&) = delete;
    WindowsProcessJob& operator=(WindowsProcessJob&&) = delete;
    ~WindowsProcessJob();

    /// Installs one atomic JOB_LIST launch modifier. Optional reader caps are applied inside
    /// that same modifier before child creation. Refuses an existing modifier or reuse.
    [[nodiscard]] std::expected<void, QString> installOn(
        QProcess& process, std::optional<WindowsJobPipeLimits> pipe_limits = std::nullopt);

    /// Clears the modifier and launch attributes after QProcess::start. When @p started is true,
    /// also proves the modifier ran and the Job recorded at least one associated process.
    [[nodiscard]] std::expected<void, QString> completeLaunch(const QProcess& process,
                                                              bool started);

    [[nodiscard]] std::expected<WindowsJobAccounting, QString> accounting() const;

    /// Requests termination for every Job member. Completion must be observed separately.
    [[nodiscard]] std::expected<void, QString> terminate();

    /// Closes the owned Job only after checked accounting reports zero active members.
    [[nodiscard]] std::expected<void, QString> closeEmpty();

private:
    struct Api {
        std::function<HANDLE(LPSECURITY_ATTRIBUTES, LPCWSTR)> create_job;
        std::function<BOOL(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD)> set_information;
        std::function<BOOL(LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD, PSIZE_T)>
            initialize_attributes;
        std::function<BOOL(
            LPPROC_THREAD_ATTRIBUTE_LIST, DWORD, DWORD_PTR, PVOID, SIZE_T, PVOID, PSIZE_T)>
            update_attribute;
        std::function<void(LPPROC_THREAD_ATTRIBUTE_LIST)> delete_attributes;
        std::function<BOOL(HANDLE, JOBOBJECTINFOCLASS, LPVOID, DWORD, LPDWORD)> query_information;
        std::function<BOOL(HANDLE, UINT)> terminate_job;
        std::function<BOOL(HANDLE)> close_handle;
        std::function<DWORD()> last_error;
    };

    friend class ::WindowsProcessJobTests;
    friend class ::AiMcpStdioSessionTests;
    friend class ::AiMcpSessionPoolTests;

    explicit WindowsProcessJob(Api api);
    [[nodiscard]] static Api nativeApi();
    [[nodiscard]] static std::expected<std::unique_ptr<WindowsProcessJob>, QString> createWithApi(
        Api api);

    [[nodiscard]] std::expected<void, QString> prepare();
    [[nodiscard]] static std::expected<void, QString> validatePipeLimits(
        const QProcess& process, const std::optional<WindowsJobPipeLimits>& limits);
    [[nodiscard]] std::expected<void, QString> checkedClose();
    void destroyLaunchAttributes() noexcept;
    void applyPipeLimits();
    void applyLaunchArguments(QProcess::CreateProcessArguments* arguments);
    [[nodiscard]] bool apiComplete() const;
    [[noreturn]] static void failFast(const QString& error) noexcept;

    Api m_api;
    HANDLE m_job{nullptr};
    std::array<HANDLE, 1> m_job_values{nullptr};
    std::vector<std::byte> m_attribute_storage;
    LPPROC_THREAD_ATTRIBUTE_LIST m_attribute_list{nullptr};
    STARTUPINFOEXW m_extended_startup{};
    QPointer<QProcess> m_installed_process;
    std::optional<WindowsJobPipeLimits> m_pipe_limits;
    bool m_modifier_invoked{false};
    bool m_launch_completed{false};
};

}  // namespace sak
