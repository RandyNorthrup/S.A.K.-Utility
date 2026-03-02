// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file uninstall_worker.cpp
/// @brief Executes the uninstall pipeline on a background thread

#include "sak/uninstall_worker.h"
#include "sak/leftover_scanner.h"
#include "sak/registry_snapshot_engine.h"
#include "sak/restore_point_manager.h"

#include <QProcess>
#include <QRegularExpression>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace sak {

namespace {
constexpr int kProcessStartTimeoutMs  = 10000;
constexpr int kCancellationPollMs      = 1000;
constexpr int kProcessKillWaitMs       = 5000;
constexpr int kUwpRemovalTimeoutMs     = 60000;
constexpr int kMsiRebootRequiredCode   = 3010;
}  // namespace

UninstallWorker::UninstallWorker(const ProgramInfo& program, Mode mode,
                                 ScanLevel scanLevel,
                                 bool createRestorePoint,
                                 QObject* parent)
    : WorkerBase(parent)
    , m_program(program)
    , m_mode(mode)
    , m_scanLevel(scanLevel)
    , m_createRestorePoint(createRestorePoint)
{
}

auto UninstallWorker::execute() -> std::expected<void, sak::error_code>
{
    UninstallReport report;
    report.programName = m_program.displayName;
    report.programVersion = m_program.displayVersion;
    report.programPublisher = m_program.publisher;
    report.startTime = QDateTime::currentDateTime();
    report.scanLevel = m_scanLevel;

    // Phase 1: Create restore point (if requested)
    if (m_createRestorePoint) {
        reportProgress(0, 100, "Creating system restore point...");
        if (createRestorePoint()) {
            report.restorePointCreated = true;
            report.restorePointName = QString("SAK: Before uninstall %1")
                .arg(m_program.displayName);
            Q_EMIT restorePointCreated(report.restorePointName);
        }
    }

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    // Phase 2: Handle by mode
    switch (m_mode) {
    case Mode::Standard: {
        // 2a: Capture registry snapshot (before state)
        reportProgress(10, 100, "Capturing registry snapshot...");
        [[maybe_unused]] auto snap_ok = captureRegistrySnapshot();
        Q_EMIT registrySnapshotCaptured();

        if (checkStop()) {
            return std::unexpected(sak::error_code::operation_cancelled);
        }

        // 2b: Run native uninstaller and wait for exit
        reportProgress(20, 100, "Running native uninstaller...");
        Q_EMIT nativeUninstallerStarted(m_program.displayName);

        if (!runNativeUninstaller()) {
            report.uninstallResult = UninstallReport::UninstallResult::Failed;
            report.endTime = QDateTime::currentDateTime();
            Q_EMIT uninstallComplete(report);
            return std::unexpected(sak::error_code::execution_failed);
        }

        report.uninstallResult = UninstallReport::UninstallResult::Success;
        Q_EMIT nativeUninstallerFinished(report.nativeExitCode);
        break;
    }

    case Mode::ForcedUninstall: {
        report.uninstallResult = UninstallReport::UninstallResult::Skipped;
        // Still capture snapshot for diff reference
        reportProgress(10, 100, "Capturing registry snapshot...");
        [[maybe_unused]] auto snap_ok_forced = captureRegistrySnapshot();
        Q_EMIT registrySnapshotCaptured();
        break;
    }

    case Mode::UwpRemove:
        reportProgress(20, 100, "Removing UWP package...");
        if (!removeUwpPackage()) {
            report.uninstallResult = UninstallReport::UninstallResult::Failed;
            report.endTime = QDateTime::currentDateTime();
            Q_EMIT uninstallComplete(report);
            return std::unexpected(sak::error_code::execution_failed);
        }
        report.uninstallResult = UninstallReport::UninstallResult::Success;
        report.endTime = QDateTime::currentDateTime();
        Q_EMIT uninstallComplete(report);
        return {};  // UWP — no leftover scan needed

    case Mode::RegistryOnly:
        reportProgress(50, 100, "Removing orphaned registry entry...");
        if (!removeRegistryEntry()) {
            report.uninstallResult = UninstallReport::UninstallResult::Failed;
            report.endTime = QDateTime::currentDateTime();
            Q_EMIT uninstallComplete(report);
            return std::unexpected(sak::error_code::execution_failed);
        }
        report.uninstallResult = UninstallReport::UninstallResult::Success;
        report.registryKeysDeleted = 1;
        report.endTime = QDateTime::currentDateTime();
        Q_EMIT uninstallComplete(report);
        return {};
    }

    if (checkStop()) {
        return std::unexpected(sak::error_code::operation_cancelled);
    }

    // Phase 3: Leftover scanning
    reportProgress(40, 100, "Scanning for leftovers...");
    Q_EMIT leftoverScanStarted(m_scanLevel);

    auto leftovers = scanLeftovers();
    report.foundLeftovers = leftovers;

    Q_EMIT leftoverScanFinished(leftovers);

    report.endTime = QDateTime::currentDateTime();
    Q_EMIT uninstallComplete(report);

    return {};
}

