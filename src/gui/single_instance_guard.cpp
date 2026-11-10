// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file single_instance_guard.cpp
/// @brief One running S.A.K. Utility per portable data folder.

#include "sak/single_instance_guard.h"

#include "sak/win32mcp/browser_bridge_security.h"

#include <QCryptographicHash>
#include <QDir>
#include <QWinEventNotifier>

#include <array>
#include <string>

#include <windows.h>

#include <sddl.h>

namespace sak {

namespace {

constexpr int kObjectNameHashChars = 32;
constexpr DWORD kFinalPathBufferChars = 32'768;

QString lastErrorText(const QString& action) {
    return QStringLiteral("%1 failed (Windows error %2)").arg(action).arg(::GetLastError());
}

/// The data folder's final path: junctions, symbolic links and 8.3 names resolved, so every
/// spelling of one folder names one instance.
std::expected<QString, QString> finalDataRootPath(const QString& data_root) {
    const std::wstring native = QDir::toNativeSeparators(data_root).toStdWString();
    const HANDLE folder = ::CreateFileW(native.c_str(),
                                        FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        nullptr,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS,
                                        nullptr);
    if (folder == INVALID_HANDLE_VALUE) {
        return std::unexpected(lastErrorText(QStringLiteral("Opening the data folder")));
    }
    std::wstring buffer(kFinalPathBufferChars, L'\0');
    const DWORD length = ::GetFinalPathNameByHandleW(
        folder, buffer.data(), kFinalPathBufferChars, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    const DWORD error = ::GetLastError();
    ::CloseHandle(folder);
    if (length == 0 || length >= kFinalPathBufferChars) {
        ::SetLastError(error);
        return std::unexpected(lastErrorText(QStringLiteral("Resolving the data folder path")));
    }
    buffer.resize(length);
    return QString::fromStdWString(buffer);
}

/// GENERIC_ALL to this user and SYSTEM only, labelled Medium integrity with no-write-up: an
/// elevated instance and an unelevated launch of the same user can each open what the other
/// created, and a lower-integrity process cannot signal or hold them.
std::expected<PSECURITY_DESCRIPTOR, QString> sameUserDescriptor() {
    QString sid_error;
    const QString sid = win32mcp::currentUserSidString(&sid_error);
    if (sid.isEmpty()) {
        return std::unexpected(QStringLiteral("Reading this user's SID failed: %1").arg(sid_error));
    }
    const std::wstring sddl =
        QStringLiteral("D:P(A;;GA;;;%1)(A;;GA;;;SY)S:(ML;;NW;;;ME)").arg(sid).toStdWString();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (::ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr) == FALSE) {
        return std::unexpected(lastErrorText(QStringLiteral("Building the instance security")));
    }
    return descriptor;
}

std::wstring sessionObjectName(const QString& stem, const char* suffix) {
    return (QStringLiteral("Local\\") + stem + QString::fromLatin1(suffix)).toStdWString();
}

/// Waits for the instance that launched this one to end. Ownership arrives abandoned when it
/// exits holding the mutex, which is how it always ends.
std::expected<void, QString> waitForPreviousInstance(HANDLE instance_mutex,
                                                     std::chrono::milliseconds limit) {
    // Bounded by @p limit and run before any window exists, so nothing is
    // left unpainted while it waits; the closing instance is finishing its settings writes.
    const DWORD waited = ::WaitForSingleObject(instance_mutex, static_cast<DWORD>(limit.count()));
    if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) {
        return {};
    }
    if (waited == WAIT_TIMEOUT) {
        return std::unexpected(
            QStringLiteral("The S.A.K. Utility window that started this administrator copy did "
                           "not close within %1 seconds. Close it, then use Run as Admin again.")
                .arg(std::chrono::duration_cast<std::chrono::seconds>(limit).count()));
    }
    return std::unexpected(lastErrorText(QStringLiteral("Waiting for the previous instance")));
}

}  // namespace

