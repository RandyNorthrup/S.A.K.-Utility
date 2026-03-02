// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file organizer_panel.cpp
/// @brief Implements the file organizer panel UI with rule-based file sorting

#include "sak/organizer_panel.h"
#include "sak/organizer_worker.h"
#include "sak/logger.h"
#include "sak/detachable_log_window.h"
#include "sak/info_button.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"
#include "sak/layout_constants.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QLabel>
#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QDateTime>
#include <QScrollArea>
#include <QFrame>
#include <QDialog>
#include <QFormLayout>

namespace sak {

OrganizerPanel::OrganizerPanel(QWidget* parent)
    : QWidget(parent)
    , m_worker(nullptr)
{
    setupUi();
    setupDefaultCategories();
    logInfo("OrganizerPanel initialized");
}

OrganizerPanel::~OrganizerPanel()
{
    if (m_worker) {
        m_worker->requestStop();
        if (!m_worker->wait(15000)) {
            logError("OrganizerWorker did not stop within 15s \u2014 potential resource leak");
        }
    }
    logInfo("OrganizerPanel destroyed");
}

void OrganizerPanel::setupUi()
{
    auto* rootLayout = new QVBoxLayout(this);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    auto* scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    auto* contentWidget = new QWidget(scrollArea);
    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(ui::kMarginMedium, ui::kMarginMedium,
                                     ui::kMarginMedium, ui::kMarginMedium);
    mainLayout->setSpacing(ui::kSpacingDefault);

    scrollArea->setWidget(contentWidget);
    rootLayout->addWidget(scrollArea);

    // Panel header — consistent title + muted subtitle
    sak::createPanelHeader(contentWidget, tr("Directory Organizer"),
        tr("Automatically sort and organize files by type, date, or custom rules"), mainLayout);

    setupUi_directoryAndCategories(mainLayout);
    setupUi_controlsAndConnections(mainLayout);
}

// ----------------------------------------------------------------------------
// setupUi helpers
// ----------------------------------------------------------------------------

void OrganizerPanel::setupUi_directoryAndCategories(QVBoxLayout* mainLayout)
{
    // Target directory group
    auto* pathGroup = new QGroupBox("Target Directory", this);
    auto* pathLayout = new QHBoxLayout(pathGroup);

    m_target_path = new QLineEdit(this);
    m_target_path->setPlaceholderText("Select directory to organize...");
    m_target_path->setAccessibleName(QStringLiteral("Target Directory Path"));
    m_target_path->setToolTip(QStringLiteral("Path to the directory that will be organized"));
    pathLayout->addWidget(m_target_path, 1);

    m_browse_button = new QPushButton("Browse...", this);
    m_browse_button->setAccessibleName(QStringLiteral("Browse Directory"));
    m_browse_button->setToolTip(QStringLiteral("Browse for a directory to organize"));
    pathLayout->addWidget(m_browse_button);

    mainLayout->addWidget(pathGroup);

    // Category mapping group
    auto* categoryGroup = new QGroupBox("Category Mapping", this);
    auto* categoryLayout = new QVBoxLayout(categoryGroup);

    m_category_table = new QTableWidget(this);
    m_category_table->setColumnCount(2);
    m_category_table->setHorizontalHeaderLabels({"Category", "Extensions (comma-separated)"});
    m_category_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_category_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_category_table->setAlternatingRowColors(true);
    m_category_table->setMinimumHeight(200);
    m_category_table->setAccessibleName(QStringLiteral("Category Mappings Table"));
    m_category_table->setToolTip(QStringLiteral("File categories and their associated extensions"));
    categoryLayout->addWidget(m_category_table);

    auto* buttonLayout = new QHBoxLayout();
    m_add_category_button = new QPushButton("Add Category", this);
    m_add_category_button->setAccessibleName(QStringLiteral("Add Category"));
    m_add_category_button->setToolTip(QStringLiteral("Add a new file category row"));
    m_remove_category_button = new QPushButton("Remove Selected", this);
    m_remove_category_button->setAccessibleName(QStringLiteral("Remove Category"));
    m_remove_category_button->setToolTip(QStringLiteral(
        "Remove the selected category from the list"));
    buttonLayout->addWidget(m_add_category_button);
    buttonLayout->addWidget(m_remove_category_button);
    buttonLayout->addStretch();
    categoryLayout->addLayout(buttonLayout);

    mainLayout->addWidget(categoryGroup);
}

void OrganizerPanel::setupUi_controlsAndConnections(QVBoxLayout* mainLayout)
{
    // Options widgets (hidden — managed via Settings modal)
    m_collision_strategy = new QComboBox(this);
    m_collision_strategy->addItems({"Rename", "Skip", "Overwrite"});
    m_collision_strategy->setAccessibleName(QStringLiteral("Collision Strategy"));
    m_collision_strategy->setToolTip(QStringLiteral("How to handle filename conflicts"));
    m_collision_strategy->setVisible(false);

    m_preview_mode_checkbox = new QCheckBox("Preview Mode (Dry Run)", this);
    m_preview_mode_checkbox->setChecked(true);
    m_preview_mode_checkbox->setAccessibleName(QStringLiteral("Preview Mode"));
    m_preview_mode_checkbox->setToolTip(QStringLiteral(
        "Show what would happen without moving files"));
    m_preview_mode_checkbox->setVisible(false);

    // Control buttons
    auto* controlLayout = new QHBoxLayout();

    auto* settingsBtn = new QPushButton("Settings", this);
    settingsBtn->setAccessibleName(QStringLiteral("Organizer Settings"));
    settingsBtn->setToolTip(QStringLiteral("Configure organizer settings"));
    connect(settingsBtn, &QPushButton::clicked, this, &OrganizerPanel::onSettingsClicked);
    controlLayout->addWidget(settingsBtn);

    controlLayout->addStretch();

    m_preview_button = new QPushButton("Preview", this);
    m_preview_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_preview_button->setAccessibleName(QStringLiteral("Preview Organization"));
    m_preview_button->setToolTip(QStringLiteral(
        "Preview file organization without making changes"));
    controlLayout->addWidget(m_preview_button);

    m_execute_button = new QPushButton("Execute", this);
    m_execute_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_execute_button->setAccessibleName(QStringLiteral("Execute Organization"));
    m_execute_button->setToolTip(QStringLiteral("Organize files into category folders"));
    m_execute_button->setStyleSheet(ui::kPrimaryButtonStyle);
    controlLayout->addWidget(m_execute_button);

    m_cancel_button = new QPushButton("Cancel", this);
    m_cancel_button->setMinimumWidth(sak::kButtonWidthSmall);
    m_cancel_button->setEnabled(false);
    m_cancel_button->setAccessibleName(QStringLiteral("Cancel Organization"));
    m_cancel_button->setToolTip(QStringLiteral("Cancel the current organization operation"));
    controlLayout->addWidget(m_cancel_button);

    m_logToggle = new LogToggleSwitch(tr("Log"), this);
    controlLayout->insertWidget(1, m_logToggle);

    mainLayout->addLayout(controlLayout);

    // Connect signals
    connect(m_browse_button, &QPushButton::clicked, this, &OrganizerPanel::onBrowseClicked);
    connect(m_preview_button, &QPushButton::clicked, this, &OrganizerPanel::onPreviewClicked);
    connect(m_execute_button, &QPushButton::clicked, this, &OrganizerPanel::onExecuteClicked);
    connect(m_cancel_button, &QPushButton::clicked, this, &OrganizerPanel::onCancelClicked);
    connect(m_add_category_button, &QPushButton::clicked, this,
        &OrganizerPanel::onAddCategoryClicked);
    connect(m_remove_category_button, &QPushButton::clicked, this,
        &OrganizerPanel::onRemoveCategoryClicked);
}

void OrganizerPanel::setupDefaultCategories()
{
    QMap<QString, QString> defaults = {
        {"Images", "jpg,jpeg,png,gif,bmp,svg,webp,ico"},
        {"Documents", "pdf,doc,docx,txt,rtf,odt,xls,xlsx,ppt,pptx"},
        {"Audio", "mp3,wav,flac,aac,ogg,m4a,wma"},
        {"Video", "mp4,avi,mkv,mov,wmv,flv,webm"},
        {"Archives", "zip,rar,7z,tar,gz,bz2"},
        {"Code", "cpp,h,py,js,java,cs,html,css,json,xml"}
    };

    m_category_table->setRowCount(defaults.size());
    int row = 0;
    for (auto it = defaults.begin(); it != defaults.end(); ++it) {
        m_category_table->setItem(row, 0, new QTableWidgetItem(it.key()));
        m_category_table->setItem(row, 1, new QTableWidgetItem(it.value()));
        ++row;
    }
}

void OrganizerPanel::onBrowseClicked()
{
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select Directory to Organize",
        m_target_path->text(),
        QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks);

