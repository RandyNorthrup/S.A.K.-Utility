// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ai_mcp_native_fixture.h
/// @brief Shared hermetic launch and observation support for built-in Win32 MCP tests.

#pragma once

#include "sak/ai/ai_mcp_stdio_session.h"
#include "sak/contained_process_quarantine.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QtGlobal>
#include <QVector>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <intrin.h>

namespace sak::testfixtures::mcp {

inline constexpr char kModeEnvironment[] = "SAK_WIN32_MCP_MODE";
inline constexpr char kLocalAppDataEnvironment[] = "LOCALAPPDATA";
inline constexpr char kSecurityProfileEnvironment[] = "WIN32_MCP_SECURITY_PROFILE";
inline constexpr int kRequestTimeoutMs = 5000;
inline constexpr DWORD kProcessExitWaitMs = 5000;
// Long enough for the Worker to reach waitForFinished, so a hung child usually reports the
// observed "Process operation timed out" rather than only the dispatch deadline.
inline constexpr int kHungChildCleanupDeadlineMs = 250;
inline constexpr int kCustodyRecoveryAttempts = 3;

enum class SecurityProfile {
    ReadOnly,
    Full
};

/// Hard lifetime for explicitly selected, test-owned child modes, including blocked stdin.
/// Never construct this guard in the parent test runner. Its thread stops/joins before the
/// synchronization members are destroyed, and normal fixture exit does not wait out the limit.
class OwnedChildDeadline {
public:
    OwnedChildDeadline(int lifetime_ms, UINT exit_code) {
        if (lifetime_ms <= 0) {
            qFatal("Owned child deadline must be positive");
        }
        m_thread = std::jthread([this, lifetime_ms, exit_code](std::stop_token stop) {
            std::unique_lock lock(m_mutex);
            m_condition.wait_for(lock, stop, std::chrono::milliseconds(lifetime_ms), []() {
                return false;
            });
            if (!stop.stop_requested()) {
                // Another fixture thread may hold CRT/DLL locks while blocked on stdin.
                // Self-termination skips DLL detach; ExitProcess can deadlock on those locks.
                if (::TerminateProcess(::GetCurrentProcess(), exit_code) == FALSE) {
                    __fastfail(FAST_FAIL_FATAL_APP_EXIT);
                }
            }
        });
    }

    OwnedChildDeadline(const OwnedChildDeadline&) = delete;
    OwnedChildDeadline& operator=(const OwnedChildDeadline&) = delete;

private:
    std::mutex m_mutex;
    std::condition_variable_any m_condition;
    std::jthread m_thread;
};

template <typename Value>
QString failureDiagnostic(const std::expected<Value, QString>& result) {
    return result ? QString{} : result.error();
}

/// Fixture recovery for contained-process custody: retries until at most @p expected_count
/// owners remain. Each retry is one ordinary bounded cleanup per retained owner.
inline std::expected<void, QString> releaseCustodyTo(int expected_count) {
    QString last_error = QStringLiteral("Custody retry never ran");
    for (int attempt = 0; attempt < kCustodyRecoveryAttempts; ++attempt) {
        if (sak::ContainedProcessQuarantine::retainedCount() <= expected_count) {
            return {};
        }
        const auto retried = sak::ContainedProcessQuarantine::retryRetained();
        if (!retried) {
            last_error = retried.error();
        }
    }
    if (sak::ContainedProcessQuarantine::retainedCount() <= expected_count) {
        return {};
    }
    return std::unexpected(last_error);
}

/// Whether process @p pid is assigned to @p job. Asking through the owner's own Job handle
/// observes containment directly: a closed handle or a vanished process is an error, never a
/// false negative. (A console child is not the Job's only member -- its conhost.exe joins too.)
inline std::expected<bool, QString> isProcessInJob(qint64 pid, HANDLE job) {
    if (pid <= 0 || static_cast<quint64>(pid) > std::numeric_limits<DWORD>::max()) {
        return std::unexpected(QStringLiteral("Cannot query Job membership of pid %1").arg(pid));
    }
    const HANDLE process =
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr) {
        const DWORD error = ::GetLastError();
        return std::unexpected(QStringLiteral("OpenProcess(%1) for Job membership failed (err=%2)")
                                   .arg(pid)
                                   .arg(error));
    }
    BOOL in_job = FALSE;
    const BOOL queried = ::IsProcessInJob(process, job, &in_job);
    const DWORD query_error = queried == FALSE ? ::GetLastError() : ERROR_SUCCESS;
    if (::CloseHandle(process) == FALSE) {
        const DWORD error = ::GetLastError();
        return std::unexpected(
            QStringLiteral("CloseHandle(process %1) failed (err=%2)").arg(pid).arg(error));
    }
    if (queried == FALSE) {
        return std::unexpected(
            QStringLiteral("IsProcessInJob(%1) failed (err=%2)").arg(pid).arg(query_error));
    }
    return in_job != FALSE;
}

