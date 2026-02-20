// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/quick_actions_panel.h"
#include "sak/quick_action_controller.h"
#include "sak/actions/action_factory.h"

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
#include <QDialog>
#include <QDialogButtonBox>
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
    loadSettings();
    createActions();

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

    // Note: Scans only triggered by user clicking Refresh or individual action buttons
    // Not all actions apply to every PC, so no auto-scan on startup
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
    subtitle->setStyleSheet("color: #64748b; margin-bottom: 10px;");
    main_layout->addWidget(subtitle);

    // Settings section
    auto* settings_group = new QGroupBox("Settings");
    settings_group->setStyleSheet(
        "QGroupBox {"
        "  font-weight: 600;"
        "  border: 1px solid #cbd5e1;"
        "  border-radius: 12px;"
        "  margin-top: 18px;"
        "  padding: 18px 10px 10px 10px;"
        "  background-color: rgba(255, 255, 255, 0.92);"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  subcontrol-position: top left;"
        "  padding: 0 8px;"
        "  color: #334155;"
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

    // Settings button
    auto* settings_button = new QPushButton("Settings...");
    settings_button->setMaximumWidth(100);
    connect(settings_button, &QPushButton::clicked, this, &QuickActionsPanel::showSettingsDialog);
    settings_layout->addWidget(settings_button, 1, 2);

    // Initialize checkboxes (hidden in main UI)
    m_confirm_checkbox = new QCheckBox("Confirm before executing actions");
    m_confirm_checkbox->setChecked(true);
    m_notifications_checkbox = new QCheckBox("Show completion notifications");
    m_notifications_checkbox->setChecked(true);
    m_logging_checkbox = new QCheckBox("Enable detailed logging");
    m_logging_checkbox->setChecked(true);
    m_compression_checkbox = new QCheckBox("Compress backups (saves space)");
    m_compression_checkbox->setChecked(true);

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

    action_buttons_layout->addWidget(m_open_folder_button);
    action_buttons_layout->addWidget(m_view_log_button);
    action_buttons_layout->addStretch();

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
    QString backup_location = m_backup_location_edit->text();
    if (backup_location.isEmpty()) {
        backup_location = "C:/SAK_Backups";
    }

    m_controller->setBackupLocation(backup_location);
    
    // Create all actions using factory
    auto actions = ActionFactory::createAllActions(backup_location);
    for (auto& action : actions) {
        m_controller->registerAction(std::move(action));
    }
    
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
            "  font-weight: 600;"
            "  border: 1px solid #cbd5e1;"
            "  border-radius: 12px;"
            "  margin-top: 18px;"
            "  padding: 18px 10px 10px 10px;"
            "  background-color: rgba(255, 255, 255, 0.9);"
            "}"
            "QGroupBox::title {"
            "  subcontrol-origin: margin;"
            "  subcontrol-position: top left;"
            "  padding: 0 8px;"
            "  color: #334155;"
            "}"
        );

        auto* cat_layout = new QVBoxLayout(group_box);
        
        // Description
        auto* desc_label = new QLabel(cat_info.description);
        desc_label->setStyleSheet("color: #64748b; font-weight: 400; font-size: 11px;");
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
    
    // Set initial text
    QString text = QString("%1\n%2").arg(action->name(), action->description());
    button->setText(text);
    button->setStyleSheet(
        "QPushButton {"
        "  text-align: left;"
        "  padding: 12px;"
        "  border: 1px solid #cbd5e1;"
        "  border-radius: 12px;"
        "  background-color: rgba(255, 255, 255, 0.96);"
        "  font-weight: 500;"
        "  color: #1e293b;"
        "}"
        "QPushButton:hover {"
        "  border-color: #3b82f6;"
        "  background-color: #e0f2fe;"
        "}"
        "QPushButton:pressed {"
        "  background-color: #dbeafe;"
        "}"
        "QPushButton:disabled {"
        "  background-color: #e2e8f0;"
        "  color: #94a3b8;"
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

    button->setEnabled(true);

    // Build button text with status indicator and scan results
    QString status_icon;
    switch (action->status()) {
        case QuickAction::ActionStatus::Idle:
            status_icon = "[Idle]";
            break;
        case QuickAction::ActionStatus::Scanning:
            status_icon = "[Scanning...]";
            break;
        case QuickAction::ActionStatus::Ready:
            status_icon = action->lastScanResult().applicable ? "[Ready]" : "[N/A]";
            break;
        case QuickAction::ActionStatus::Running:
            status_icon = "[Running]";
            button->setEnabled(false);
            break;
        case QuickAction::ActionStatus::Success:
            status_icon = "[Success]";
            break;
        case QuickAction::ActionStatus::Failed:
            status_icon = "[Failed]";
            break;
        case QuickAction::ActionStatus::Cancelled:
            status_icon = "[Cancelled]";
            break;
    }

    // Build button text
    QString text = QString("%1 %2\n%3")
                      .arg(status_icon, action->name(), action->description());

    // Add scan result if available
    if (action->status() == QuickAction::ActionStatus::Ready) {
        const auto& scan_result = action->lastScanResult();
        if (scan_result.applicable) {
            QString size_text = formatBytes(scan_result.bytes_affected);
            qint64 est_seconds = scan_result.estimated_duration_ms / 1000;
            QString time_text = formatDuration(est_seconds);
            text += QString("\n%1 • %2 estimated")
                       .arg(size_text, time_text);
        } else {
            text += "\nNot applicable";
            button->setEnabled(false);
        }
    }

    button->setText(text);
}

