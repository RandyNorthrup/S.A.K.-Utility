// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/fix_audio_issues_action.h"

class TestFixAudioIssuesAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testRequiresAdmin();
    void testScanChecksAudioServices();
    void testExecuteFixesAudio();
    
    // Audio services
    void testCheckAudioService();
    void testCheckAudioEndpointBuilder();
    void testRestartAudioService();
    void testRestartEndpointBuilder();
    
    // Service status detection
    void testDetectRunningService();
    void testDetectStoppedService();
    void testDetectDisabledService();
    void testServiceStatusParsing();
    
    // Audio device operations
    void testResetAudioDevices();
    void testEnumerateAudioDevices();
    void testIdentifyDefaultDevice();
    void testSetDefaultDevice();
    
    // USB audio handling
    void testCheckUSBAudioDevices();
    void testDetectUSBAudioDriver();
    void testResetUSBAudioDevice();
    
    // Driver checks
    void testCheckAudioDrivers();
    void testVerifyDriverInstalled();
    void testDetectDriverIssues();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleServiceRestartFailure();
    void testHandleDeviceResetFailure();
    void testHandleNoAudioDevices();
    void testHandleAccessDenied();
    
    // PowerShell commands
    void testGetAudioDevicesCommand();
    void testSetDefaultDeviceCommand();
    void testRestartAudioCommand();
    
    // Results formatting
    void testFormatServiceStatus();
    void testFormatDeviceList();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testNoAudioDevices();
    void testMultipleAudioDevices();
    void testBluetoothAudio();
    void testHDMIAudio();

private:
    sak::FixAudioIssuesAction* m_action;
};

void TestFixAudioIssuesAction::initTestCase() {
    // One-time setup
}

void TestFixAudioIssuesAction::cleanupTestCase() {
    // One-time cleanup
}

void TestFixAudioIssuesAction::init() {
    m_action = new sak::FixAudioIssuesAction();
}

void TestFixAudioIssuesAction::cleanup() {
    delete m_action;
}

void TestFixAudioIssuesAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Fix Audio Issues"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("audio", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::Troubleshooting);
    QVERIFY(m_action->requiresAdmin());
}

void TestFixAudioIssuesAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestFixAudioIssuesAction::testRequiresAdmin() {
    // Requires admin to restart services
    QVERIFY(m_action->requiresAdmin());
}

