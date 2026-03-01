// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QPushButton>
#include <QListWidget>
#include <QTextEdit>
#include <QLabel>
#include <QSpinBox>
#include <QCheckBox>
#include <memory>

class QVBoxLayout;

class DuplicateFinderWorker;

namespace sak {
class DetachableLogWindow;
class LogToggleSwitch;

/**
 * @brief Duplicate file finder feature panel
 * 
 * Scans directories for duplicate files and reports results.
 */
class DuplicateFinderPanel : public QWidget {
    Q_OBJECT

public:
    explicit DuplicateFinderPanel(QWidget* parent = nullptr);
    ~DuplicateFinderPanel() override;

    DuplicateFinderPanel(const DuplicateFinderPanel&) = delete;
    DuplicateFinderPanel& operator=(const DuplicateFinderPanel&) = delete;
    DuplicateFinderPanel(DuplicateFinderPanel&&) = delete;
    DuplicateFinderPanel& operator=(DuplicateFinderPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onAddDirectoryClicked();
    void onRemoveDirectoryClicked();
    void onScanClicked();
    void onCancelClicked();
    void onSettingsClicked();
    
    void onWorkerStarted();
    void onWorkerFinished();
    void onWorkerFailed(int errorCode, const QString& errorMessage);
    void onWorkerCancelled();
    void onScanProgress(int current, int total, const QString& path);
    void onResultsReady(const QString& summary, int duplicateCount, qint64 wastedSpace);

private:
    void setupUi();
    void createDirectoryGroup(QVBoxLayout* layout);
    void createOptionsWidgets();
    void createControlButtons(QVBoxLayout* layout);
    void setOperationRunning(bool running);
    void logMessage(const QString& message);

    QListWidget* m_directory_list{nullptr};
    QPushButton* m_add_directory_button{nullptr};
    QPushButton* m_remove_directory_button{nullptr};
    
    QSpinBox* m_min_size_spinbox{nullptr};
    QCheckBox* m_recursive_checkbox{nullptr};
    

    
    QPushButton* m_scan_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    
    LogToggleSwitch* m_logToggle{nullptr};
    
    std::unique_ptr<DuplicateFinderWorker> m_worker;
    bool m_operation_running{false};
};

} // namespace sak
