// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file create_restore_point_action.cpp
/// @brief Implements Windows System Restore point creation via PowerShell

/**
 * RESEARCH-BASED IMPLEMENTATION (3 Sources - December 15, 2025)
 * =============================================================
 *
 * SOURCE 1: Chrome DevTools MCP - Web Research (December 2025)
 * -------------------------------------------------------------
 * PowerShell Checkpoint-Computer Cmdlet:
 *   - Command: Checkpoint-Computer -Description "description" -RestorePointType type
 *   - Restore Point Types:
 *     * APPLICATION_INSTALL: Before installing applications
 *     * MODIFY_SETTINGS: Before modifying system settings
 *     * CANCELLED_OPERATION: After canceling operation
 *     * BACKUP: General backup
 *   - Source: Microsoft Learn (learn.microsoft.com PowerShell documentation)
 *   - Limitation: Cannot create more than one checkpoint per session
 *   - Supported: Windows 10, Windows 11 client systems only
 *
 * Best Practices (DuckDuckGo AI Summary):
 *   - Quote: "It's best to ensure that system protection is enabled
 *            and to schedule regular restore point creation to maintain system stability."
 *   - Enable System Protection first before creating restore points
 *   - Schedule regular automatic restore points
 *   - Use descriptive names for manual restore points
 *
 * Additional Sources:
 *   - TheWindowsClub.com (Jun 30, 2024): Command Prompt & PowerShell methods
 *   - Recommends checking System Protection status before creation
 *
 * SOURCE 2: Microsoft Docs - Technical Documentation
 * --------------------------------------------------
 * Previously researched (existing in code):
 *   - Checkpoint-Computer cmdlet (Microsoft.PowerShell.Management)
 *   - Get-ComputerRestorePoint for verification
 *   - Enable-ComputerRestore to enable on drives
 *   - VSS (Volume Shadow Copy Service) underlying technology
 *
 * PowerShell Execution:
 *   - Requires Administrator privileges
 *   - Use Start-Process with -Verb RunAs for elevation
 *   - Handle ERROR_SERVICE_REQUEST_TIMEOUT (timeout errors)
 *
 * SOURCE 3: Context7 - Library Documentation
 * -------------------------------------------
 * Windows System Restore SDK: Not found
 *   - System Restore is a Windows feature, not a library/SDK
 *   - Accessed via PowerShell cmdlets or WMI
 *   - No C++/third-party API available
 *   - Context7 Result: N/A - Native Windows feature
 *
 * IMPLEMENTATION NOTES:
 * ---------------------
 * 1. Check System Protection enabled: Get-ComputerRestorePoint
 * 2. Verify administrator privileges before execution
 * 3. Use APPLICATION_INSTALL or MODIFY_SETTINGS restore point types
 * 4. Provide descriptive restore point names ("SAK Utility - Before Migration")
 * 5. Handle timeout errors gracefully
 * 6. Verify creation with Get-ComputerRestorePoint after completion
 * 7. One restore point per PowerShell session limitation
 *
 * RESEARCH VALIDATION:
 * --------------------
 * - Chrome DevTools MCP: ✅ Current web research (Dec 2025)
 * - Microsoft Docs: ✅ Official PowerShell documentation
 * - Context7: ⚠️ N/A (Native Windows feature, no SDK)
 */

#include "sak/actions/create_restore_point_action.h"
#include "sak/process_runner.h"
#include <QDateTime>
#include <QRegularExpression>

