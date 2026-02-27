// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QSpinBox>
#include <QCheckBox>
#include <QLineEdit>
#include <QPushButton>
#include <memory>

namespace sak {

class ConfigManager;

/**
 * @brief Settings dialog for application configuration
 * 
 * Provides GUI interface to ConfigManager with:
 * - Tabbed interface for different setting categories
 * - Real-time validation
 * - Apply/OK/Cancel buttons
 * - Reset to defaults
 */
class SettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit SettingsDialog(QWidget* parent = nullptr);
    ~SettingsDialog() override = default;

    SettingsDialog(const SettingsDialog&) = delete;
    SettingsDialog& operator=(const SettingsDialog&) = delete;
    SettingsDialog(SettingsDialog&&) = delete;
    SettingsDialog& operator=(SettingsDialog&&) = delete;

private Q_SLOTS:
    void onApplyClicked();
    void onOkClicked();
    void onCancelClicked();
    void onResetToDefaultsClicked();
    void onSettingChanged();

private:
    void setupUI();
    void createBackupTab();
    
    void loadSettings();
    void saveSettings();
    void applySettings();
    bool validateSettings();

    // UI Components
    QTabWidget* m_tabWidget{nullptr};
    QPushButton* m_okButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QPushButton* m_applyButton{nullptr};
    QPushButton* m_resetButton{nullptr};

    // Backup Tab
    QSpinBox* m_backupThreadCount{nullptr};
    QCheckBox* m_backupVerifyMD5{nullptr};
    QLineEdit* m_lastBackupLocation{nullptr};

    // Quick Actions Settings (in Backup tab)
    QLineEdit* m_quickActionsBackupLocation{nullptr};
    QCheckBox* m_quickActionsConfirm{nullptr};
    QCheckBox* m_quickActionsNotifications{nullptr};
    QCheckBox* m_quickActionsLogging{nullptr};
    QCheckBox* m_quickActionsCompress{nullptr};

    bool m_settingsModified{false};
};

} // namespace sak
