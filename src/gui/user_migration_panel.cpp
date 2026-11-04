// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file user_migration_panel.cpp
/// @brief Implements the user profile migration panel UI for backup and restore

#include "sak/user_migration_panel.h"

#include "sak/action_constants.h"
#include "sak/detachable_log_window.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/running_operations.h"
#include "sak/style_constants.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/widget_helpers.h"

#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileDialog>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QMessageBox>
#include <QProgressBar>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>

namespace sak {

namespace {

constexpr int kProgressBarMinimum = 0;
/// This panel's owner token on the application's shared quick-action controller.
constexpr auto kQuickActionOwner = "backup_restore";

}  // namespace

UserMigrationPanel::UserMigrationPanel(QuickActionController& quick_actions, QWidget* parent)
    : QWidget(parent), m_quick_actions(quick_actions) {
    setupUi();
    setupConnections();
    createQuickActions();

    appendLog("User Migration Panel initialized");
    appendLog("Click 'Backup User Profiles...' to start the migration wizard");
}

UserMigrationPanel::~UserMigrationPanel() = default;

void UserMigrationPanel::setupUi() {
    Q_ASSERT(layout() == nullptr);  // setupUi not called twice
    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* scroll_area = new QScrollArea(this);
    scroll_area->setWidgetResizable(true);
    scroll_area->setFrameShape(QFrame::NoFrame);

    auto* content_widget = new QWidget(scroll_area);
    auto* main_layout = new QVBoxLayout(content_widget);
    main_layout->setContentsMargins(sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium,
                                    sak::ui::kMarginMedium);
    main_layout->setSpacing(sak::ui::kSpacingLarge);

    scroll_area->setWidget(content_widget);
    root_layout->addWidget(scroll_area);

    sak::createPanelHeader(content_widget,
                           QStringLiteral(":/icons/icons/panel_backup_restore.svg"),
                           tr("Backup and Restore"),
                           tr("Comprehensive backup and restore wizards"
                              " for Windows user profiles."),
                           main_layout);

    createMigrationCards(content_widget, main_layout);
    main_layout->addWidget(createQuickToolsSection(content_widget));

    main_layout->addStretch();

    // Bottom row: Log toggle
    auto* bottom_layout = new QHBoxLayout();
    bottom_layout->setContentsMargins(sak::ui::kMarginMedium,
                                      sak::ui::kMarginTight,
                                      sak::ui::kMarginMedium,
                                      sak::ui::kMarginSmall);


    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    bottom_layout->addWidget(m_logToggle);
    bottom_layout->addStretch();
    root_layout->addLayout(bottom_layout);
}

void UserMigrationPanel::createMigrationCards(QWidget* parent, QVBoxLayout* layout) {
    Q_ASSERT(layout);
    auto* row = new QHBoxLayout();
    row->setSpacing(sak::ui::kSpacingLarge);

    row->addWidget(
        createMigrationCard(parent,
                            {.icon = QStringLiteral(":/icons/icons/backup.svg"),
                             .title = tr("Backup User Profiles"),
                             .desc = tr("Scan and select users, choose folders, configure"
                                        " filters, and create backup packages."),
                             .btn_text = tr("Start Backup Wizard..."),
                             .btn_style = sak::ui::kSecondaryButtonStyle,
                             .tip = tr("Step-by-step wizard to select apps, configure"
                                       " options, and create backups"),
                             .acc = QStringLiteral("Start Backup Wizard")},
                            m_backupButton));

    row->addWidget(createMigrationCard(parent,
                                       {.icon = QStringLiteral(":/icons/icons/restore.svg"),
                                        .title = tr("Restore User Profiles"),
                                        .desc = tr("Select backup, map users, configure merge"
                                                   " options, and restore data with permissions."),
                                        .btn_text = tr("Start Restore Wizard..."),
                                        .btn_style = sak::ui::kSecondaryButtonStyle,
                                        .tip = tr("Step-by-step wizard to select backups, map"
                                                  " users, and restore data"),
                                        .acc = QStringLiteral("Start Restore Wizard")},
                                       m_restoreButton));

    layout->addLayout(row);
}

QFrame* UserMigrationPanel::createMigrationCard(QWidget* parent,
                                                const MigrationCardConfig& config,
                                                QPushButton*& btn) {
    constexpr int kLogoSize = 96;
    auto* card = new QFrame(parent);
    card->setProperty("sakCard", true);
    card->setStyleSheet(sak::ui::cardFrameStyle(sak::ui::kCssRadiusXLargePx));
    auto* lay = new QVBoxLayout(card);
    lay->setSpacing(sak::ui::kSpacingMedium);
    lay->setContentsMargins(
        sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone, sak::ui::kMarginNone);

    auto* logo = new QLabel(card);
    logo->setPixmap(QIcon(config.icon).pixmap(kLogoSize, kLogoSize));
    logo->setAlignment(Qt::AlignCenter);
    logo->setStyleSheet(sak::ui::kTransparentWidgetStyle);
    lay->addWidget(logo);

    auto* title_label = new QLabel(config.title, card);
    title_label->setStyleSheet(sak::ui::cardTitleTextStyle());
    lay->addWidget(title_label);

    auto* desc_label = new QLabel(config.desc, card);
    desc_label->setWordWrap(true);
    desc_label->setStyleSheet(sak::ui::cardDescriptionTextStyle());
    lay->addWidget(desc_label);

    lay->addStretch();
    btn = new QPushButton(config.btn_text, card);
    btn->setMinimumHeight(sak::kButtonHeightTall);
    btn->setStyleSheet(config.btn_style);
    btn->setToolTip(config.tip);
    btn->setAccessibleName(config.acc);
    lay->addWidget(btn);
    return card;
}

void UserMigrationPanel::setupConnections() {
    connect(m_backupButton, &QPushButton::clicked, this, &UserMigrationPanel::onBackupSelected);
    connect(m_restoreButton, &QPushButton::clicked, this, &UserMigrationPanel::onRestoreBackup);
}

void UserMigrationPanel::onBackupSelected() {
    // Launch the comprehensive user profile backup wizard
    auto* wizard = new UserProfileBackupWizard(this);

    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Backup Wizard Completed ===");
            Q_EMIT statusMessage("User profile backup completed", sak::kTimerStatusDefaultMs);
        } else {
            appendLog("User profile backup wizard cancelled");
            Q_EMIT statusMessage("Backup wizard cancelled", sak::kTimerStatusMessageMs);
        }
        wizard->deleteLater();
    });

    // Show wizard
    appendLog("Launching backup wizard...");
    Q_EMIT statusMessage("Backup wizard launched", sak::kTimerStatusMessageMs);
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void UserMigrationPanel::onRestoreBackup() {
    // Launch the comprehensive user profile restore wizard
    auto* wizard = new UserProfileRestoreWizard(this);

    // Connect wizard completion to log updates
    connect(wizard, &QDialog::finished, this, [this, wizard](int result) {
        if (result == QDialog::Accepted) {
            appendLog("=== User Profile Restore Wizard Completed ===");
            appendLog(QString("Restored from: %1").arg(wizard->backupPath()));
            Q_EMIT statusMessage("User profile restore completed", sak::kTimerStatusDefaultMs);
        } else {
            appendLog("User profile restore wizard cancelled");
            Q_EMIT statusMessage("Restore wizard cancelled", sak::kTimerStatusMessageMs);
        }
        wizard->deleteLater();
    });

    // Show wizard
    appendLog("Launching restore wizard...");
    Q_EMIT statusMessage("Restore wizard launched", sak::kTimerStatusMessageMs);
    wizard->show();
    wizard->raise();
    wizard->activateWindow();
}