/// Parses a fake server's process-id tools/call result: one text block holding the server's
/// own canonical decimal PID, with isError explicitly false.
inline std::expected<qint64, QString> fakeProcessId(const QJsonObject& result) {
    const auto is_error = result.value(QStringLiteral("isError"));
    const auto content_value = result.value(QStringLiteral("content"));
    if (!is_error.isBool() || is_error.toBool() || !content_value.isArray()) {
        return std::unexpected(QStringLiteral("Fake MCP process-id result shape is invalid"));
    }
    const auto content = content_value.toArray();
    if (content.size() != 1 || !content.at(0).isObject()) {
        return std::unexpected(QStringLiteral("Fake MCP process-id content shape is invalid"));
    }
    const auto block = content.at(0).toObject();
    const auto text_value = block.value(QStringLiteral("text"));
    if (block.value(QStringLiteral("type")).toString() != QLatin1String("text") ||
        !text_value.isString()) {
        return std::unexpected(QStringLiteral("Fake MCP process-id content is not text"));
    }
    const QString text = text_value.toString();
    bool valid_pid = false;
    const qint64 pid = text.toLongLong(&valid_pid);
    if (!valid_pid || QString::number(pid) != text) {
        return std::unexpected(QStringLiteral("Fake MCP process-id text is not canonical decimal"));
    }
    return pid;
}

inline QString builtInCommand() {
    return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("sak_utility.exe"));
}

inline QString rendezvousPath(const QString& local_app_data) {
    return QDir(local_app_data).filePath(QStringLiteral("SAK/browser_bridge.json"));
}

inline QProcessEnvironment environment(const QString& local_app_data, SecurityProfile profile) {
    QProcessEnvironment result = QProcessEnvironment::systemEnvironment();
    result.insert(QString::fromLatin1(kModeEnvironment), QStringLiteral("1"));
    result.insert(QString::fromLatin1(kLocalAppDataEnvironment), local_app_data);
    result.insert(QStringLiteral("WIN32_MCP_RESULT_ENVELOPE"), QStringLiteral("true"));
    result.insert(QStringLiteral("WIN32_MCP_REDACT_SENSITIVE_OUTPUT"), QStringLiteral("true"));
    if (profile == SecurityProfile::ReadOnly) {
        result.insert(QString::fromLatin1(kSecurityProfileEnvironment),
                      QStringLiteral("read_only"));
    } else {
        result.remove(QString::fromLatin1(kSecurityProfileEnvironment));
    }
    return result;
}

