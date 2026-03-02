// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel_dialogs.cpp
/// @brief Dialog implementations for uninstall confirmation, forced uninstall,
///        batch queue management, and program properties

#include "sak/advanced_uninstall_panel.h"
#include "sak/advanced_uninstall_controller.h"
#include "sak/restore_point_manager.h"
#include "sak/style_constants.h"
#include "sak/widget_helpers.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPixmap>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QVBoxLayout>

namespace sak {

// ── Uninstall Confirmation Dialog ───────────────────────────────────────────

void AdvancedUninstallPanel::showUninstallConfirmation(
    const ProgramInfo& program)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Uninstall Program"));
    dialog.setMinimumWidth(450);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Program info header
    auto* headerLabel = new QLabel(
        tr("Uninstall <b>%1</b>?").arg(program.displayName), &dialog);
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    if (!program.publisher.isEmpty()) {
        auto* pubLabel = new QLabel(
            tr("Publisher: %1").arg(program.publisher), &dialog);
        pubLabel->setStyleSheet(
            QString("color: %1;").arg(ui::kColorTextSecondary));
        layout->addWidget(pubLabel);
    }

    // Scan level selection
    auto* scanGroup = new QGroupBox(tr("Leftover Scan Level"), &dialog);
    auto* scanLayout = new QVBoxLayout(scanGroup);

    auto* radioSafe = new QRadioButton(
        tr("Safe — Scan common locations only"), &dialog);
    auto* radioModerate = new QRadioButton(
        tr("Moderate — Scan common + registry (recommended)"), &dialog);
    auto* radioAdvanced = new QRadioButton(
        tr("Advanced — Deep scan including services, tasks, firewall rules"),
        &dialog);

    // Select default based on controller preference
    switch (m_controller->defaultScanLevel()) {
    case ScanLevel::Safe:     radioSafe->setChecked(true); break;
    case ScanLevel::Moderate: radioModerate->setChecked(true); break;
    case ScanLevel::Advanced: radioAdvanced->setChecked(true); break;
    }

    scanLayout->addWidget(radioSafe);
    scanLayout->addWidget(radioModerate);
    scanLayout->addWidget(radioAdvanced);
    layout->addWidget(scanGroup);

    // Options
    auto* restoreCheck = new QCheckBox(
        tr("Create system restore point before uninstall"), &dialog);
    restoreCheck->setChecked(m_controller->autoRestorePoint());

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setToolTip(
            tr("Requires administrator privileges. Run SAK as administrator to enable."));
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
    }
    layout->addWidget(restoreCheck);

    auto* autoCleanCheck = new QCheckBox(
        tr("Automatically clean safe leftover items"), &dialog);
    autoCleanCheck->setChecked(m_controller->autoCleanSafe());
    layout->addWidget(autoCleanCheck);

    // Buttons
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Uninstall"));
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        ScanLevel level = ScanLevel::Moderate;
        if (radioSafe->isChecked()) level = ScanLevel::Safe;
        else if (radioAdvanced->isChecked()) level = ScanLevel::Advanced;

        m_controller->uninstallProgram(
            program, level,
            restoreCheck->isChecked(),
            autoCleanCheck->isChecked());
    }
}

// ── Forced Uninstall Dialog ─────────────────────────────────────────────────

void AdvancedUninstallPanel::showForcedUninstallDialog(
    const ProgramInfo& program)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Forced Uninstall"));
    dialog.setMinimumWidth(480);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Warning header
    auto* warningLabel = new QLabel(
        tr("<b style='color: %1;'>⚠ Forced Uninstall</b>")
            .arg(ui::kColorWarning),
        &dialog);
    layout->addWidget(warningLabel);

    auto* descLabel = new QLabel(
        tr("This will skip the native uninstaller for <b>%1</b> and attempt "
           "to remove all traces directly.\n\n"
           "Use this when:\n"
           "• The native uninstaller is broken or missing\n"
           "• The program won't uninstall normally\n"
           "• You want to perform a deep clean\n\n"
           "A complete leftover scan will be performed after removal.")
            .arg(program.displayName),
        &dialog);
    descLabel->setWordWrap(true);
    layout->addWidget(descLabel);

    // Scan level
    auto* scanGroup = new QGroupBox(tr("Scan Level"), &dialog);
    auto* scanLayout = new QVBoxLayout(scanGroup);

    auto* radioModerate = new QRadioButton(
        tr("Moderate — Registry + file system scan"), &dialog);
    auto* radioAdvanced = new QRadioButton(
        tr("Advanced — Deep scan including system objects (recommended)"),
        &dialog);
    radioAdvanced->setChecked(true);

    scanLayout->addWidget(radioModerate);
    scanLayout->addWidget(radioAdvanced);
    layout->addWidget(scanGroup);

    // Restore point
    auto* restoreCheck = new QCheckBox(
        tr("Create system restore point before forced removal"), &dialog);
    restoreCheck->setChecked(true);

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
        restoreCheck->setToolTip(
            tr("Requires administrator privileges."));
    }
    layout->addWidget(restoreCheck);

    // Buttons
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* okBtn = buttonBox->button(QDialogButtonBox::Ok);
    okBtn->setText(tr("Force Uninstall"));
    okBtn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        ScanLevel level = radioAdvanced->isChecked()
            ? ScanLevel::Advanced : ScanLevel::Moderate;

        m_controller->forceUninstall(
            program, level, restoreCheck->isChecked());
    }
}

