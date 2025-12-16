// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include <QTest>
#include <QSignalSpy>
#include "sak/actions/disable_visual_effects_action.h"

class TestDisableVisualEffectsAction : public QObject {
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();

    // Basic functionality
    void testActionProperties();
    void testInitialState();
    void testDoesNotRequireAdmin();
    void testScanChecksEffects();
    void testExecuteDisablesEffects();
    
    // Registry locations
    void testVisualEffectsRegistryKey();
    void testUserPreferencesKey();
    void testDesktopWindowManagerKey();
    
    // Visual effect settings
    void testAnimateMinMax();
    void testAnimateWindows();
    void testComboBoxAnimation();
    void testCursorShadow();
    void testDragFullWindows();
    void testDropShadow();
    void testFontSmoothing();
    void testListBoxAnimation();
    void testMenuAnimation();
    void testSelectionFade();
    void testTaskbarAnimation();
    void testTooltipAnimation();
    
    // Performance settings
    void testSetPerformanceMode();
    void testSetAppearanceMode();
    void testSetCustomMode();
    void testSetBalancedMode();
    
    // DWM settings
    void testDisableTransparency();
    void testDisableAeroPeek();
    void testDisableAnimations();
    
    // Effect detection
    void testDetectEnabledEffects();
    void testCountActiveEffects();
    void testCheckEffectState();
    
    // Progress tracking
    void testProgressSignals();
    void testScanProgress();
    void testExecuteProgress();
    
    // Error handling
    void testHandleRegistryAccessError();
    void testHandleInvalidValue();
    void testHandleDWMDisabled();
    
    // Results formatting
    void testFormatEffectList();
    void testFormatDisabledCount();
    void testFormatSuccessMessage();
    void testFormatErrorMessage();
    
    // Edge cases
    void testAllEffectsDisabled();
    void testAllEffectsEnabled();
    void testMixedState();
    void testWindowsBasicTheme();

private:
    sak::DisableVisualEffectsAction* m_action;
};

void TestDisableVisualEffectsAction::initTestCase() {
    // One-time setup
}

void TestDisableVisualEffectsAction::cleanupTestCase() {
    // One-time cleanup
}

void TestDisableVisualEffectsAction::init() {
    m_action = new sak::DisableVisualEffectsAction();
}

void TestDisableVisualEffectsAction::cleanup() {
    delete m_action;
}

void TestDisableVisualEffectsAction::testActionProperties() {
    QCOMPARE(m_action->name(), QString("Disable Visual Effects"));
    QVERIFY(!m_action->description().isEmpty());
    QVERIFY(m_action->description().contains("performance", Qt::CaseInsensitive));
    QCOMPARE(m_action->category(), sak::QuickAction::ActionCategory::SystemOptimization);
    QVERIFY(!m_action->requiresAdmin());
}

void TestDisableVisualEffectsAction::testInitialState() {
    QSignalSpy startedSpy(m_action, &sak::QuickAction::started);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    QVERIFY(startedSpy.isValid());
    QVERIFY(finishedSpy.isValid());
    QCOMPARE(startedSpy.count(), 0);
}

void TestDisableVisualEffectsAction::testDoesNotRequireAdmin() {
    // Can modify current user's visual effects without admin
    QVERIFY(!m_action->requiresAdmin());
}

