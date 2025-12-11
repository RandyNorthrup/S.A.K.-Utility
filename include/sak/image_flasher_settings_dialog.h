// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <QDialog>
#include <QCheckBox>
#include <QSpinBox>
#include <QComboBox>
#include "config_manager.h"

/**
 * @brief Settings dialog for Image Flasher configuration
 * 
 * Allows users to configure Image Flasher behavior including
 * verification mode, buffer size, and other options.
 */
class ImageFlasherSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ImageFlasherSettingsDialog(sak::ConfigManager* config, QWidget* parent = nullptr);
    ~ImageFlasherSettingsDialog() override;

private Q_SLOTS:
    void onAccept();
    void onResetDefaults();

private:
    void setupUI();
    void loadSettings();
    void saveSettings();

    sak::ConfigManager* m_config;
    
    // UI Components
    QComboBox* m_validationModeCombo;
    QSpinBox* m_bufferSizeSpin;
    QCheckBox* m_unmountOnCompletionCheck;
    QCheckBox* m_showSystemDriveWarningCheck;
    QCheckBox* m_showLargeDriveWarningCheck;
    QSpinBox* m_largeDriveThresholdSpin;
    QSpinBox* m_maxConcurrentWritesSpin;
    QCheckBox* m_enableNotificationsCheck;
};