void UserMigrationPanel::appendLog(const QString& message) {
    Q_ASSERT(!message.isEmpty());
    Q_EMIT logOutput(message);
}

// ============================================================================
// Quick Tools Section
// ============================================================================

QGroupBox* UserMigrationPanel::createQuickToolsSection(QWidget* parent) {
    auto* group = new QGroupBox(tr("Quick Tools"), parent);
    auto* layout = new QVBoxLayout(group);
    layout->setSpacing(sak::ui::kSpacingDefault);

    auto* desc = new QLabel(tr("One-click system protection and documentation tools"), group);
    desc->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextSecondary, sak::ui::kFontSizeBody));
    desc->setWordWrap(true);
    layout->addWidget(desc);

    auto* btn_layout = new QHBoxLayout();
    btn_layout->setSpacing(sak::ui::kSpacingDefault);

    auto make_action_button = [&](const QString& text, const QString& tooltip, const QString& key) {
        auto* btn = new QPushButton(text, group);
        btn->setMinimumHeight(sak::ui::kUiButtonHeightDialog);
        btn->setMaximumHeight(sak::ui::kUiButtonHeightDialog);
        btn->setToolTip(tooltip);
        btn->setAccessibleName(text);
        btn->setStyleSheet(sak::ui::kCompactPrimaryButtonStyle);
        m_action_buttons.insert(key, btn);
        btn_layout->addWidget(btn);
        return btn;
    };

    make_action_button(tr("Screenshot Settings"),
                       tr("Capture screenshots of all Windows Settings pages"),
                       QStringLiteral("Screenshot Settings"));

    make_action_button(tr("BitLocker Key Backup"),
                       tr("Backup BitLocker recovery keys (requires admin)"),
                       QStringLiteral("BitLocker Key Backup"));

    btn_layout->addStretch();
    layout->addLayout(btn_layout);

    m_action_progress_bar = new QProgressBar(group);
    m_action_progress_bar->setRange(kProgressBarMinimum, kPercentMax);
    m_action_progress_bar->setValue(0);
    m_action_progress_bar->setVisible(false);
    layout->addWidget(m_action_progress_bar);

    return group;
}