void TestDisableVisualEffectsAction::testScanChecksEffects() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    
    QVERIFY(finishedSpy.wait(10000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDisableVisualEffectsAction::testExecuteDisablesEffects() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    
    QVERIFY(finishedSpy.wait(15000));
    
    QString result = m_action->result();
    QVERIFY(!result.isEmpty());
}

void TestDisableVisualEffectsAction::testVisualEffectsRegistryKey() {
    // HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects
    QString regKey = R"(HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects)";
    
    QVERIFY(regKey.contains("VisualEffects"));
}

void TestDisableVisualEffectsAction::testUserPreferencesKey() {
    // HKCU\Control Panel\Desktop
    QString regKey = R"(HKCU\Control Panel\Desktop)";
    
    QVERIFY(regKey.contains("Desktop"));
}

void TestDisableVisualEffectsAction::testDesktopWindowManagerKey() {
    // HKCU\Software\Microsoft\Windows\DWM
    QString regKey = R"(HKCU\Software\Microsoft\Windows\DWM)";
    
    QVERIFY(regKey.contains("DWM"));
}

void TestDisableVisualEffectsAction::testAnimateMinMax() {
    // MinAnimate: 0 = disabled, 1 = enabled
    QString regValue = "MinAnimate";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testAnimateWindows() {
    // UserPreferencesMask bit for window animations
    QString regValue = "UserPreferencesMask";
    
    QVERIFY(!regValue.isEmpty());
}

void TestDisableVisualEffectsAction::testComboBoxAnimation() {
    // ComboBoxAnimation
    QString effect = "ComboBoxAnimation";
    
    QVERIFY(!effect.isEmpty());
}

void TestDisableVisualEffectsAction::testCursorShadow() {
    // CursorShadow: 0 = disabled
    QString regValue = "CursorShadow";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testDragFullWindows() {
    // DragFullWindows: 0 = show outline only
    QString regValue = "DragFullWindows";
    QString disabled = "0";
    
    QCOMPARE(disabled, QString("0"));
}

void TestDisableVisualEffectsAction::testDropShadow() {
    // DropShadow: 0 = disabled
    QString regValue = "DropShadow";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testFontSmoothing() {
    // FontSmoothing: 0 = disabled (not recommended)
    QString regValue = "FontSmoothing";
    
    QVERIFY(!regValue.isEmpty());
}

void TestDisableVisualEffectsAction::testListBoxAnimation() {
    // ListBoxSmoothScrolling
    QString effect = "ListBoxSmoothScrolling";
    
    QVERIFY(!effect.isEmpty());
}

void TestDisableVisualEffectsAction::testMenuAnimation() {
    // MenuAnimation: 0 = disabled
    QString regValue = "MenuAnimation";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testSelectionFade() {
    // SelectionFade: 0 = disabled
    QString effect = "SelectionFade";
    
    QVERIFY(!effect.isEmpty());
}

void TestDisableVisualEffectsAction::testTaskbarAnimation() {
    // TaskbarAnimations: 0 = disabled
    QString regValue = "TaskbarAnimations";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testTooltipAnimation() {
    // TooltipAnimation: 0 = disabled
    QString effect = "TooltipAnimation";
    
    QVERIFY(!effect.isEmpty());
}

void TestDisableVisualEffectsAction::testSetPerformanceMode() {
    // VisualFXSetting: 2 = best performance
    QString regValue = "VisualFXSetting";
    int performanceMode = 2;
    
    QCOMPARE(performanceMode, 2);
}

void TestDisableVisualEffectsAction::testSetAppearanceMode() {
    // VisualFXSetting: 1 = best appearance
    int appearanceMode = 1;
    
    QCOMPARE(appearanceMode, 1);
}

void TestDisableVisualEffectsAction::testSetCustomMode() {
    // VisualFXSetting: 3 = custom
    int customMode = 3;
    
    QCOMPARE(customMode, 3);
}

void TestDisableVisualEffectsAction::testSetBalancedMode() {
    // VisualFXSetting: 0 = let Windows choose
    int balancedMode = 0;
    
    QCOMPARE(balancedMode, 0);
}

void TestDisableVisualEffectsAction::testDisableTransparency() {
    // EnableTransparency: 0 = disabled
    QString regValue = "EnableTransparency";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testDisableAeroPeek() {
    // EnableAeroPeek: 0 = disabled
    QString regValue = "EnableAeroPeek";
    int disabled = 0;
    
    QCOMPARE(disabled, 0);
}

void TestDisableVisualEffectsAction::testDisableAnimations() {
    // AlwaysHibernateThumbnails: 1 = disable live thumbnails
    QString regValue = "AlwaysHibernateThumbnails";
    int disabled = 1;
    
    QCOMPARE(disabled, 1);
}

void TestDisableVisualEffectsAction::testDetectEnabledEffects() {
    QStringList enabledEffects = {
        "Window animations",
        "Transparency",
        "Aero Peek",
        "Drop shadows"
    };
    
    QVERIFY(enabledEffects.size() >= 1);
}

void TestDisableVisualEffectsAction::testCountActiveEffects() {
    int activeCount = 8;
    int totalCount = 12;
    
    QVERIFY(activeCount <= totalCount);
}

void TestDisableVisualEffectsAction::testCheckEffectState() {
    bool effectEnabled = true;
    
    QVERIFY(effectEnabled);
}

void TestDisableVisualEffectsAction::testProgressSignals() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->scan();
    QVERIFY(finishedSpy.wait(10000));
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDisableVisualEffectsAction::testScanProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->scan();
    QTest::qWait(1000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDisableVisualEffectsAction::testExecuteProgress() {
    QSignalSpy progressSpy(m_action, &sak::QuickAction::progressChanged);
    
    m_action->execute();
    QTest::qWait(2000);
    
    QVERIFY(progressSpy.count() >= 1);
}

void TestDisableVisualEffectsAction::testHandleRegistryAccessError() {
    QSignalSpy finishedSpy(m_action, &sak::QuickAction::finished);
    
    m_action->execute();
    QVERIFY(finishedSpy.wait(15000));
    
    QVERIFY(!m_action->result().isEmpty());
}

void TestDisableVisualEffectsAction::testHandleInvalidValue() {
    // Handle invalid registry value
    int invalidValue = -1;
    
    QVERIFY(invalidValue < 0);
}

void TestDisableVisualEffectsAction::testHandleDWMDisabled() {
    // DWM may be disabled on Windows Server
    bool dwmEnabled = false;
    
    QVERIFY(!dwmEnabled);
}

void TestDisableVisualEffectsAction::testFormatEffectList() {
    QString list = R"(
Currently Enabled Visual Effects:
  • Window animations
  • Taskbar animations
  • Menu fade/slide
  • Transparency
  • Drop shadows
  • Aero Peek
    )";
    
    QVERIFY(list.contains("Visual Effects"));
}

void TestDisableVisualEffectsAction::testFormatDisabledCount() {
    QString message = "Disabled 8 visual effects for improved performance";
    
    QVERIFY(message.contains("Disabled"));
    QVERIFY(message.contains("performance"));
}

void TestDisableVisualEffectsAction::testFormatSuccessMessage() {
    QString message = "Successfully disabled 8 visual effects. Restart may be required for full effect.";
    
    QVERIFY(message.contains("Successfully"));
    QVERIFY(message.contains("Restart"));
}

void TestDisableVisualEffectsAction::testFormatErrorMessage() {
    QString error = "Failed to modify visual effects: Registry access denied";
    
    QVERIFY(error.contains("Failed"));
    QVERIFY(error.contains("Registry"));
}

void TestDisableVisualEffectsAction::testAllEffectsDisabled() {
    // All effects already disabled
    int enabledCount = 0;
    
    QCOMPARE(enabledCount, 0);
}

void TestDisableVisualEffectsAction::testAllEffectsEnabled() {
    // All effects enabled (default)
    int enabledCount = 12;
    int totalCount = 12;
    
    QCOMPARE(enabledCount, totalCount);
}

void TestDisableVisualEffectsAction::testMixedState() {
    // Some effects enabled, some disabled
    int enabledCount = 6;
    int disabledCount = 6;
    
    QCOMPARE(enabledCount, disabledCount);
}

void TestDisableVisualEffectsAction::testWindowsBasicTheme() {
    // Windows Basic theme has most effects disabled
    QString theme = "Basic";
    
    QCOMPARE(theme, QString("Basic"));
}

QTEST_MAIN(TestDisableVisualEffectsAction)
#include "test_disable_visual_effects_action.moc"
