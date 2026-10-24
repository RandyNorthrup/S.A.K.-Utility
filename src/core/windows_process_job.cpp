// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/windows_process_job.h"

#include <QtGlobal>

#include <cstring>
#include <utility>

#include <private/qobject_p.h>
#include <private/qprocess_p.h>
#include <private/qwindowspipereader_p.h>

namespace sak {

namespace {

constexpr qint64 kMaxConfiguredPipeBytes = 64 * 1024 * 1024;

bool hasExpectedPipeChannels(const QProcessPrivate& process_private) {
    return process_private.stdoutChannel.type == QProcessPrivate::Channel::Normal &&
           process_private.stderrChannel.type == QProcessPrivate::Channel::Normal &&
           process_private.stdoutChannel.reader != nullptr &&
           process_private.stderrChannel.reader != nullptr;
}

bool arePipeReadersFresh(const QWindowsPipeReader& stdout_reader,
                         const QWindowsPipeReader& stderr_reader) {
    return stdout_reader.bytesAvailable() == 0 && stderr_reader.bytesAvailable() == 0 &&
           stdout_reader.maxReadBufferSize() == 0 && stderr_reader.maxReadBufferSize() == 0;
}

QString nativeFailure(const QString& operation, DWORD error) {
    return QStringLiteral("%1 failed (err=%2)").arg(operation).arg(error);
}

}  // namespace

WindowsProcessJob::WindowsProcessJob(Api api) : m_api(std::move(api)) {}

WindowsProcessJob::Api WindowsProcessJob::nativeApi() {
    Api api;
    api.create_job = [](LPSECURITY_ATTRIBUTES attributes, LPCWSTR name) {
        return ::CreateJobObjectW(attributes, name);
    };
    api.set_information = [](HANDLE job, JOBOBJECTINFOCLASS info_class, LPVOID info, DWORD size) {
        return ::SetInformationJobObject(job, info_class, info, size);
    };
    api.initialize_attributes =
        [](LPPROC_THREAD_ATTRIBUTE_LIST attributes, DWORD count, DWORD flags, PSIZE_T size) {
            return ::InitializeProcThreadAttributeList(attributes, count, flags, size);
        };
    api.update_attribute = [](LPPROC_THREAD_ATTRIBUTE_LIST attributes,
                              DWORD flags,
                              DWORD_PTR key,
                              PVOID value,
                              SIZE_T size,
                              PVOID previous,
                              PSIZE_T return_size) {
        return ::UpdateProcThreadAttribute(
            attributes, flags, key, value, size, previous, return_size);
    };
    api.delete_attributes = [](LPPROC_THREAD_ATTRIBUTE_LIST attributes) {
        ::DeleteProcThreadAttributeList(attributes);
    };
    api.query_information =
        [](HANDLE job, JOBOBJECTINFOCLASS info_class, LPVOID info, DWORD size, LPDWORD returned) {
            return ::QueryInformationJobObject(job, info_class, info, size, returned);
        };
    api.terminate_job = [](HANDLE job, UINT exit_code) {
        return ::TerminateJobObject(job, exit_code);
    };
    api.close_handle = [](HANDLE handle) {
        return ::CloseHandle(handle);
    };
    api.last_error = [] {
        return ::GetLastError();
    };
    return api;
}

std::expected<std::unique_ptr<WindowsProcessJob>, QString> WindowsProcessJob::create() {
    return createWithApi(nativeApi());
}

std::expected<std::unique_ptr<WindowsProcessJob>, QString> WindowsProcessJob::createWithApi(
    Api api) {
    auto owner = std::unique_ptr<WindowsProcessJob>(new WindowsProcessJob(std::move(api)));
    const auto prepared = owner->prepare();
    if (prepared) {
        return owner;
    }

    const QString preparation_error = prepared.error();
    owner->destroyLaunchAttributes();
    if (owner->m_job != nullptr) {
        const auto closed = owner->checkedClose();
        if (!closed) {
            owner->failFast(preparation_error + QStringLiteral("; ") + closed.error());
        }
    }
    return std::unexpected(preparation_error);
}

bool WindowsProcessJob::apiComplete() const {
    return m_api.create_job && m_api.set_information && m_api.initialize_attributes &&
           m_api.update_attribute && m_api.delete_attributes && m_api.query_information &&
           m_api.terminate_job && m_api.close_handle && m_api.last_error;
}

std::expected<void, QString> WindowsProcessJob::prepare() {
    if (!apiComplete()) {
        return std::unexpected(QStringLiteral("Windows process Job API table is incomplete"));
    }

    m_job = m_api.create_job(nullptr, nullptr);
    if (m_job == nullptr) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Create unnamed Windows process Job"), error));
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (m_api.set_information(m_job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ==
        FALSE) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Set Windows process Job kill-on-close limit"), error));
    }

    SIZE_T attribute_bytes = 0;
    const BOOL sizing_result = m_api.initialize_attributes(nullptr, 1, 0, &attribute_bytes);
    const DWORD sizing_error = sizing_result == FALSE ? m_api.last_error() : ERROR_SUCCESS;
    if (sizing_result != FALSE || sizing_error != ERROR_INSUFFICIENT_BUFFER ||
        attribute_bytes == 0) {
        return std::unexpected(
            nativeFailure(QStringLiteral("Size Windows process Job attribute list"), sizing_error));
    }

    m_attribute_storage.resize(attribute_bytes);
    m_attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_attribute_storage.data());
    if (m_api.initialize_attributes(m_attribute_list, 1, 0, &attribute_bytes) == FALSE) {
        const DWORD error = m_api.last_error();
        m_attribute_list = nullptr;
        m_attribute_storage.clear();
        return std::unexpected(
            nativeFailure(QStringLiteral("Initialize Windows process Job attribute list"), error));
    }