inline std::expected<void, QString> validateBeforeLaunch(
    const QString& command,
    const QProcessEnvironment& process_environment,
    const QString& local_app_data,
    SecurityProfile profile,
    const QString& tool_name) {
    if (!QFileInfo(command).isFile()) {
        return std::unexpected(QStringLiteral("Built-in MCP app is missing: %1").arg(command));
    }
    if (QFileInfo(command).fileName() != QLatin1String("sak_utility.exe")) {
        return std::unexpected(QStringLiteral("Built-in MCP command is not sak_utility.exe"));
    }
    if (process_environment.value(QString::fromLatin1(kModeEnvironment)) != QLatin1String("1")) {
        return std::unexpected(QStringLiteral("Built-in MCP mode is not enabled"));
    }
    if (process_environment.value(QString::fromLatin1(kLocalAppDataEnvironment)) !=
        local_app_data) {
        return std::unexpected(QStringLiteral("Built-in MCP LOCALAPPDATA is not sandboxed"));
    }
    const bool has_profile =
        process_environment.contains(QString::fromLatin1(kSecurityProfileEnvironment));
    if (profile == SecurityProfile::ReadOnly &&
        process_environment.value(QString::fromLatin1(kSecurityProfileEnvironment)) !=
            QLatin1String("read_only")) {
        return std::unexpected(QStringLiteral("Built-in MCP read-only profile is invalid"));
    }
    if (profile == SecurityProfile::Full && has_profile) {
        return std::unexpected(QStringLiteral("Built-in MCP full profile token must be absent"));
    }
    if (tool_name != QLatin1String("health_check")) {
        return std::unexpected(QStringLiteral("Built-in MCP probe is not health_check"));
    }
    return {};
}