QString SingleInstanceGuard::objectNameStem(const QString& final_data_root_path) {
    // NTFS names are case-insensitive and a trailing separator names the same folder.
    QString key = QDir::toNativeSeparators(final_data_root_path).toLower();
    while (key.endsWith(QLatin1Char('\\'))) {
        key.chop(1);
    }
    const QByteArray digest =
        QCryptographicHash::hash(key.toUtf8(), QCryptographicHash::Sha256).toHex();
    return QStringLiteral("SAKUtility-") + QString::fromLatin1(digest.left(kObjectNameHashChars));
}

std::expected<std::unique_ptr<SingleInstanceGuard>, QString> SingleInstanceGuard::claim(
    const QString& data_root, std::chrono::milliseconds wait_for_previous_exit) {
    const auto final_path = finalDataRootPath(data_root);
    if (!final_path) {
        return std::unexpected(final_path.error());
    }
    const auto descriptor = sameUserDescriptor();
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = *descriptor;
    attributes.bInheritHandle = FALSE;
    const QString stem = objectNameStem(*final_path);

    // The event exists before the mutex: a second launch that finds the mutex can always signal
    // an event the running instance already holds open.
    const HANDLE event =
        ::CreateEventW(&attributes, FALSE, FALSE, sessionObjectName(stem, "-activate").c_str());
    if (event == nullptr) {
        ::LocalFree(*descriptor);
        return std::unexpected(lastErrorText(QStringLiteral("Creating the activation event")));
    }
    const HANDLE mutex =
        ::CreateMutexW(&attributes, TRUE, sessionObjectName(stem, "-instance").c_str());
    const DWORD create_error = ::GetLastError();
    ::LocalFree(*descriptor);
    if (mutex == nullptr) {
        ::CloseHandle(event);
        ::SetLastError(create_error);
        return std::unexpected(lastErrorText(QStringLiteral("Creating the instance mutex")));
    }

    Role role = create_error == ERROR_ALREADY_EXISTS ? Role::Secondary : Role::Primary;
    if (role == Role::Secondary && wait_for_previous_exit.count() > 0) {
        const auto waited = waitForPreviousInstance(mutex, wait_for_previous_exit);
        if (!waited) {
            ::CloseHandle(mutex);
            ::CloseHandle(event);
            return std::unexpected(waited.error());
        }
        role = Role::Primary;
    }
    return std::unique_ptr<SingleInstanceGuard>(new SingleInstanceGuard(role, mutex, event));
}

SingleInstanceGuard::SingleInstanceGuard(Role role, void* instance_mutex, void* activation_event)
    : m_role(role), m_instance_mutex(instance_mutex), m_activation_event(activation_event) {}

SingleInstanceGuard::~SingleInstanceGuard() {
    delete m_notifier;
    ::CloseHandle(static_cast<HANDLE>(m_activation_event));
    if (m_role == Role::Secondary) {
        ::CloseHandle(static_cast<HANDLE>(m_instance_mutex));
    }
    // A primary keeps its mutex handle -- and so its ownership -- until the process ends, after
    // the last configuration write (see the class comment). Closing it here would free the name
    // for a new launch while static destructors may still write Utility.ini.
}

std::expected<void, QString> SingleInstanceGuard::activateRunningInstance() const {
    // Set by the user's launch, this process may give the foreground away; the running instance
    // then raises its own window.
    ::AllowSetForegroundWindow(ASFW_ANY);
    if (::SetEvent(static_cast<HANDLE>(m_activation_event)) == FALSE) {
        return std::unexpected(lastErrorText(QStringLiteral("Signalling the running instance")));
    }
    return {};
}

void SingleInstanceGuard::listenForActivation() {
    if (m_role != Role::Primary || m_notifier != nullptr) {
        return;
    }
    m_notifier = new QWinEventNotifier(static_cast<HANDLE>(m_activation_event), this);
    connect(m_notifier, &QWinEventNotifier::activated, this, [this]() {
        Q_EMIT activationRequested();
    });
}

}  // namespace sak