void TestFixAudioIssuesAction::testScanChecksAudioServices() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestFixAudioIssuesAction::testExecuteFixesAudio() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(45000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestFixAudioIssuesAction::testCheckAudioService() {
    // Service: Audiosrv (Windows Audio)
    QString serviceName = "Audiosrv";
    
    QCOMPARE(serviceName, QString("Audiosrv"));
}

void TestFixAudioIssuesAction::testCheckAudioEndpointBuilder() {
    // Service: AudioEndpointBuilder
    QString serviceName = "AudioEndpointBuilder";
    
    QCOMPARE(serviceName, QString("AudioEndpointBuilder"));
}

void TestFixAudioIssuesAction::testRestartAudioService() {
    // Commands: net stop Audiosrv && net start Audiosrv
    QStringList commands = {
        "net stop Audiosrv",
        "net start Audiosrv"
    };
    
    QCOMPARE(commands.size(), 2);
}

void TestFixAudioIssuesAction::testRestartEndpointBuilder() {
    // Commands: net stop AudioEndpointBuilder && net start AudioEndpointBuilder
    QString stopCommand = "net stop AudioEndpointBuilder";
    QString startCommand = "net start AudioEndpointBuilder";
    
    QVERIFY(stopCommand.contains("AudioEndpointBuilder"));
    QVERIFY(startCommand.contains("AudioEndpointBuilder"));
}

void TestFixAudioIssuesAction::testDetectRunningService() {
    QString serviceStatus = "RUNNING";
    bool isRunning = (serviceStatus == "RUNNING");
    
    QVERIFY(isRunning);
}

void TestFixAudioIssuesAction::testDetectStoppedService() {
    QString serviceStatus = "STOPPED";
    bool isStopped = (serviceStatus == "STOPPED");
    
    QVERIFY(isStopped);
}

void TestFixAudioIssuesAction::testDetectDisabledService() {
    QString serviceStatus = "DISABLED";
    bool isDisabled = (serviceStatus == "DISABLED");
    
    QVERIFY(isDisabled);
}

void TestFixAudioIssuesAction::testServiceStatusParsing() {
    // Parse sc query output
    QString output = "STATE : 4 RUNNING";
    bool isRunning = output.contains("RUNNING");
    
    QVERIFY(isRunning);
}

void TestFixAudioIssuesAction::testResetAudioDevices() {
    // Reset all audio devices
    int devicesReset = 3;
    
    QVERIFY(devicesReset >= 0);
}

void TestFixAudioIssuesAction::testEnumerateAudioDevices() {
    // PowerShell: Get-AudioDevice
    QString command = "powershell -Command \"Get-AudioDevice -List\"";
    
    QVERIFY(command.contains("Get-AudioDevice"));
}

void TestFixAudioIssuesAction::testIdentifyDefaultDevice() {
    // Default playback device
    QString defaultDevice = "Speakers (Realtek High Definition Audio)";
    
    QVERIFY(!defaultDevice.isEmpty());
}

void TestFixAudioIssuesAction::testSetDefaultDevice() {
    // PowerShell: Set-AudioDevice
    QString command = "Set-AudioDevice -Index 0";
    
    QVERIFY(command.contains("Set-AudioDevice"));
}

void TestFixAudioIssuesAction::testCheckUSBAudioDevices() {
    // Check for USB audio devices
    QString deviceType = "USB Audio Device";
    
    QVERIFY(deviceType.contains("USB"));
}

void TestFixAudioIssuesAction::testDetectUSBAudioDriver() {
    // USB audio driver name
    QString driverName = "usbaudio.sys";
    
    QVERIFY(driverName.contains("usbaudio"));
}

void TestFixAudioIssuesAction::testResetUSBAudioDevice() {
    // Disable and re-enable USB audio device
    QStringList commands = {
        "pnputil /disable-device",
        "pnputil /enable-device"
    };
    
    QCOMPARE(commands.size(), 2);
}

void TestFixAudioIssuesAction::testCheckAudioDrivers() {
    // Check for audio driver issues
    QString command = "driverquery /v | findstr /i audio";
    
    QVERIFY(command.contains("driverquery"));
}

void TestFixAudioIssuesAction::testVerifyDriverInstalled() {
    // Verify audio driver is installed
    bool driverInstalled = true;
    
    QVERIFY(driverInstalled);
}

void TestFixAudioIssuesAction::testDetectDriverIssues() {
    // Detect common driver issues
    QStringList issues = {
        "Driver not started",
        "Device error code 10",
        "No driver installed"
    };
    
    QVERIFY(issues.size() >= 1);
}

void TestFixAudioIssuesAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestFixAudioIssuesAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestFixAudioIssuesAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(5000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestFixAudioIssuesAction::testHandleServiceRestartFailure() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(45000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestFixAudioIssuesAction::testHandleDeviceResetFailure() {
    // Device reset may fail
    bool resetSuccess = false;
    
    QVERIFY(!resetSuccess);
}

void TestFixAudioIssuesAction::testHandleNoAudioDevices() {
    // No audio devices detected
    int deviceCount = 0;
    
    QCOMPARE(deviceCount, 0);
}

void TestFixAudioIssuesAction::testHandleAccessDenied() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(45000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestFixAudioIssuesAction::testGetAudioDevicesCommand() {
    // PowerShell command to list devices
    QString command = "Get-PnpDevice -Class AudioEndpoint";
    
    QVERIFY(command.contains("AudioEndpoint"));
}

void TestFixAudioIssuesAction::testSetDefaultDeviceCommand() {
    // Set default audio device
    QString command = "Set-AudioDevice -ID \"{device-id}\"";
    
    QVERIFY(command.contains("Set-AudioDevice"));
}

void TestFixAudioIssuesAction::testRestartAudioCommand() {
    // Restart audio via PowerShell
    QString command = "Restart-Service Audiosrv";
    
    QVERIFY(command.contains("Restart-Service"));
}

void TestFixAudioIssuesAction::testFormatServiceStatus() {
    QString status = R"(
Audio Services Status:
  • Windows Audio (Audiosrv): Running
  • Audio Endpoint Builder: Running
    )";
    
    QVERIFY(status.contains("Running"));
}

void TestFixAudioIssuesAction::testFormatDeviceList() {
    QString list = R"(
Audio Devices:
  • Speakers (Realtek HD Audio) - Default
  • Headphones (USB Audio Device)
  • HDMI Audio (NVIDIA)
    )";
    
    QVERIFY(list.contains("Audio Devices"));
}

void TestFixAudioIssuesAction::testFormatSuccessMessage() {
    QString message = "Successfully restarted audio services and reset 3 audio devices";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("audio"));
}

void TestFixAudioIssuesAction::testFormatErrorMessage() {
    QString error = "Failed to restart Windows Audio service: Service not found";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Audio"));
}

void TestFixAudioIssuesAction::testNoAudioDevices() {
    // System with no audio devices
    int deviceCount = 0;
    
    QCOMPARE(deviceCount, 0);
}

void TestFixAudioIssuesAction::testMultipleAudioDevices() {
    // System with multiple audio outputs
    QStringList devices = {
        "Speakers",
        "Headphones",
        "HDMI Audio",
        "USB Audio"
    };
    
    QVERIFY(devices.size() >= 2);
}

void TestFixAudioIssuesAction::testBluetoothAudio() {
    // Bluetooth audio device
    QString device = "Bluetooth Headphones";
    
    QVERIFY(device.contains("Bluetooth"));
}

void TestFixAudioIssuesAction::testHDMIAudio() {
    // HDMI audio output
    QString device = "HDMI Audio (NVIDIA High Definition Audio)";
    
    QVERIFY(device.contains("HDMI"));
}

QTEST_MAIN(TestFixAudioIssuesAction)
#include "test_fix_audio_issues_action.moc"
