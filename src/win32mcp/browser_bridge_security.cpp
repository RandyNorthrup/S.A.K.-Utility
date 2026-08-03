// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/win32mcp/browser_bridge_security.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QVector>

#include <sddl.h>

namespace sak::win32mcp {

namespace {

constexpr int kNonceHexWidth = 16;  // 64-bit nonce as zero-padded hex
constexpr int kHexBase = 16;

void setError(QString* error, const QString& message) {
    if (error) {
        *error = message;
    }
}

QString lastError(const QString& api) {
    return QStringLiteral("%1 failed (GetLastError=%2)").arg(api).arg(GetLastError());
}

// Random 64-bit value as zero-padded hex, drawn from the OS CSPRNG.
QString randomHex64() {
    const quint64 value = QRandomGenerator::system()->generate64();
    return QStringLiteral("%1").arg(value, kNonceHexWidth, kHexBase, QLatin1Char('0'));
}

}  // namespace

QString currentUserSidString(QString* error) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) == FALSE) {
        setError(error, lastError(QStringLiteral("OpenProcessToken")));
        return {};
    }
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    QVector<char> buffer(static_cast<int>(needed) > 0 ? static_cast<int>(needed) : 1);
    QString result;
    if (GetTokenInformation(token, TokenUser, buffer.data(), needed, &needed) != FALSE) {
        const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.constData());
        LPWSTR sid_string = nullptr;
        if (ConvertSidToStringSidW(user->User.Sid, &sid_string) != FALSE) {
            result = QString::fromWCharArray(sid_string);
            LocalFree(sid_string);
        } else {
            setError(error, lastError(QStringLiteral("ConvertSidToStringSid")));
        }
    } else {
        setError(error, lastError(QStringLiteral("GetTokenInformation")));
    }
    CloseHandle(token);
    return result;
}

QString browserBridgePipeName(QString* error) {
    const QString sid = currentUserSidString(error);
    if (sid.isEmpty()) {
        return {};
    }
    DWORD session = 0;
    if (ProcessIdToSessionId(GetCurrentProcessId(), &session) == FALSE) {
        setError(error, lastError(QStringLiteral("ProcessIdToSessionId")));
        return {};
    }
    return QStringLiteral("\\\\.\\pipe\\SAK_BrowserBridge_%1_%2_%3")
        .arg(sid)
        .arg(session)
        .arg(randomHex64());
}

QString generateBridgeToken() {
    return randomHex64() + randomHex64();  // 128-bit
}

QString browserBridgeRendezvousPath() {
    QString base = qEnvironmentVariable("LOCALAPPDATA");
    if (base.isEmpty()) {
        base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
    }
    return QDir(base).filePath(QStringLiteral("SAK/browser_bridge.json"));
}

bool writeRendezvousRecord(const QString& path, const RendezvousRecord& record, QString* error) {
    const QFileInfo info(path);
    if (!QDir().mkpath(info.absolutePath())) {
        setError(error, QStringLiteral("Cannot create directory %1").arg(info.absolutePath()));
        return false;
    }
    const QJsonObject object{{QStringLiteral("pipe_name"), record.pipe_name},
                             {QStringLiteral("token"), record.token},
                             {QStringLiteral("protocol"), record.protocol},
                             {QStringLiteral("app_pid"), record.app_pid}};
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        setError(error,
                 QStringLiteral("Cannot open %1 for writing: %2").arg(path, file.errorString()));
        return false;
    }
    const QByteArray bytes = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (file.write(bytes) != bytes.size() || !file.flush()) {
        setError(error, QStringLiteral("Cannot write %1: %2").arg(path, file.errorString()));
        return false;
    }
    return true;
}

bool readRendezvousRecord(const QString& path, RendezvousRecord* out, QString* error) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        setError(error, QStringLiteral("Cannot open %1: %2").arg(path, file.errorString()));
        return false;
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !doc.isObject()) {
        setError(error,
                 QStringLiteral("Malformed rendezvous record: %1").arg(parse_error.errorString()));
        return false;
    }
    const QJsonObject object = doc.object();
    out->pipe_name = object.value(QStringLiteral("pipe_name")).toString();
    out->token = object.value(QStringLiteral("token")).toString();
    out->protocol = object.value(QStringLiteral("protocol")).toInt();
    out->app_pid = static_cast<qint64>(object.value(QStringLiteral("app_pid")).toDouble());
    return true;
}

bool buildBridgePipeSecurity(SECURITY_ATTRIBUTES* attributes,
                             PSECURITY_DESCRIPTOR* descriptor,
                             QString* error) {
    const QString sid = currentUserSidString(error);
    if (sid.isEmpty()) {
        return false;
    }
    // DACL: PROTECTED (no inherited ACEs), GENERIC_ALL to this user SID + SYSTEM only.
    // SACL: Medium mandatory-integrity label, NO_READ_UP | NO_WRITE_UP -- a below-Medium
    // process (e.g. a sandboxed browser renderer, the real injection-to-local vector)
    // cannot open the pipe for read OR write, so it can neither drive it nor occupy the
    // single instance. NR is essential: with NW alone a low-IL subject could still open
    // for read and wedge the sole pipe instance (denial of service).
    const QString sddl = QStringLiteral("D:P(A;;GA;;;%1)(A;;GA;;;SY)S:(ML;;NRNW;;;ME)").arg(sid);
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
            reinterpret_cast<LPCWSTR>(sddl.utf16()), SDDL_REVISION_1, &sd, nullptr) == FALSE) {
        setError(error,
                 lastError(QStringLiteral("ConvertStringSecurityDescriptorToSecurityDescriptor")));
        return false;
    }
    attributes->nLength = sizeof(*attributes);
    attributes->bInheritHandle = FALSE;
    attributes->lpSecurityDescriptor = sd;
    *descriptor = sd;
    return true;
}

}  // namespace sak::win32mcp
