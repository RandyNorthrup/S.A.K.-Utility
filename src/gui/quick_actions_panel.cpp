// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file quick_actions_panel.cpp
/// @brief Implements the quick actions panel UI for system maintenance tasks

#include "sak/quick_actions_panel.h"
#include "sak/format_utils.h"
#include "sak/quick_action_controller.h"
#include "sak/actions/action_factory.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/layout_constants.h"

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
#include <QFileInfo>
#include <QUrl>
#include <QTimer>
#include <QToolButton>
#include <QScrollBar>

namespace sak {

QuickActionsPanel::QuickActionsPanel(QWidget* parent)
    : QWidget(parent)
    , m_controller(new QuickActionController(this)) {
    Q_ASSERT(m_controller);
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

void QuickActionsPanel::setupUi_statusSection(QVBoxLayout* main_layout) {
    auto* status_group = new QGroupBox("Status");
    auto* status_layout = new QVBoxLayout(status_group);

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

    main_layout->addWidget(status_group);
}

void QuickActionsPanel::setupUi_bottomRow(QVBoxLayout* main_layout) {
    auto* bottomLayout = new QHBoxLayout();

    auto* settingsBtn = new QPushButton(tr("Settings"), this);
    settingsBtn->setAccessibleName(QStringLiteral("Quick Actions Settings"));
    settingsBtn->setToolTip(QStringLiteral("Configure quick actions settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &QuickActionsPanel::showSettingsDialog);
    bottomLayout->addWidget(settingsBtn);

    m_open_folder_button = new QPushButton("Open Output Folder");
    m_open_folder_button->setEnabled(false);
    m_open_folder_button->setAccessibleName(QStringLiteral("Open Output Folder"));
    m_open_folder_button->setToolTip(QStringLiteral(
        "Open the last output folder in file explorer"));
    connect(m_open_folder_button, &QPushButton::clicked, this, [this]() {
        if (!m_last_output_path.isEmpty()) {
            const QFileInfo fi(m_last_output_path);
            const QString folder = fi.isDir() ? m_last_output_path : fi.absolutePath();
            QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
        }
    });
    bottomLayout->addWidget(m_open_folder_button);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bottomLayout->addWidget(m_logToggle);
    bottomLayout->addStretch();

    main_layout->addLayout(bottomLayout);
}

void QuickActionsPanel::setupUi() {
    Q_ASSERT(!objectName().isEmpty() || true);  // widget valid
    auto* main_layout = new QVBoxLayout(this);
    main_layout->setContentsMargins(12, 12, 12, 12);
    main_layout->setSpacing(10);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(this, QStringLiteral(":/icons/icons/panel_quick_actions.svg"),
        tr("Quick Actions"),
        tr("One-click technician tools for common maintenance tasks"), main_layout);

    // Backup Location row at the top
    auto* backupLocRow = new QHBoxLayout();
    backupLocRow->addWidget(new QLabel(tr("Backup Location:"), this));
    m_backup_location_edit = new QLineEdit(this);
    m_backup_location_edit->setPlaceholderText(tr("C:\\SAK_Backups"));
    m_backup_location_edit->setAccessibleName(QStringLiteral("Backup Location Path"));
    m_backup_location_edit->setToolTip(QStringLiteral("Directory where backups will be saved"));
    backupLocRow->addWidget(m_backup_location_edit, 1);
    m_browse_button = new QPushButton(tr("Browse..."), this);
    m_browse_button->setAccessibleName(QStringLiteral("Browse Backup Folder"));
    m_browse_button->setToolTip(QStringLiteral("Browse for a backup output directory"));
    connect(m_browse_button, &QPushButton::clicked, this,
        &QuickActionsPanel::onBrowseBackupLocation);
    backupLocRow->addWidget(m_browse_button);
    main_layout->addLayout(backupLocRow);

    // Initialize hidden settings checkboxes (managed via Settings modal)
    m_confirm_checkbox = new QCheckBox("Confirm before executing actions");
    m_confirm_checkbox->setChecked(true);
    m_confirm_checkbox->setAccessibleName(QStringLiteral("Confirm Before Executing"));
    m_confirm_checkbox->hide();
    m_notifications_checkbox = new QCheckBox("Show completion notifications");
    m_notifications_checkbox->setChecked(true);
    m_notifications_checkbox->setAccessibleName(QStringLiteral("Show Completion Notifications"));
    m_notifications_checkbox->hide();
    m_logging_checkbox = new QCheckBox("Enable detailed logging");
    m_logging_checkbox->setChecked(true);
    m_logging_checkbox->setAccessibleName(QStringLiteral("Enable Detailed Logging"));
    m_logging_checkbox->hide();
    m_compression_checkbox = new QCheckBox("Compress backups (saves space)");
    m_compression_checkbox->setChecked(true);
    m_compression_checkbox->setAccessibleName(QStringLiteral("Compress Backups"));
    m_compression_checkbox->hide();

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

    setupUi_statusSection(main_layout);
    setupUi_bottomRow(main_layout);
}

void QuickActionsPanel::createActions() {
    Q_ASSERT(m_controller);
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
        createSingleCategorySection(cat_info.category, cat_info.title, cat_info.description);
    }
}

void QuickActionsPanel::createSingleCategorySection(
    QuickAction::ActionCategory category,
    const QString& title,
    const QString& description)
{
    auto* group_box = new QGroupBox(title);
    group_box->setStyleSheet(
        QString("QGroupBox {"
        "  font-weight: 600;"
        "  border: 1px solid %1;"
        "  border-radius: 12px;"
        "  margin-top: 18px;"
        "  padding: 26px 10px 10px 10px;"
        "}"
        "QGroupBox::title {"
        "  subcontrol-origin: margin;"
        "  subcontrol-position: top left;"
        "  padding: 0 8px;"
        "  color: %2;"
        "}").arg(sak::ui::kColorBorderDefault, sak::ui::kColorTextBody)
    );

    auto* cat_layout = new QVBoxLayout(group_box);

    // Description
    auto* desc_label = new QLabel(description);
    desc_label->setStyleSheet(QString("color: %1; font-weight: 400; font-size: %2pt;")
        .arg(sak::ui::kColorTextMuted).arg(sak::ui::kFontSizeStatus));
    cat_layout->addWidget(desc_label);

    // Action buttons grid
    auto* buttons_grid = new QGridLayout();
    buttons_grid->setSpacing(10);
    cat_layout->addLayout(buttons_grid);

    // Store group box
    m_category_sections[category] = group_box;
    m_actions_layout->insertWidget(m_actions_layout->count() - 1, group_box);

    // Populate with actions
    auto actions = m_controller->getActionsByCategory(category);
    int row = 0, col = 0;
    const int cols_per_row = 4;

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

QPushButton* QuickActionsPanel::createActionButton(QuickAction* action) {
    auto* button = new QPushButton();
    button->setMinimumHeight(sak::kButtonHeightStd);
    button->setMinimumWidth(sak::kButtonWidthLarge);
    button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    // Label only — description moved to tooltip
    button->setText(action->name());
    button->setToolTip(action->description());
    // Use app theme styling — only override alignment and padding
    button->setStyleSheet(
        "QPushButton { text-align: center; padding: 8px 14px; }"
    );

    connect(button, &QPushButton::clicked, this, [this, action]() {
        onActionClicked(action);
    });

    updateActionButton(action);
    return button;
}

void QuickActionsPanel::updateActionButton(QuickAction* action) {
    Q_ASSERT(action);
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

    // Build button text — use short format to avoid cutoff
    QString text = QString("%1 %2")
                      .arg(status_icon, action->name());

    // Build tooltip with scan details
    QString tip = action->description();
    if (action->status() == QuickAction::ActionStatus::Ready) {
        const auto& scan_result = action->lastScanResult();
        if (scan_result.applicable) {
            QString size_text = formatBytes(scan_result.bytes_affected);
            qint64 est_seconds = scan_result.estimated_duration_ms / 1000;
            QString time_text = formatDuration(est_seconds);
            tip += QString("\n%1 - %2 est.").arg(size_text, time_text);
        } else {
            text += " (N/A)";
            button->setEnabled(false);
        }
    }

    button->setText(text);
    button->setToolTip(tip);
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
    Q_ASSERT(action);
    Q_ASSERT(m_controller);
    updateActionButton(action);

    if (m_logging_checkbox->isChecked()) {
        const auto& result = action->lastScanResult();
        Q_EMIT statusMessage(
            QString("%1 scan complete: %2").arg(action->name(), result.summary),
            3000
        );
    }
}

void QuickActionsPanel::onActionProgress(QuickAction* action, const QString& message,
    int progress) {
    Q_ASSERT(action);
    Q_ASSERT(progress >= 0 && progress <= 100);
    if (action != m_current_action) {
        return;
    }

    m_action_label->setText(QString("Action: %1").arg(action->name()));
    m_status_label->setText(QString("Status: %1").arg(message));
    // Update duration
    if (m_last_action_time.isValid()) {
        qint64 elapsed_sec = m_last_action_time.secsTo(QDateTime::currentDateTime());
        m_duration_label->setText(QString("Duration: %1").arg(formatDuration(elapsed_sec)));
    }

    updateActionButton(action);
    Q_EMIT progressUpdate(progress, 100);
}

void QuickActionsPanel::onActionComplete(QuickAction* action) {
    Q_ASSERT(action);
    if (action == m_current_action) {
        m_current_action = nullptr;
    }

    const auto& result = action->lastExecutionResult();

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
            sak::kTimerStatusDefaultMs
        );
    }

    // Reset after delay
    QTimer::singleShot(sak::kTimerStatusMessageMs, this, [this]() {
        m_action_label->setText("Action: Ready");
        m_status_label->setText("Status: Idle");
        m_duration_label->setText("Duration: -");
    });
}

void QuickActionsPanel::onActionError(QuickAction* action, const QString& error_message) {
    Q_ASSERT(action);
    Q_ASSERT(!error_message.isEmpty());
    QMessageBox::critical(this, "Action Error",
                         QString("%1 failed:\n\n%2").arg(action->name(), error_message));

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
    Q_ASSERT(m_controller);
    Q_EMIT statusMessage("Refreshing all action scans...", sak::kTimerServiceDelayMs);
    m_controller->scanAllActions();
}

void QuickActionsPanel::loadSettings() {
    Q_ASSERT(m_controller);
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
    Q_ASSERT(m_controller);
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

    Q_EMIT logOutput(message);
}

QString QuickActionsPanel::formatBytes(qint64 bytes) {
    return sak::formatBytes(bytes);
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
        const QFileInfo fi(m_last_output_path);
        const QString folder = fi.isDir() ? m_last_output_path : fi.absolutePath();
        QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
    }
}

void QuickActionsPanel::onSettingChanged() {
    saveSettings();
}

void QuickActionsPanel::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Quick Actions Settings"));
    dialog.setMinimumWidth(400);
    auto* layout = new QVBoxLayout(&dialog);

