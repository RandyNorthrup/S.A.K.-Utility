// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file uninstall_worker.cpp
/// @brief Executes the uninstall pipeline on a background thread

#include "sak/uninstall_worker.h"

#include "sak/layout_constants.h"
#include "sak/leftover_scanner.h"
#include "sak/process_runner.h"
#include "sak/registry_snapshot_engine.h"
#include "sak/restore_point_manager.h"

#include <QRegularExpression>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {
constexpr int kProgressSnapshot = 10;
constexpr int kProgressNativeUninstaller = 20;
constexpr int kProgressLeftoverScan = 40;
constexpr int kProgressRegistryRemoval = 50;
constexpr int kRestorePointProgramNameMaxChars = 40;
constexpr int kRegistryHivePrefixLength = 5;
constexpr int kUwpRemovalTimeoutMs = 60'000;
// Headless silent uninstall: a hard ceiling so a /qn uninstaller that still stalls cannot hang
// the run forever (the GUI path waits indefinitely because a human is watching). Generous so a
// legitimately long silent uninstall of a large product is not killed mid-removal.
constexpr int kHeadlessUninstallTimeoutMs = 20 * 60 * 1000;
constexpr int kMsiRebootRequiredCode = 3010;
constexpr int kQuotedCaptureGroup = 1;
constexpr int kUnquotedCaptureGroup = 2;
constexpr int kExecutableExtensionLength = 4;
constexpr auto kQuotedArgumentPattern = "\\\"([^\\\"]*)\\\"|(\\S+)";

struct ParsedCommand {
    QString exe;
    QStringList args;
};

[[nodiscard]] QStringList splitArgsRespectingQuotes(const QString& remainder) {
    const auto trimmed = remainder.trimmed();
    if (trimmed.isEmpty()) {
        return {};
    }

    static const QRegularExpression kArgRe(QString::fromLatin1(kQuotedArgumentPattern));
    QStringList args;
    auto it = kArgRe.globalMatch(trimmed);
    while (it.hasNext()) {
        const auto match = it.next();
        args.append(match.captured(kQuotedCaptureGroup).isEmpty()
                        ? match.captured(kUnquotedCaptureGroup)
                        : match.captured(kQuotedCaptureGroup));
    }
    return args;
}

[[nodiscard]] ParsedCommand parseUninstallCommand(const QString& cmd) {
    ParsedCommand parsed;
    if (cmd.isEmpty()) {
        return parsed;
    }

    if (cmd.startsWith('"')) {
        const int endQuote = cmd.indexOf('"', 1);
        if (endQuote <= 0) {
            return parsed;
        }
        parsed.exe = cmd.mid(1, endQuote - 1);
        parsed.args = splitArgsRespectingQuotes(cmd.mid(endQuote + 1));
        return parsed;
    }

    const int exeEnd = cmd.indexOf(".exe", 0, Qt::CaseInsensitive);
    if (exeEnd > 0) {
        parsed.exe = cmd.left(exeEnd + kExecutableExtensionLength);
        parsed.args = splitArgsRespectingQuotes(cmd.mid(exeEnd + kExecutableExtensionLength));
        return parsed;
    }

    parsed.exe = cmd;
    return parsed;
}

}  // namespace

UninstallWorker::UninstallWorker(const ProgramInfo& program,
                                 Mode mode,
                                 ScanLevel scanLevel,
                                 bool createRestorePoint,
                                 QObject* parent)
    : WorkerBase(parent)
    , m_program(program)
    , m_mode(mode)
    , m_scanLevel(scanLevel)
    , m_createRestorePoint(createRestorePoint) {}

auto UninstallWorker::executeStandardMode(UninstallReport& report)
    -> std::expected<void, sak::error_code> {
    reportProgress(kProgressSnapshot, sak::kPercentMax, "Capturing registry snapshot...");
    captureSnapshotOrWarn(report);

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    reportProgress(kProgressNativeUninstaller, sak::kPercentMax, "Running native uninstaller...");
    Q_EMIT nativeUninstallerStarted(m_program.displayName);

    int exit_code = 0;
    if (!runNativeUninstaller(exit_code)) {
        // Capture the failing code (surfaced in the report), then return only: WorkerBase::run()
        // emits the single terminal `failed` signal. Emitting uninstallComplete here too gave the
        // batch orchestrator two terminal outcomes.
        report.nativeExitCode = exit_code;
        return std::unexpected(sak::error_code::execution_failed);
    }

    // Copy the native exit code so a 3010 (reboot-required success) reaches the report/UI.
    report.nativeExitCode = exit_code;
    report.uninstallResult = UninstallReport::UninstallResult::Success;
    Q_EMIT nativeUninstallerFinished(report.nativeExitCode);
    return {};
}

