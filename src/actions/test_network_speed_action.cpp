// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/actions/test_network_speed_action.h"
#include "sak/process_runner.h"
#include <QProcess>
#include <QRegularExpression>

namespace sak {

TestNetworkSpeedAction::TestNetworkSpeedAction(QObject* parent)
    : QuickAction(parent)
{
}

// ENTERPRISE-GRADE: Comprehensive network connectivity test using Test-NetConnection
void TestNetworkSpeedAction::checkConnectivity() {
    QString ps_cmd = QString("Test-NetConnection -ComputerName '8.8.8.8' -InformationLevel Detailed | "
                             "Select-Object PingSucceeded,PingReplyDetails,RemoteAddress | "
                             "ForEach-Object { "
                             "Write-Output \"PING_SUCCESS:$($_.PingSucceeded)\"; "
                             "Write-Output \"PING_RTT:$($_.PingReplyDetails.RoundtripTime)\"; "
                             "Write-Output \"REMOTE_ADDR:$($_.RemoteAddress)\" "
                             "}");
    
    ProcessResult proc = runPowerShell(ps_cmd, 10000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Connectivity test warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        if (line.contains("PING_SUCCESS:")) {
            m_has_internet = line.contains("True", Qt::CaseInsensitive);
        }
        if (line.contains("PING_RTT:")) {
            QString rtt = line.split(':').value(1).trimmed();
            if (!rtt.isEmpty()) {
                m_latency = rtt.toInt();
            }
        }
    }
}

// ENTERPRISE-GRADE: Multiple server download speed test with Measure-Command
void TestNetworkSpeedAction::testDownloadSpeed() {
    Q_EMIT executionProgress("Testing download speed with multiple servers...", 30);
    
    // Test with 3 different servers for reliability
    QStringList test_urls = {
        "http://speedtest.tele2.net/10MB.zip",
        "http://ipv4.download.thinkbroadband.com/10MB.zip",
        "https://speed.hetzner.de/10MB.bin"
    };
    
    QString ps_cmd = QString(
        "$urls = @('%1', '%2', '%3'); "
        "$speeds = @(); "
        "foreach ($url in $urls) { "
        "  try { "
        "    $start = Get-Date; "
        "    $response = Invoke-WebRequest -Uri $url -UseBasicParsing -TimeoutSec 30 -ErrorAction Stop; "
        "    $end = Get-Date; "
        "    $duration = ($end - $start).TotalSeconds; "
        "    if ($duration -gt 0) { "
        "      $sizeMB = $response.Content.Length / 1MB; "
        "      $speedMbps = ($sizeMB * 8) / $duration; "
        "      $speeds += $speedMbps; "
        "      Write-Output \"SERVER_SPEED:$([math]::Round($speedMbps, 2))\"; "
        "    } "
        "  } catch { "
        "    Write-Output \"SERVER_ERROR:$url\"; "
        "  } "
        "} "
        "if ($speeds.Count -gt 0) { "
        "  $avgSpeed = ($speeds | Measure-Object -Average).Average; "
        "  Write-Output \"AVG_DOWNLOAD_SPEED:$([math]::Round($avgSpeed, 2))\"; "
        "  $maxSpeed = ($speeds | Measure-Object -Maximum).Maximum; "
        "  Write-Output \"MAX_DOWNLOAD_SPEED:$([math]::Round($maxSpeed, 2))\"; "
        "  Write-Output \"TESTS_SUCCESSFUL:$($speeds.Count)\"; "
        "} else { "
        "  Write-Output \"ALL_TESTS_FAILED\"; "
        "}"
    ).arg(test_urls[0], test_urls[1], test_urls[2]);
    
    ProcessResult proc = runPowerShell(ps_cmd, 120000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Download speed test warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');
    
    QVector<double> speeds;
    int successful_tests = 0;
    
    for (const QString& line : lines) {
        if (line.contains("SERVER_SPEED:")) {
            double speed = line.split(':').value(1).trimmed().toDouble();
            speeds.append(speed);
        }
        if (line.contains("AVG_DOWNLOAD_SPEED:")) {
            m_download_speed = line.split(':').value(1).trimmed().toDouble();
        }
        if (line.contains("MAX_DOWNLOAD_SPEED:")) {
            m_max_download_speed = line.split(':').value(1).trimmed().toDouble();
        }
        if (line.contains("TESTS_SUCCESSFUL:")) {
            successful_tests = line.split(':').value(1).trimmed().toInt();
        }
    }
    
    m_download_tests_successful = successful_tests;
}

// ENTERPRISE-GRADE: Upload speed test with timing measurement
void TestNetworkSpeedAction::testUploadSpeed() {
    Q_EMIT executionProgress("Testing upload speed...", 60);
    
    // Upload test using HTTP POST with measured timing
    QString ps_cmd = QString(
        "$data = [byte[]]::new(1MB); "
        "$rnd = [System.Random]::new(); "
        "$rnd.NextBytes($data); "
        "$url = 'https://httpbin.org/post'; "
        "try { "
        "  $start = Get-Date; "
        "  $response = Invoke-WebRequest -Uri $url -Method POST -Body $data -UseBasicParsing -TimeoutSec 30 -ErrorAction Stop; "
        "  $end = Get-Date; "
        "  $duration = ($end - $start).TotalSeconds; "
        "  if ($duration -gt 0) { "
        "    $sizeMB = $data.Length / 1MB; "
        "    $speedMbps = ($sizeMB * 8) / $duration; "
        "    Write-Output \"UPLOAD_SPEED:$([math]::Round($speedMbps, 2))\"; "
        "    Write-Output \"UPLOAD_SUCCESS:True\"; "
        "  } "
        "} catch { "
        "  Write-Output \"UPLOAD_SUCCESS:False\"; "
        "  Write-Output \"UPLOAD_ERROR:$($_.Exception.Message)\"; "
        "}"
    );
    
    ProcessResult proc = runPowerShell(ps_cmd, 45000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Upload speed test warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        if (line.contains("UPLOAD_SPEED:")) {
            m_upload_speed = line.split(':').value(1).trimmed().toDouble();
        }
        if (line.contains("UPLOAD_SUCCESS:")) {
            m_upload_test_successful = line.contains("True", Qt::CaseInsensitive);
        }
    }
}

// ENTERPRISE-GRADE: Advanced latency test with jitter and packet loss
void TestNetworkSpeedAction::testLatencyAndJitter() {
    Q_EMIT executionProgress("Measuring latency, jitter, and packet loss...", 45);
    
    QString ps_cmd = QString(
        "$pings = @(); "
        "$host = '8.8.8.8'; "
        "for ($i = 0; $i -lt 10; $i++) { "
        "  $result = Test-NetConnection -ComputerName $host -InformationLevel Quiet; "
        "  if ($result) { "
        "    $testResult = Test-NetConnection -ComputerName $host; "
        "    if ($testResult.PingReplyDetails) { "
        "      $pings += $testResult.PingReplyDetails.RoundtripTime; "
        "    } "
        "  } "
        "  Start-Sleep -Milliseconds 100; "
        "} "
        "if ($pings.Count -gt 0) { "
        "  $avgLatency = ($pings | Measure-Object -Average).Average; "
        "  $minLatency = ($pings | Measure-Object -Minimum).Minimum; "
        "  $maxLatency = ($pings | Measure-Object -Maximum).Maximum; "
        "  $jitter = $maxLatency - $minLatency; "
        "  $packetLoss = ((10 - $pings.Count) / 10) * 100; "
        "  Write-Output \"AVG_LATENCY:$([math]::Round($avgLatency, 2))\"; "
        "  Write-Output \"MIN_LATENCY:$minLatency\"; "
        "  Write-Output \"MAX_LATENCY:$maxLatency\"; "
        "  Write-Output \"JITTER:$([math]::Round($jitter, 2))\"; "
        "  Write-Output \"PACKET_LOSS:$([math]::Round($packetLoss, 2))\"; "
        "  Write-Output \"PINGS_SUCCESSFUL:$($pings.Count)\"; "
        "} else { "
        "  Write-Output \"LATENCY_TEST_FAILED\"; "
        "}"
    );
    
    ProcessResult proc = runPowerShell(ps_cmd, 15000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Latency/jitter test warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        if (line.contains("AVG_LATENCY:")) {
            m_latency = line.split(':').value(1).trimmed().toInt();
        }
        if (line.contains("MIN_LATENCY:")) {
            m_min_latency = line.split(':').value(1).trimmed().toInt();
        }
        if (line.contains("MAX_LATENCY:")) {
            m_max_latency = line.split(':').value(1).trimmed().toInt();
        }
        if (line.contains("JITTER:")) {
            m_jitter = line.split(':').value(1).trimmed().toDouble();
        }
        if (line.contains("PACKET_LOSS:")) {
            m_packet_loss = line.split(':').value(1).trimmed().toDouble();
        }
    }
}

// ENTERPRISE-GRADE: Get public IP and ISP information
void TestNetworkSpeedAction::getPublicIPInfo() {
    Q_EMIT executionProgress("Retrieving public IP and ISP information...", 15);
    
    QString ps_cmd = QString(
        "try { "
        "  $response = Invoke-RestMethod -Uri 'https://ipapi.co/json/' -TimeoutSec 10 -ErrorAction Stop; "
        "  Write-Output \"PUBLIC_IP:$($response.ip)\"; "
        "  Write-Output \"ISP:$($response.org)\"; "
        "  Write-Output \"CITY:$($response.city)\"; "
        "  Write-Output \"REGION:$($response.region)\"; "
        "  Write-Output \"COUNTRY:$($response.country_name)\"; "
        "} catch { "
        "  Write-Output \"IP_INFO_FAILED\"; "
        "}"
    );
    
    ProcessResult proc = runPowerShell(ps_cmd, 15000);
    if (!proc.std_err.trimmed().isEmpty()) {
        Q_EMIT logMessage("Public IP lookup warning: " + proc.std_err.trimmed());
    }
    QString output = proc.std_out;
    QStringList lines = output.split('\n');
    
    for (const QString& line : lines) {
        if (line.contains("PUBLIC_IP:")) {
            m_public_ip = line.split(':',Qt::SkipEmptyParts).value(1).trimmed();
        }
        if (line.contains("ISP:")) {
            m_isp = line.mid(line.indexOf(':') + 1).trimmed();
        }
        if (line.contains("CITY:")) {
            m_city = line.split(':',Qt::SkipEmptyParts).value(1).trimmed();
        }
        if (line.contains("REGION:")) {
            m_region = line.split(':',Qt::SkipEmptyParts).value(1).trimmed();
        }
        if (line.contains("COUNTRY:")) {
            m_country = line.split(':',Qt::SkipEmptyParts).value(1).trimmed();
        }
    }
}

void TestNetworkSpeedAction::scan() {
    setStatus(ActionStatus::Scanning);

    checkConnectivity();

    ScanResult result;
    result.applicable = m_has_internet;
    result.summary = m_has_internet
        ? "Internet connectivity detected"
        : "No internet connectivity";
    result.details = "Speed test requires internet access";
    if (!m_has_internet) {
        result.warning = "Network speed test cannot run without connectivity";
    }

    setScanResult(result);
    setStatus(ActionStatus::Ready);
    Q_EMIT scanComplete(result);
}

void TestNetworkSpeedAction::execute() {
    if (isCancelled()) {
        return;
    }

    setStatus(ActionStatus::Running);
    QDateTime start_time = QDateTime::currentDateTime();
    
    // Phase 1: Get public IP and ISP information
    getPublicIPInfo();
    
    // Phase 2: Check connectivity
    Q_EMIT executionProgress("Checking internet connectivity...", 10);
    checkConnectivity();
    
    if (!m_has_internet) {
        ExecutionResult result;
        result.success = false;
        result.message = "No internet connection detected";
        result.log = "Cannot perform speed test without internet connectivity.\n"
                     "Please check your network connection and try again.";
        result.duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
        setExecutionResult(result);
        setStatus(ActionStatus::Failed);
        Q_EMIT executionComplete(result);
        return;
    }
    
    // Phase 3: Test latency, jitter, and packet loss
    testLatencyAndJitter();
    
    // Phase 4: Test download speed
    testDownloadSpeed();
    
    // Phase 5: Test upload speed
    testUploadSpeed();
    
    Q_EMIT executionProgress("Generating comprehensive report...", 90);
    
    // Build enterprise-grade report with box-drawing
    QString report;
    report += QString("╔").repeated(1) + QString("═").repeated(78) + QString("╗\n");
    report += QString("║") + QString(" NETWORK SPEED TEST RESULTS").leftJustified(78) + QString("║\n");
    report += QString("╠").repeated(1) + QString("═").repeated(78) + QString("╣\n");
    
    // Connection Information
    if (!m_public_ip.isEmpty()) {
        report += QString("║ Public IP:    %1").arg(m_public_ip).leftJustified(79) + QString("║\n");
    }
    if (!m_isp.isEmpty()) {
        report += QString("║ ISP:          %1").arg(m_isp).leftJustified(79) + QString("║\n");
    }
    if (!m_city.isEmpty() && !m_country.isEmpty()) {
        QString location = QString("%1, %2, %3").arg(m_city, m_region, m_country);
        report += QString("║ Location:     %1").arg(location).leftJustified(79) + QString("║\n");
    }
    report += QString("╠").repeated(1) + QString("═").repeated(78) + QString("╣\n");
    
    // Download Speed
    if (m_download_speed > 0) {
        report += QString("║ Download Speed (Avg):  %1 Mbps").arg(m_download_speed, 0, 'f', 2).leftJustified(79) + QString("║\n");
        if (m_max_download_speed > 0) {
            report += QString("║ Download Speed (Max):  %1 Mbps").arg(m_max_download_speed, 0, 'f', 2).leftJustified(79) + QString("║\n");
        }
        report += QString("║ Successful Tests:     %1/3 servers").arg(m_download_tests_successful).leftJustified(79) + QString("║\n");
    } else {
        report += QString("║ Download Speed:        Test failed (check firewall/connection)").leftJustified(79) + QString("║\n");
    }
    report += QString("╠").repeated(1) + QString("═").repeated(78) + QString("╣\n");
    
    // Upload Speed
    if (m_upload_test_successful && m_upload_speed > 0) {
        report += QString("║ Upload Speed:          %1 Mbps").arg(m_upload_speed, 0, 'f', 2).leftJustified(79) + QString("║\n");
    } else {
        report += QString("║ Upload Speed:          Test failed (may require HTTPS access)").leftJustified(79) + QString("║\n");
    }
    report += QString("╠").repeated(1) + QString("═").repeated(78) + QString("╣\n");
    
    // Latency and Quality Metrics
    if (m_latency > 0) {
        report += QString("║ Latency (Avg):         %1 ms").arg(m_latency).leftJustified(79) + QString("║\n");
        report += QString("║ Latency Range:         %1 - %2 ms").arg(m_min_latency).arg(m_max_latency).leftJustified(79) + QString("║\n");
        report += QString("║ Jitter:                %1 ms").arg(m_jitter, 0, 'f', 2).leftJustified(79) + QString("║\n");
        report += QString("║ Packet Loss:           %1%").arg(m_packet_loss, 0, 'f', 2).leftJustified(79) + QString("║\n");
        report += QString("╠").repeated(1) + QString("═").repeated(78) + QString("╣\n");
        
        // Connection Quality Assessment
        QString quality;
        QString recommendation;
        
        if (m_latency < 20 && m_jitter < 10 && m_packet_loss < 1.0) {
            quality = "Excellent";
            recommendation = "Ideal for gaming, video calls, and streaming";
        } else if (m_latency < 50 && m_jitter < 20 && m_packet_loss < 2.0) {
            quality = "Good";
            recommendation = "Suitable for most online activities";
        } else if (m_latency < 100 && m_jitter < 30 && m_packet_loss < 5.0) {
            quality = "Fair";
            recommendation = "May experience delays in real-time applications";
        } else {
            quality = "Poor";
            recommendation = "Not recommended for latency-sensitive tasks";
        }
        
        report += QString("║ Connection Quality:    %1").arg(quality).leftJustified(79) + QString("║\n");
        report += QString("║ Recommendation:        %1").arg(recommendation).leftJustified(79) + QString("║\n");
    } else {
        report += QString("║ Latency Test:          Failed to measure latency").leftJustified(79) + QString("║\n");
    }
    
    report += QString("╚").repeated(1) + QString("═").repeated(78) + QString("╝\n");
    
    Q_EMIT executionProgress("Speed test complete", 100);
    
    qint64 duration_ms = start_time.msecsTo(QDateTime::currentDateTime());
    
    ExecutionResult result;
    result.duration_ms = duration_ms;
    result.success = (m_download_speed > 0 || m_latency > 0);
    
    if (result.success) {
        result.message = QString("Network speed test complete - %1 Mbps down, %2 ms latency")
                            .arg(m_download_speed, 0, 'f', 2)
                            .arg(m_latency);
    } else {
        result.message = "Network speed test completed with limited results";
    }
    
    result.log = report;
    
    setExecutionResult(result);
    setStatus(result.success ? ActionStatus::Success : ActionStatus::Failed);
    Q_EMIT executionComplete(result);
}

} // namespace sak