bool UninstallWorker::createRestorePoint()
{
    RestorePointManager mgr;
    QString desc = QString("SAK: Before uninstall %1")
        .arg(m_program.displayName.left(40));
    return mgr.createRestorePoint(desc);
}

bool UninstallWorker::captureRegistrySnapshot()
{
    m_registrySnapshotBefore = RegistrySnapshotEngine::captureSnapshot();
    return !m_registrySnapshotBefore.isEmpty();
}

bool UninstallWorker::runNativeUninstaller()
{
    QString cmd = m_program.uninstallString;
    if (cmd.isEmpty()) {
        return false;
    }

    // Check for MSI installer
    if (isMsiInstaller()) {
        cmd = buildMsiUninstallCommand();
    }

    QProcess proc;

    // Parse command into program + arguments
    QString exe;
    QStringList args;

    if (cmd.startsWith('"')) {
        // Quoted path
        int end_quote = cmd.indexOf('"', 1);
        if (end_quote > 0) {
            exe = cmd.mid(1, end_quote - 1);
            QString remainder = cmd.mid(end_quote + 1).trimmed();
            if (!remainder.isEmpty()) {
                // Split remaining args — handle quoted args
                QRegularExpression arg_re(R"(\"([^\"]*)\"|(\S+))");
                auto it = arg_re.globalMatch(remainder);
                while (it.hasNext()) {
                    auto match = it.next();
                    args.append(match.captured(1).isEmpty()
                                    ? match.captured(2) : match.captured(1));
                }
            }
        }
    } else {
        // Unquoted — find exe by looking for .exe
        int exe_end = cmd.indexOf(".exe", 0, Qt::CaseInsensitive);
        if (exe_end > 0) {
            exe = cmd.left(exe_end + 4);
            QString remainder = cmd.mid(exe_end + 4).trimmed();
            if (!remainder.isEmpty()) {
                QRegularExpression arg_re(R"(\"([^\"]*)\"|(\S+))");
                auto it = arg_re.globalMatch(remainder);
                while (it.hasNext()) {
                    auto match = it.next();
                    args.append(match.captured(1).isEmpty()
                                    ? match.captured(2) : match.captured(1));
                }
            }
        } else {
            // Fallback: treat entire string as command
            exe = cmd;
        }
    }

    proc.setProgram(exe);
    proc.setArguments(args);
    proc.start();

    if (!proc.waitForStarted(kProcessStartTimeoutMs)) {
        return false;
    }

    // Wait indefinitely for uninstaller — user may need to interact with it
    // Check for cancellation periodically
    while (!proc.waitForFinished(kCancellationPollMs)) {
        if (stopRequested()) {
            proc.kill();
            proc.waitForFinished(kProcessKillWaitMs);
            return false;
        }
    }

    return proc.exitStatus() == QProcess::NormalExit
        && (proc.exitCode() == 0 || proc.exitCode() == kMsiRebootRequiredCode);
}