auto UninstallWorker::executeUwpMode(UninstallReport& report)
    -> std::expected<void, sak::error_code> {
    reportProgress(kProgressNativeUninstaller, sak::kPercentMax, "Removing UWP package...");
    if (!removeUwpPackage()) {
        return std::unexpected(sak::error_code::execution_failed);  // run() emits `failed` once
    }
    report.uninstallResult = UninstallReport::UninstallResult::Success;
    report.endTime = QDateTime::currentDateTime();
    Q_EMIT uninstallComplete(report);
    return {};
}

auto UninstallWorker::executeRegistryMode(UninstallReport& report)
    -> std::expected<void, sak::error_code> {
    reportProgress(kProgressRegistryRemoval,
                   sak::kPercentMax,
                   "Removing orphaned registry entry...");
    if (!removeRegistryEntry()) {
        return std::unexpected(sak::error_code::execution_failed);  // run() emits `failed` once
    }
    report.uninstallResult = UninstallReport::UninstallResult::Success;
    report.registryKeysDeleted = 1;
    report.endTime = QDateTime::currentDateTime();
    Q_EMIT uninstallComplete(report);
    return {};
}

auto UninstallWorker::runRestorePointPhase(UninstallReport& report)
    -> std::expected<void, sak::error_code> {
    if (!m_createRestorePoint) {
        return {};
    }
    reportProgress(0, sak::kPercentMax, "Creating system restore point...");
    if (!createRestorePoint()) {
        // FAIL CLOSED: the user asked for a rollback point before this (potentially destructive)
        // uninstall. If it cannot be created, refuse rather than run with no way back.
        return std::unexpected(sak::error_code::execution_failed);
    }
    report.restorePointCreated = true;
    report.restorePointName = QString("SAK: Before uninstall %1").arg(m_program.displayName);
    Q_EMIT restorePointCreated(report.restorePointName);
    return {};
}

void UninstallWorker::captureSnapshotOrWarn(UninstallReport& report) {
    if (captureRegistrySnapshot()) {
        Q_EMIT registrySnapshotCaptured();
        return;
    }
    // Fail-closed reporting: an empty/failed before-snapshot means the leftover diff is degraded.
    // Surface it instead of signalling a snapshot that was never captured.
    report.errorLog.append(
        QStringLiteral("Registry snapshot capture failed or was empty; "
                       "leftover detection may be incomplete."));
}

auto UninstallWorker::runLeftoverPhase(UninstallReport& report)
    -> std::expected<void, sak::error_code> {
    reportProgress(kProgressLeftoverScan, sak::kPercentMax, "Scanning for leftovers...");
    Q_EMIT leftoverScanStarted(m_scanLevel);

    auto leftovers = scanLeftovers();
    report.foundLeftovers = leftovers;

    // A cancel during the (possibly long) scan must not be reported as a completed uninstall:
    // return operation_cancelled BEFORE the terminal uninstallComplete so run() emits a single
    // `cancelled` signal instead of handing the orchestrator both a success and a cancel.
    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    Q_EMIT leftoverScanFinished(leftovers);
    report.endTime = QDateTime::currentDateTime();
    Q_EMIT uninstallComplete(report);
    return {};
}

auto UninstallWorker::execute() -> std::expected<void, sak::error_code> {
    UninstallReport report;
    report.programName = m_program.displayName;
    report.programVersion = m_program.displayVersion;
    report.programPublisher = m_program.publisher;
    report.startTime = QDateTime::currentDateTime();
    report.scanLevel = m_scanLevel;

    // Phase 1: Create restore point (if requested) -- fails closed if it cannot be created.
    if (auto rp = runRestorePointPhase(report); !rp) {
        return rp;
    }

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    // Phase 2: Handle by mode
    switch (m_mode) {
    case Mode::Standard: {
        auto result = executeStandardMode(report);
        if (!result) {
            return result;
        }
        break;
    }
    case Mode::ForcedUninstall: {
        report.uninstallResult = UninstallReport::UninstallResult::Skipped;
        reportProgress(kProgressSnapshot, sak::kPercentMax, "Capturing registry snapshot...");
        captureSnapshotOrWarn(report);
        break;
    }
    case Mode::UwpRemove:
        return executeUwpMode(report);
    case Mode::RegistryOnly:
        return executeRegistryMode(report);
    }

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    // Phase 3: Leftover scanning (returns operation_cancelled if stopped mid-scan).
    return runLeftoverPhase(report);
}

bool UninstallWorker::createRestorePoint() {
    RestorePointManager mgr;
    QString desc = QString("SAK: Before uninstall %1")
                       .arg(m_program.displayName.left(kRestorePointProgramNameMaxChars));
    return mgr.createRestorePoint(desc);
}

bool UninstallWorker::captureRegistrySnapshot() {
    m_registrySnapshotBefore = RegistrySnapshotEngine::captureSnapshot();
    return !m_registrySnapshotBefore.isEmpty();
}

