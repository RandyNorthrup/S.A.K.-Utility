// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/fix_audio_issues_action.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QThread>
#include <QTextStream>

namespace sak {

FixAudioIssuesAction::FixAudioIssuesAction(QObject* parent)
    : QuickAction(parent)
{
}

// ENTERPRISE-GRADE: Comprehensive service status check using PowerShell Get-Service
FixAudioIssuesAction::AudioServiceStatus FixAudioIssuesAction::checkAudioService(const QString& service_name) {
    AudioServiceStatus svc_status;
    svc_status.service_name = service_name;
    svc_status.is_running = false;
    
    QString ps_cmd = QString("Get-Service -Name %1 | Select-Object Status | Format-List").arg(service_name);
    
    ProcessResult proc = runPowerShell(ps_cmd, 5000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Audio service check warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    
    // Parse output
    QStringList lines = output.split('\n');
    for (const QString& line : lines) {
        if (line.contains("Status", Qt::CaseInsensitive)) {
            svc_status.status = line.split(':').last().trimmed();
            svc_status.is_running = svc_status.status.contains("Running", Qt::CaseInsensitive);
        }
    }
    
    return svc_status;
}

// ENTERPRISE-GRADE: Graceful service restart with status monitoring
bool FixAudioIssuesAction::restartAudioService() {
    Q_EMIT executionProgress("Restarting Windows Audio Service (AudioSrv)...", 15);
    
    // Stop-Service with proper error handling
    QString stop_cmd = "Stop-Service -Name Audiosrv -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2";
    ProcessResult stop_proc = runPowerShell(stop_cmd, 10000);
    if (!stop_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("AudioSrv stop warning: " + stop_proc.std_err.trimmed());
    }
    
    // Start-Service with verification
    QString start_cmd = "Start-Service -Name Audiosrv; Get-Service -Name Audiosrv | Select-Object Status";
    ProcessResult proc = runPowerShell(start_cmd, 10000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("AudioSrv start warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    return output.contains("Running", Qt::CaseInsensitive);
}

// ENTERPRISE-GRADE: Audio Endpoint Builder restart (critical for audio enumeration)
bool FixAudioIssuesAction::restartAudioEndpointBuilder() {
    Q_EMIT executionProgress("Restarting Audio Endpoint Builder...", 35);
    
    QString stop_cmd = "Stop-Service -Name AudioEndpointBuilder -Force -ErrorAction SilentlyContinue; Start-Sleep -Seconds 2";
    ProcessResult stop_proc = runPowerShell(stop_cmd, 10000);
    if (!stop_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("AudioEndpointBuilder stop warning: " + stop_proc.std_err.trimmed());
    }
    
    QString start_cmd = "Start-Service -Name AudioEndpointBuilder; Get-Service -Name AudioEndpointBuilder | Select-Object Status";
    ProcessResult proc = runPowerShell(start_cmd, 10000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("AudioEndpointBuilder start warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    return output.contains("Running", Qt::CaseInsensitive);
}

// ENTERPRISE-GRADE: Enumerate and reset audio devices
int FixAudioIssuesAction::resetAudioDevices() {
    Q_EMIT executionProgress("Enumerating and resetting audio devices...", 55);
    
    // Get count of audio devices
    QString count_cmd = "((Get-PnpDevice -Class 'AudioEndpoint','MEDIA' | Where-Object {$_.Status -ne 'Unknown'}) | Measure-Object).Count";
    ProcessResult count_proc = runPowerShell(count_cmd, 5000);
    if (!count_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Audio device count warning: " + count_proc.std_err.trimmed());
    }
    int device_count = count_proc.std_out.trimmed().toInt();
    
    // Disable and re-enable audio devices
    QString reset_cmd = "$devices = Get-PnpDevice -Class 'AudioEndpoint','MEDIA' | Where-Object {$_.Status -ne 'Unknown'}; "
                       "$devices | Disable-PnpDevice -Confirm:$false -ErrorAction SilentlyContinue; "
                       "Start-Sleep -Seconds 3; "
                       "$devices | Enable-PnpDevice -Confirm:$false -ErrorAction SilentlyContinue";
    
    ProcessResult reset_proc = runPowerShell(reset_cmd, 20000);
    if (!reset_proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Audio device reset warning: " + reset_proc.std_err.trimmed());
    }
    
    return device_count;
}

// ENTERPRISE-GRADE: Check for USB audio driver issues
QString FixAudioIssuesAction::checkUSBAudioDevices() {
    Q_EMIT executionProgress("Checking USB audio devices...", 75);
    
    QString ps_cmd = "Get-PnpDevice -Class 'USB' | Where-Object {$_.FriendlyName -like '*Audio*'} | Select-Object Status,FriendlyName,InstanceId | Format-Table -AutoSize | Out-String -Width 200";
    
    ProcessResult proc = runPowerShell(ps_cmd, 5000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("USB audio check warning: " + proc.std_err.trimmed());
    }
    return proc.std_out;
}

void FixAudioIssuesAction::scan() {
    setStatus(ActionStatus::Scanning);

    AudioServiceStatus audiosrv = checkAudioService("Audiosrv");
    AudioServiceStatus endpoint = checkAudioService("AudioEndpointBuilder");

    ScanResult result;
    result.applicable = true;
    result.summary = QString("Audio services: %1/%2 running")
        .arg(audiosrv.is_running ? "AudioSrv" : "AudioSrv stopped")
        .arg(endpoint.is_running ? "Endpoint" : "Endpoint stopped");
    result.details = "Repair will restart services and reset audio devices";

    if (!audiosrv.is_running || !endpoint.is_running) {
        result.warning = "One or more audio services are stopped";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void FixAudioIssuesAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    Q_EMIT executionProgress("Diagnosing audio services...", 5);
    
    // PHASE 1: Check service status
    AudioServiceStatus audiosrv = checkAudioService("Audiosrv");
    AudioServiceStatus endpoint_builder = checkAudioService("AudioEndpointBuilder");
    
    QString report = "╔════════════════════════════════════════════════════════════════╗\n";
    report += "║              AUDIO SYSTEM DIAGNOSTIC REPORT                   ║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // Service status before restart
    report += QString("║ AudioSrv:             %1\n").arg(audiosrv.is_running ? "Running" : "STOPPED").leftJustified(67, ' ') + "║\n";
    report += QString("║ AudioEndpointBuilder: %1\n").arg(endpoint_builder.is_running ? "Running" : "STOPPED").leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // PHASE 2: Restart audio services
    Q_EMIT executionProgress("Restarting audio services...", 20);
    bool audiosrv_restarted = restartAudioService();
    bool endpoint_restarted = restartAudioEndpointBuilder();
    
    report += QString("║ AudioSrv Restart:     %1\n").arg(audiosrv_restarted ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += QString("║ Endpoint Restart:     %1\n").arg(endpoint_restarted ? "SUCCESS" : "FAILED").leftJustified(67, ' ') + "║\n";
    report += "╠════════════════════════════════════════════════════════════════╣\n";
    
    // PHASE 3: Reset audio devices
    int device_count = resetAudioDevices();
    report += QString("║ Audio Devices Reset:  %1 devices\n").arg(device_count).leftJustified(67, ' ') + "║\n";
    
    // PHASE 4: Check USB audio
    QString usb_info = checkUSBAudioDevices();
    if (!usb_info.trimmed().isEmpty()) {
        report += "║ USB Audio Devices:    Detected                   ║\n";
    }
    
    report += "╚════════════════════════════════════════════════════════════════╝\n";
    
    Q_EMIT executionProgress("Audio diagnostics complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.files_processed = device_count;
    
    bool overall_success = audiosrv_restarted && endpoint_restarted;
    
    if (overall_success) {
        result.success = true;
        result.message = QString("Audio system repaired: %1 devices, %2 services restarted")
                            .arg(device_count).arg(2);
        result.log = report;
        result.log += QString("\nCompleted in %1 seconds\n").arg(duration_ms / 1000);
        result.log += "RECOMMENDATIONS:\n";
        result.log += "• Test audio playback in system settings\n";
        result.log += "• Reboot if issues persist\n";
        result.log += "• Check Device Manager for driver errors\n";
        setStatus(ActionStatus::Success);
    } else {
        result.success = false;
        result.message = "Audio service restart encountered errors";
        result.log = report;
        result.log += "\nSome services failed to restart - administrative privileges may be required\n";
        setStatus(ActionStatus::Failed);
    }
    
    setExecutionResult(result);
    Q_EMIT executionComplete(result);
}

} // namespace sak
