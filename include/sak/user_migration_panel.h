// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTableView>
#include <QTextEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <memory>
#include <vector>

class QFrame;

namespace sak {
class UserDataManager;
class LogToggleSwitch;

/**
 * @brief Windows User Migration Panel
 *
 * Provides comprehensive user profile migration and restore functionality:
 * - User-friendly backup wizard with automatic profile scanning
 * - Intelligent folder selection (Documents, Desktop, Pictures, etc.)
 * - Permission handling and elevation when needed
 * - Restore wizard with user mapping and conflict resolution
 * - Detailed operation logging and progress tracking
 *
 * Similar to Application Installation panel but for user data.
 */
class UserMigrationPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent widget
     */
    explicit UserMigrationPanel(QWidget* parent = nullptr);

    /**
     * @brief Destructor
     */
    ~UserMigrationPanel() override;

    // Disable copy and move
    UserMigrationPanel(const UserMigrationPanel&) = delete;
    UserMigrationPanel& operator=(const UserMigrationPanel&) = delete;
    UserMigrationPanel(UserMigrationPanel&&) = delete;
    UserMigrationPanel& operator=(UserMigrationPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Main actions
    void onBackupSelected();
    void onRestoreBackup();
    void onSettingsClicked();

private:
    /// @brief Configuration for a migration card widget
    struct MigrationCardConfig {
        QString card_style;
        QString icon;
        QString title;
        QString desc;
        QString btn_text;
        QString btn_style;
        QString tip;
        QString acc;
    };

    void setupUi();
    void createMigrationCards(QWidget* parent, QVBoxLayout* layout);
    QFrame* createMigrationCard(QWidget* parent,
                                const MigrationCardConfig& config,
                                QPushButton*& btn);
    void setupConnections();
    void appendLog(const QString& message);

    // UI Components
    QPushButton* m_backupButton{nullptr};
    QPushButton* m_restoreButton{nullptr};
    LogToggleSwitch* m_logToggle{nullptr};

    // Data
    std::shared_ptr<UserDataManager> m_dataManager;
};

}  // namespace sak