    m_job_values[0] = m_job;
    if (m_api.update_attribute(m_attribute_list,
                               0,
                               PROC_THREAD_ATTRIBUTE_JOB_LIST,
                               m_job_values.data(),
                               sizeof(m_job_values[0]) * m_job_values.size(),
                               nullptr,
                               nullptr) == FALSE) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Attach Windows process Job launch attribute"), error));
    }
    return {};
}

std::expected<void, QString> WindowsProcessJob::validatePipeLimits(
    const QProcess& process, const std::optional<WindowsJobPipeLimits>& limits) {
    if (!limits) {
        return {};
    }
    if (limits->stdout_bytes <= 0 || limits->stderr_bytes <= 0 ||
        limits->stdout_bytes > kMaxConfiguredPipeBytes ||
        limits->stderr_bytes > kMaxConfiguredPipeBytes) {
        return std::unexpected(QStringLiteral("Windows process Job pipe limits are invalid"));
    }
    if (process.processChannelMode() != QProcess::SeparateChannels) {
        return std::unexpected(
            QStringLiteral("Windows process Job pipe limits require separate channels"));
    }
    if (std::strcmp(qVersion(), QT_VERSION_STR) != 0) {
        return std::unexpected(
            QStringLiteral("Windows process Job private Qt runtime version differs"));
    }
    return {};
}

std::expected<void, QString> WindowsProcessJob::installOn(
    QProcess& process, std::optional<WindowsJobPipeLimits> pipe_limits) {
    if (m_job == nullptr) {
        return std::unexpected(QStringLiteral("Windows process Job is closed"));
    }
    if (m_launch_completed) {
        return std::unexpected(QStringLiteral("Windows process Job launch was already completed"));
    }
    if (m_installed_process != nullptr) {
        return std::unexpected(
            QStringLiteral("Windows process Job launch hook is already installed"));
    }
    if (m_attribute_list == nullptr || m_job_values[0] != m_job) {
        return std::unexpected(
            QStringLiteral("Windows process Job launch attributes are unavailable"));
    }
    if (process.createProcessArgumentsModifier()) {
        return std::unexpected(
            QStringLiteral("QProcess already has a CreateProcess arguments modifier"));
    }
    const auto validated = validatePipeLimits(process, pipe_limits);
    if (!validated) {
        return std::unexpected(validated.error());
    }

    m_modifier_invoked = false;
    m_installed_process = &process;
    m_pipe_limits = pipe_limits;
    process.setCreateProcessArgumentsModifier(
        [this](QProcess::CreateProcessArguments* arguments) { applyLaunchArguments(arguments); });
    return {};
}