void QuickActionsPanel::onActionClicked(QuickAction* action) {
    if (!action) {
        return;
    }

    // Check if confirmation needed
    if (m_confirm_checkbox->isChecked()) {
        QMessageBox confirm_box(this);
        confirm_box.setWindowTitle("Confirm Action");
        confirm_box.setText(QString("Execute %1?").arg(action->name()));
        confirm_box.setInformativeText(action->description());
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
        Q_EMIT statusMessage(
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
    Q_EMIT progressUpdate(progress, 100);
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
        Q_EMIT statusMessage(
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
        m_controller->setBackupLocation(m_backup_location);
        saveSettings();
    }
}

void QuickActionsPanel::refreshAllScans() {
    Q_EMIT statusMessage("Refreshing all action scans...", 2000);
    m_controller->scanAllActions();
}

void QuickActionsPanel::loadSettings() {
    QSettings settings("SAK", "QuickActions");
    
    m_backup_location = settings.value("backup_location", "C:\\SAK_Backups").toString();
    m_backup_location_edit->setText(m_backup_location);
    m_controller->setBackupLocation(m_backup_location);
    
    m_confirm_before_execute = settings.value("confirm_before_execute", true).toBool();
    m_confirm_checkbox->setChecked(m_confirm_before_execute);
    
    m_show_notifications = settings.value("show_notifications", true).toBool();
    m_notifications_checkbox->setChecked(m_show_notifications);
    
    m_enable_logging = settings.value("enable_logging", true).toBool();
    m_logging_checkbox->setChecked(m_enable_logging);
    m_controller->setLoggingEnabled(m_enable_logging);
    
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

    m_controller->setLoggingEnabled(m_enable_logging);
    m_controller->setBackupLocation(m_backup_location);
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

void QuickActionsPanel::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle("Quick Actions Settings");
    dialog.setMinimumWidth(400);
    
    auto* layout = new QVBoxLayout(&dialog);
    
    auto* group = new QGroupBox("Preferences");
    auto* group_layout = new QVBoxLayout(group);
    
    // Temporarily add checkboxes to dialog
    group_layout->addWidget(m_confirm_checkbox);
    group_layout->addWidget(m_notifications_checkbox);
    group_layout->addWidget(m_logging_checkbox);
    group_layout->addWidget(m_compression_checkbox);
    
    layout->addWidget(group);
    layout->addStretch();
    
    // Buttons
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);
    
    dialog.exec();
    
    // Hide checkboxes after dialog closes
    for (auto* checkbox : {m_confirm_checkbox, m_notifications_checkbox, m_logging_checkbox, m_compression_checkbox}) {
        checkbox->setParent(this);
        checkbox->hide();
    }
}

} // namespace sak
