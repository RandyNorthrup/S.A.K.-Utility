// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file microsoft_defender_action.cpp
/// @brief Microsoft Defender quick scan, full scan and signature update through MpCmdRun.exe

#include "sak/actions/microsoft_defender_action.h"

#include "sak/action_constants.h"
#include "sak/layout_constants.h"
#include "sak/process_runner.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <string>

#include <windows.h>

#include <knownfolders.h>
#include <shlobj.h>

namespace sak {

namespace {

constexpr wchar_t kDefenderRegistryKey[] = L"SOFTWARE\\Microsoft\\Windows Defender";
constexpr wchar_t kInstallLocationValue[] = L"InstallLocation";
constexpr auto kMpCmdRunFileName = "MpCmdRun.exe";
// Model- and log-facing detail from MpCmdRun's own output, kept short: the full text is in the
// result log, the message only needs enough to tell the failures apart.
constexpr int kOutputDetailMaxChars = 400;
constexpr int kExitCodeHexBase = 16;
constexpr int kExitCodeHexDigits = 8;  // a full 32-bit code, so HRESULTs read as written
constexpr int kMillisecondsPerMinute = sak::kMillisecondsPerSecond * sak::kSecondsPerMinute;

[[nodiscard]] bool isScan(DefenderOperation operation) {
    return operation != DefenderOperation::SignatureUpdate;
}

// MpCmdRun reports through stdout as often as stderr, so the detail is whichever has text.
[[nodiscard]] QString outputDetail(const ProcessResult& result) {
    QString detail = result.std_err.trimmed();
    if (detail.isEmpty()) {
        detail = result.std_out.trimmed();
    }
    detail.replace(QLatin1Char('\r'), QLatin1Char(' '));
    detail.replace(QLatin1Char('\n'), QLatin1Char(' '));
    return detail.left(kOutputDetailMaxChars);
}

[[nodiscard]] QString exitCodeText(int exit_code) {
    return QStringLiteral("%1 (0x%2)")
        .arg(exit_code)
        .arg(static_cast<quint32>(exit_code),
             kExitCodeHexDigits,
             kExitCodeHexBase,
             QLatin1Char('0'));
}

[[nodiscard]] QString withDetail(QString message, const ProcessResult& result) {
    const QString detail = outputDetail(result);
    if (!detail.isEmpty()) {
        message += QStringLiteral(" MpCmdRun output: %1").arg(detail);
    }
    return message;
}

[[nodiscard]] std::expected<QString, QString> readDefenderInstallLocation() {
    DWORD bytes = 0;
    LSTATUS rc = RegGetValueW(HKEY_LOCAL_MACHINE,
                              kDefenderRegistryKey,
                              kInstallLocationValue,
                              RRF_RT_REG_SZ,
                              nullptr,
                              nullptr,
                              &bytes);
    if (rc == ERROR_FILE_NOT_FOUND) {
        return std::unexpected(QStringLiteral(
            "Microsoft Defender has no registered install location "
            "(HKLM\\SOFTWARE\\Microsoft\\Windows Defender\\InstallLocation is missing). Defender "
            "may be removed or turned off in favour of another antivirus."));
    }
    std::wstring buffer(bytes / sizeof(wchar_t) + 1, L'\0');
    if (rc == ERROR_SUCCESS) {
        rc = RegGetValueW(HKEY_LOCAL_MACHINE,
                          kDefenderRegistryKey,
                          kInstallLocationValue,
                          RRF_RT_REG_SZ,
                          nullptr,
                          buffer.data(),
                          &bytes);
    }
    if (rc != ERROR_SUCCESS) {
        return std::unexpected(
            QStringLiteral("Cannot read Microsoft Defender's install location (error %1)").arg(rc));
    }
    return QString::fromWCharArray(buffer.c_str());
}

[[nodiscard]] std::expected<QString, QString> knownFolder(REFKNOWNFOLDERID folder_id,
                                                          const QString& label) {
    PWSTR raw = nullptr;
    const HRESULT hr = SHGetKnownFolderPath(folder_id, KF_FLAG_DEFAULT, nullptr, &raw);
    const QString path = (SUCCEEDED(hr) && raw != nullptr) ? QString::fromWCharArray(raw)
                                                           : QString();
    CoTaskMemFree(raw);
    if (path.isEmpty()) {
        return std::unexpected(QStringLiteral("Cannot resolve the %1 folder (HRESULT 0x%2)")
                                   .arg(label)
                                   .arg(static_cast<quint32>(hr),
                                        kExitCodeHexDigits,
                                        kExitCodeHexBase,
                                        QLatin1Char('0')));
    }
    return path;
}

// The platform folder Defender itself registers, checked against the two places Defender
// installs MpCmdRun.exe, then required to hold a real file. Every miss is reported, never
// replaced by a guessed path.
[[nodiscard]] std::expected<QString, QString> resolveMpCmdRun() {
    const auto install_location = readDefenderInstallLocation();
    if (!install_location) {
        return std::unexpected(install_location.error());
    }
    const auto program_data = knownFolder(FOLDERID_ProgramData, QStringLiteral("ProgramData"));
    if (!program_data) {
        return std::unexpected(program_data.error());
    }
    const auto program_files = knownFolder(FOLDERID_ProgramFiles, QStringLiteral("Program Files"));
    if (!program_files) {
        return std::unexpected(program_files.error());
    }
    const auto path = MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
        *install_location,
        QDir(*program_data).filePath(QStringLiteral("Microsoft/Windows Defender/Platform")),
        QDir(*program_files).filePath(QStringLiteral("Windows Defender")));
    if (!path) {
        return path;
    }
    const QFileInfo info(*path);
    if (!info.isFile() || info.isSymLink()) {
        return std::unexpected(
            QStringLiteral("MpCmdRun.exe is not present in Microsoft Defender's registered "
                           "install location: %1")
                .arg(*path));
    }
    return path;
}

}  // namespace