void WindowsProcessJob::applyPipeLimits() {
    if (!m_pipe_limits) {
        return;
    }
    if (m_installed_process->processChannelMode() != QProcess::SeparateChannels) {
        failFast(QStringLiteral("QProcess pipe channels changed after Job preparation"));
    }
    auto* const process_private =
        static_cast<QProcessPrivate*>(QObjectPrivate::get(m_installed_process.data()));
    if (!hasExpectedPipeChannels(*process_private)) {
        failFast(QStringLiteral("QProcess pipe readers are unavailable before child launch"));
    }
    auto* const stdout_reader = process_private->stdoutChannel.reader;
    auto* const stderr_reader = process_private->stderrChannel.reader;
    if (!arePipeReadersFresh(*stdout_reader, *stderr_reader)) {
        failFast(QStringLiteral("QProcess pipe readers are not fresh before child launch"));
    }
    stdout_reader->setMaxReadBufferSize(m_pipe_limits->stdout_bytes);
    stderr_reader->setMaxReadBufferSize(m_pipe_limits->stderr_bytes);
    if (stdout_reader->maxReadBufferSize() != m_pipe_limits->stdout_bytes ||
        stderr_reader->maxReadBufferSize() != m_pipe_limits->stderr_bytes) {
        failFast(QStringLiteral("QProcess pipe limits were not installed before child launch"));
    }
    // QProcess's default readyRead slots copy every private-reader chunk to a
    // second, uncapped QRingBuffer. Disconnect both before CreateProcess; the
    // bounded caller reads these private readers directly instead.
    const bool stdout_disconnected = QObject::disconnect(
        stdout_reader, SIGNAL(readyRead()), m_installed_process, SLOT(_q_canReadStandardOutput()));
    const bool stderr_disconnected = QObject::disconnect(
        stderr_reader, SIGNAL(readyRead()), m_installed_process, SLOT(_q_canReadStandardError()));
    if (!stdout_disconnected || !stderr_disconnected) {
        failFast(QStringLiteral("Could not disable unbounded QProcess channel buffering"));
    }
}

void WindowsProcessJob::applyLaunchArguments(QProcess::CreateProcessArguments* arguments) {
    if (arguments == nullptr || arguments->startupInfo == nullptr || m_job == nullptr ||
        m_attribute_list == nullptr || m_job_values[0] != m_job || m_installed_process == nullptr) {
        failFast(QStringLiteral("QProcess JOB_LIST modifier received incomplete launch state"));
    }
    if (m_modifier_invoked) {
        failFast(QStringLiteral("QProcess JOB_LIST modifier was invoked more than once"));
    }
    if (arguments->startupInfo->cb != sizeof(STARTUPINFOW)) {
        failFast(
            QStringLiteral("QProcess JOB_LIST modifier received unexpected STARTUPINFO shape"));
    }

    applyPipeLimits();
    m_modifier_invoked = true;
    m_extended_startup = {};
    m_extended_startup.StartupInfo = *arguments->startupInfo;
    m_extended_startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
    m_extended_startup.lpAttributeList = m_attribute_list;
    arguments->startupInfo = reinterpret_cast<STARTUPINFOW*>(&m_extended_startup);
    arguments->flags |= EXTENDED_STARTUPINFO_PRESENT;
}