QVector<LeftoverItem> UninstallWorker::scanLeftovers()
{
    LeftoverScanner scanner(m_program, m_scanLevel);

    // Capture after-snapshot for diff
    QSet<QString> snapshot_after;
    if (!m_registrySnapshotBefore.isEmpty()) {
        snapshot_after = RegistrySnapshotEngine::captureSnapshot();
    }

    // Provide a progress callback that emits our signal
    // and continuously bridges the WorkerBase stop flag to the scanner
    auto progress_cb = [this](const QString& path, int found) {
        m_scanStopFlag.store(stopRequested());
        Q_EMIT leftoverScanProgress(path, found);
    };

    // Initialize the stop flag from current state
    m_scanStopFlag.store(stopRequested());

    // Scan leftovers (scanner handles file system + registry pattern search)
    auto results = scanner.scan(m_scanStopFlag, progress_cb);

    // Add registry diff results from snapshot comparison
    // (scanner does pattern-based registry search; this adds snapshot-diff items)
    if (!m_registrySnapshotBefore.isEmpty() && !snapshot_after.isEmpty()) {
        QStringList patterns;
        patterns.append(m_program.displayName);
        if (!m_program.publisher.isEmpty()) {
            patterns.append(m_program.publisher);
        }

        auto diff_items = RegistrySnapshotEngine::diffSnapshots(
            m_registrySnapshotBefore, snapshot_after, patterns);

        // Deduplicate: only add diff items not already found by scanner
        QSet<QString> existing_paths;
        for (const auto& r : results) {
            existing_paths.insert(r.path);
        }
        for (const auto& di : diff_items) {
            if (!existing_paths.contains(di.path)) {
                results.append(di);
            }
        }
    }

    return results;
}

bool UninstallWorker::removeUwpPackage()
{
    if (m_program.packageFullName.isEmpty()) {
        return false;
    }

    QProcess ps;
    ps.setProgram("powershell.exe");

    if (m_program.source == ProgramInfo::Source::Provisioned) {
        // Remove provisioned package (all-users)
        ps.setArguments({
            "-NoProfile", "-NonInteractive", "-Command",
            QString("Remove-AppxProvisionedPackage -Online "
                    "-PackageName '%1' -ErrorAction Stop")
                .arg(m_program.packageFullName)
        });
    } else {
        // Remove per-user package
        ps.setArguments({
            "-NoProfile", "-NonInteractive", "-Command",
            QString("Remove-AppxPackage -Package '%1' -ErrorAction Stop")
                .arg(m_program.packageFullName)
        });
    }

    ps.start();

    if (!ps.waitForFinished(kUwpRemovalTimeoutMs)) {
        return false;
    }

    return ps.exitCode() == 0;
}

bool UninstallWorker::removeRegistryEntry()
{
#ifdef Q_OS_WIN
    if (m_program.registryKeyPath.isEmpty()) {
        return false;
    }

    // Parse the registry key path
    QString path = m_program.registryKeyPath;
    HKEY hive = nullptr;

    if (path.startsWith("HKLM\\")) {
        hive = HKEY_LOCAL_MACHINE;
        path = path.mid(5);
    } else if (path.startsWith("HKCU\\")) {
        hive = HKEY_CURRENT_USER;
        path = path.mid(5);
    } else {
        return false;
    }

    LONG rc = RegDeleteKeyExW(hive,
                              reinterpret_cast<LPCWSTR>(path.utf16()),
                              KEY_WOW64_64KEY, 0);

    return rc == ERROR_SUCCESS;
#else
    return false;
#endif
}

bool UninstallWorker::isMsiInstaller() const
{
    if (m_program.uninstallString.isEmpty()) {
        return false;
    }

    const QString lower = m_program.uninstallString.toLower();
    return lower.contains("msiexec")
        || lower.contains("msiexec.exe");
}

QString UninstallWorker::buildMsiUninstallCommand() const
{
    QString guid = extractGuidFromUninstallString();
    if (guid.isEmpty()) {
        return m_program.uninstallString;  // fallback to original
    }

    return QString("msiexec.exe /x %1 /norestart").arg(guid);
}

QString UninstallWorker::extractGuidFromUninstallString() const
{
    // Look for {GUID} pattern in uninstall string
    static const QRegularExpression guid_re(R"(\{[0-9A-Fa-f\-]{36}\})");
    auto match = guid_re.match(m_program.uninstallString);

    if (match.hasMatch()) {
        return match.captured(0);
    }
    return {};
}

} // namespace sak