    if (!dir.isEmpty()) {
        m_target_path->setText(dir);
        logMessage(QString("Target directory selected: %1").arg(dir));
    }
}

void OrganizerPanel::onPreviewClicked()
{
    m_preview_mode_checkbox->setChecked(true);
    onExecuteClicked();
}

void OrganizerPanel::onExecuteClicked()
{
    if (m_target_path->text().isEmpty()) {
        QMessageBox::warning(this, "Validation Error", "Please select a target directory.");
        return;
    }

    QDir targetDir(m_target_path->text());
    if (!targetDir.exists()) {
        QMessageBox::warning(this, "Validation Error", "Target directory does not exist.");
        return;
    }

    // Clean up previous worker
    m_worker.reset();

    // Create worker configuration
    OrganizerWorker::Config config;
    config.target_directory = m_target_path->text();
    config.category_mapping = getCategoryMapping();
    config.preview_mode = m_preview_mode_checkbox->isChecked();
    config.create_subdirectories = true;

    QString strategy = m_collision_strategy->currentText().toLower();
    config.collision_strategy = strategy;

    // Create and configure worker
    m_worker = std::make_unique<OrganizerWorker>(config, this);

    connect(m_worker.get(), &OrganizerWorker::started, this, &OrganizerPanel::onWorkerStarted);
    connect(m_worker.get(), &OrganizerWorker::finished, this, &OrganizerPanel::onWorkerFinished);
    connect(m_worker.get(), &OrganizerWorker::failed, this, &OrganizerPanel::onWorkerFailed);
    connect(m_worker.get(), &OrganizerWorker::cancelled, this, &OrganizerPanel::onWorkerCancelled);
    connect(m_worker.get(), &OrganizerWorker::fileProgress, this, &OrganizerPanel::onFileProgress);
    connect(m_worker.get(), &OrganizerWorker::previewResults, this,
        &OrganizerPanel::onPreviewResults);

    setOperationRunning(true);
    Q_EMIT statusMessage("Starting...", 0);
    m_worker->start();

    QString mode = config.preview_mode ? "Preview" : "Execute";
    logInfo("Organization operation initiated ({}): {}", mode.toStdString(),
                  config.target_directory.toStdString());
}