std::expected<void, QString> WindowsProcessJob::completeLaunch(const QProcess& process,
                                                               bool started) {
    if (m_installed_process == nullptr) {
        return std::unexpected(QStringLiteral("Windows process Job launch hook is not installed"));
    }
    QProcess* const installed_process = m_installed_process;
    installed_process->setCreateProcessArgumentsModifier({});
    m_installed_process = nullptr;
    destroyLaunchAttributes();
    m_launch_completed = true;

    if (installed_process != &process) {
        return std::unexpected(
            QStringLiteral("QProcess launch completion does not match installed process"));
    }
    if (!started) {
        return {};
    }
    if (!m_modifier_invoked) {
        return std::unexpected(QStringLiteral("QProcess JOB_LIST launch modifier was not invoked"));
    }
    const auto observed = accounting();
    if (!observed) {
        return std::unexpected(observed.error());
    }
    if (observed->total_processes < 1) {
        return std::unexpected(
            QStringLiteral("QProcess JOB_LIST launch recorded no associated process"));
    }
    return {};
}

std::expected<WindowsJobAccounting, QString> WindowsProcessJob::accounting() const {
    if (m_job == nullptr) {
        return std::unexpected(QStringLiteral("Windows process Job is closed"));
    }
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (m_api.query_information(
            m_job, JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr) ==
        FALSE) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Query Windows process Job accounting"), error));
    }
    return WindowsJobAccounting{.total_processes = accounting.TotalProcesses,
                                .active_processes = accounting.ActiveProcesses};
}

std::expected<void, QString> WindowsProcessJob::terminate() {
    if (m_job == nullptr) {
        return std::unexpected(QStringLiteral("Windows process Job is closed"));
    }
    if (m_api.terminate_job(m_job, kWindowsProcessJobFailureExitCode) == FALSE) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Terminate Windows process Job"), error));
    }
    return {};
}

std::expected<void, QString> WindowsProcessJob::closeEmpty() {
    if (m_job == nullptr) {
        return {};
    }
    if (m_installed_process != nullptr) {
        return std::unexpected(
            QStringLiteral("Windows process Job launch hook is still installed"));
    }
    const auto observed = accounting();
    if (!observed) {
        return std::unexpected(observed.error());
    }
    if (observed->active_processes != 0) {
        return std::unexpected(QStringLiteral("Windows process Job still has %1 active processes")
                                   .arg(observed->active_processes));
    }
    destroyLaunchAttributes();
    return checkedClose();
}

std::expected<void, QString> WindowsProcessJob::checkedClose() {
    if (m_job == nullptr) {
        return {};
    }
    if (m_api.close_handle(m_job) == FALSE) {
        const DWORD error = m_api.last_error();
        return std::unexpected(
            nativeFailure(QStringLiteral("Close empty Windows process Job"), error));
    }
    m_job = nullptr;
    m_job_values[0] = nullptr;
    return {};
}

void WindowsProcessJob::destroyLaunchAttributes() noexcept {
    if (m_attribute_list != nullptr) {
        m_api.delete_attributes(m_attribute_list);
        m_attribute_list = nullptr;
    }
    m_attribute_storage.clear();
}

WindowsProcessJob::~WindowsProcessJob() {
    if (m_installed_process != nullptr) {
        failFast(QStringLiteral("Windows process Job destroyed with launch hook installed"));
    }
    if (m_job == nullptr) {
        destroyLaunchAttributes();
        return;
    }
    const auto observed = accounting();
    if (!observed) {
        failFast(observed.error());
    }
    if (observed->active_processes != 0) {
        failFast(QStringLiteral("Windows process Job destroyed with %1 active processes")
                     .arg(observed->active_processes));
    }
    destroyLaunchAttributes();
    const auto closed = checkedClose();
    if (!closed) {
        failFast(closed.error());
    }
}

[[noreturn]] void WindowsProcessJob::failFast(const QString& error) noexcept {
    qFatal("%s", qPrintable(error));
}

}  // namespace sak