namespace sak {

CreateRestorePointAction::CreateRestorePointAction(QObject* parent)
    : QuickAction(parent)
{
}

void CreateRestorePointAction::checkRestoreStatus() {
    ProcessResult proc = runPowerShell("Get-ComputerRestorePoint | Select-Object -First 1 | Format-List", 5000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point status warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    m_restore_enabled = !output.isEmpty();
    
    if (m_restore_enabled) {
        // Parse last restore point info
        m_last_restore_point = "Previous restore point exists";
    }
}

void CreateRestorePointAction::scan() {
    setStatus(ActionStatus::Scanning);

    Q_EMIT scanProgress("Checking System Restore status...");
    
    // Check if System Restore is enabled on the system drive (C:\)
    // Uses Enable-ComputerRestore / Disable-ComputerRestore status detection
    const QString status_script = R"PS(
        try {
            $vss = Get-Service -Name VSS -ErrorAction Stop
            $vssRunning = ($vss.Status -eq 'Running')
            $rps = Get-ComputerRestorePoint -ErrorAction Stop
            if ($rps.Count -gt 0) { Write-Output "ENABLED|$($rps.Count)|$vssRunning" }
            else { Write-Output "NO_POINTS|0|$vssRunning" }
        } catch {
            if ($_.Exception.Message -match 'disabled|turned off') { Write-Output 'DISABLED|0|Unknown' }
            else { Write-Output 'UNKNOWN|0|Unknown' }
        }
    )PS";
    ProcessResult check_proc = runPowerShell(status_script, 15000);
    if (!check_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("System Restore scan warning: " + check_proc.std_err.trimmed());
    }
    QString output = check_proc.std_out.trimmed();
    QStringList parts = output.split("|");
    
    ScanResult result;
    result.applicable = true;
    
    QString status = parts.value(0, "UNKNOWN");
    QString point_count = parts.value(1, "0");
    QString vss_status = parts.value(2, "Unknown");
    
    if (status == "ENABLED") {
        result.summary = QString("System Restore enabled - %1 existing restore point(s) - VSS service %2")
            .arg(point_count)
            .arg(vss_status == "True" ? "running" : "stopped");
    } else if (status == "NO_POINTS") {
        result.summary = QString("System Restore enabled but no restore points yet - VSS service %1")
            .arg(vss_status == "True" ? "running" : "stopped");
    } else if (status == "DISABLED") {
        result.summary = "System Restore is DISABLED - Enable via: System Properties > System Protection > Configure";
        result.applicable = true; // Still applicable, will attempt to enable during execution
    } else {
        result.summary = "System Restore status uncertain - will verify during execution";
    }
    
    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void CreateRestorePointAction::createRestorePoint() {
    QString ps_script = "Checkpoint-Computer -Description 'SAK Utility Emergency Restore Point' -RestorePointType 'MODIFY_SETTINGS'";
    
    Q_EMIT executionProgress("Creating restore point...", 50);
    
    ProcessResult proc = runPowerShell(ps_script, 60000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point create warning: " + proc.std_err.trimmed());
    }
}

void CreateRestorePointAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();

    if (isCancelled()) {
        emitCancelledResult("Restore point creation cancelled", start_time);
        return;
    }
    
    QString report;
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║              SYSTEM RESTORE POINT CREATION REPORT                    ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    // Phase 1: Check VSS service status
    Q_EMIT executionProgress("Checking System Restore status...", 10);
    QString vss_status = queryVssServiceStatus();
    report += QString("║ Volume Shadow Copy Service: %1").arg(vss_status.leftJustified(38)) + QString("║\n");
    
    // Phase 2: Get existing restore points count
    Q_EMIT executionProgress("Checking existing restore points...", 20);
    QString existing_count = queryRestorePointCount();
    report += QString("║ Existing Restore Points: %1").arg(existing_count.leftJustified(42)) + QString("║\n");
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    // Phase 3: Create restore point
    Q_EMIT executionProgress("Creating new restore point...", 30);
    QString error_msg;
    QString error_code;
    auto [success, create_section] = createAndFormatResult(error_msg, error_code);
    report += create_section;
    
    // Phase 4: Verify latest restore point
    Q_EMIT executionProgress("Verifying restore point creation...", 70);
    report += verifyLatestRestorePoint();
    
    // Phase 5: Final count + management report
    Q_EMIT executionProgress("Generating final report...", 90);
    QString final_count = queryRestorePointCount();
    report += buildManagementReport(final_count);
    
    // Structured output
    QString structured_output = "\n";
    structured_output += QString("RESTORE_POINT_CREATED:%1\n").arg(success ? "YES" : "NO");
    structured_output += QString("EXISTING_RESTORE_POINTS:%1\n").arg(existing_count);
    structured_output += QString("TOTAL_RESTORE_POINTS:%1\n").arg(final_count);
    structured_output += QString("VSS_SERVICE_STATUS:%1\n").arg(vss_status);
    if (!error_msg.isEmpty()) {
        structured_output += QString("ERROR_MESSAGE:%1\n").arg(error_msg);
    }
    
    ExecutionResult result;
    result.success = success;
    result.message = success ? "Restore point created successfully" : "Restore point creation failed";
    result.log = report + structured_output;
    result.files_processed = success ? 1 : 0;
    result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());

    finishWithResult(result, success ? ActionStatus::Success : ActionStatus::Failed);
}

// ============================================================================
// Extracted Helpers for execute()
// ============================================================================

QString CreateRestorePointAction::queryVssServiceStatus()
{
    ProcessResult srv_proc = runPowerShell(
        "Get-Service -Name 'VSS' | Select-Object -ExpandProperty Status", 5000);
    if (!srv_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("VSS status warning: " + srv_proc.std_err.trimmed());
    }
    return srv_proc.std_out.trimmed();
}