bool UninstallWorker::buildSilentUninstallCommand(const ProgramInfo& program, QString& cmdOut) {
    // Prefer the publisher-provided silent command.
    if (!program.quietUninstallString.isEmpty()) {
        cmdOut = program.quietUninstallString;
        return true;
    }
    // MSI: build a fully-silent removal from the product GUID (/qn = no UI, /norestart).
    if (program.uninstallString.toLower().contains(QLatin1String("msiexec"))) {
        static const QRegularExpression guid_re(QStringLiteral(R"(\{[0-9A-Fa-f\-]{36}\})"));
        const auto match = guid_re.match(program.uninstallString);
        if (match.hasMatch()) {
            cmdOut = QStringLiteral("msiexec.exe /x %1 /qn /norestart").arg(match.captured(0));
            return true;
        }
    }
    // No reliable silent path: a bare interactive uninstallString would hang a headless run.
    return false;
}

bool UninstallWorker::nativeUninstallSucceeded(int exitStatus, int exitCode, bool cancelled) {
    return !cancelled && exitStatus == 0 && (exitCode == 0 || exitCode == kMsiRebootRequiredCode);
}

bool UninstallWorker::runNativeUninstaller(int& exitCode) {
    exitCode = 0;
    QString cmd;
    int timeout_ms = 0;  // GUI: wait indefinitely -- a user may interact with the uninstaller.
    if (m_headlessSilent) {
        if (!buildSilentUninstallCommand(m_program, cmd)) {
            return false;  // refuse: no silent command, never launch an interactive uninstaller
        }
        timeout_ms = kHeadlessUninstallTimeoutMs;
    } else {
        cmd = m_program.uninstallString;
        if (cmd.isEmpty()) {
            return false;
        }
        if (isMsiInstaller()) {
            cmd = buildMsiUninstallCommand();
        }
    }

    const auto parsed = parseUninstallCommand(cmd);
    if (parsed.exe.isEmpty()) {
        return false;
    }

    const auto result =
        sak::runProcess(parsed.exe, parsed.args, timeout_ms, [this]() { return stopRequested(); });

    exitCode = result.exit_code;
    return nativeUninstallSucceeded(result.exit_status, result.exit_code, result.cancelled);
}

QVector<LeftoverItem> UninstallWorker::scanLeftovers() {
    LeftoverScanner scanner(m_program, m_scanLevel, m_registrySnapshotBefore);

    m_scanStopFlag.store(stopRequested());

    auto progress_cb = [this](const QString& path, int found) {
        m_scanStopFlag.store(stopRequested());
        Q_EMIT leftoverScanProgress(path, found);
    };

    return scanner.scan(m_scanStopFlag, progress_cb);
}

bool UninstallWorker::removeUwpPackage() {
    if (m_program.packageFullName.isEmpty()) {
        return false;
    }

    QString script;
    if (m_program.source == ProgramInfo::Source::Provisioned) {
        // Remove provisioned package (all-users)
        script = QString(
                     "Remove-AppxProvisionedPackage -Online "
                     "-PackageName '%1' -ErrorAction Stop")
                     .arg(m_program.packageFullName);
    } else {
        // Remove per-user package
        script = QString("Remove-AppxPackage -Package '%1' -ErrorAction Stop")
                     .arg(m_program.packageFullName);
    }

    const auto result = sak::runPowerShell(script, kUwpRemovalTimeoutMs, true, false, [this]() {
        return stopRequested();
    });
    return result.succeeded();
}

bool UninstallWorker::removeRegistryEntry() {
#ifdef Q_OS_WIN
    if (m_program.registryKeyPath.isEmpty()) {
        return false;
    }

    // Parse the registry key path
    QString path = m_program.registryKeyPath;
    HKEY hive = nullptr;

    if (path.startsWith("HKLM\\")) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(kRegistryHivePrefixLength);
    } else if (path.startsWith("HKCU\\")) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(kRegistryHivePrefixLength);
    } else {
        return false;
    }

    LONG rc = RegDeleteKeyExW(hive, reinterpret_cast<LPCWSTR>(path.utf16()), KEY_WOW64_64KEY, 0);

    return rc == ERROR_SUCCESS;
#else
    return false;
#endif
}

bool UninstallWorker::isMsiInstaller() const {
    if (m_program.uninstallString.isEmpty()) {
        return false;
    }

    const QString lower = m_program.uninstallString.toLower();
    return lower.contains("msiexec") || lower.contains("msiexec.exe");
}

QString UninstallWorker::buildMsiUninstallCommand() const {
    QString guid = extractGuidFromUninstallString();
    if (guid.isEmpty()) {
        return m_program.uninstallString;  // fallback to original
    }

    return QString("msiexec.exe /x %1 /norestart").arg(guid);
}

QString UninstallWorker::extractGuidFromUninstallString() const {
    // Look for {GUID} pattern in uninstall string
    static const QRegularExpression guid_re(R"(\{[0-9A-Fa-f\-]{36}\})");
    auto match = guid_re.match(m_program.uninstallString);

    if (match.hasMatch()) {
        return match.captured(0);
    }
    return {};
}

}  // namespace sak
