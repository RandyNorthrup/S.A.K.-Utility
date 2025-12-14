// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/quick_actions_panel.h"
#include "sak/quick_action_controller.h"
#include "sak/actions/disk_cleanup_action.h"
#include "sak/actions/quickbooks_backup_action.h"
#include "sak/actions/clear_browser_cache_action.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QScrollArea>
#include <QGroupBox>
#include <QPushButton>
#include <QLabel>
#include <QLineEdit>
#include <QCheckBox>
#include <QProgressBar>
#include <QTextEdit>
#include <QFileDialog>
#include <QMessageBox>
#include <QSettings>
#include <QDateTime>
#include <QDesktopServices>
#include <QUrl>
#include <QTimer>
#include <QToolButton>
#include <QScrollBar>

namespace sak {

QuickActionsPanel::QuickActionsPanel(QWidget* parent)
    : QWidget(parent)
    , m_controller(new QuickActionController(this)) {
    setupUi();
    createActions();
    loadSettings();

    // Connect controller signals
    connect(m_controller, &QuickActionController::actionScanComplete,
            this, &QuickActionsPanel::onActionScanComplete, Qt::QueuedConnection);
    connect(m_controller, &QuickActionController::actionExecutionProgress,
            this, &QuickActionsPanel::onActionProgress, Qt::QueuedConnection);
    connect(m_controller, &QuickActionController::actionExecutionComplete,
            this, &QuickActionsPanel::onActionComplete, Qt::QueuedConnection);
    connect(m_controller, &QuickActionController::actionError,
            this, &QuickActionsPanel::onActionError, Qt::QueuedConnection);
    connect(m_controller, &QuickActionController::logMessage,
            this, &QuickActionsPanel::appendLog, Qt::QueuedConnection);

    // Auto-scan on startup
    QTimer::singleShot(500, this, &QuickActionsPanel::refreshAllScans);
}

QuickActionsPanel::~QuickActionsPanel() {
    saveSettings();
}

void QuickActionsPanel::setupUi() {
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(20, 20, 20, 20);
    main_layout->setSpacing(15);

    // Header
    auto* header_label = new QLabel("<h2>Quick Actions</h2>");
    main_layout->addWidget(header_label);

    auto* subtitle = new QLabel("One-click technician tools for common maintenance tasks");
    subtitle->setStyleSheet("color: #666; margin-bottom: 10px;");
    main_layout->addWidget(subtitle);

    // Settings section
    auto* settings_group = new QGroupBox("Settings");
    settings_group->setStyleSheet(
        "QGroupBox {"
        "  font-weight: bold;"
        "  border: 2px solid #ddd;"
        "  border-radius: 5px;"
        "  margin-top: 10px;"
        "  padding-top: 10px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  subcontrol-position: top left;"
        "  padding: 0 5px;"
        "  color: #333;"
        "}"
    );
    
    auto* settings_layout = new QGridLayout(settings_group);
    settings_layout->setSpacing(10);

    // Backup location
    auto* backup_label = new QLabel("Backup Location:");
    m_backup_location_edit = new QLineEdit();
    m_backup_location_edit->setPlaceholderText("C:\\SAK_Backups");
    auto* browse_button = new QPushButton("Browse...");
    connect(browse_button, &QPushButton::clicked, this, &QuickActionsPanel::onBrowseBackupLocation);

    settings_layout->addWidget(backup_label, 0, 0);
    settings_layout->addWidget(m_backup_location_edit, 0, 1);
    settings_layout->addWidget(browse_button, 0, 2);

    // Checkboxes
    m_confirm_checkbox = new QCheckBox("Confirm before executing actions");
    m_confirm_checkbox->setChecked(true);
    m_notifications_checkbox = new QCheckBox("Show completion notifications");
    m_notifications_checkbox->setChecked(true);
    m_logging_checkbox = new QCheckBox("Enable detailed logging");
    m_logging_checkbox->setChecked(true);
    m_compression_checkbox = new QCheckBox("Compress backups (saves space)");
    m_compression_checkbox->setChecked(true);

    settings_layout->addWidget(m_confirm_checkbox, 1, 0, 1, 3);
    settings_layout->addWidget(m_notifications_checkbox, 2, 0, 1, 3);
    settings_layout->addWidget(m_logging_checkbox, 3, 0, 1, 3);
    settings_layout->addWidget(m_compression_checkbox, 4, 0, 1, 3);

    main_layout->addWidget(settings_group);

    // Actions section (scrollable)
    auto* scroll_area = new QScrollArea();
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* scroll_widget = new QWidget();
    m_actions_layout = new QVBoxLayout(scroll_widget);
    m_actions_layout->setSpacing(15);
    m_actions_layout->addStretch();

    scroll_area->setWidget(scroll_widget);
    main_layout->addWidget(scroll_area, 1);

    // Status section
    auto* status_group = new QGroupBox("Status");
    status_group->setStyleSheet(settings_group->styleSheet());
    auto* status_layout = new QVBoxLayout(status_group);

    // Progress bar
    m_progress_bar = new QProgressBar();
    m_progress_bar->setRange(0, 100);
    m_progress_bar->setValue(0);
    m_progress_bar->setTextVisible(true);
    status_layout->addWidget(m_progress_bar);

    // Status labels
    auto* labels_layout = new QGridLayout();
    labels_layout->setSpacing(5);

    m_action_label = new QLabel("Action: Ready");
    m_status_label = new QLabel("Status: Idle");
    m_location_label = new QLabel("Location: -");
    m_duration_label = new QLabel("Duration: -");
    m_bytes_label = new QLabel("Bytes: -");

    labels_layout->addWidget(m_action_label, 0, 0);
    labels_layout->addWidget(m_status_label, 0, 1);
    labels_layout->addWidget(m_location_label, 1, 0);
    labels_layout->addWidget(m_duration_label, 1, 1);
    labels_layout->addWidget(m_bytes_label, 2, 0, 1, 2);

    status_layout->addLayout(labels_layout);

    // Action buttons
    auto* action_buttons_layout = new QHBoxLayout();
    m_open_folder_button = new QPushButton("Open Output Folder");
    m_open_folder_button->setEnabled(false);
    connect(m_open_folder_button, &QPushButton::clicked, this, [this]() {
        if (!m_last_output_path.isEmpty()) {
            QDesktopServices::openUrl(QUrl::fromLocalFile(m_last_output_path));
        }
    });

    m_view_log_button = new QPushButton("View Log");
    connect(m_view_log_button, &QPushButton::clicked, this, [this]() {
        if (m_log_viewer->isVisible()) {
            m_log_viewer->hide();
        } else {
            m_log_viewer->show();
        }
    });

    auto* refresh_button = new QPushButton("Refresh All Scans");
    connect(refresh_button, &QPushButton::clicked, this, &QuickActionsPanel::refreshAllScans);

    action_buttons_layout->addWidget(m_open_folder_button);
    action_buttons_layout->addWidget(m_view_log_button);
    action_buttons_layout->addStretch();
    action_buttons_layout->addWidget(refresh_button);

    status_layout->addLayout(action_buttons_layout);

    main_layout->addWidget(status_group);

    // Log viewer (hidden by default)
    m_log_viewer = new QTextEdit();
    m_log_viewer->setReadOnly(true);
    m_log_viewer->setMaximumHeight(150);
    m_log_viewer->hide();
    main_layout->addWidget(m_log_viewer);
}

void QuickActionsPanel::createActions() {
    // Register all actions with the controller
    
    // System Optimization
    m_controller->registerAction(std::make_unique<DiskCleanupAction>());
    m_controller->registerAction(std::make_unique<ClearBrowserCacheAction>());
    
    // Quick Backups
    m_controller->registerAction(std::make_unique<QuickBooksBackupAction>(
        m_backup_location_edit->text()));
    
    // Create category sections after registering actions
    createCategorySections();
}

void QuickActionsPanel::createCategorySections() {
    // Define category display names and order
    struct CategoryInfo {
        QuickAction::ActionCategory category;
        QString title;
        QString description;
    };

    const std::vector<CategoryInfo> categories = {
        {QuickAction::ActionCategory::SystemOptimization, 
         "System Optimization", 
         "Clean temporary files, optimize performance"},
        {QuickAction::ActionCategory::QuickBackup,
         "Quick Backups",
         "Fast backup of critical user data"},
        {QuickAction::ActionCategory::Maintenance,
         "Maintenance",
         "Regular maintenance and health checks"},
        {QuickAction::ActionCategory::Troubleshooting,
         "Troubleshooting",
         "Diagnostic and repair tools"},
        {QuickAction::ActionCategory::EmergencyRecovery,
         "Emergency Recovery",
         "Critical recovery operations"}
    };

    for (const auto& cat_info : categories) {
        auto* group_box = new QGroupBox(cat_info.title);
        group_box->setStyleSheet(
            "QGroupBox {"
            "  font-weight: bold;"
            "  border: 2px solid #ddd;"
            "  border-radius: 5px;"
            "  margin-top: 10px;"
            "  padding: 15px;"
            "  background-color: #f9f9f9;"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  subcontrol-position: top left;"
            "  padding: 0 5px;"
            "  color: #333;"
            "}"
        );

        auto* cat_layout = new QVBoxLayout(group_box);
        
        // Description
        auto* desc_label = new QLabel(cat_info.description);
        desc_label->setStyleSheet("color: #666; font-weight: normal; font-size: 11px;");
        cat_layout->addWidget(desc_label);

        // Action buttons grid
        auto* buttons_grid = new QGridLayout();
        buttons_grid->setSpacing(10);
        cat_layout->addLayout(buttons_grid);

        // Store group box
        m_category_sections[cat_info.category] = group_box;
        m_actions_layout->insertWidget(m_actions_layout->count() - 1, group_box);

        // Populate with actions
        auto actions = m_controller->getActionsByCategory(cat_info.category);
        int row = 0, col = 0;
        const int cols_per_row = 2;

        for (auto* action : actions) {
            auto* button = createActionButton(action);
            buttons_grid->addWidget(button, row, col);
            m_action_buttons[action] = button;

            col++;
            if (col >= cols_per_row) {
                col = 0;
                row++;
            }
        }

        // Hide empty categories
        if (actions.empty()) {
            group_box->hide();
        }
    }
}

QPushButton* QuickActionsPanel::createActionButton(QuickAction* action) {
    auto* button = new QPushButton();
    button->setMinimumHeight(60);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    button->setStyleSheet(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 10px;"
        "  border: 2px solid #ccc;"
        "  border-radius: 5px;"
        "  background-color: white;"
        "  font-weight: normal;"
        "}"
        "QPushButton:hover {"
        "  border-color: #0078d4;"
        "  background-color: #f0f8ff;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #e0e0e0;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #f5f5f5;"
        "  color: #999;"
        "}"
    );

    connect(button, &QPushButton::clicked, this, [this, action]() {
        onActionClicked(action);
    });

    updateActionButton(action);
    return button;
}

void QuickActionsPanel::updateActionButton(QuickAction* action) {
    auto* button = m_action_buttons.value(action, nullptr);
    if (!button) {
        return;
    }

    QString text = QString("<b>%1</b><br><span style='font-size:10px; color:#666;'>%2</span>")
                      .arg(action->name(), action->description());

    // Add scan result if available
    if (action->status() == QuickAction::ActionStatus::Ready) {
        const auto& scan_result = action->lastScanResult();
        if (scan_result.applicable) {
            QString size_text = formatBytes(scan_result.bytes_affected);
            qint64 est_seconds = scan_result.estimated_duration_ms / 1000;
            QString time_text = formatDuration(est_seconds);
            text += QString("<br><span style='font-size:9px; color:#0078d4;'>%1 • %2 estimated</span>")
                       .arg(size_text, time_text);
        } else {
            text += "<br><span style='font-size:9px; color:#999;'>Not applicable</span>";
            button->setEnabled(false);
        }
    }

    // Status indicator
    QString status_icon;
    switch (action->status()) {
        case QuickAction::ActionStatus::Idle:
            status_icon = "⚪";
            break;
        case QuickAction::ActionStatus::Scanning:
            status_icon = "🔍";
            break;
        case QuickAction::ActionStatus::Ready:
            status_icon = action->lastScanResult().applicable ? "✅" : "❌";
            break;
        case QuickAction::ActionStatus::Running:
            status_icon = "⏳";
            button->setEnabled(false);
            break;
        case QuickAction::ActionStatus::Success:
            status_icon = "✔️";
            break;
        case QuickAction::ActionStatus::Failed:
            status_icon = "❌";
            break;
        case QuickAction::ActionStatus::Cancelled:
            status_icon = "⚠️";
            break;
    }

    button->setText(status_icon + " " + text);
}

void QuickActionsPanel::onActionClicked(QuickAction* action) {
    if (!action) {
        return;
    }

    // Check if confirmation needed
    if (m_confirm_checkbox->isChecked()) {
        const auto& scan_result = action->lastScanResult();
        QString warning_text;
        if (!scan_result.warning.isEmpty()) {
            warning_text = QString("\n\nWarning: %1").arg(scan_result.warning);
        }

        QMessageBox confirm_box(this);
        confirm_box.setWindowTitle("Confirm Action");
        confirm_box.setText(QString("Execute %1?").arg(action->name()));
        confirm_box.setInformativeText(
            QString("%1\n\nThis will affect approximately %2 in %3 files.%4")
                .arg(action->description())
                .arg(formatBytes(scan_result.bytes_affected))
                .arg(scan_result.files_count)
                .arg(warning_text)
        );
        confirm_box.setStandardButtons(QMessageBox::Yes | QMessageBox::No);
        confirm_box.setDefaultButton(QMessageBox::No);
        confirm_box.setIcon(QMessageBox::Question);

        if (confirm_box.exec() != QMessageBox::Yes) {
            return;
        }
    }

    // Execute
    m_current_action = action;
    m_last_action_time = QDateTime::currentDateTime();
    m_controller->executeAction(action->name(), false);
}

void QuickActionsPanel::onActionScanComplete(QuickAction* action) {
    updateActionButton(action);
    
    if (m_logging_checkbox->isChecked()) {
        const auto& result = action->lastScanResult();
        Q_EMIT status_message(
            QString("%1 scan complete: %2").arg(action->name(), result.summary),
            3000
        );
    }
}

void QuickActionsPanel::onActionProgress(QuickAction* action, const QString& message, int progress) {
    if (action != m_current_action) {
        return;
    }

    m_action_label->setText(QString("Action: %1").arg(action->name()));
    m_status_label->setText(QString("Status: %1").arg(message));
    m_progress_bar->setValue(progress);

    // Update duration
    if (m_last_action_time.isValid()) {
        qint64 elapsed = m_last_action_time.secsTo(QDateTime::currentDateTime());
        m_duration_label->setText(QString("Duration: %1").arg(formatDuration(elapsed)));
    }

    updateActionButton(action);
    Q_EMIT progress_update(progress, 100);
}

void QuickActionsPanel::onActionComplete(QuickAction* action) {
    if (action == m_current_action) {
        m_current_action = nullptr;
    }

    const auto& result = action->lastExecutionResult();
    
    m_progress_bar->setValue(100);
    m_status_label->setText(QString("Status: %1").arg(result.message));
    m_bytes_label->setText(QString("Bytes: %1").arg(formatBytes(result.bytes_processed)));
    
    if (!result.output_path.isEmpty()) {
        m_location_label->setText(QString("Location: %1").arg(result.output_path));
        m_last_output_path = result.output_path;
        m_open_folder_button->setEnabled(true);
    }

    updateActionButton(action);

    // Show notification
    if (m_notifications_checkbox->isChecked()) {
        QString title = result.success ? "Action Complete" : "Action Failed";
        Q_EMIT status_message(
            QString("%1: %2").arg(action->name(), result.message),
            5000
        );
    }

    // Reset after delay
    QTimer::singleShot(3000, this, [this]() {
        m_progress_bar->setValue(0);
        m_action_label->setText("Action: Ready");
        m_status_label->setText("Status: Idle");
        m_duration_label->setText("Duration: -");
    });
}

void QuickActionsPanel::onActionError(QuickAction* action, const QString& error_message) {
    QMessageBox::critical(this, "Action Error",
                         QString("%1 failed:\n\n%2").arg(action->name(), error_message));
    
    m_progress_bar->setValue(0);
    m_status_label->setText(QString("Status: Error - %1").arg(error_message));
    updateActionButton(action);
}

void QuickActionsPanel::onBrowseBackupLocation() {
    QString dir = QFileDialog::getExistingDirectory(
        this,
        "Select Backup Location",
        m_backup_location_edit->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
    );

    if (!dir.isEmpty()) {
        m_backup_location_edit->setText(dir);
        m_backup_location = dir;
        saveSettings();
    }
}

void QuickActionsPanel::refreshAllScans() {
    Q_EMIT status_message("Refreshing all action scans...", 2000);
    m_controller->scanAllActions();
}

void QuickActionsPanel::loadSettings() {
    QSettings settings("SAK", "QuickActions");
    
    m_backup_location = settings.value("backup_location", "C:\\SAK_Backups").toString();
    m_backup_location_edit->setText(m_backup_location);
    
    m_confirm_before_execute = settings.value("confirm_before_execute", true).toBool();
    m_confirm_checkbox->setChecked(m_confirm_before_execute);
    
    m_show_notifications = settings.value("show_notifications", true).toBool();
    m_notifications_checkbox->setChecked(m_show_notifications);
    
    m_enable_logging = settings.value("enable_logging", true).toBool();
    m_logging_checkbox->setChecked(m_enable_logging);
    
    m_compress_backups = settings.value("compress_backups", true).toBool();
    m_compression_checkbox->setChecked(m_compress_backups);
}

void QuickActionsPanel::saveSettings() {
    QSettings settings("SAK", "QuickActions");
    
    m_backup_location = m_backup_location_edit->text();
    m_confirm_before_execute = m_confirm_checkbox->isChecked();
    m_show_notifications = m_notifications_checkbox->isChecked();
    m_enable_logging = m_logging_checkbox->isChecked();
    m_compress_backups = m_compression_checkbox->isChecked();
    
    settings.setValue("backup_location", m_backup_location);
    settings.setValue("confirm_before_execute", m_confirm_before_execute);
    settings.setValue("show_notifications", m_show_notifications);
    settings.setValue("enable_logging", m_enable_logging);
    settings.setValue("compress_backups", m_compress_backups);
}

void QuickActionsPanel::appendLog(const QString& message) {
    if (!m_enable_logging) {
        return;
    }

    m_log_viewer->append(message);
    
    // Auto-scroll to bottom
    auto* scrollbar = m_log_viewer->verticalScrollBar();
    scrollbar->setValue(scrollbar->maximum());
}

QString QuickActionsPanel::formatBytes(qint64 bytes) {
    const qint64 kb = 1024;
    const qint64 mb = kb * 1024;
    const qint64 gb = mb * 1024;

    if (bytes >= gb) {
        return QString("%1 GB").arg(bytes / static_cast<double>(gb), 0, 'f', 2);
    } else if (bytes >= mb) {
        return QString("%1 MB").arg(bytes / static_cast<double>(mb), 0, 'f', 1);
    } else if (bytes >= kb) {
        return QString("%1 KB").arg(bytes / kb);
    } else {
        return QString("%1 bytes").arg(bytes);
    }
}

QString QuickActionsPanel::formatDuration(qint64 seconds) {
    if (seconds < 60) {
        return QString("%1s").arg(seconds);
    } else if (seconds < 3600) {
        return QString("%1m %2s").arg(seconds / 60).arg(seconds % 60);
    } else {
        return QString("%1h %2m").arg(seconds / 3600).arg((seconds % 3600) / 60);
    }
}

void QuickActionsPanel::onBackupLocationChanged() {
    saveSettings();
}

void QuickActionsPanel::onOpenBackupFolder() {
    if (!m_last_output_path.isEmpty()) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_last_output_path));
    }
}

void QuickActionsPanel::onViewLog() {
    m_log_viewer->setVisible(!m_log_viewer->isVisible());
}

void QuickActionsPanel::onSettingChanged() {
    saveSettings();
}

} // namespace sak