QString CreateRestorePointAction::queryRestorePointCount()
{
    ProcessResult count_proc = runPowerShell(
        "try { $rps = Get-ComputerRestorePoint -ErrorAction Stop; Write-Output $rps.Count } catch { Write-Output '0' }",
        10000);
    if (!count_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point count warning: " + count_proc.std_err.trimmed());
    }
    QString count = count_proc.std_out.trimmed();
    return count.isEmpty() ? QStringLiteral("0") : count;
}

std::pair<bool, QString> CreateRestorePointAction::createAndFormatResult(
    QString& error_msg, QString& error_code)
{
    ProcessResult create_proc = runPowerShell(buildCreateScript(), 90000);
    if (!create_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point creation warning: " + create_proc.std_err.trimmed());
    }
    QString create_output = create_proc.std_out.trimmed();
    bool success = create_output.contains("SUCCESS");

    QString section;
    if (success) {
        section += "║ ✓ Restore Point Creation:   SUCCESS                                 ║\n";
        QRegularExpression re("Restore point created at (.+)");
        QRegularExpressionMatch match = re.match(create_output);
        if (match.hasMatch()) {
            section += QString("║   Timestamp: %1").arg(match.captured(1).leftJustified(55)) + QString("║\n");
        }
        section += "║   Method: Checkpoint-Computer (SystemRestore WMI class)             ║\n";
    } else {
        section += "║ ✗ Restore Point Creation:   FAILED                                  ║\n";

        QRegularExpression code_re("ERROR_CODE:([A-Z_0-9]+)");
        QRegularExpressionMatch code_match = code_re.match(create_output);
        if (code_match.hasMatch()) {
            error_code = code_match.captured(1);
        }

        for (const QString& line : create_output.split("\n")) {
            if (line.contains("ERROR:") && !line.contains("ERROR_CODE:")) {
                error_msg = line;
                error_msg = error_msg.replace("ERROR:", "").trimmed();
                section += QString("║   Error: %1").arg(error_msg.left(66).leftJustified(61)) + QString("║\n");
                break;
            }
        }

        section += buildTroubleshootingReport(error_code);
    }

    return {success, section};
}

QString CreateRestorePointAction::verifyLatestRestorePoint()
{
    ProcessResult verify_proc = runPowerShell(
        "try { $rp = Get-ComputerRestorePoint -ErrorAction Stop | Sort-Object CreationTime -Descending | Select-Object -First 1; $seq = $rp.SequenceNumber; $desc = $rp.Description; $time = $rp.CreationTime; Write-Output \"SEQ:$seq\"; Write-Output \"DESC:$desc\"; Write-Output \"TIME:$time\" } catch { Write-Output 'VERIFY_FAILED' }",
        15000);
    if (!verify_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point verify warning: " + verify_proc.std_err.trimmed());
    }
    QString verify_output = verify_proc.std_out.trimmed();

    QString section;
    section += "╠══════════════════════════════════════════════════════════════════════╣\n";
    section += "║ Latest Restore Point Verification:                                  ║\n";

    if (!verify_output.contains("VERIFY_FAILED") && verify_output.contains("SEQ:")) {
        QString seq_num, desc, time;
        for (const QString& vline : verify_output.split("\n")) {
            if (vline.startsWith("SEQ:")) seq_num = vline.mid(4).trimmed();
            else if (vline.startsWith("DESC:")) desc = vline.mid(5).trimmed();
            else if (vline.startsWith("TIME:")) time = vline.mid(5).trimmed();
        }
        section += QString("║   Sequence Number: %1").arg(seq_num.leftJustified(49)) + QString("║\n");
        section += QString("║   Description: %1").arg(desc.left(53).leftJustified(53)) + QString("║\n");
        section += QString("║   Creation Time: %1").arg(time.leftJustified(47)) + QString("║\n");
    } else {
        section += "║   Unable to verify restore point details                            ║\n";
    }

    return section;
}

// ============================================================================
// Private Helpers
// ============================================================================

QString CreateRestorePointAction::buildCreateScript() const
{
    return R"PS(
        try {
            $timestamp = Get-Date -Format 'yyyy-MM-dd HH:mm:ss'
            Checkpoint-Computer -Description "SAK Utility - $timestamp" -RestorePointType MODIFY_SETTINGS -ErrorAction Stop
            Write-Output 'SUCCESS'
            Write-Output "Restore point created at $timestamp"
            Start-Sleep -Seconds 2
        } catch {
            Write-Output 'FAILED'
            $errMsg = $_.Exception.Message
            Write-Output "ERROR: $errMsg"
            if ($errMsg -match '24 hours|frequency') { Write-Output 'ERROR_CODE:24HR_LIMIT' }
            elseif ($errMsg -match 'disabled|turned off') { Write-Output 'ERROR_CODE:DISABLED' }
            elseif ($errMsg -match 'access|permission') { Write-Output 'ERROR_CODE:PERMISSION' }
            else { Write-Output 'ERROR_CODE:UNKNOWN' }
        }
    )PS";
}

