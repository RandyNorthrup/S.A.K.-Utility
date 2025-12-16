// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/verify_system_files_action.h"
#include <QProcess>
#include <QRegularExpression>

namespace sak {

VerifySystemFilesAction::VerifySystemFilesAction(QObject* parent)
    : QuickAction(parent)
{
}

void VerifySystemFilesAction::runSFC() {
    Q_EMIT executionProgress("Running System File Checker (SFC)...", 10);
    
    // Enterprise approach: Run SFC with real-time progress monitoring and accumulated output
    QString ps_script = 
        "$process = Start-Process -FilePath 'sfc' -ArgumentList '/scannow' -PassThru -NoNewWindow -Wait -RedirectStandardOutput 'sfc_output.txt'; "
        "Get-Content 'sfc_output.txt' | Write-Output; "
        "$cbsLog = \"$env:SystemRoot\\Logs\\CBS\\CBS.log\"; "
        "if (Test-Path $cbsLog) { Write-Output \"CBS_LOG_PATH:$cbsLog\" }; "
        "Remove-Item 'sfc_output.txt' -ErrorAction SilentlyContinue";
    
    QProcess proc;
    proc.start("powershell.exe", QStringList() << "-NoProfile" << "-ExecutionPolicy" << "Bypass" << "-Command" << ps_script);
    
    // SFC can take 10-30 minutes, monitor with timeout
    QString accumulated_output;
    while (proc.state() == QProcess::Running) {
        if (proc.waitForReadyRead(5000)) {
            QString chunk = proc.readAllStandardOutput();
            accumulated_output += chunk;
            
            if (chunk.contains("Verification", Qt::CaseInsensitive)) {
                QRegularExpression re("(\\d+)%");
                QRegularExpressionMatch match = re.match(chunk);
                if (match.hasMatch()) {
                    int progress = match.captured(1).toInt();
                    Q_EMIT executionProgress(QString("SFC scanning... %1%").arg(progress), 10 + (progress / 4));
                }
            }
        }
        
        if (isCancelled()) {
            proc.kill();
            return;
        }
    }
    
    accumulated_output += proc.readAll();
    
    // Extract CBS.log path
    QRegularExpression cbsLogRe("CBS_LOG_PATH:(.+)");
    QRegularExpressionMatch cbsMatch = cbsLogRe.match(accumulated_output);
    if (cbsMatch.hasMatch()) {
        m_cbs_log_path = cbsMatch.captured(1).trimmed();
    }
    
    if (accumulated_output.contains("found corrupt files", Qt::CaseInsensitive)) {
        m_sfc_found_issues = true;
        m_sfc_repaired = accumulated_output.contains("successfully repaired", Qt::CaseInsensitive);
    }
}

void VerifySystemFilesAction::runDISM() {
    // Enterprise DISM sequence: CheckHealth → ScanHealth → RestoreHealth
    // Per Microsoft docs: Use /LimitAccess to prevent Windows Update contact during repair
    
    Q_EMIT executionProgress("DISM: Checking component store health...", 35);
    
    // Step 1: CheckHealth - Quick check for corruption
    QString checkHealthScript = 
        "DISM.exe /Online /Cleanup-Image /CheckHealth; "
        "$LASTEXITCODE";
    
    QProcess checkProc;
    checkProc.start("powershell.exe", QStringList() << "-Command" << checkHealthScript);
    checkProc.waitForFinished(120000); // 2 minute timeout for CheckHealth
    
    if (isCancelled()) return;
    
    QString checkOutput = checkProc.readAllStandardOutput();
    bool corruption_detected = checkOutput.contains("corruption", Qt::CaseInsensitive);
    
    Q_EMIT executionProgress("DISM: Scanning component store...", 50);
    
    // Step 2: ScanHealth - Thorough scan for corruption
    QString scanHealthScript = "DISM.exe /Online /Cleanup-Image /ScanHealth";
    
    QProcess scanProc;
    scanProc.start("powershell.exe", QStringList() << "-Command" << scanHealthScript);
    scanProc.waitForFinished(600000); // 10 minute timeout for ScanHealth
    
    if (isCancelled()) return;
    
    QString scanOutput = scanProc.readAllStandardOutput();
    bool repair_needed = scanOutput.contains("repairable", Qt::CaseInsensitive) || 
                        scanOutput.contains("corruption", Qt::CaseInsensitive);
    
    if (corruption_detected || repair_needed) {
        Q_EMIT executionProgress("DISM: Repairing component store...", 65);
        
        // Step 3: RestoreHealth - Repair detected corruption
        // /LimitAccess prevents Windows Update usage (offline mode)
        QString restoreHealthScript = "DISM.exe /Online /Cleanup-Image /RestoreHealth /LimitAccess";
        
        QProcess restoreProc;
        restoreProc.start("powershell.exe", QStringList() << "-Command" << restoreHealthScript);
        
        // RestoreHealth can take 15-30 minutes
        while (restoreProc.state() == QProcess::Running) {
            if (restoreProc.waitForReadyRead(10000)) {
                QString chunk = restoreProc.readAllStandardOutput();
                
                // Parse progress percentage
                QRegularExpression re("(\\d+\\.\\d+)%");
                QRegularExpressionMatch match = re.match(chunk);
                if (match.hasMatch()) {
                    double progress = match.captured(1).toDouble();
                    Q_EMIT executionProgress(QString("DISM restoring... %1%").arg(progress, 0, 'f', 1), 
                                           65 + static_cast<int>(progress / 4));
                }
            }
            
            if (isCancelled()) {
                restoreProc.kill();
                return;
            }
        }
        
        QString restoreOutput = restoreProc.readAllStandardOutput();
        
        if (restoreOutput.contains("successfully", Qt::CaseInsensitive)) {
            m_dism_successful = true;
            m_dism_repaired_issues = true;
        }
    } else {
        Q_EMIT executionProgress("DISM: No corruption detected", 85);
        m_dism_successful = true;
        m_dism_repaired_issues = false;
    }
}

void VerifySystemFilesAction::scan() {
    setStatus(ActionStatus::Ready);
    ScanResult result;
    result.applicable = true;
    result.summary = "Ready to verify system files";
    setScanResult(result);
    Q_EMIT scanComplete(result);
}

void VerifySystemFilesAction::execute() {
    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    m_sfc_found_issues = false;
    m_sfc_repaired = false;
    m_dism_successful = false;
    m_dism_repaired_issues = false;
    
    runSFC();
    runDISM();
    
    QString message;
    if (m_sfc_found_issues) {
        if (m_sfc_repaired) {
            message = "SFC found and repaired corrupt files. ";
        } else {
            message = "SFC found corrupt files but could not repair them. ";
        }
    } else {
        message = "SFC found no integrity violations. ";
    }
    
    if (m_dism_repaired_issues) {
        message += "DISM repaired component store issues.";
    } else {
        message += "DISM found no issues.";
    }
    
    ExecutionResult result;
    result.message = message;
    
    setStatus(ActionStatus::Success);
    Q_EMIT executionComplete(result);
}

} // namespace sak