void OrganizerPanel::onCancelClicked()
{
    if (m_worker != nullptr) {
        m_worker->requestStop();
        logMessage("Cancellation requested...");
        Q_EMIT statusMessage("Cancelling...", 0);
        logInfo("Organization cancellation requested by user");
    }
}

void OrganizerPanel::onAddCategoryClicked()
{
    int row = m_category_table->rowCount();
    m_category_table->insertRow(row);
    m_category_table->setItem(row, 0, new QTableWidgetItem("New Category"));
    m_category_table->setItem(row, 1, new QTableWidgetItem(""));
    m_category_table->editItem(m_category_table->item(row, 0));
}

void OrganizerPanel::onRemoveCategoryClicked()
{
    auto selected = m_category_table->selectedItems();
    if (selected.isEmpty()) {
        QMessageBox::information(this, "No Selection", "Please select a category to remove.");
        return;
    }

    int row = m_category_table->currentRow();
    if (row >= 0) {
        m_category_table->removeRow(row);
    }
}

void OrganizerPanel::onWorkerStarted()
{
    QString mode = m_preview_mode_checkbox->isChecked() ? "preview" : "organization";
    logMessage(QString("Starting %1...").arg(mode));
    Q_EMIT statusMessage(QString("%1 in progress").arg(mode), 0);
}