MicrosoftDefenderAction::MicrosoftDefenderAction(DefenderOperation operation, QObject* parent)
    : QuickAction(parent), m_operation(operation) {}

QString MicrosoftDefenderAction::operationName(DefenderOperation operation) {
    switch (operation) {
    case DefenderOperation::QuickScan:
        return QString::fromLatin1(quick_action_elevation::kDefenderQuickScanName);
    case DefenderOperation::FullScan:
        return QString::fromLatin1(quick_action_elevation::kDefenderFullScanName);
    case DefenderOperation::SignatureUpdate:
        return QString::fromLatin1(quick_action_elevation::kDefenderSignatureUpdateName);
    }
    return {};
}

QString MicrosoftDefenderAction::name() const {
    return operationName(m_operation);
}

QString MicrosoftDefenderAction::description() const {
    switch (m_operation) {
    case DefenderOperation::QuickScan:
        return QStringLiteral(
            "Run a Microsoft Defender quick scan (MpCmdRun.exe). Reports clean, threats needing "
            "attention, or failure.");
    case DefenderOperation::FullScan:
        return QStringLiteral(
            "Run a Microsoft Defender full scan of every drive (MpCmdRun.exe); can take hours. "
            "Reports clean, threats needing attention, or failure.");
    case DefenderOperation::SignatureUpdate:
        return QStringLiteral(
            "Update Microsoft Defender security intelligence (signatures) with MpCmdRun.exe.");
    }
    return {};
}

int MicrosoftDefenderAction::timeoutMs(DefenderOperation operation) {
    switch (operation) {
    case DefenderOperation::QuickScan:
        return kDefenderQuickScanTimeoutMs;
    case DefenderOperation::FullScan:
        return kDefenderFullScanTimeoutMs;
    case DefenderOperation::SignatureUpdate:
        return kDefenderSignatureUpdateTimeoutMs;
    }
    return kDefenderSignatureUpdateTimeoutMs;
}

