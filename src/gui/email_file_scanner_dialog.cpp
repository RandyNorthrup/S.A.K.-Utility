// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_file_scanner_dialog.cpp
/// @brief Scans common Windows locations for PST/OST/MBOX files

#include "sak/email_file_scanner_dialog.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTableWidget>
#include <QVBoxLayout>

namespace sak {

// ============================================================================
// Construction
// ============================================================================

EmailFileScannerDialog::EmailFileScannerDialog(QWidget* parent) : QDialog(parent) {
    setWindowTitle(tr("Scan for Email Files"));
    setModal(true);
    resize(kDialogWidthXLarge, kDialogHeightLarge);
    setupUi();
}

EmailFileScannerDialog::~EmailFileScannerDialog() = default;

void EmailFileScannerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
    if (!m_has_scanned) {
        m_has_scanned = true;
        QMetaObject::invokeMethod(this,
                                  &EmailFileScannerDialog::onScanClicked,
                                  Qt::QueuedConnection);
    }
}

QString EmailFileScannerDialog::selectedFilePath() const {
    return m_selected_path;
}

// ============================================================================
// UI Setup
// ============================================================================

void EmailFileScannerDialog::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(
        ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge, ui::kMarginLarge);
    layout->setSpacing(ui::kSpacingDefault);

    auto* heading = new QLabel(tr("Scan common locations for PST, OST, and MBOX files"), this);
    heading->setStyleSheet(QStringLiteral("font-size: %1pt; font-weight: 600; "
                                          "color: %2;")
                               .arg(ui::kFontSizeSection)
                               .arg(ui::kColorTextHeading));
    layout->addWidget(heading);

    // Results table
    m_results_table = new QTableWidget(this);
    m_results_table->setColumnCount(3);
    m_results_table->setHorizontalHeaderLabels({tr("File Path"), tr("Type"), tr("Size")});
    m_results_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_results_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_results_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_results_table->horizontalHeader()->setStretchLastSection(false);
    m_results_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_results_table->setColumnWidth(1, 80);
    m_results_table->setColumnWidth(2, 100);
    m_results_table->verticalHeader()->setVisible(false);
    connect(m_results_table,
            &QTableWidget::cellDoubleClicked,
            this,
            &EmailFileScannerDialog::onFileDoubleClicked);
    layout->addWidget(m_results_table, 1);

    // Status + progress
    auto* status_row = new QHBoxLayout();
    m_status_label = new QLabel(tr("Scanning for email files..."), this);
    m_status_label->setStyleSheet(QStringLiteral("color: %1;").arg(ui::kColorTextMuted));
    status_row->addWidget(m_status_label, 1);

    m_progress_bar = new QProgressBar(this);
    m_progress_bar->setMaximumWidth(200);
    m_progress_bar->setVisible(false);
    status_row->addWidget(m_progress_bar);
    layout->addLayout(status_row);

    // Button row
    auto* button_row = new QHBoxLayout();

    m_scan_button = new QPushButton(tr("Rescan"), this);
    m_scan_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(m_scan_button, &QPushButton::clicked, this, &EmailFileScannerDialog::onScanClicked);
    button_row->addWidget(m_scan_button);

    button_row->addStretch();

    m_open_button = new QPushButton(tr("Open Selected"), this);
    m_open_button->setEnabled(false);
    m_open_button->setStyleSheet(ui::kPrimaryButtonStyle);
    connect(m_open_button, &QPushButton::clicked, this, &EmailFileScannerDialog::onOpenClicked);
    button_row->addWidget(m_open_button);

    m_cancel_button = new QPushButton(tr("Cancel"), this);
    m_cancel_button->setStyleSheet(ui::kSecondaryButtonStyle);
    connect(m_cancel_button, &QPushButton::clicked, this, &QDialog::reject);
    button_row->addWidget(m_cancel_button);

    layout->addLayout(button_row);
}

// ============================================================================
// Slots
// ============================================================================

void EmailFileScannerDialog::onScanClicked() {
    m_results_table->setRowCount(0);
    m_files_found = 0;
    m_progress_bar->setVisible(true);
    m_progress_bar->setRange(0, 0);
    m_scan_button->setEnabled(false);
    m_status_label->setText(tr("Scanning..."));
    QApplication::processEvents();

    const auto paths = commonScanPaths();
    m_progress_bar->setRange(0, paths.size());

    for (int idx = 0; idx < paths.size(); ++idx) {
        m_progress_bar->setValue(idx);
        m_status_label->setText(tr("Scanning: %1").arg(paths.at(idx)));
        QApplication::processEvents();
        scanDirectory(paths.at(idx));
    }

    m_progress_bar->setVisible(false);
    m_scan_button->setEnabled(true);
    m_open_button->setEnabled(m_results_table->rowCount() > 0);
    m_status_label->setText(tr("Found %1 email files").arg(m_files_found));
}

