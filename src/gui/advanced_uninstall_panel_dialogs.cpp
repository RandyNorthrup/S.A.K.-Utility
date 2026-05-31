// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel_dialogs.cpp
/// @brief Dialog implementations for uninstall confirmation, forced uninstall,
///        batch queue management, and program properties

#include "sak/advanced_uninstall_controller.h"
#include "sak/advanced_uninstall_panel.h"
#include "sak/layout_constants.h"
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

constexpr int kUninstallConfirmMinWidth = 450;
constexpr int kForcedUninstallMinWidth = 480;
constexpr int kBatchUninstallMinWidth = 550;
constexpr int kBatchUninstallMinHeight = 400;
constexpr int kProgramPropertiesMinWidth = 520;
constexpr int kSettingsDialogMinWidth = 520;
constexpr int kDialogStatusTimeoutMs = sak::kTimerStatusMessageMs;
constexpr int kProgramIconPreviewSize = 32;

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

void addUninstallProgramHeader(QDialog* dialog,
                               QVBoxLayout* layout,
                               const sak::ProgramInfo& program) {
    auto* headerLabel = new QLabel(QObject::tr("Uninstall <b>%1</b>?").arg(program.displayName),
                                   dialog);
    headerLabel->setWordWrap(true);
    layout->addWidget(headerLabel);

    if (program.publisher.isEmpty()) {
        return;
    }

    auto* pubLabel = new QLabel(QObject::tr("Publisher: %1").arg(program.publisher), dialog);
    pubLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextSecondary));
    layout->addWidget(pubLabel);
}

struct ScanLevelRadioGroup {
    QRadioButton* safe = nullptr;
    QRadioButton* moderate = nullptr;
    QRadioButton* advanced = nullptr;
};

void selectScanLevelRadio(const ScanLevelRadioGroup& radios, sak::ScanLevel defaultLevel) {
    switch (defaultLevel) {
    case sak::ScanLevel::Safe:
        radios.safe->setChecked(true);
        break;
    case sak::ScanLevel::Moderate:
        radios.moderate->setChecked(true);
        break;
    case sak::ScanLevel::Advanced:
        radios.advanced->setChecked(true);
        break;
    }
}

ScanLevelRadioGroup addScanLevelGroup(QDialog* dialog,
                                      QVBoxLayout* layout,
                                      sak::ScanLevel defaultLevel) {
    auto* scanGroup = new QGroupBox(QObject::tr("Leftover Scan Level"), dialog);
    auto* scanLayout = new QVBoxLayout(scanGroup);
    ScanLevelRadioGroup radios;
    radios.safe = new QRadioButton(QObject::tr("Safe -- Scan common locations only"), dialog);
    radios.moderate =
        new QRadioButton(QObject::tr("Moderate -- Scan common + registry (recommended)"), dialog);
    radios.advanced = new QRadioButton(
        QObject::tr("Advanced -- Deep scan including services, tasks, firewall rules"), dialog);
    selectScanLevelRadio(radios, defaultLevel);
    scanLayout->addWidget(radios.safe);
    scanLayout->addWidget(radios.moderate);
    scanLayout->addWidget(radios.advanced);
    layout->addWidget(scanGroup);
    return radios;
}

sak::ScanLevel selectedScanLevel(const ScanLevelRadioGroup& radios) {
    if (radios.safe->isChecked()) {
        return sak::ScanLevel::Safe;
    }
    if (radios.advanced->isChecked()) {
        return sak::ScanLevel::Advanced;
    }
    return sak::ScanLevel::Moderate;
}

QCheckBox* addRestorePointOption(QDialog* dialog, QVBoxLayout* layout, bool autoRestorePoint) {
    auto* restoreCheck = new QCheckBox(QObject::tr("Create system restore point before uninstall"),
                                       dialog);
    restoreCheck->setChecked(autoRestorePoint);
    if (!sak::RestorePointManager::isElevated()) {
        restoreCheck->setToolTip(
            QObject::tr("Requires administrator privileges. Run SAK as administrator to enable."));
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
    }
    layout->addWidget(restoreCheck);
    return restoreCheck;
}

QCheckBox* addAutoCleanOption(QDialog* dialog, QVBoxLayout* layout, bool autoCleanSafe) {
    auto* autoCleanCheck = new QCheckBox(QObject::tr("Automatically clean safe leftover items"),
                                         dialog);
    autoCleanCheck->setChecked(autoCleanSafe);
    layout->addWidget(autoCleanCheck);
    return autoCleanCheck;
}

QDialogButtonBox* addUninstallConfirmationButtons(QDialog* dialog, QVBoxLayout* layout) {
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    buttonBox->button(QDialogButtonBox::Ok)->setText(QObject::tr("Uninstall"));
    layout->addWidget(buttonBox);
    QObject::connect(buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttonBox;
}

QLabel* addForcedUninstallWarning(QDialog* dialog, QVBoxLayout* layout) {
    auto* warningLabel = new QLabel(QString::fromLatin1(sak::ui::kHtmlBoldColor)
                                        .arg(sak::ui::htmlColor(sak::ui::kColorWarning),
                                             QObject::tr("(!) Forced Uninstall")),
                                    dialog);
    layout->addWidget(warningLabel);
    return warningLabel;
}

QLabel* addForcedUninstallDescription(QDialog* dialog,
                                      QVBoxLayout* layout,
                                      const sak::ProgramInfo& program) {
    auto* descLabel =
        new QLabel(QObject::tr("This will skip the native uninstaller for <b>%1</b> and attempt "
                               "to remove all traces directly.\n\n"
                               "Use this when:\n"
                               "* The native uninstaller is broken or missing\n"
                               "* The program won't uninstall normally\n"
                               "* You want to perform a deep clean\n\n"
                               "A complete leftover scan will be performed after removal.")
                       .arg(program.displayName),
                   dialog);
    descLabel->setWordWrap(true);
    layout->addWidget(descLabel);
    return descLabel;
}

struct ForcedUninstallScanRadios {
    QRadioButton* moderate = nullptr;
    QRadioButton* advanced = nullptr;
};

ForcedUninstallScanRadios addForcedUninstallScanGroup(QDialog* dialog, QVBoxLayout* layout) {
    auto* scanGroup = new QGroupBox(QObject::tr("Scan Level"), dialog);
    auto* scanLayout = new QVBoxLayout(scanGroup);

    ForcedUninstallScanRadios radios;
    radios.moderate = new QRadioButton(QObject::tr("Moderate -- Registry + file system scan"),
                                       dialog);
    radios.advanced = new QRadioButton(
        QObject::tr("Advanced -- Deep scan including system objects (recommended)"), dialog);
    radios.advanced->setChecked(true);

    scanLayout->addWidget(radios.moderate);
    scanLayout->addWidget(radios.advanced);
    layout->addWidget(scanGroup);
    return radios;
}

QCheckBox* addForcedUninstallRestoreOption(QDialog* dialog, QVBoxLayout* layout) {
    auto* restoreCheck =
        new QCheckBox(QObject::tr("Create system restore point before forced removal"), dialog);
    restoreCheck->setChecked(true);

    if (!sak::RestorePointManager::isElevated()) {
        restoreCheck->setEnabled(false);
        restoreCheck->setChecked(false);
        restoreCheck->setToolTip(QObject::tr("Requires administrator privileges."));
    }

    layout->addWidget(restoreCheck);
    return restoreCheck;
}

QDialogButtonBox* addForcedUninstallButtons(QDialog* dialog, QVBoxLayout* layout) {
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* okBtn = buttonBox->button(QDialogButtonBox::Ok);
    okBtn->setText(QObject::tr("Force Uninstall"));
    okBtn->setStyleSheet(sak::ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);

    QObject::connect(buttonBox, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(buttonBox, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return buttonBox;
}

sak::ScanLevel selectedForcedUninstallScanLevel(const ForcedUninstallScanRadios& radios) {
    return radios.advanced->isChecked() ? sak::ScanLevel::Advanced : sak::ScanLevel::Moderate;
}

}  // namespace

namespace sak {

// -- Uninstall Confirmation Dialog -------------------------------------------

void AdvancedUninstallPanel::showUninstallConfirmation(const ProgramInfo& program) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Uninstall Program"));
    dialog.setMinimumWidth(kUninstallConfirmMinWidth);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    addUninstallProgramHeader(&dialog, layout, program);
    const ScanLevelRadioGroup scanRadios =
        addScanLevelGroup(&dialog, layout, m_controller->defaultScanLevel());
    QCheckBox* restoreCheck =
        addRestorePointOption(&dialog, layout, m_controller->autoRestorePoint());
    QCheckBox* autoCleanCheck = addAutoCleanOption(&dialog, layout, m_controller->autoCleanSafe());
    addUninstallConfirmationButtons(&dialog, layout);

    if (dialog.exec() == QDialog::Accepted) {
        m_controller->uninstallProgram(program,
                                       selectedScanLevel(scanRadios),
                                       restoreCheck->isChecked(),
                                       autoCleanCheck->isChecked());
    }
}

// -- Forced Uninstall Dialog -------------------------------------------------

void AdvancedUninstallPanel::showForcedUninstallDialog(const ProgramInfo& program) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Forced Uninstall"));
    dialog.setMinimumWidth(kForcedUninstallMinWidth);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    addForcedUninstallWarning(&dialog, layout);
    addForcedUninstallDescription(&dialog, layout, program);
    const ForcedUninstallScanRadios scanRadios = addForcedUninstallScanGroup(&dialog, layout);
    QCheckBox* restoreCheck = addForcedUninstallRestoreOption(&dialog, layout);
    addForcedUninstallButtons(&dialog, layout);

    if (dialog.exec() == QDialog::Accepted) {
        m_controller->forceUninstall(program,
                                     selectedForcedUninstallScanLevel(scanRadios),
                                     restoreCheck->isChecked());
    }
}

// -- Batch Uninstall Dialog --------------------------------------------------

void AdvancedUninstallPanel::populateBatchUninstallQueueList(
    const QVector<UninstallQueueItem>& queue, QListWidget* queueList, qint64* totalBytesOut) const {
    qint64 totalBytes = 0;
    for (const auto& item : queue) {
        QString text = item.program.displayName;
        if (!item.program.displayVersion.isEmpty()) {
            text += " (" + item.program.displayVersion + ")";
        }
        if (item.program.estimatedSizeKB > 0) {
            text += " \u2014 " + formatSize(item.program.estimatedSizeKB * sak::kBytesPerKB);
            totalBytes += item.program.estimatedSizeKB * sak::kBytesPerKB;
        }
        queueList->addItem(text);
    }

    *totalBytesOut = totalBytes;
}

void AdvancedUninstallPanel::wireBatchUninstallQueueActions(const BatchQueueWidgets& widgets,
                                                            QDialog* dialog) {
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
            tr("<b>Batch Uninstall Queue</b> -- %1 programs").arg(w.queue_list->count()));

        qint64 newTotal = 0;
        for (const auto& qi : m_controller->queue()) {
            newTotal += qi.program.estimatedSizeKB * sak::kBytesPerKB;
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
                                    "the batch at any time -- remaining programs will be skipped."),
                                 dialog);
    noteLabel->setWordWrap(true);
    noteLabel->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    layout->addWidget(noteLabel);

    return restoreCheck;
}