void UserMigrationPanel::createQuickActions() {
    // No default output folder: both Quick Tools ask where to save on every run (Screenshot
    // Settings wrote to a fixed C:/SAK_Backups without saying so).
    const QString owner = QString::fromLatin1(kQuickActionOwner);
    RunningOperations::add(
        this,
        tr("Quick action"),
        [this, owner]() { return m_quick_actions.hasRunsOf(owner); },
        [this, owner]() { m_quick_actions.cancelRunsOf(owner); });

    connectRunsOf(m_quick_actions,
                  owner,
                  this,
                  {.queued =
                       [this](const QuickActionRun& run, const QString& running_action) {
                           const QString text = quickActionQueuedText(run, running_action);
                           appendLog(text);
                           Q_EMIT statusMessage(text, sak::kTimerStatusDefaultMs);
                       },
                   .progress =
                       [this](const QuickActionRun&, const QString& message, int percent) {
                           Q_EMIT statusMessage(message, sak::kTimerStatusMessageMs);
                           m_action_progress_bar->setValue(percent);
                       },
                   .log = [this](const QuickActionRun&, const QString& line) { appendLog(line); },
                   .finished =
                       [this](const QuickActionRun& run,
                              const QuickAction::ExecutionResult& result,
                              QuickAction::ActionStatus status) {
                           onQuickActionFinished(run, result, status);
                       }});

    for (auto it = m_action_buttons.constBegin(); it != m_action_buttons.constEnd(); ++it) {
        const QString action_name = it.key();
        if (m_quick_actions.getAction(action_name) != nullptr) {
            connect(
                it.value(),
                &QPushButton::clicked,
                this,
                [this, action_name]() { onQuickActionClicked(action_name); },
                Qt::QueuedConnection);
        }
    }
}

QString UserMigrationPanel::chooseQuickActionOutputFolder(const QString& action_name) {
    if (action_name == QLatin1String(quick_action_elevation::kBackupBitLockerKeysName)) {
        // Recovery keys are asked a destination every time, with no starting folder: a folder on
        // the system drive is usually on the very volume they unlock. The action refuses any
        // folder on a BitLocker volume whose key it is backing up.
        const QString destination = QFileDialog::getExistingDirectory(
            this,
            tr("Choose where to save the BitLocker recovery keys (use another drive; NTFS can "
               "restrict who reads them)"),
            QString(),
            QFileDialog::ShowDirsOnly);
        if (destination.isEmpty()) {
            appendLog(tr("BitLocker key backup cancelled: no destination was chosen"));
        }
        return destination;
    }
    // Screenshot Settings, the panel's other Quick Tool: asked every time, starting in
    // Documents, like the System Inventory Report.
    const QString folder = QFileDialog::getExistingDirectory(
        this,
        tr("Choose where to save the Settings screenshots"),
        QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation),
        QFileDialog::ShowDirsOnly);
    if (folder.isEmpty()) {
        appendLog(tr("Screenshot Settings cancelled: no folder was chosen"));
    }
    return folder;
}

bool UserMigrationPanel::confirmOutputFolderWarning(const QString& action_name,
                                                    const QString& warning) {
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(action_name);
    box.setText(warning);
    const QPushButton* const go_ahead = box.addButton(tr("Save Here Anyway"),
                                                      QMessageBox::AcceptRole);
    QPushButton* const cancel = box.addButton(QMessageBox::Cancel);
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    return box.clickedButton() == go_ahead;
}

void UserMigrationPanel::onQuickActionClicked(const QString& action_name) {
    const QString folder = chooseQuickActionOutputFolder(action_name);
    if (folder.isEmpty()) {
        return;
    }
    const QuickAction* const action = m_quick_actions.getAction(action_name);
    const QString warning = action != nullptr ? action->outputFolderWarning(folder) : QString();
    if (!warning.isEmpty() && !confirmOutputFolderWarning(action_name, warning)) {
        appendLog(tr("%1 cancelled at the warning about %2: nothing was written")
                      .arg(action_name, QDir::toNativeSeparators(folder)));
        return;
    }
    appendLog(QString("Executing: %1").arg(action_name));
    Q_EMIT statusMessage(QString("Running: %1...").arg(action_name), sak::kTimerStatusMessageMs);
    m_action_progress_bar->setValue(0);
    m_action_progress_bar->setVisible(true);
    // The folder travels with this run: one queued behind another caller's still writes here.
    m_quick_actions.requestRun(
        {action_name, QString::fromLatin1(kQuickActionOwner), folder, !warning.isEmpty()});
}

void UserMigrationPanel::onQuickActionFinished(const QuickActionRun& run,
                                               const QuickAction::ExecutionResult& result,
                                               QuickAction::ActionStatus status) {
    if (!m_quick_actions.hasRunsOf(QString::fromLatin1(kQuickActionOwner))) {
        m_action_progress_bar->setVisible(false);
    }
    const QString text = quickActionOutcomeText(run, result, status);
    appendLog(text);
    Q_EMIT statusMessage(text, sak::kTimerStatusDefaultMs);
    if (status == QuickAction::ActionStatus::Failed) {
        sak::logError("Backup and Restore quick action failed: {} - {}",
                      run.action_name.toStdString(),
                      result.message.toStdString());
    }
}

}  // namespace sak