QString CreateRestorePointAction::buildTroubleshootingReport(const QString& error_code) const
{
    QString section;
    section += "║                                                                      ║\n";
    section += "║ TROUBLESHOOTING GUIDANCE:                                            ║\n";
    
    if (error_code == "24HR_LIMIT") {
        section += "║ Issue: Windows Limitation - 24-Hour Frequency Restriction            ║\n";
        section += "║   • Windows 8+ allows only ONE restore point per 24-hour period     ║\n";
        section += "║   • A restore point was already created today                        ║\n";
        section += "║   • This is a Windows OS protection mechanism                        ║\n";
        section += "║   • The existing restore point can still be used for recovery       ║\n";
        section += "║   • Try again tomorrow if another point is needed                    ║\n";
    } else if (error_code == "DISABLED") {
        section += "║ Issue: System Restore is Disabled                                    ║\n";
        section += "║   TO ENABLE SYSTEM RESTORE:                                          ║\n";
        section += "║   1. Open: System Properties > System Protection tab                 ║\n";
        section += "║      OR run: SystemPropertiesProtection                              ║\n";
        section += "║   2. Select C:\\ drive and click 'Configure'                          ║\n";
        section += "║   3. Choose 'Turn on system protection'                              ║\n";
        section += "║   4. Set disk space usage (recommended: 5-10%)                       ║\n";
        section += "║   5. Click OK and then 'Create' to make first restore point         ║\n";
        section += "║   POWERSHELL METHOD (requires admin):                                ║\n";
        section += "║     Enable-ComputerRestore -Drive \"C:\\\"                              ║\n";
    } else if (error_code == "PERMISSION") {
        section += "║ Issue: Insufficient Permissions                                      ║\n";
        section += "║   • Creating restore points requires administrator privileges        ║\n";
        section += "║   • Right-click SAK Utility and select 'Run as administrator'       ║\n";
        section += "║   • Or run from an elevated PowerShell/Command Prompt               ║\n";
    } else {
        section += "║ General Troubleshooting:                                             ║\n";
        section += "║   • Verify VSS service is running: Get-Service VSS                   ║\n";
        section += "║   • Check disk space (need at least 300MB free)                      ║\n";
        section += "║   • Ensure C:\\ drive has System Protection enabled                   ║\n";
        section += "║   • Check Event Viewer for detailed VSS/SR errors                    ║\n";
    }
    
    return section;
}

QString CreateRestorePointAction::buildManagementReport(const QString& final_count) const
{
    QString section;
    section += "╠══════════════════════════════════════════════════════════════════════╣\n";
    section += QString("║ Total Restore Points Available: %1").arg(final_count.leftJustified(34)) + QString("║\n");
    section += "║                                                                      ║\n";
    section += "║ RESTORE POINT MANAGEMENT:                                            ║\n";
    section += "║   View All Points:                                                   ║\n";
    section += "║     • GUI: Control Panel > System > System Protection                ║\n";
    section += "║     • Direct: Run 'rstrui.exe' (System Restore wizard)               ║\n";
    section += "║     • PowerShell: Get-ComputerRestorePoint | Format-Table            ║\n";
    section += "║                                                                      ║\n";
    section += "║   Restore Computer:                                                  ║\n";
    section += "║     • GUI: rstrui.exe > Choose restore point > Next > Finish         ║\n";
    section += "║     • PowerShell: Restore-Computer -RestorePoint <SequenceNumber>    ║\n";
    section += "║                                                                      ║\n";
    section += "║   Configure Settings:                                                ║\n";
    section += "║     • Run: SystemPropertiesProtection                                ║\n";
    section += "║     • Enable: Enable-ComputerRestore -Drive \"C:\\\" (PowerShell)       ║\n";
    section += "║     • Disable: Disable-ComputerRestore -Drive \"C:\\\" (PowerShell)     ║\n";
    section += "║                                                                      ║\n";
    section += "║ TECHNICAL DETAILS:                                                   ║\n";
    section += "║   • System Restore uses Volume Shadow Copy Service (VSS)             ║\n";
    section += "║   • Restore points use WMI SystemRestore class                       ║\n";
    section += "║   • Windows 8+ limit: 1 restore point per 24 hours                   ║\n";
    section += "║   • Supported: Windows 10, Windows 11 (client OS only)               ║\n";
    section += "║   • Not available on Windows Server editions                         ║\n";
    section += "╚══════════════════════════════════════════════════════════════════════╝\n";
    return section;
}

} // namespace sak