void OrganizerPanel::onWorkerFinished()
{
    setOperationRunning(false);
    QString mode = m_preview_mode_checkbox->isChecked() ? "Preview" : "Organization";
    Q_EMIT statusMessage(QString("%1 complete").arg(mode), sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(100, 100);
    logMessage(QString("%1 completed successfully").arg(mode));
    QMessageBox::information(this, QString("%1 Complete").arg(mode),
                            QString("%1 operation completed successfully").arg(mode));
    logInfo("Organization operation completed successfully");
}

void OrganizerPanel::onWorkerFailed(int errorCode, const QString& errorMessage)
{
    setOperationRunning(false);
    Q_EMIT statusMessage("Organization failed", sak::kTimerStatusDefaultMs);
    Q_EMIT progressUpdate(0, 100);
    logMessage(QString("Organization failed: Error %1: %2").arg(errorCode).arg(errorMessage));
    QMessageBox::warning(this, "Organization Failed",
                        QString("Error %1: %2").arg(errorCode).arg(errorMessage));
    logError("Organization failed: {}", errorMessage.toStdString());
}

void OrganizerPanel::onWorkerCancelled()
{
    setOperationRunning(false);
    logMessage("Organization cancelled by user");
    Q_EMIT statusMessage("Organization cancelled", sak::kTimerStatusMessageMs);
    Q_EMIT progressUpdate(0, 100);
}

void OrganizerPanel::onFileProgress(int current, int total, const QString& filePath)
{
    Q_EMIT progressUpdate(current, total);

    QString filename = QFileInfo(filePath).fileName();
    Q_EMIT statusMessage(QString("Processing: %1").arg(filename), 0);
}

void OrganizerPanel::onPreviewResults(const QString& summary, int operationCount)
{
    QMessageBox::information(this, "Preview Results", summary);
    logMessage(QString("Preview completed: %1 operations planned").arg(operationCount));
}

QMap<QString, QStringList> OrganizerPanel::getCategoryMapping() const
{
    QMap<QString, QStringList> mapping;

    for (int row = 0; row < m_category_table->rowCount(); ++row) {
        auto* categoryItem = m_category_table->item(row, 0);
        auto* extensionsItem = m_category_table->item(row, 1);

        if (!categoryItem || !extensionsItem) continue;

        QString category = categoryItem->text().trimmed();
        QString extensionsStr = extensionsItem->text().trimmed();

        if (category.isEmpty() || extensionsStr.isEmpty()) continue;

        QStringList extensions = extensionsStr.split(',', Qt::SkipEmptyParts);
        for (auto& ext : extensions) {
            ext = ext.trimmed();
        }
        mapping[category] = extensions;
    }

    return mapping;
}

void OrganizerPanel::setOperationRunning(bool running)
{
    m_operation_running = running;

    m_target_path->setEnabled(!running);
    m_browse_button->setEnabled(!running);
    m_category_table->setEnabled(!running);
    m_add_category_button->setEnabled(!running);
    m_remove_category_button->setEnabled(!running);
    m_collision_strategy->setEnabled(!running);
    m_preview_mode_checkbox->setEnabled(!running);

    m_preview_button->setEnabled(!running);
    m_execute_button->setEnabled(!running);
    m_cancel_button->setEnabled(running);
}

void OrganizerPanel::logMessage(const QString& message)
{
    Q_EMIT logOutput(message);
}

void OrganizerPanel::onSettingsClicked()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Directory Organizer Settings"));
    dialog.setMinimumWidth(sak::kDialogWidthSmall);

    auto* layout = new QFormLayout(&dialog);

    auto* collisionCombo = new QComboBox(&dialog);
    collisionCombo->addItems({"Rename", "Skip", "Overwrite"});
    collisionCombo->setCurrentIndex(m_collision_strategy->currentIndex());
    layout->addRow(
        InfoButton::createInfoLabel(tr("Collision Strategy:"),
            tr("How to handle files when a file with the same name already exists in the "
               "destination folder"), &dialog),
        collisionCombo);

    auto* previewCheck = new QCheckBox(tr("Preview Mode (Dry Run)"), &dialog);
    previewCheck->setChecked(m_preview_mode_checkbox->isChecked());
    auto* previewRow = new QHBoxLayout();
    previewRow->addWidget(previewCheck);
    previewRow->addWidget(new InfoButton(
        tr("When enabled, shows what would happen without actually moving any files"), &dialog));
    previewRow->addStretch();
    layout->addRow(previewRow);

    auto* btnLayout = new QHBoxLayout();
    btnLayout->addStretch();
    auto* okBtn = new QPushButton(tr("OK"), &dialog);
    auto* cancelBtn = new QPushButton(tr("Cancel"), &dialog);
    connect(okBtn, &QPushButton::clicked, &dialog, &QDialog::accept);
    connect(cancelBtn, &QPushButton::clicked, &dialog, &QDialog::reject);
    btnLayout->addWidget(okBtn);
    btnLayout->addWidget(cancelBtn);
    layout->addRow(btnLayout);

    if (dialog.exec() == QDialog::Accepted) {
        m_collision_strategy->setCurrentIndex(collisionCombo->currentIndex());
        m_preview_mode_checkbox->setChecked(previewCheck->isChecked());
    }
}

} // namespace sak