void EmailFileScannerDialog::onOpenClicked() {
    int row = m_results_table->currentRow();
    if (row < 0) {
        return;
    }
    auto* item = m_results_table->item(row, 0);
    if (item == nullptr) {
        return;
    }
    m_selected_path = item->text();
    accept();
}

void EmailFileScannerDialog::onFileDoubleClicked(int row, int /*column*/) {
    auto* item = m_results_table->item(row, 0);
    if (item == nullptr) {
        return;
    }
    m_selected_path = item->text();
    accept();
}

// ============================================================================
// Helpers
// ============================================================================

void EmailFileScannerDialog::addFoundFile(const QString& path,
                                          qint64 size_bytes,
                                          const QString& type) {
    int row = m_results_table->rowCount();
    m_results_table->insertRow(row);
    m_results_table->setItem(row, 0, new QTableWidgetItem(path));
    m_results_table->setItem(row, 1, new QTableWidgetItem(type));

    QString size_str;
    if (size_bytes >= kBytesPerGB) {
        size_str =
            QStringLiteral("%1 GB").arg(static_cast<double>(size_bytes) / kBytesPerGBf, 0, 'f', 2);
    } else if (size_bytes >= kBytesPerMB) {
        size_str =
            QStringLiteral("%1 MB").arg(static_cast<double>(size_bytes) / kBytesPerMBf, 0, 'f', 1);
    } else {
        size_str =
            QStringLiteral("%1 KB").arg(static_cast<double>(size_bytes) / kBytesPerKBf, 0, 'f', 0);
    }
    m_results_table->setItem(row, 2, new QTableWidgetItem(size_str));
    ++m_files_found;
}

void EmailFileScannerDialog::scanDirectory(const QString& dir_path) {
    QDir dir(dir_path);
    if (!dir.exists()) {
        return;
    }

    static const QStringList kFilters = {
        QStringLiteral("*.pst"),
        QStringLiteral("*.ost"),
        QStringLiteral("*.mbox"),
    };

    // Scan immediate directory
    const auto entries = dir.entryInfoList(kFilters, QDir::Files | QDir::Readable);
    for (const auto& entry : entries) {
        QString suffix = entry.suffix().toLower();
        QString type;
        if (suffix == QLatin1String("pst")) {
            type = QStringLiteral("PST");
        } else if (suffix == QLatin1String("ost")) {
            type = QStringLiteral("OST");
        } else {
            type = QStringLiteral("MBOX");
        }
        addFoundFile(entry.absoluteFilePath(), entry.size(), type);
    }

    // Recurse one level into subdirectories
    const auto subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
    for (const auto& subdir : subdirs) {
        QDir sub(subdir.absoluteFilePath());
        const auto sub_entries = sub.entryInfoList(kFilters, QDir::Files | QDir::Readable);
        for (const auto& entry : sub_entries) {
            QString suffix = entry.suffix().toLower();
            QString type;
            if (suffix == QLatin1String("pst")) {
                type = QStringLiteral("PST");
            } else if (suffix == QLatin1String("ost")) {
                type = QStringLiteral("OST");
            } else {
                type = QStringLiteral("MBOX");
            }
            addFoundFile(entry.absoluteFilePath(), entry.size(), type);
        }
    }
}

// static
QStringList EmailFileScannerDialog::commonScanPaths() {
    QStringList paths;
    QString home = QDir::homePath();

    // Outlook default data locations
    paths << home + QStringLiteral("/AppData/Local/Microsoft/Outlook");
    paths << home + QStringLiteral("/Documents/Outlook Files");
    paths << home + QStringLiteral("/Documents");
    paths << home + QStringLiteral("/Desktop");
    paths << home + QStringLiteral("/Downloads");

    // Thunderbird profile directory
    paths << home + QStringLiteral("/AppData/Roaming/Thunderbird/Profiles");

    // Windows Mail / eM Client
    paths << home + QStringLiteral("/AppData/Roaming/eM Client");

    // Common backup locations on all fixed drives
    for (const auto& storage : QStorageInfo::mountedVolumes()) {
        if (!storage.isReady() || storage.isReadOnly()) {
            continue;
        }
        QString root = storage.rootPath();
        if (root == QLatin1String("C:/")) {
            continue;  // Already covered by home paths above
        }
        paths << root + QStringLiteral("Backups");
        paths << root + QStringLiteral("Email Backups");
    }

    return paths;
}

}  // namespace sak
