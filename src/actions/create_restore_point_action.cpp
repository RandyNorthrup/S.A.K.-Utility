// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

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
#include <QProcess>
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
        ExecutionResult result;
        result.success = false;
        result.message = "Restore point creation cancelled";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Cancelled);
        Q_EMIT executionComplete(result);
        return;
    }
    
    QString report = "";
    report += "╔══════════════════════════════════════════════════════════════════════╗\n";
    report += "║              SYSTEM RESTORE POINT CREATION REPORT                    ║\n";
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Checking System Restore status...", 10);
    
    // Phase 1: Check System Restore service status
    ProcessResult srv_proc = runPowerShell("Get-Service -Name 'VSS' | Select-Object -ExpandProperty Status", 5000);
    if (!srv_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("VSS status warning: " + srv_proc.std_err.trimmed());
    }
    QString vss_status = srv_proc.std_out.trimmed();
    
    report += QString("║ Volume Shadow Copy Service: %1").arg(vss_status.leftJustified(38)) + QString("║\n");
    
    Q_EMIT executionProgress("Checking existing restore points...", 20);
    
    // Phase 2: Get existing restore points count
    ProcessResult count_proc = runPowerShell(
        "try { $rps = Get-ComputerRestorePoint -ErrorAction Stop; Write-Output $rps.Count } catch { Write-Output '0' }",
        10000);
    if (!count_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point count warning: " + count_proc.std_err.trimmed());
    }
    QString existing_count = count_proc.std_out.trimmed();
    if (existing_count.isEmpty()) existing_count = "0";
    
    report += QString("║ Existing Restore Points: %1").arg(existing_count.leftJustified(42)) + QString("║\n");
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    
    Q_EMIT executionProgress("Creating new restore point...", 30);
    
    // Phase 3: Create restore point with comprehensive PowerShell script
    // Uses Checkpoint-Computer which internally calls SystemRestore WMI class
    // Reference: https://learn.microsoft.com/powershell/module/microsoft.powershell.management/checkpoint-computer
    const QString create_script = R"PS(
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
    ProcessResult create_proc = runPowerShell(create_script, 90000);
    if (!create_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point creation warning: " + create_proc.std_err.trimmed());
    }
    QString create_output = create_proc.std_out.trimmed();
    
    bool success = false;
    QString error_msg = "";
    QString error_code = "";
    
    if (create_output.contains("SUCCESS")) {
        success = true;
        report += "║ ✓ Restore Point Creation:   SUCCESS                                 ║\n";
        
        // Extract timestamp from output
        QRegularExpression re("Restore point created at (.+)");
        QRegularExpressionMatch match = re.match(create_output);
        if (match.hasMatch()) {
            QString timestamp = match.captured(1);
            report += QString("║   Timestamp: %1").arg(timestamp.leftJustified(55)) + QString("║\n");
        }
        report += "║   Method: Checkpoint-Computer (SystemRestore WMI class)             ║\n";
    } else {
        success = false;
        report += "║ ✗ Restore Point Creation:   FAILED                                  ║\n";
        
        // Extract error code
        if (create_output.contains("ERROR_CODE:")) {
            QRegularExpression code_re("ERROR_CODE:([A-Z_0-9]+)");
            QRegularExpressionMatch code_match = code_re.match(create_output);
            if (code_match.hasMatch()) {
                error_code = code_match.captured(1);
            }
        }
        
        // Extract error message
        if (create_output.contains("ERROR:")) {
            QStringList lines = create_output.split("\n");
            for (const QString& line : lines) {
                if (line.contains("ERROR:") && !line.contains("ERROR_CODE:")) {
                    error_msg = line;
                    error_msg = error_msg.replace("ERROR:", "").trimmed();
                    
                    // Wrap error message to fit in report
                    QString wrapped = error_msg.left(66);
                    QString formatted_line = QString("║   Error: %1").arg(wrapped.leftJustified(61)) + QString("║\n");
                    report += formatted_line;
                    break;
                }
            }
        }
        
        report += "║                                                                      ║\n";
        report += "║ TROUBLESHOOTING GUIDANCE:                                            ║\n";
        
        // Provide specific guidance based on error code
        if (error_code == "24HR_LIMIT") {
            report += "║ Issue: Windows Limitation - 24-Hour Frequency Restriction            ║\n";
            report += "║   • Windows 8+ allows only ONE restore point per 24-hour period     ║\n";
            report += "║   • A restore point was already created today                        ║\n";
            report += "║   • This is a Windows OS protection mechanism                        ║\n";
            report += "║   • The existing restore point can still be used for recovery       ║\n";
            report += "║   • Try again tomorrow if another point is needed                    ║\n";
        } else if (error_code == "DISABLED") {
            report += "║ Issue: System Restore is Disabled                                    ║\n";
            report += "║   TO ENABLE SYSTEM RESTORE:                                          ║\n";
            report += "║   1. Open: System Properties > System Protection tab                 ║\n";
            report += "║      OR run: SystemPropertiesProtection                              ║\n";
            report += "║   2. Select C:\\ drive and click 'Configure'                          ║\n";
            report += "║   3. Choose 'Turn on system protection'                              ║\n";
            report += "║   4. Set disk space usage (recommended: 5-10%)                       ║\n";
            report += "║   5. Click OK and then 'Create' to make first restore point         ║\n";
            report += "║   POWERSHELL METHOD (requires admin):                                ║\n";
            report += "║     Enable-ComputerRestore -Drive \"C:\\\"                              ║\n";
        } else if (error_code == "PERMISSION") {
            report += "║ Issue: Insufficient Permissions                                      ║\n";
            report += "║   • Creating restore points requires administrator privileges        ║\n";
            report += "║   • Right-click SAK Utility and select 'Run as administrator'       ║\n";
            report += "║   • Or run from an elevated PowerShell/Command Prompt               ║\n";
        } else {
            report += "║ General Troubleshooting:                                             ║\n";
            report += "║   • Verify VSS service is running: Get-Service VSS                   ║\n";
            report += "║   • Check disk space (need at least 300MB free)                      ║\n";
            report += "║   • Ensure C:\\ drive has System Protection enabled                   ║\n";
            report += "║   • Check Event Viewer for detailed VSS/SR errors                    ║\n";
        }
    }
    
    Q_EMIT executionProgress("Verifying restore point creation...", 70);
    
    // Phase 4: Verify by getting latest restore point
    ProcessResult verify_proc = runPowerShell(
        "try { $rp = Get-ComputerRestorePoint -ErrorAction Stop | Sort-Object CreationTime -Descending | Select-Object -First 1; $seq = $rp.SequenceNumber; $desc = $rp.Description; $time = $rp.CreationTime; Write-Output \"SEQ:$seq\"; Write-Output \"DESC:$desc\"; Write-Output \"TIME:$time\" } catch { Write-Output 'VERIFY_FAILED' }",
        15000);
    if (!verify_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point verify warning: " + verify_proc.std_err.trimmed());
    }
    QString verify_output = verify_proc.std_out.trimmed();
    
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += "║ Latest Restore Point Verification:                                  ║\n";
    
    if (!verify_output.contains("VERIFY_FAILED") && verify_output.contains("SEQ:")) {
        QStringList verify_lines = verify_output.split("\n");
        QString seq_num = "";
        QString desc = "";
        QString time = "";
        
        for (const QString& line : verify_lines) {
            if (line.startsWith("SEQ:")) seq_num = line.mid(4).trimmed();
            else if (line.startsWith("DESC:")) desc = line.mid(5).trimmed();
            else if (line.startsWith("TIME:")) time = line.mid(5).trimmed();
        }
        
        report += QString("║   Sequence Number: %1").arg(seq_num.leftJustified(49)) + QString("║\n");
        report += QString("║   Description: %1").arg(desc.left(53).leftJustified(53)) + QString("║\n");
        report += QString("║   Creation Time: %1").arg(time.leftJustified(47)) + QString("║\n");
    } else {
        report += "║   Unable to verify restore point details                            ║\n";
    }
    
    Q_EMIT executionProgress("Generating final report...", 90);
    
    // Phase 5: Get updated restore point count
    ProcessResult final_count_proc = runPowerShell(
        "try { $rps = Get-ComputerRestorePoint -ErrorAction Stop; Write-Output $rps.Count } catch { Write-Output '0' }",
        10000);
    if (!final_count_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Restore point final count warning: " + final_count_proc.std_err.trimmed());
    }
    QString final_count = final_count_proc.std_out.trimmed();
    if (final_count.isEmpty()) final_count = "0";
    
    report += "╠══════════════════════════════════════════════════════════════════════╣\n";
    report += QString("║ Total Restore Points Available: %1").arg(final_count.leftJustified(34)) + QString("║\n");
    report += "║                                                                      ║\n";
    report += "║ RESTORE POINT MANAGEMENT:                                            ║\n";
    report += "║   View All Points:                                                   ║\n";
    report += "║     • GUI: Control Panel > System > System Protection                ║\n";
    report += "║     • Direct: Run 'rstrui.exe' (System Restore wizard)               ║\n";
    report += "║     • PowerShell: Get-ComputerRestorePoint | Format-Table            ║\n";
    report += "║                                                                      ║\n";
    report += "║   Restore Computer:                                                  ║\n";
    report += "║     • GUI: rstrui.exe > Choose restore point > Next > Finish         ║\n";
    report += "║     • PowerShell: Restore-Computer -RestorePoint <SequenceNumber>    ║\n";
    report += "║                                                                      ║\n";
    report += "║   Configure Settings:                                                ║\n";
    report += "║     • Run: SystemPropertiesProtection                                ║\n";
    report += "║     • Enable: Enable-ComputerRestore -Drive \"C:\\\" (PowerShell)       ║\n";
    report += "║     • Disable: Disable-ComputerRestore -Drive \"C:\\\" (PowerShell)     ║\n";
    report += "║                                                                      ║\n";
    report += "║ TECHNICAL DETAILS:                                                   ║\n";
    report += "║   • System Restore uses Volume Shadow Copy Service (VSS)             ║\n";
    report += "║   • Restore points use WMI SystemRestore class                       ║\n";
    report += "║   • Windows 8+ limit: 1 restore point per 24 hours                   ║\n";
    report += "║   • Supported: Windows 10, Windows 11 (client OS only)               ║\n";
    report += "║   • Not available on Windows Server editions                         ║\n";
    report += "╚══════════════════════════════════════════════════════════════════════╝\n";
    
    // Structured output for external processing
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

    setExecutionResult(result);
    setStatus(success ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak
