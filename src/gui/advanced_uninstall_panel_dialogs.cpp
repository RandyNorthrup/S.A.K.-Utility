// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel_dialogs.cpp
/// @brief Dialog implementations for uninstall confirmation, forced uninstall,
///        batch queue management, and program properties

#include "sak/advanced_uninstall_controller.h"
#include "sak/advanced_uninstall_panel.h"
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

namespace {

QString programSourceLabel(sak::ProgramInfo::Source source) {
    switch (source) {
    case sak::ProgramInfo::Source::RegistryHKLM:
        return QObject::tr("Win32 (HKLM)");
    case sak::ProgramInfo::Source::RegistryHKLM_WOW64:
        return QObject::tr("Win32 (WOW64)");
    case sak::ProgramInfo::Source::RegistryHKCU:
        return QObject::tr("Win32 (HKCU)");
    case sak::ProgramInfo::Source::UWP:
        return QObject::tr("UWP App");
    case sak::ProgramInfo::Source::Provisioned:
        return QObject::tr("Provisioned UWP");
    }
    return {};
}

QStringList programFlagLabels(const sak::ProgramInfo& program) {
    QStringList flags;
    if (program.isSystemComponent) {
        flags << QObject::tr("System Component");
    }
    if (program.isBloatware) {
        flags << QObject::tr("Potential Bloatware");
    }
    if (program.isOrphaned) {
        flags << QObject::tr("Orphaned");
    }
    return flags;
}

}  // namespace

namespace sak {

// ── Uninstall Confirmation Dialog ───────────────────────────────────────────

void AdvancedUninstallPanel::showUninstallConfirmation(const ProgramInfo& program) {
    Q_ASSERT(m_controller);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Uninstall Program"));
    dialog.setMinimumWidth(450);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Program info header
    auto* headerLabel = new QLabel(tr("Uninstall <b>%1</b>?").arg(program.displayName), &dialog);
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    if (!program.publisher.isEmpty()) {
        auto* pubLabel = new QLabel(tr("Publisher: %1").arg(program.publisher), &dialog);
        pubLabel->setStyleSheet(QString("color: %1;").arg(ui::kColorTextSecondary));
        layout->addWidget(pubLabel);
    }

    // Scan level selection
    auto* scanGroup = new QGroupBox(tr("Leftover Scan Level"), &dialog);
    auto* scanLayout = new QVBoxLayout(scanGroup);

    auto* radioSafe = new QRadioButton(tr("Safe — Scan common locations only"), &dialog);
    auto* radioModerate = new QRadioButton(tr("Moderate — Scan common + registry (recommended)"),
                                           &dialog);
    auto* radioAdvanced = new QRadioButton(
        tr("Advanced — Deep scan including services, tasks, firewall rules"), &dialog);

    // Select default based on controller preference
    switch (m_controller->defaultScanLevel()) {
    case ScanLevel::Safe:
        radioSafe->setChecked(true);
        break;
    case ScanLevel::Moderate:
        radioModerate->setChecked(true);
        break;
    case ScanLevel::Advanced:
        radioAdvanced->setChecked(true);
        break;
    }

    scanLayout->addWidget(radioSafe);
    scanLayout->addWidget(radioModerate);
    scanLayout->addWidget(radioAdvanced);
    layout->addWidget(scanGroup);

    // Options
    auto* restoreCheck = new QCheckBox(tr("Create system restore point before uninstall"), &dialog);
    restoreCheck->setChecked(m_controller->autoRestorePoint());

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setToolTip(
            tr("Requires administrator privileges. Run SAK as administrator to enable."));
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
    }
    layout->addWidget(restoreCheck);

    auto* autoCleanCheck = new QCheckBox(tr("Automatically clean safe leftover items"), &dialog);
    autoCleanCheck->setChecked(m_controller->autoCleanSafe());
    layout->addWidget(autoCleanCheck);

    // Buttons
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           &dialog);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Uninstall"));
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        ScanLevel level = ScanLevel::Moderate;
        if (radioSafe->isChecked()) {
            level = ScanLevel::Safe;
        } else if (radioAdvanced->isChecked()) {
            level = ScanLevel::Advanced;
        }

        m_controller->uninstallProgram(
            program, level, restoreCheck->isChecked(), autoCleanCheck->isChecked());
    }
}

// ── Forced Uninstall Dialog ─────────────────────────────────────────────────

void AdvancedUninstallPanel::showForcedUninstallDialog(const ProgramInfo& program) {
    Q_ASSERT(m_controller);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Forced Uninstall"));
    dialog.setMinimumWidth(480);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Warning header
    auto* warningLabel = new QLabel(
        tr("<b style='color: %1;'>⚠ Forced Uninstall</b>").arg(ui::kColorWarning), &dialog);
    layout->addWidget(warningLabel);

    auto* descLabel =
        new QLabel(tr("This will skip the native uninstaller for <b>%1</b> and attempt "
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

    auto* radioModerate = new QRadioButton(tr("Moderate — Registry + file system scan"), &dialog);
    auto* radioAdvanced = new QRadioButton(
        tr("Advanced — Deep scan including system objects (recommended)"), &dialog);
    radioAdvanced->setChecked(true);

    scanLayout->addWidget(radioModerate);
    scanLayout->addWidget(radioAdvanced);
    layout->addWidget(scanGroup);

    // Restore point
    auto* restoreCheck = new QCheckBox(tr("Create system restore point before forced removal"),
                                       &dialog);
    restoreCheck->setChecked(true);

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
        restoreCheck->setToolTip(tr("Requires administrator privileges."));
    }
    layout->addWidget(restoreCheck);

    // Buttons
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           &dialog);
    auto* okBtn = buttonBox->button(QDialogButtonBox::Ok);
    okBtn->setText(tr("Force Uninstall"));
    okBtn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        ScanLevel level = radioAdvanced->isChecked() ? ScanLevel::Advanced : ScanLevel::Moderate;

        m_controller->forceUninstall(program, level, restoreCheck->isChecked());
    }
}

// ── Batch Uninstall Dialog ──────────────────────────────────────────────────

void AdvancedUninstallPanel::populateBatchUninstallQueueList(
    const QVector<UninstallQueueItem>& queue, QListWidget* queueList, qint64* totalBytesOut) const {
    Q_ASSERT(queueList);
    Q_ASSERT(totalBytesOut);

    qint64 totalBytes = 0;
    for (const auto& item : queue) {
        QString text = item.program.displayName;
        if (!item.program.displayVersion.isEmpty()) {
            text += " (" + item.program.displayVersion + ")";
        }
        if (item.program.estimatedSizeKB > 0) {
            text += " \u2014 " + formatSize(item.program.estimatedSizeKB * 1024);
            totalBytes += item.program.estimatedSizeKB * 1024;
        }
        queueList->addItem(text);
    }

    *totalBytesOut = totalBytes;
}

void AdvancedUninstallPanel::wireBatchUninstallQueueActions(const BatchQueueWidgets& widgets,
                                                            QDialog* dialog) {
    Q_ASSERT(widgets.queue_list);
    Q_ASSERT(widgets.header_label);
    Q_ASSERT(widgets.total_label);
    Q_ASSERT(widgets.remove_btn);
    Q_ASSERT(widgets.clear_btn);
    Q_ASSERT(dialog);

    connect(widgets.queue_list,
            &QListWidget::currentRowChanged,
            widgets.remove_btn,
            [removeBtn = widgets.remove_btn](int row) { removeBtn->setEnabled(row >= 0); });

    connect(widgets.remove_btn, &QPushButton::clicked, dialog, [this, w = widgets]() {
        int row = w.queue_list->currentRow();
        if (row < 0) {
            return;
        }

        m_controller->removeFromQueue(row);
        delete w.queue_list->takeItem(row);

        w.header_label->setText(
            tr("<b>Batch Uninstall Queue</b> — %1 programs").arg(w.queue_list->count()));

        qint64 newTotal = 0;
        for (const auto& qi : m_controller->queue()) {
            newTotal += qi.program.estimatedSizeKB * 1024;
        }
        w.total_label->setText(tr("Total size: %1").arg(formatSize(newTotal)));
    });

    connect(widgets.clear_btn, &QPushButton::clicked, dialog, [this, dialog]() {
        m_controller->clearQueue();
        dialog->reject();
    });
}

QCheckBox* AdvancedUninstallPanel::addBatchUninstallOptions(QDialog* dialog,
                                                            QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* restoreCheck = new QCheckBox(tr("Create single restore point before batch"), dialog);
    restoreCheck->setChecked(m_controller->autoRestorePoint());

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
        restoreCheck->setToolTip(tr("Requires administrator privileges."));
    }
    layout->addWidget(restoreCheck);

    auto* noteLabel = new QLabel(tr("Programs will be uninstalled sequentially. You may cancel "
                                    "the batch at any time — remaining programs will be skipped."),
                                 dialog);
    noteLabel->setWordWrap(true);
    noteLabel->setStyleSheet(QString("color: %1;").arg(ui::kColorTextMuted));
    layout->addWidget(noteLabel);

    return restoreCheck;
}

QDialogButtonBox* AdvancedUninstallPanel::addBatchUninstallButtons(QDialog* dialog,
                                                                   QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* startBtn = buttonBox->button(QDialogButtonBox::Ok);
    startBtn->setText(tr("Start Batch Uninstall"));
    startBtn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);
    return buttonBox;
}

void AdvancedUninstallPanel::showBatchUninstallDialog() {
    Q_ASSERT(m_controller);
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

    auto* headerLabel =
        new QLabel(tr("<b>Batch Uninstall Queue</b> — %1 programs").arg(queue.size()), &dialog);
    layout->addWidget(headerLabel);

    // Queue list
    auto* queueList = new QListWidget(&dialog);
    queueList->setSelectionMode(QAbstractItemView::SingleSelection);

    qint64 totalSize = 0;
    populateBatchUninstallQueueList(queue, queueList, &totalSize);
    layout->addWidget(queueList, 1);

    // Queue actions
    auto* actionRow = new QHBoxLayout();
    auto* removeBtn = new QPushButton(tr("Remove Selected"), &dialog);
    removeBtn->setEnabled(false);
    actionRow->addWidget(removeBtn);

    auto* clearBtn = new QPushButton(tr("Clear Queue"), &dialog);
    actionRow->addWidget(clearBtn);

    actionRow->addStretch();

    auto* totalLabel = new QLabel(tr("Total size: %1").arg(formatSize(totalSize)), &dialog);
    totalLabel->setStyleSheet(
        QString("color: %1; font-weight: bold;").arg(ui::kColorTextSecondary));
    actionRow->addWidget(totalLabel);

    layout->addLayout(actionRow);

    auto* restoreCheck = addBatchUninstallOptions(&dialog, layout);
    auto* buttonBox = addBatchUninstallButtons(&dialog, layout);

    wireBatchUninstallQueueActions({queueList, headerLabel, totalLabel, removeBtn, clearBtn},
                                   &dialog);

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

void AdvancedUninstallPanel::populateProgramPropertiesForm(const ProgramInfo& program,
                                                           QWidget* scrollWidget,
                                                           QFormLayout* formLayout) const {
    Q_ASSERT(scrollWidget);
    Q_ASSERT(formLayout);

    const auto addRow = [&](const QString& label, const QString& value) {
        if (value.isEmpty()) {
            return;
        }
        auto* valueLabel = new QLabel(value, scrollWidget);
        valueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                            Qt::TextSelectableByKeyboard);
        valueLabel->setWordWrap(true);
        formLayout->addRow(new QLabel(QString("<b>%1:</b>").arg(label), scrollWidget), valueLabel);
    };

    if (!program.cachedImage.isNull()) {
        auto* iconLabel = new QLabel(scrollWidget);
        iconLabel->setPixmap(QPixmap::fromImage(program.cachedImage)
                                 .scaled(32, 32, Qt::KeepAspectRatio, Qt::SmoothTransformation));
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

    addRow(tr("Source"), programSourceLabel(program.source));

    const QStringList flags = programFlagLabels(program);
    if (!flags.isEmpty()) {
        addRow(tr("Flags"), flags.join(", "));
    }

    formLayout->addRow(
        new QLabel(QString("<br><b>%1</b>").arg(tr("Technical Details")), scrollWidget));

    addRow(tr("Uninstall Command"), program.uninstallString);
    addRow(tr("Quiet Uninstall"), program.quietUninstallString);
    addRow(tr("Modify Path"), program.modifyPath);
    addRow(tr("Registry Key"), program.registryKeyPath);
    addRow(tr("Package Family"), program.packageFamilyName);
    addRow(tr("Package Full Name"), program.packageFullName);
}

void AdvancedUninstallPanel::showProgramProperties(const ProgramInfo& program) {
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

    populateProgramPropertiesForm(program, scrollWidget, formLayout);

    layout->addWidget(scrollArea, 1);

    // Close button
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

// ── Settings Dialog ─────────────────────────────────────────────────────────

QCheckBox* AdvancedUninstallPanel::addSettingsSelectionGroup(QDialog* dialog,
                                                             QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* group = new QGroupBox(tr("Leftover Selection"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    auto* selectAllCheck =
        new QCheckBox(tr("Select all leftovers by default (instead of safe only)"), dialog);
    selectAllCheck->setChecked(m_controller->selectAllByDefault());
    selectAllCheck->setToolTip(
        tr("When enabled, all leftover items are pre-selected after scanning. "
           "Otherwise, only items classified as Safe are selected."));
    groupLayout->addWidget(selectAllCheck);

    layout->addWidget(group);
    return selectAllCheck;
}

QCheckBox* AdvancedUninstallPanel::addSettingsDeletionGroup(QDialog* dialog,
                                                            QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* group = new QGroupBox(tr("Deletion Behavior"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    auto* recycleBinCheck = new QCheckBox(tr("Delete to Recycle Bin instead of permanent deletion"),
                                          dialog);
    recycleBinCheck->setChecked(m_controller->useRecycleBin());
    recycleBinCheck->setToolTip(
        tr("When enabled, files and folders are sent to the Recycle Bin, "
           "allowing recovery. Registry entries and services are always "
           "removed permanently."));
    groupLayout->addWidget(recycleBinCheck);

    auto* recycleBinNote =
        new QLabel(tr("Note: Registry keys, services, scheduled tasks, and firewall "
                      "rules are always removed permanently regardless of this setting."),
                   dialog);
    recycleBinNote->setWordWrap(true);
    recycleBinNote->setStyleSheet(
        QString("color: %1; font-size: %2pt;").arg(ui::kColorTextMuted).arg(ui::kFontSizeSmall));
    groupLayout->addWidget(recycleBinNote);

    layout->addWidget(group);
    return recycleBinCheck;
}

QCheckBox* AdvancedUninstallPanel::addSettingsRestorePointGroup(QDialog* dialog,
                                                                QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* group = new QGroupBox(tr("System Protection"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    auto* restorePointCheck = new QCheckBox(tr("Create a restore point before uninstall"), dialog);
    restorePointCheck->setChecked(m_controller->autoRestorePoint());
    restorePointCheck->setToolTip(
        tr("When enabled, a Windows System Restore point is created before "
           "running the uninstaller. Requires administrator privileges."));

    bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restorePointCheck->setEnabled(false);
        restorePointCheck->setChecked(false);
        restorePointCheck->setToolTip(tr("Requires administrator privileges."));
    }

    groupLayout->addWidget(restorePointCheck);
    layout->addWidget(group);
    return restorePointCheck;
}

void AdvancedUninstallPanel::addSettingsScanLevelGroup(QDialog* dialog,
                                                       QVBoxLayout* layout,
                                                       QRadioButton*& safeRadio,
                                                       QRadioButton*& moderateRadio,
                                                       QRadioButton*& advancedRadio) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* group = new QGroupBox(tr("Default Scan Level"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    safeRadio = new QRadioButton(tr("Safe — Only obvious leftovers in known locations (fast)"),
                                 dialog);
    moderateRadio = new QRadioButton(
        tr("Moderate — Extended scanning with pattern matching (recommended)"), dialog);
    advancedRadio = new QRadioButton(tr("Advanced — Deep scan including services, tasks, firewall, "
                                        "shell extensions"),
                                     dialog);

    switch (m_controller->defaultScanLevel()) {
    case ScanLevel::Safe:
        safeRadio->setChecked(true);
        break;
    case ScanLevel::Moderate:
        moderateRadio->setChecked(true);
        break;
    case ScanLevel::Advanced:
        advancedRadio->setChecked(true);
        break;
    }

    groupLayout->addWidget(safeRadio);
    groupLayout->addWidget(moderateRadio);
    groupLayout->addWidget(advancedRadio);
    layout->addWidget(group);
}

QCheckBox* AdvancedUninstallPanel::addSettingsDisplayGroup(QDialog* dialog,
                                                           QVBoxLayout* layout) const {
    Q_ASSERT(dialog);
    Q_ASSERT(layout);

    auto* group = new QGroupBox(tr("Display"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    auto* systemComponentsCheck = new QCheckBox(tr("Show system components in program list"),
                                                dialog);
    systemComponentsCheck->setChecked(m_controller->showSystemComponents());
    systemComponentsCheck->setToolTip(
        tr("When enabled, programs marked as system components are shown "
           "in the program list. These are typically Windows components."));
    groupLayout->addWidget(systemComponentsCheck);

    layout->addWidget(group);
    return systemComponentsCheck;
}

void AdvancedUninstallPanel::showSettingsDialog() {
    Q_ASSERT(m_controller);
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Advanced Uninstall Settings"));
    dialog.setMinimumWidth(520);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    auto* selectAllCheck = addSettingsSelectionGroup(&dialog, layout);
    auto* recycleBinCheck = addSettingsDeletionGroup(&dialog, layout);
    auto* restorePointCheck = addSettingsRestorePointGroup(&dialog, layout);

    QRadioButton* safeRadio = nullptr;
    QRadioButton* moderateRadio = nullptr;
    QRadioButton* advancedRadio = nullptr;
    addSettingsScanLevelGroup(&dialog, layout, safeRadio, moderateRadio, advancedRadio);

    auto* systemComponentsCheck = addSettingsDisplayGroup(&dialog, layout);

    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                           &dialog);
    buttonBox->button(QDialogButtonBox::Ok)->setText(tr("Save Settings"));
    layout->addWidget(buttonBox);

    connect(buttonBox, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_controller->setSelectAllByDefault(selectAllCheck->isChecked());
    m_controller->setUseRecycleBin(recycleBinCheck->isChecked());
    m_controller->setAutoRestorePoint(restorePointCheck->isChecked());

    ScanLevel scanLevel = ScanLevel::Safe;
    if (advancedRadio && advancedRadio->isChecked()) {
        scanLevel = ScanLevel::Advanced;
    }
    if (moderateRadio && moderateRadio->isChecked()) {
        scanLevel = ScanLevel::Moderate;
    }
    m_controller->setDefaultScanLevel(scanLevel);

    m_controller->setShowSystemComponents(systemComponentsCheck->isChecked());
    m_controller->saveSettings();
    Q_EMIT statusMessage(tr("Settings saved."), 3000);
}

}  // namespace sak
