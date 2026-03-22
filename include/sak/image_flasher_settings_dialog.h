// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QDialog>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

class QVBoxLayout;

/**
 * @brief Settings dialog for Image Flasher configuration
 *
 * Allows users to configure Image Flasher behavior including
 * verification mode, buffer size, and other options.
 */
class ImageFlasherSettingsDialog : public QDialog {
    Q_OBJECT

public:
    explicit ImageFlasherSettingsDialog(QWidget* parent = nullptr);
    ~ImageFlasherSettingsDialog() override;

private Q_SLOTS:
    void onAccept();
    void onResetDefaults();
    void onClearDownloadCaches();

private:
    void setupUi();
    void setupUi_generalSection(QVBoxLayout* mainLayout);
    void setupUi_advancedSection(QVBoxLayout* mainLayout);
    void setupUi_buttonBar(QVBoxLayout* mainLayout);
    void loadSettings();
    void saveSettings();

    // UI Components
    QComboBox* m_validationModeCombo;
    QSpinBox* m_bufferSizeSpin;
    QCheckBox* m_unmountOnCompletionCheck;
    QCheckBox* m_showSystemDriveWarningCheck;
    QCheckBox* m_showLargeDriveWarningCheck;
    QSpinBox* m_largeDriveThresholdSpin;
    QSpinBox* m_maxConcurrentWritesSpin;
    QCheckBox* m_enableNotificationsCheck;

    // Storage
    QLabel* m_cacheInfoLabel;
    QPushButton* m_clearCacheButton;

    // Helpers
    void updateCacheInfo();
    static qint64 calculateCacheSize();
    static QStringList findCacheDirectories();
};
