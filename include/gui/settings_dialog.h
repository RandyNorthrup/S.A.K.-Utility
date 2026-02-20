#pragma once

#include <QDialog>
#include <QTabWidget>
#include <QSpinBox>
#include <QCheckBox>
#include <QComboBox>
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
    void createGeneralTab();
    void createBackupTab();
    void createDuplicateFinderTab();
    void createAdvancedTab();
    
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

    // General Tab (Window + Organizer settings)
    QCheckBox* m_restoreWindowGeometry{nullptr};
    QCheckBox* m_organizerPreviewMode{nullptr};

    // Backup Tab
    QSpinBox* m_backupThreadCount{nullptr};
    QCheckBox* m_backupVerifyMD5{nullptr};
    QLineEdit* m_lastBackupLocation{nullptr};

    // Duplicate Finder Tab
    QSpinBox* m_duplicateMinFileSize{nullptr};
    QComboBox* m_duplicateKeepStrategy{nullptr};

    // Network Transfer (Advanced Tab)
    QCheckBox* m_networkTransferEnabled{nullptr};
    QCheckBox* m_networkTransferAutoDiscovery{nullptr};
    QCheckBox* m_networkTransferEncryption{nullptr};
    QCheckBox* m_networkTransferCompression{nullptr};
    QCheckBox* m_networkTransferResume{nullptr};
    QSpinBox* m_networkTransferDiscoveryPort{nullptr};
    QSpinBox* m_networkTransferControlPort{nullptr};
    QSpinBox* m_networkTransferDataPort{nullptr};
    QSpinBox* m_networkTransferChunkSize{nullptr};
    QSpinBox* m_networkTransferMaxBandwidth{nullptr};
    QLineEdit* m_networkTransferRelayServer{nullptr};

    bool m_settingsModified{false};
};

} // namespace sak
