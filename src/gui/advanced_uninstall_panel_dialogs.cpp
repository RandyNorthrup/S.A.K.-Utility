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
    // displayName/publisher come from the uninstall registry keys, and HKCU is writable by a
    // non-admin process, so they are untrusted markup: escape them into the template that wants
    // <b>, and force plain text where the template wants none (see
    // populateProgramPropertiesForm(), which does the same for every other registry field).
    auto* header_label = new QLabel(
        QObject::tr("Uninstall <b>%1</b>?").arg(program.displayName.toHtmlEscaped()), dialog);
    header_label->setWordWrap(true);
    layout->addWidget(header_label);

    if (program.publisher.isEmpty()) {
        return;
    }

    auto* pub_label = sak::plainTextLabel(QObject::tr("Publisher: %1").arg(program.publisher),
                                          dialog);
    pub_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextSecondary));
    layout->addWidget(pub_label);
}

struct ScanLevelRadioGroup {
    QRadioButton* safe = nullptr;
    QRadioButton* moderate = nullptr;
    QRadioButton* advanced = nullptr;
};

void selectScanLevelRadio(const ScanLevelRadioGroup& radios, sak::ScanLevel default_level) {
    switch (default_level) {
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
                                      sak::ScanLevel default_level) {
    auto* scan_group = new QGroupBox(QObject::tr("Leftover Scan Level"), dialog);
    auto* scan_layout = new QVBoxLayout(scan_group);
    ScanLevelRadioGroup radios;
    radios.safe = new QRadioButton(QObject::tr("Safe -- Scan common locations only"), dialog);
    radios.moderate =
        new QRadioButton(QObject::tr("Moderate -- Scan common + registry (recommended)"), dialog);
    radios.advanced = new QRadioButton(
        QObject::tr("Advanced -- Deep scan including services, tasks, firewall rules"), dialog);
    selectScanLevelRadio(radios, default_level);
    scan_layout->addWidget(radios.safe);
    scan_layout->addWidget(radios.moderate);
    scan_layout->addWidget(radios.advanced);
    layout->addWidget(scan_group);
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

QCheckBox* addRestorePointOption(QDialog* dialog, QVBoxLayout* layout, bool auto_restore_point) {
    auto* restore_check = new QCheckBox(QObject::tr("Create system restore point before uninstall"),
                                        dialog);
    restore_check->setChecked(auto_restore_point);
    if (!sak::RestorePointManager::isElevated()) {
        restore_check->setToolTip(
            QObject::tr("Requires administrator privileges. Run SAK as administrator to enable."));
        restore_check->setEnabled(false);
        restore_check->setChecked(false);
    }
    layout->addWidget(restore_check);
    return restore_check;
}

QCheckBox* addAutoCleanOption(QDialog* dialog, QVBoxLayout* layout, bool auto_clean_safe) {
    auto* auto_clean_check = new QCheckBox(QObject::tr("Automatically clean safe leftover items"),
                                           dialog);
    auto_clean_check->setChecked(auto_clean_safe);
    layout->addWidget(auto_clean_check);
    return auto_clean_check;
}

QDialogButtonBox* addUninstallConfirmationButtons(QDialog* dialog, QVBoxLayout* layout) {
    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            dialog);
    button_box->button(QDialogButtonBox::Ok)->setText(QObject::tr("Uninstall"));
    layout->addWidget(button_box);
    QObject::connect(button_box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(button_box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return button_box;
}

QLabel* addForcedUninstallWarning(QDialog* dialog, QVBoxLayout* layout) {
    auto* warning_label = new QLabel(QString::fromLatin1(sak::ui::kHtmlBoldColor)
                                         .arg(sak::ui::htmlColor(sak::ui::kColorWarning),
                                              QObject::tr("(!) Forced Uninstall")),
                                     dialog);
    layout->addWidget(warning_label);
    return warning_label;
}

QLabel* addForcedUninstallDescription(QDialog* dialog,
                                      QVBoxLayout* layout,
                                      const sak::ProgramInfo& program) {
    auto* desc_label =
        new QLabel(QObject::tr("This will skip the native uninstaller for <b>%1</b> and attempt "
                               "to remove all traces directly.\n\n"
                               "Use this when:\n"
                               "* The native uninstaller is broken or missing\n"
                               "* The program won't uninstall normally\n"
                               "* You want to perform a deep clean\n\n"
                               "A complete leftover scan will be performed after removal.")
                       .arg(program.displayName.toHtmlEscaped()),
                   dialog);
    desc_label->setWordWrap(true);
    layout->addWidget(desc_label);
    return desc_label;
}

struct ForcedUninstallScanRadios {
    QRadioButton* moderate = nullptr;
    QRadioButton* advanced = nullptr;
};

ForcedUninstallScanRadios addForcedUninstallScanGroup(QDialog* dialog, QVBoxLayout* layout) {
    auto* scan_group = new QGroupBox(QObject::tr("Scan Level"), dialog);
    auto* scan_layout = new QVBoxLayout(scan_group);

    ForcedUninstallScanRadios radios;
    radios.moderate = new QRadioButton(QObject::tr("Moderate -- Registry + file system scan"),
                                       dialog);
    radios.advanced = new QRadioButton(
        QObject::tr("Advanced -- Deep scan including system objects (recommended)"), dialog);
    radios.advanced->setChecked(true);

    scan_layout->addWidget(radios.moderate);
    scan_layout->addWidget(radios.advanced);
    layout->addWidget(scan_group);
    return radios;
}

QCheckBox* addForcedUninstallRestoreOption(QDialog* dialog, QVBoxLayout* layout) {
    auto* restore_check =
        new QCheckBox(QObject::tr("Create system restore point before forced removal"), dialog);
    restore_check->setChecked(true);

    if (!sak::RestorePointManager::isElevated()) {
        restore_check->setEnabled(false);
        restore_check->setChecked(false);
        restore_check->setToolTip(QObject::tr("Requires administrator privileges."));
    }

    layout->addWidget(restore_check);
    return restore_check;
}

QDialogButtonBox* addForcedUninstallButtons(QDialog* dialog, QVBoxLayout* layout) {
    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            dialog);
    auto* ok_btn = button_box->button(QDialogButtonBox::Ok);
    ok_btn->setText(QObject::tr("Force Uninstall"));
    ok_btn->setStyleSheet(sak::ui::kDangerButtonStyle);
    layout->addWidget(button_box);

    QObject::connect(button_box, &QDialogButtonBox::accepted, dialog, &QDialog::accept);
    QObject::connect(button_box, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    return button_box;
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
    const ScanLevelRadioGroup scan_radios =
        addScanLevelGroup(&dialog, layout, m_controller->defaultScanLevel());
    const QCheckBox* restore_check =
        addRestorePointOption(&dialog, layout, m_controller->autoRestorePoint());
    const QCheckBox* auto_clean_check =
        addAutoCleanOption(&dialog, layout, m_controller->autoCleanSafe());
    addUninstallConfirmationButtons(&dialog, layout);

    if (dialog.exec() == QDialog::Accepted) {
        m_controller->uninstallProgram(program,
                                       selectedScanLevel(scan_radios),
                                       restore_check->isChecked(),
                                       auto_clean_check->isChecked());
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
    const ForcedUninstallScanRadios scan_radios = addForcedUninstallScanGroup(&dialog, layout);
    const QCheckBox* restore_check = addForcedUninstallRestoreOption(&dialog, layout);
    addForcedUninstallButtons(&dialog, layout);

    if (dialog.exec() == QDialog::Accepted) {
        m_controller->forceUninstall(program,
                                     selectedForcedUninstallScanLevel(scan_radios),
                                     restore_check->isChecked());
    }
}

// -- Batch Uninstall Dialog --------------------------------------------------

void AdvancedUninstallPanel::populateBatchUninstallQueueList(
    const QVector<UninstallQueueItem>& queue,
    QListWidget* queue_list,
    qint64* total_bytes_out) const {
    qint64 total_bytes = 0;
    for (const auto& item : queue) {
        QString text = item.program.displayName;
        if (!item.program.displayVersion.isEmpty()) {
            text += " (" + item.program.displayVersion + ")";
        }
        if (item.program.estimatedSizeKB > 0) {
            text += " \u2014 " + formatSize(item.program.estimatedSizeKB * sak::kBytesPerKB);
            total_bytes += item.program.estimatedSizeKB * sak::kBytesPerKB;
        }
        queue_list->addItem(text);
    }

    *total_bytes_out = total_bytes;
}

void AdvancedUninstallPanel::wireBatchUninstallQueueActions(const BatchQueueWidgets& widgets,
                                                            QDialog* dialog) {
    connect(widgets.queue_list,
            &QListWidget::currentRowChanged,
            widgets.remove_btn,
            [remove_btn = widgets.remove_btn](int row) { remove_btn->setEnabled(row >= 0); });

    connect(widgets.remove_btn, &QPushButton::clicked, dialog, [this, w = widgets]() {
        const int row = w.queue_list->currentRow();
        if (row < 0) {
            return;
        }

        m_controller->removeFromQueue(row);
        delete w.queue_list->takeItem(row);

        w.header_label->setText(
            tr("<b>Batch Uninstall Queue</b> -- %1 programs").arg(w.queue_list->count()));

        qint64 new_total = 0;
        for (const auto& qi : m_controller->queue()) {
            new_total += qi.program.estimatedSizeKB * sak::kBytesPerKB;
        }
        w.total_label->setText(tr("Total size: %1").arg(formatSize(new_total)));
    });

    connect(widgets.clear_btn, &QPushButton::clicked, dialog, [this, dialog]() {
        m_controller->clearQueue();
        dialog->reject();
    });
}

QCheckBox* AdvancedUninstallPanel::addBatchUninstallOptions(QDialog* dialog,
                                                            QVBoxLayout* layout) const {
    auto* restore_check = new QCheckBox(tr("Create single restore point before batch"), dialog);
    restore_check->setChecked(m_controller->autoRestorePoint());

    const bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restore_check->setEnabled(false);
        restore_check->setChecked(false);
        restore_check->setToolTip(tr("Requires administrator privileges."));
    }
    layout->addWidget(restore_check);

    auto* note_label =
        new QLabel(tr("Programs will be uninstalled sequentially. You may cancel "
                      "the batch at any time -- remaining programs will be skipped."),
                   dialog);
    note_label->setWordWrap(true);
    note_label->setStyleSheet(sak::ui::textColorStyle(sak::ui::kColorTextMuted));
    layout->addWidget(note_label);

    return restore_check;
}

QDialogButtonBox* AdvancedUninstallPanel::addBatchUninstallButtons(QDialog* dialog,
                                                                   QVBoxLayout* layout) const {
    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            dialog);
    auto* start_btn = button_box->button(QDialogButtonBox::Ok);
    start_btn->setText(tr("Start Batch Uninstall"));
    start_btn->setStyleSheet(ui::kDangerButtonStyle);
    layout->addWidget(button_box);
    return button_box;
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

    auto* header_label =
        new QLabel(tr("<b>Batch Uninstall Queue</b> -- %1 programs").arg(queue.size()), &dialog);
    layout->addWidget(header_label);

    // Queue list
    auto* queue_list = new QListWidget(&dialog);
    queue_list->setSelectionMode(QAbstractItemView::SingleSelection);

    qint64 total_size = 0;
    populateBatchUninstallQueueList(queue, queue_list, &total_size);
    layout->addWidget(queue_list, 1);

    // Queue actions
    auto* action_row = new QHBoxLayout();
    auto* remove_btn = new QPushButton(tr("Remove Selected"), &dialog);
    remove_btn->setEnabled(false);
    action_row->addWidget(remove_btn);

    auto* clear_btn = new QPushButton(tr("Clear Queue"), &dialog);
    action_row->addWidget(clear_btn);

    action_row->addStretch();

    auto* total_label = new QLabel(tr("Total size: %1").arg(formatSize(total_size)), &dialog);
    total_label->setStyleSheet(
        ui::fontWeightAndColorStyle(ui::kFontWeightBold, ui::kColorTextSecondary));
    action_row->addWidget(total_label);

    layout->addLayout(action_row);

    auto* restore_check = addBatchUninstallOptions(&dialog, layout);
    auto* button_box = addBatchUninstallButtons(&dialog, layout);

    wireBatchUninstallQueueActions({.queue_list = queue_list,
                                    .header_label = header_label,
                                    .total_label = total_label,
                                    .remove_btn = remove_btn,
                                    .clear_btn = clear_btn},
                                   &dialog);

    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() == QDialog::Accepted) {
        if (m_controller->queue().isEmpty()) {
            Q_EMIT statusMessage(tr("Batch queue is empty."), kDialogStatusTimeoutMs);
            return;
        }
        setOperationRunning(true);
        logMessage(QString("Starting batch uninstall of %1 programs...")
                       .arg(m_controller->queue().size()));
        m_controller->startBatchUninstall(restore_check->isChecked());
    }
}

// -- Program Properties Dialog -----------------------------------------------

void AdvancedUninstallPanel::populateProgramPropertiesForm(const ProgramInfo& program,
                                                           QWidget* scroll_widget,
                                                           QFormLayout* form_layout) const {
    // Every `value` below is a raw uninstall-registry string (name, publisher, install location,
    // uninstall command, ...), and HKCU is writable without elevation, so the value label must
    // show it verbatim instead of letting QLabel auto-detect markup inside it. Only `label` --
    // always a tr() literal from this function -- is allowed to carry markup.
    const auto add_row = [&](const QString& label, const QString& value) {
        if (value.isEmpty()) {
            return;
        }
        auto* value_label = sak::plainTextLabel(value, scroll_widget);
        value_label->setTextInteractionFlags(Qt::TextSelectableByMouse |
                                             Qt::TextSelectableByKeyboard);
        value_label->setWordWrap(true);
        form_layout->addRow(new QLabel(QString("<b>%1:</b>").arg(label), scroll_widget),
                            value_label);
    };

    if (!program.cachedImage.isNull()) {
        auto* icon_label = new QLabel(scroll_widget);
        icon_label->setPixmap(QPixmap::fromImage(program.cachedImage)
                                  .scaled(kProgramIconPreviewSize,
                                          kProgramIconPreviewSize,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation));
        form_layout->addRow(icon_label);
    }

    add_row(tr("Name"), program.displayName);
    add_row(tr("Publisher"), program.publisher);
    add_row(tr("Version"), program.displayVersion);
    add_row(tr("Install Date"), program.installDate);
    add_row(tr("Install Location"), program.installLocation);

    if (program.estimatedSizeKB > 0) {
        add_row(tr("Estimated Size"), formatSize(program.estimatedSizeKB * sak::kBytesPerKB));
    }

    add_row(tr("Source"), programSourceLabel(program.source));

    const QStringList flags = programFlagLabels(program);
    if (!flags.isEmpty()) {
        add_row(tr("Flags"), flags.join(", "));
    }

    form_layout->addRow(
        new QLabel(QString("<br><b>%1</b>").arg(tr("Technical Details")), scroll_widget));

    add_row(tr("Uninstall Command"), program.uninstallString);
    add_row(tr("Quiet Uninstall"), program.quietUninstallString);
    add_row(tr("Modify Path"), program.modifyPath);
    add_row(tr("Registry Key"), program.registryKeyPath);
    add_row(tr("Package Family"), program.packageFamilyName);
    add_row(tr("Package Full Name"), program.packageFullName);
}

void AdvancedUninstallPanel::showProgramProperties(const ProgramInfo& program) {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Program Properties -- %1").arg(program.displayName));
    dialog.setMinimumWidth(kProgramPropertiesMinWidth);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    // Scroll area for properties
    auto* scroll_area = new QScrollArea(&dialog);
    scroll_area->setWidgetResizable(true);
    auto* scroll_widget = new QWidget(scroll_area);
    auto* form_layout = new QFormLayout(scroll_widget);
    form_layout->setSpacing(ui::kSpacingSmall);
    form_layout->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    scroll_area->setWidget(scroll_widget);

    populateProgramPropertiesForm(program, scroll_widget, form_layout);

    layout->addWidget(scroll_area, 1);

    // Close button
    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    layout->addWidget(button_box);

    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    dialog.exec();
}

// -- Settings Dialog ---------------------------------------------------------

QCheckBox* AdvancedUninstallPanel::addSettingsSelectionGroup(QDialog* dialog,
                                                             QVBoxLayout* layout) const {
    auto* group = new QGroupBox(tr("Leftover Selection"), dialog);
    auto* group_layout = new QVBoxLayout(group);

    auto* select_all_check =
        new QCheckBox(tr("Select all leftovers by default (instead of safe only)"), dialog);
    select_all_check->setChecked(m_controller->selectAllByDefault());
    select_all_check->setToolTip(
        tr("When enabled, all leftover items are pre-selected after scanning. "
           "Otherwise, only items classified as Safe are selected."));
    group_layout->addWidget(select_all_check);

    layout->addWidget(group);
    return select_all_check;
}

QCheckBox* AdvancedUninstallPanel::addSettingsDeletionGroup(QDialog* dialog,
                                                            QVBoxLayout* layout) const {
    auto* group = new QGroupBox(tr("Deletion Behavior"), dialog);
    auto* group_layout = new QVBoxLayout(group);

    auto* recycle_bin_check =
        new QCheckBox(tr("Delete to Recycle Bin instead of permanent deletion"), dialog);
    recycle_bin_check->setChecked(m_controller->useRecycleBin());
    recycle_bin_check->setToolTip(
        tr("When enabled, files and folders are sent to the Recycle Bin, "
           "allowing recovery. Registry entries and services are always "
           "removed permanently."));
    group_layout->addWidget(recycle_bin_check);

    auto* recycle_bin_note =
        new QLabel(tr("Note: Registry keys, services, scheduled tasks, and firewall "
                      "rules are always removed permanently regardless of this setting."),
                   dialog);
    recycle_bin_note->setWordWrap(true);
    recycle_bin_note->setStyleSheet(
        sak::ui::textColorAndFontSizeStyle(sak::ui::kColorTextMuted, sak::ui::kFontSizeSmall));
    group_layout->addWidget(recycle_bin_note);

    layout->addWidget(group);
    return recycle_bin_check;
}

QCheckBox* AdvancedUninstallPanel::addSettingsRestorePointGroup(QDialog* dialog,
                                                                QVBoxLayout* layout) const {
    auto* group = new QGroupBox(tr("System Protection"), dialog);
    auto* group_layout = new QVBoxLayout(group);

    auto* restore_point_check = new QCheckBox(tr("Create a restore point before uninstall"),
                                              dialog);
    restore_point_check->setChecked(m_controller->autoRestorePoint());
    restore_point_check->setToolTip(
        tr("When enabled, a Windows System Restore point is created before "
           "running the uninstaller. Requires administrator privileges."));

    const bool elevated = RestorePointManager::isElevated();
    if (!elevated) {
        restore_point_check->setEnabled(false);
        restore_point_check->setChecked(false);
        restore_point_check->setToolTip(tr("Requires administrator privileges."));
    }

    group_layout->addWidget(restore_point_check);
    layout->addWidget(group);
    return restore_point_check;
}

void AdvancedUninstallPanel::addSettingsScanLevelGroup(QDialog* dialog,
                                                       QVBoxLayout* layout,
                                                       QRadioButton*& safe_radio,
                                                       QRadioButton*& moderate_radio,
                                                       QRadioButton*& advanced_radio) const {
    auto* group = new QGroupBox(tr("Default Scan Level"), dialog);
    auto* group_layout = new QVBoxLayout(group);

    safe_radio = new QRadioButton(tr("Safe -- Only obvious leftovers in known locations (fast)"),
                                  dialog);
    moderate_radio = new QRadioButton(
        tr("Moderate -- Extended scanning with pattern matching (recommended)"), dialog);
    advanced_radio =
        new QRadioButton(tr("Advanced -- Deep scan including services, tasks, firewall, "
                            "shell extensions"),
                         dialog);

    switch (m_controller->defaultScanLevel()) {
    case ScanLevel::Safe:
        safe_radio->setChecked(true);
        break;
    case ScanLevel::Moderate:
        moderate_radio->setChecked(true);
        break;
    case ScanLevel::Advanced:
        advanced_radio->setChecked(true);
        break;
    }

    group_layout->addWidget(safe_radio);
    group_layout->addWidget(moderate_radio);
    group_layout->addWidget(advanced_radio);
    layout->addWidget(group);
}

QCheckBox* AdvancedUninstallPanel::addSettingsDisplayGroup(QDialog* dialog,
                                                           QVBoxLayout* layout) const {
    auto* group = new QGroupBox(tr("Display"), dialog);
    auto* group_layout = new QVBoxLayout(group);

    auto* system_components_check = new QCheckBox(tr("Show system components in program list"),
                                                  dialog);
    system_components_check->setChecked(m_controller->showSystemComponents());
    system_components_check->setToolTip(
        tr("When enabled, programs marked as system components are shown "
           "in the program list. These are typically Windows components."));
    group_layout->addWidget(system_components_check);

    layout->addWidget(group);
    return system_components_check;
}

void AdvancedUninstallPanel::showSettingsDialog() {
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Advanced Uninstall Settings"));
    dialog.setMinimumWidth(kSettingsDialogMinWidth);

    auto* layout = new QVBoxLayout(&dialog);
    layout->setSpacing(ui::kSpacingLarge);

    auto* select_all_check = addSettingsSelectionGroup(&dialog, layout);
    auto* recycle_bin_check = addSettingsDeletionGroup(&dialog, layout);
    auto* restore_point_check = addSettingsRestorePointGroup(&dialog, layout);

    QRadioButton* safe_radio = nullptr;
    QRadioButton* moderate_radio = nullptr;
    QRadioButton* advanced_radio = nullptr;
    addSettingsScanLevelGroup(&dialog, layout, safe_radio, moderate_radio, advanced_radio);

    auto* system_components_check = addSettingsDisplayGroup(&dialog, layout);

    auto* button_box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel,
                                            &dialog);
    button_box->button(QDialogButtonBox::Ok)->setText(tr("Save Settings"));
    layout->addWidget(button_box);

    connect(button_box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(button_box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

    if (dialog.exec() != QDialog::Accepted) {
        return;
    }

    m_controller->setSelectAllByDefault(select_all_check->isChecked());
    m_controller->setUseRecycleBin(recycle_bin_check->isChecked());
    // Preserve the stored restore-point preference when unelevated: addSettingsRestorePointGroup
    // force-unchecked and disabled the box because restore points require admin, so its state does
    // NOT reflect the user's intent. Writing it would silently wipe a previously-enabled
    // preference just because Settings was opened without elevation.
    if (RestorePointManager::isElevated()) {
        m_controller->setAutoRestorePoint(restore_point_check->isChecked());
    }

    ScanLevel scan_level = ScanLevel::Safe;
    if ((advanced_radio != nullptr) && advanced_radio->isChecked()) {
        scan_level = ScanLevel::Advanced;
    }
    if ((moderate_radio != nullptr) && moderate_radio->isChecked()) {
        scan_level = ScanLevel::Moderate;
    }
    m_controller->setDefaultScanLevel(scan_level);

    m_controller->setShowSystemComponents(system_components_check->isChecked());
    m_controller->saveSettings();
    Q_EMIT statusMessage(tr("Settings saved."), kDialogStatusTimeoutMs);
}

}  // namespace sak