QDialogButtonBox* AdvancedUninstallPanel::addBatchUninstallButtons(QDialog* dialog,
                                                                   QVBoxLayout* layout) const {
    auto* buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, dialog);
    auto* startBtn = buttonBox->button(QDialogButtonBox::Ok);
    startBtn->setText(tr("Start Batch Uninstall"));
    startBtn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(buttonBox);
    return buttonBox;
}

void AdvancedUninstallPanel::showBatchUninstallDialog() {
    const auto queue = m_controller->queue();
    if (queue.isEmpty()) {
        Q_EMIT statusMessage(tr("Batch queue is empty."), kDialogStatusTimeoutMs);
        return;
    }

    QDialog dialog(this);
    dialog.setWindowTitle(tr("Batch Uninstall"));
    dialog.setMinimumSize(kBatchUninstallMinWidth, kBatchUninstallMinHeight);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    auto* headerLabel =
        new QLabel(tr("<b>Batch Uninstall Queue</b> -- %1 programs").arg(queue.size()), &dialog);
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
        ui::fontWeightAndColorStyle(ui::kFontWeightBold, ui::kColorTextSecondary));
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
            Q_EMIT statusMessage(tr("Batch queue is empty."), kDialogStatusTimeoutMs);
            return;
        }
        setOperationRunning(true);
        logMessage(QString("Starting batch uninstall of %1 programs...")
                       .arg(m_controller->queue().size()));
        m_controller->startBatchUninstall(restoreCheck->isChecked());
    }
}

// -- Program Properties Dialog -----------------------------------------------

void AdvancedUninstallPanel::populateProgramPropertiesForm(const ProgramInfo& program,
                                                           QWidget* scrollWidget,
                                                           QFormLayout* formLayout) const {
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
                                 .scaled(kProgramIconPreviewSize,
                                         kProgramIconPreviewSize,
                                         Qt::KeepAspectRatio,
                                         Qt::SmoothTransformation));
        formLayout->addRow(iconLabel);
    }

    addRow(tr("Name"), program.displayName);
    addRow(tr("Publisher"), program.publisher);
    addRow(tr("Version"), program.displayVersion);
    addRow(tr("Install Date"), program.installDate);
    addRow(tr("Install Location"), program.installLocation);

    if (program.estimatedSizeKB > 0) {
        addRow(tr("Estimated Size"), formatSize(program.estimatedSizeKB * sak::kBytesPerKB));
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
    dialog.setWindowTitle(tr("Program Properties -- %1").arg(program.displayName));
    dialog.setMinimumWidth(kProgramPropertiesMinWidth);

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

// -- Settings Dialog ---------------------------------------------------------

QCheckBox* AdvancedUninstallPanel::addSettingsSelectionGroup(QDialog* dialog,
                                                             QVBoxLayout* layout) const {
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
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeSmall));
    groupLayout->addWidget(recycleBinNote);

    layout->addWidget(group);
    return recycleBinCheck;
}

QCheckBox* AdvancedUninstallPanel::addSettingsRestorePointGroup(QDialog* dialog,
                                                                QVBoxLayout* layout) const {
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
    auto* group = new QGroupBox(tr("Default Scan Level"), dialog);
    auto* groupLayout = new QVBoxLayout(group);

    safeRadio = new QRadioButton(tr("Safe -- Only obvious leftovers in known locations (fast)"),
                                 dialog);
    moderateRadio = new QRadioButton(
        tr("Moderate -- Extended scanning with pattern matching (recommended)"), dialog);
    advancedRadio =
        new QRadioButton(tr("Advanced -- Deep scan including services, tasks, firewall, "
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
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Advanced Uninstall Settings"));
    dialog.setMinimumWidth(kSettingsDialogMinWidth);

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
    Q_EMIT statusMessage(tr("Settings saved."), kDialogStatusTimeoutMs);
}

}  // namespace sak