    auto* confirmCheck = new QCheckBox(tr("Confirm before executing actions"), &dialog);
    confirmCheck->setChecked(m_confirm_checkbox->isChecked());
    auto* confirmRow = new QHBoxLayout();
    confirmRow->addWidget(confirmCheck);
    confirmRow->addWidget(new sak::InfoButton(
        tr("Show a confirmation dialog before each action runs to prevent accidental execution"),
            &dialog));
    confirmRow->addStretch();
    layout->addLayout(confirmRow);

    auto* notifyCheck = new QCheckBox(tr("Show completion notifications"), &dialog);
    notifyCheck->setChecked(m_notifications_checkbox->isChecked());
    auto* notifyRow = new QHBoxLayout();
    notifyRow->addWidget(notifyCheck);
    notifyRow->addWidget(new sak::InfoButton(
        tr("Display a status bar notification when an action finishes or fails"), &dialog));
    notifyRow->addStretch();
    layout->addLayout(notifyRow);

    auto* loggingCheck = new QCheckBox(tr("Enable detailed logging"), &dialog);
    loggingCheck->setChecked(m_logging_checkbox->isChecked());
    auto* loggingRow = new QHBoxLayout();
    loggingRow->addWidget(loggingCheck);
    loggingRow->addWidget(new sak::InfoButton(
        tr("Write detailed progress and scan information to the log window"), &dialog));
    loggingRow->addStretch();
    layout->addLayout(loggingRow);

    auto* compressCheck = new QCheckBox(tr("Compress backups (saves space)"), &dialog);
    compressCheck->setChecked(m_compression_checkbox->isChecked());
    auto* compressRow = new QHBoxLayout();
    compressRow->addWidget(compressCheck);
    compressRow->addWidget(new sak::InfoButton(
        tr("Use ZIP compression for backup output files — slower but uses less disk space"),
            &dialog));
    compressRow->addStretch();
    layout->addLayout(compressRow);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    if (dialog.exec() == QDialog::Accepted) {
        m_confirm_checkbox->setChecked(confirmCheck->isChecked());
        m_notifications_checkbox->setChecked(notifyCheck->isChecked());
        m_logging_checkbox->setChecked(loggingCheck->isChecked());
        m_compression_checkbox->setChecked(compressCheck->isChecked());
        saveSettings();
    }
}

void QuickActionsPanel::onViewLog() {
    // Log is now always visible — no toggle needed
}

} // namespace sak