QStringList MicrosoftDefenderAction::mpCmdRunArguments(DefenderOperation operation) {
    switch (operation) {
    case DefenderOperation::QuickScan:
        return {QStringLiteral("-Scan"), QStringLiteral("-ScanType"), QStringLiteral("1")};
    case DefenderOperation::FullScan:
        return {QStringLiteral("-Scan"), QStringLiteral("-ScanType"), QStringLiteral("2")};
    case DefenderOperation::SignatureUpdate:
        return {QStringLiteral("-SignatureUpdate")};
    }
    return {};
}

MicrosoftDefenderAction::Verdict MicrosoftDefenderAction::verdictFor(DefenderOperation operation,
                                                                     const ProcessResult& result) {
    const QString what = operationName(operation);
    if (result.cancelled) {
        return {DefenderOutcome::Cancelled, false, QStringLiteral("%1 was cancelled.").arg(what)};
    }
    if (result.timed_out) {
        return {DefenderOutcome::TimedOut,
                false,
                QStringLiteral("%1 did not finish within %2 minutes and was stopped.")
                    .arg(what)
                    .arg(timeoutMs(operation) / kMillisecondsPerMinute)};
    }
    if (result.exit_status != 0) {
        return {
            DefenderOutcome::Failed,
            false,
            withDetail(QStringLiteral("%1 failed: MpCmdRun.exe terminated abnormally.").arg(what),
                       result)};
    }
    if (result.exit_code == 0) {
        return isScan(operation)
                   ? Verdict{DefenderOutcome::Clean,
                             true,
                             QStringLiteral("%1 finished: no threats found, or every threat found "
                                            "was remediated.")
                                 .arg(what)}
                   : Verdict{DefenderOutcome::SignaturesUpdated,
                             true,
                             QStringLiteral("%1 finished: security intelligence is up to date.")
                                 .arg(what)};
    }
    if (isScan(operation) && result.exit_code == kMpCmdRunScanAttentionExitCode) {
        return {DefenderOutcome::ThreatsNeedAttention,
                false,
                QStringLiteral("%1 finished with exit code 2: Microsoft Defender found malware "
                               "that was not remediated or needs user action, or the scan hit "
                               "errors. Review Windows Security > Virus & threat protection > "
                               "Protection history.")
                    .arg(what)};
    }
    return {DefenderOutcome::Failed,
            false,
            withDetail(QStringLiteral("%1 failed: MpCmdRun.exe exited with code %2.")
                           .arg(what, exitCodeText(result.exit_code)),
                       result)};
}

std::expected<QString, QString> MicrosoftDefenderAction::mpCmdRunPathFromInstallLocation(
    const QString& install_location, const QString& platform_root, const QString& program_root) {
    const QString raw = QDir::fromNativeSeparators(install_location.trimmed());
    if (raw.isEmpty()) {
        return std::unexpected(
            QStringLiteral("Microsoft Defender's registered install location is empty"));
    }
    if (!QDir::isAbsolutePath(raw) || raw.startsWith(QStringLiteral("//"))) {
        return std::unexpected(
            QStringLiteral("Microsoft Defender's registered install location is not an absolute "
                           "local path: %1")
                .arg(install_location));
    }
    const QString folder = QDir::cleanPath(raw);
    const QString platform = QDir::cleanPath(QDir::fromNativeSeparators(platform_root));
    const QString program = QDir::cleanPath(QDir::fromNativeSeparators(program_root));
    // A platform update installs into one version folder directly under Platform; the folder
    // name itself (4.18.x-y) is Defender's to choose and is taken as registered.
    const QString platform_prefix = platform + QLatin1Char('/');
    const bool version_folder = !platform_root.trimmed().isEmpty() &&
                                folder.startsWith(platform_prefix, Qt::CaseInsensitive) &&
                                !folder.mid(platform_prefix.size()).isEmpty() &&
                                !folder.mid(platform_prefix.size()).contains(QLatin1Char('/'));
    const bool inbox_folder = !program_root.trimmed().isEmpty() &&
                              folder.compare(program, Qt::CaseInsensitive) == 0;
    if (!version_folder && !inbox_folder) {
        return std::unexpected(
            QStringLiteral("Microsoft Defender's registered install location is not one of its "
                           "own folders: %1")
                .arg(install_location));
    }
    return QDir::toNativeSeparators(folder + QLatin1Char('/') +
                                    QString::fromLatin1(kMpCmdRunFileName));
}

