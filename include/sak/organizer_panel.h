// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QLineEdit>
#include <QTableWidget>
#include <QTextEdit>
#include <QLabel>
#include <QCheckBox>
#include <QComboBox>
#include <QMap>
#include <memory>

class QVBoxLayout;
class OrganizerWorker;

namespace sak {
class DetachableLogWindow;
class LogToggleSwitch;

/**
 * @brief Directory organizer feature panel
 * 
 * Organizes files by extension into categorized subdirectories.
 */
class OrganizerPanel : public QWidget {
    Q_OBJECT

public:
    explicit OrganizerPanel(QWidget* parent = nullptr);
    ~OrganizerPanel() override;

    OrganizerPanel(const OrganizerPanel&) = delete;
    OrganizerPanel& operator=(const OrganizerPanel&) = delete;
    OrganizerPanel(OrganizerPanel&&) = delete;
    OrganizerPanel& operator=(OrganizerPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onBrowseClicked();
    void onPreviewClicked();
    void onExecuteClicked();
    void onCancelClicked();
    void onAddCategoryClicked();
    void onRemoveCategoryClicked();
    void onSettingsClicked();
    
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int errorCode, const QString& errorMessage);
    void onWorkerCancelled();
    void onFileProgress(int current, int total, const QString& filePath);
    void onPreviewResults(const QString& summary, int operationCount);

private:
    void setupUi();
    void setupUi_directoryAndCategories(QVBoxLayout* mainLayout);
    void setupUi_controlsAndConnections(QVBoxLayout* mainLayout);
    void setupDefaultCategories();
    QMap<QString, QStringList> getCategoryMapping() const;
    void setOperationRunning(bool running);
    void logMessage(const QString& message);

    QLineEdit* m_target_path{nullptr};
    QPushButton* m_browse_button{nullptr};
    
    QTableWidget* m_category_table{nullptr};
    QPushButton* m_add_category_button{nullptr};
    QPushButton* m_remove_category_button{nullptr};
    
    QComboBox* m_collision_strategy{nullptr};
    QCheckBox* m_preview_mode_checkbox{nullptr};
    

    
    QPushButton* m_preview_button{nullptr};
    QPushButton* m_execute_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    
    LogToggleSwitch* m_logToggle{nullptr};
    
    std::unique_ptr<OrganizerWorker> m_worker;
    bool m_operation_running{false};
};

} // namespace sak