inline std::expected<QJsonObject, QString> healthPayload(const QJsonObject& result) {
    const QJsonValue is_error = result.value(QStringLiteral("isError"));
    if (!is_error.isUndefined() && !is_error.isBool()) {
        return std::unexpected(QStringLiteral("health_check isError is not boolean"));
    }
    if (is_error.toBool(false)) {
        return std::unexpected(QStringLiteral("health_check returned isError"));
    }
    const QJsonArray content = result.value(QStringLiteral("content")).toArray();
    if (content.size() != 1 || !content.at(0).isObject()) {
        return std::unexpected(QStringLiteral("health_check content shape is invalid"));
    }
    const QJsonObject block = content.at(0).toObject();
    if (block.value(QStringLiteral("type")).toString() != QLatin1String("text")) {
        return std::unexpected(QStringLiteral("health_check content is not text"));
    }
    QJsonParseError parse_error;
    const QJsonDocument document = QJsonDocument::fromJson(
        block.value(QStringLiteral("text")).toString().toUtf8(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return std::unexpected(QStringLiteral("health_check text is not a JSON object"));
    }
    return document.object();
}

inline std::expected<qint64, QString> healthPid(const QJsonObject& result) {
    const auto payload = healthPayload(result);
    if (!payload) {
        return std::unexpected(payload.error());
    }
    if (payload->value(QStringLiteral("status")).toString() != QLatin1String("ok")) {
        return std::unexpected(QStringLiteral("health_check status is invalid"));
    }
    if (payload->value(QStringLiteral("server")).toString() != QLatin1String("sak-win32-mcp")) {
        return std::unexpected(QStringLiteral("health_check server identity is invalid"));
    }
    if (!payload->value(QStringLiteral("native")).toBool(false)) {
        return std::unexpected(QStringLiteral("health_check native field is invalid"));
    }
    const qint64 pid = payload->value(QStringLiteral("pid")).toInteger();
    if (pid <= 0 || static_cast<quint64>(pid) > std::numeric_limits<DWORD>::max()) {
        return std::unexpected(QStringLiteral("health_check pid is outside the Windows PID range"));
    }
    return pid;
}

inline bool containsTool(const QVector<sak::ai::AiMcpToolDescriptor>& tools, const QString& name) {
    return std::any_of(tools.cbegin(), tools.cend(), [&name](const auto& tool) {
        return tool.name == name;
    });
}

class ProcessGeneration {
public:
    static std::expected<ProcessGeneration, QString> observe(qint64 pid) {
        if (pid <= 0 || static_cast<quint64>(pid) > std::numeric_limits<DWORD>::max()) {
            return std::unexpected(QStringLiteral("Cannot observe invalid process id %1").arg(pid));
        }
        HANDLE handle = ::OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
                                      FALSE,
                                      static_cast<DWORD>(pid));
        if (handle == nullptr) {
            const DWORD error = ::GetLastError();
            return std::unexpected(
                QStringLiteral("OpenProcess(%1) failed (err=%2)").arg(pid).arg(error));
        }
        return ProcessGeneration(handle, pid);
    }

    ProcessGeneration(const ProcessGeneration&) = delete;
    ProcessGeneration& operator=(const ProcessGeneration&) = delete;

    ProcessGeneration(ProcessGeneration&& other) noexcept
        : m_handle(std::exchange(other.m_handle, nullptr)), m_pid(other.m_pid) {}

    ProcessGeneration& operator=(ProcessGeneration&& other) noexcept {
        if (this != &other) {
            closeOrFail();
            m_handle = std::exchange(other.m_handle, nullptr);
            m_pid = other.m_pid;
        }
        return *this;
    }

    ~ProcessGeneration() { closeOrFail(); }

    [[nodiscard]] std::expected<bool, QString> isAlive() const {
        if (m_handle == nullptr) {
            return std::unexpected(QStringLiteral("Process observation handle is closed"));
        }
        const DWORD wait_result = ::WaitForSingleObject(m_handle, 0);
        if (wait_result == WAIT_TIMEOUT) {
            return true;
        }
        if (wait_result == WAIT_OBJECT_0) {
            return false;
        }
        if (wait_result == WAIT_FAILED) {
            const DWORD error = ::GetLastError();
            return std::unexpected(
                QStringLiteral("Process %1 wait failed (err=%2)").arg(m_pid).arg(error));
        }
        return std::unexpected(
            QStringLiteral("Process %1 wait returned %2").arg(m_pid).arg(wait_result));
    }

    [[nodiscard]] std::expected<DWORD, QString> waitForExit() {
        if (m_handle == nullptr) {
            return std::unexpected(QStringLiteral("Process observation handle is closed"));
        }
        const DWORD wait_result = ::WaitForSingleObject(m_handle, kProcessExitWaitMs);
        if (wait_result == WAIT_TIMEOUT) {
            return std::unexpected(QStringLiteral("Process %1 did not exit within %2 ms")
                                       .arg(m_pid)
                                       .arg(kProcessExitWaitMs));
        }
        if (wait_result == WAIT_FAILED) {
            const DWORD error = ::GetLastError();
            return std::unexpected(
                QStringLiteral("Process %1 exit wait failed (err=%2)").arg(m_pid).arg(error));
        }
        if (wait_result != WAIT_OBJECT_0) {
            return std::unexpected(
                QStringLiteral("Process %1 exit wait returned %2").arg(m_pid).arg(wait_result));
        }
        DWORD exit_code = 0;
        if (::GetExitCodeProcess(m_handle, &exit_code) == FALSE) {
            const DWORD error = ::GetLastError();
            return std::unexpected(
                QStringLiteral("GetExitCodeProcess(%1) failed (err=%2)").arg(m_pid).arg(error));
        }
        // A signaled process handle is authoritative. Exit code 259 is legitimate after signal,
        // even though the same numeric value also denotes STILL_ACTIVE before termination.
        const auto closed = close();
        if (!closed) {
            return std::unexpected(closed.error());
        }
        return exit_code;
    }

private:
    ProcessGeneration(HANDLE handle, qint64 pid) : m_handle(handle), m_pid(pid) {}

    [[nodiscard]] std::expected<void, QString> close() noexcept {
        if (m_handle == nullptr) {
            return {};
        }
        const HANDLE handle = std::exchange(m_handle, nullptr);
        if (::CloseHandle(handle) == FALSE) {
            const DWORD error = ::GetLastError();
            return std::unexpected(
                QStringLiteral("CloseHandle(process %1) failed (err=%2)").arg(m_pid).arg(error));
        }
        return {};
    }

    void closeOrFail() noexcept {
        const auto closed = close();
        if (!closed) {
            qFatal("%s", qPrintable(closed.error()));
        }
    }

    HANDLE m_handle{nullptr};
    qint64 m_pid{0};
};

}  // namespace sak::testfixtures::mcp