// ── Batch Uninstall Dialog ──────────────────────────────────────────────────

void AdvancedUninstallPanel::showBatchUninstallDialog()
{
    const auto queue = m_controller->queue();
    if (queue.isEmpty()) {
        Q_EMIT statusMessage(tr("Batch queue is empty."), 3000);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Batch Uninstall"));
    dialog.setMinimumSize(550, 400);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    auto* headerLabel = new QLabel(
        tr("<b>Batch Uninstall Queue</b> — %1 programs")
            .arg(queue.size()),
        &dialog);
    layout->addWidget(headerLabel);

    // Queue list
    auto* queueList = new QListWidget(&dialog);
    queueList->setSelectionMode(QAbstractItemView::SingleSelection);

    qint64 totalSize = 0;
    for (const auto& item : queue) {
        QString text = item.program.displayName;
        if (!item.program.displayVersion.isEmpty()) {
            text += " (" + item.program.displayVersion + ")";
        }
        if (item.program.estimatedSizeKB > 0) {
            text += " \u2014 " + formatSize(item.program.estimatedSizeKB * 1024);
            totalSize += item.program.estimatedSizeKB * 1024;
        }
        queueList->addItem(text);
    }
    layout->addWidget(queueList, 1);

    // Queue actions
    auto* actionRow = new QHBoxLayout();
    auto* removeBtn = new QPushButton(tr("Remove Selected"), &dialog);
    removeBtn->setEnabled(false);
    actionRow->addWidget(removeBtn);

    auto* clearBtn = new QPushButton(tr("Clear Queue"), &dialog);
    actionRow->addWidget(clearBtn);

    actionRow->addStretch();

    auto* totalLabel = new QLabel(
        tr("Total size: %1").arg(formatSize(totalSize)), &dialog);
    totalLabel->setStyleSheet(
        QString("color: %1; font-weight: bold;").arg(ui::kColorTextSecondary));
    actionRow->addWidget(totalLabel);

    layout->addLayout(actionRow);

    // Options
    auto* restoreCheck = new QCheckBox(
        tr("Create single restore point before batch"), &dialog);
    restoreCheck->setChecked(m_controller->autoRestorePoint());

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
        restoreCheck->setToolTip(tr("Requires administrator privileges."));
    }
    layout->addWidget(restoreCheck);

    // Note
    auto* noteLabel = new QLabel(
        tr("Programs will be uninstalled sequentially. You may cancel "
           "the batch at any time — remaining programs will be skipped."),
        &dialog);
    noteLabel->setWordWrap(true);
    noteLabel->setStyleSheet(
        QString("color: %1;").arg(ui::kColorTextMuted));
    layout->addWidget(noteLabel);

    // Buttons
    auto* buttonBox = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    auto* startBtn = buttonBox->button(QDialogButtonBox::Ok);
    startBtn->setText(tr("Start Batch Uninstall"));
    startBtn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);

    // Wire actions
    connect(queueList, &QListWidget::currentRowChanged,
            removeBtn, [removeBtn](int row) {
                removeBtn->setEnabled(row >= 0);
            });

    connect(removeBtn, &QPushButton::clicked, &dialog,
            [this, queueList, headerLabel, totalLabel]() {
                int row = queueList->currentRow();
                if (row >= 0) {
                    m_controller->removeFromQueue(row);
                    delete queueList->takeItem(row);
                    headerLabel->setText(
                        tr("<b>Batch Uninstall Queue</b> — %1 programs")
                            .arg(queueList->count()));
                    // Recalculate total
                    qint64 newTotal = 0;
                    for (const auto& qi : m_controller->queue()) {
                        newTotal += qi.program.estimatedSizeKB * 1024;
                    }
                    totalLabel->setText(
                        tr("Total size: %1").arg(formatSize(newTotal)));
                }
            });

    connect(clearBtn, &QPushButton::clicked, &dialog,
            [this, queueList, &dialog]() {
                m_controller->clearQueue();
                dialog.reject();
            });

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        if (m_controller->queue().isEmpty()) {
            Q_EMIT statusMessage(tr("Batch queue is empty."), 3000);
            return;
        }
        setOperationRunning(true);
        logMessage(QString("Starting batch uninstall of %1 programs...")
                       .arg(m_controller->queue().size()));
        m_controller->startBatchUninstall(restoreCheck->isChecked());
    }
}