void MicrosoftDefenderAction::scan() {
    setStatus(ActionStatus::Scanning);
    ScanResult result;
    result.applicable = true;
    result.summary = QStringLiteral("Ready: %1").arg(name());
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void MicrosoftDefenderAction::execute() {
    const QDateTime started = QDateTime::currentDateTime();
    if (isCancelled()) {
        emitCancelledResult(QStringLiteral("%1 was cancelled.").arg(name()), started);
        return;
    }
    setStatus(ActionStatus::Running);
    Q_EMIT executionProgress(QStringLiteral("Locating Microsoft Defender..."), progress::kStep5);
    const auto mpcmdrun = resolveMpCmdRun();
    if (!mpcmdrun) {
        emitFailedResult(QStringLiteral("%1 could not start: %2").arg(name(), mpcmdrun.error()),
                         mpcmdrun.error(),
                         started);
        return;
    }
    const ExecutionResult result = runMpCmdRun(*mpcmdrun, started);
    if (isCancelled()) {
        finishWithResult(result, ActionStatus::Cancelled);
        return;
    }
    finishWithResult(result, result.success ? ActionStatus::Success : ActionStatus::Failed);
}

QuickAction::ExecutionResult MicrosoftDefenderAction::runMpCmdRun(const QString& mpcmdrun,
                                                                  const QDateTime& started) {
    Q_EMIT executionProgress(QStringLiteral("%1 running...").arg(name()), progress::kStep10);
    const ProcessResult proc =
        runProcess(mpcmdrun, mpCmdRunArguments(m_operation), timeoutMs(m_operation), [this]() {
            return isCancelled();
        });
    QStringList log_lines{QStringLiteral("MpCmdRun: %1 %2")
                              .arg(mpcmdrun, mpCmdRunArguments(m_operation).join(QLatin1Char(' '))),
                          QStringLiteral("Exit code: %1").arg(exitCodeText(proc.exit_code))};
    // Stopping MpCmdRun.exe does not stop a scan the Defender service is running for it. A scan
    // that was cancelled or ran out of time is cancelled in the service too, so "stopped" means
    // the scan really stopped.
    if (isScan(m_operation) && (proc.cancelled || proc.timed_out)) {
        cancelServiceScan(mpcmdrun, &log_lines);
    }
    const Verdict verdict = verdictFor(m_operation, proc);
    if (!proc.std_out.trimmed().isEmpty()) {
        log_lines << QStringLiteral("Output:") << proc.std_out.trimmed();
    }
    if (!proc.std_err.trimmed().isEmpty()) {
        log_lines << QStringLiteral("Errors:") << proc.std_err.trimmed();
    }
    Q_EMIT executionProgress(verdict.message, progress::kComplete);

    ExecutionResult result;
    result.success = verdict.success;
    result.message = verdict.message;
    result.duration_ms = started.msecsTo(QDateTime::currentDateTime());
    result.log = log_lines.join(QLatin1Char('\n'));
    return result;
}

void MicrosoftDefenderAction::cancelServiceScan(const QString& mpcmdrun,
                                                QStringList* log_lines) const {
    const ProcessResult cancel = runProcess(mpcmdrun,
                                            {QStringLiteral("-Scan"), QStringLiteral("-Cancel")},
                                            kDefenderScanCancelTimeoutMs);
    log_lines->append(cancel.completedSuccessfully()
                          ? QStringLiteral("Defender service scan cancelled (MpCmdRun -Scan "
                                           "-Cancel)")
                          : withDetail(QStringLiteral("MpCmdRun -Scan -Cancel did not confirm the "
                                                      "service scan stopped (exit code %1).")
                                           .arg(exitCodeText(cancel.exit_code)),
                                       cancel));
}

}  // namespace sak