// ── Program Properties Dialog ───────────────────────────────────────────────

void AdvancedUninstallPanel::showProgramProperties(const ProgramInfo& program)
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Program Properties — %1").arg(program.displayName));
    dialog.setMinimumWidth(520);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Scroll area for properties
    auto* scrollArea = new QScrollArea(&dialog);
    scrollArea->setWidgetResizable(true);
    auto* scrollWidget = new QWidget(scrollArea);
    auto* formLayout = new QFormLayout(scrollWidget);
    formLayout->setSpacing(ui::kSpacingSmall);
    formLayout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    scrollArea->setWidget(scrollWidget);

    // Helper to add a row if the value is non-empty
    auto addRow = [&](const QString& label, const QString& value) {
        if (value.isEmpty()) return;
        auto* valueLabel = new QLabel(value, scrollWidget);
        valueLabel->setTextInteractionFlags(
            Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        valueLabel->setWordWrap(true);
        formLayout->addRow(
            new QLabel(QString("<b>%1:</b>").arg(label), scrollWidget),
            valueLabel);
    };

    // Icon and name header
    if (!program.cachedImage.isNull()) {
        auto* iconLabel = new QLabel(scrollWidget);
        iconLabel->setPixmap(
            QPixmap::fromImage(program.cachedImage).scaled(
                32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
        formLayout->addRow(iconLabel);
    }

    addRow(tr("Name"), program.displayName);
    addRow(tr("Publisher"), program.publisher);
    addRow(tr("Version"), program.displayVersion);
    addRow(tr("Install Date"), program.installDate);
    addRow(tr("Install Location"), program.installLocation);

    if (program.estimatedSizeKB > 0) {
        addRow(tr("Estimated Size"), formatSize(program.estimatedSizeKB * 1024));
    }

    // Source type
    QString sourceText;
    switch (program.source) {
    case ProgramInfo::Source::RegistryHKLM:       sourceText = tr("Win32 (HKLM)"); break;
    case ProgramInfo::Source::RegistryHKLM_WOW64: sourceText = tr("Win32 (WOW64)"); break;
    case ProgramInfo::Source::RegistryHKCU:       sourceText = tr("Win32 (HKCU)"); break;
    case ProgramInfo::Source::UWP:                sourceText = tr("UWP App"); break;
    case ProgramInfo::Source::Provisioned:        sourceText = tr("Provisioned UWP"); break;
    }
    addRow(tr("Source"), sourceText);

    // Status flags
    QStringList flags;
    if (program.isSystemComponent) flags << tr("System Component");
    if (program.isBloatware)       flags << tr("Potential Bloatware");
    if (program.isOrphaned)        flags << tr("Orphaned");
    if (!flags.isEmpty()) {
        addRow(tr("Flags"), flags.join(", "));
    }

    // Technical details
    formLayout->addRow(new QLabel(
        QString("<br><b>%1</b>").arg(tr("Technical Details")), scrollWidget));

    addRow(tr("Uninstall Command"), program.uninstallString);
    addRow(tr("Quiet Uninstall"), program.quietUninstallString);
    addRow(tr("Modify Path"), program.modifyPath);
    addRow(tr("Registry Key"), program.registryKeyPath);
    addRow(tr("Package Family"), program.packageFamilyName);
    addRow(tr("Package Full Name"), program.packageFullName);

    layout->addWidget(scrollArea, 1);

    // Close button
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

} // namespace sak
