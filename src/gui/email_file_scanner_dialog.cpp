// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_file_scanner_dialog.cpp
/// @brief Scans common Windows locations for PST/OST/MBOX files

#include "sak/email_file_scanner_dialog.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QTableWidget>
#include <QtConcurrent/QtConcurrent>
#include <QVBoxLayout>

namespace sak {

namespace {

// Result of scanning a single directory off the GUI thread.
struct ScannedFile {
    QString path;
    qint64 size_bytes{0};
    QString type;
};

QString classifySuffix(const QString& suffix) {
    if (suffix == QLatin1String("pst")) {
        return QStringLiteral("PST");
    }
    if (suffix == QLatin1String("ost")) {
        return QStringLiteral("OST");
    }
    return QStringLiteral("MBOX");
}

QVector<ScannedFile> scanPathsWorker(const QStringList& paths) {
    static const QStringList kFilters = {
        QStringLiteral("*.pst"),
        QStringLiteral("*.ost"),
        QStringLiteral("*.mbox"),
    };
    QVector<ScannedFile> results;
    results.reserve(64);

    auto harvest = [&](const QDir& dir) {
        const auto entries = dir.entryInfoList(kFilters, QDir::Files | QDir::Readable);
        for (const auto& entry : entries) {
            results.append(ScannedFile{
                entry.absoluteFilePath(), entry.size(), classifySuffix(entry.suffix().toLower())});
        }
    };

    for (const auto& path : paths) {
        QDir dir(path);
        if (!dir.exists()) {
            continue;
        }
        harvest(dir);
        // Recurse one level.
        const auto subdirs = dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable);
        for (const auto& subdir : subdirs) {
            harvest(QDir(subdir.absoluteFilePath()));
        }
    }
    return results;
}

}  // namespace

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
    connect(m_results_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        const bool has_selection = (row >= 0);
        m_open_button->setEnabled(has_selection);
        m_open_folder_button->setEnabled(has_selection);
    });
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

    m_open_folder_button = new QPushButton(tr("Open Containing Folder"), this);
    m_open_folder_button->setEnabled(false);
    m_open_folder_button->setStyleSheet(ui::kSecondaryButtonStyle);
    m_open_folder_button->setToolTip(tr("Open the folder containing the selected file"));
    connect(m_open_folder_button,
            &QPushButton::clicked,
            this,
            &EmailFileScannerDialog::onOpenContainingFolderClicked);
    button_row->addWidget(m_open_folder_button);

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
    m_progress_bar->setRange(0, 0);  // indeterminate while scanning
    m_scan_button->setEnabled(false);
    m_open_button->setEnabled(false);
    m_open_folder_button->setEnabled(false);
    m_status_label->setText(tr("Scanning..."));

    // Run the actual filesystem traversal off the GUI thread.  The previous
    // implementation paired synchronous `QDir::entryInfoList` calls with
    // `QApplication::processEvents()` to keep the UI "responsive", which
    // re-entered the event loop from a slot and could crash when a fresh
    // rescan was triggered mid-loop.  `QtConcurrent::run` + `QFutureWatcher`
    // is the idiomatic, crash-free replacement.
    auto* watcher = new QFutureWatcher<QVector<ScannedFile>>(this);
    connect(watcher, &QFutureWatcher<QVector<ScannedFile>>::finished, this, [this, watcher] {
        const auto results = watcher->result();
        watcher->deleteLater();

        // Batch-populate the table in a single pass so sort/repaint only run
        // once regardless of how many hits we found.
        m_results_table->setUpdatesEnabled(false);
        m_results_table->setSortingEnabled(false);
        m_results_table->setRowCount(results.size());
        for (int row = 0; row < results.size(); ++row) {
            const auto& r = results.at(row);
            m_results_table->setItem(row, 0, new QTableWidgetItem(r.path));
            m_results_table->setItem(row, 1, new QTableWidgetItem(r.type));
            QString size_str;
            if (r.size_bytes >= kBytesPerGB) {
                size_str = QStringLiteral("%1 GB").arg(
                    static_cast<double>(r.size_bytes) / kBytesPerGBf, 0, 'f', 2);
            } else if (r.size_bytes >= kBytesPerMB) {
                size_str = QStringLiteral("%1 MB").arg(
                    static_cast<double>(r.size_bytes) / kBytesPerMBf, 0, 'f', 1);
            } else {
                size_str = QStringLiteral("%1 KB").arg(
                    static_cast<double>(r.size_bytes) / kBytesPerKBf, 0, 'f', 0);
            }
            m_results_table->setItem(row, 2, new QTableWidgetItem(size_str));
        }
        m_results_table->setSortingEnabled(true);
        m_results_table->setUpdatesEnabled(true);
        m_files_found = results.size();

        m_progress_bar->setVisible(false);
        m_scan_button->setEnabled(true);
        const bool has_rows = (m_results_table->rowCount() > 0);
        m_open_button->setEnabled(has_rows);
        m_open_folder_button->setEnabled(has_rows);
        m_status_label->setText(tr("Found %1 email files").arg(m_files_found));
    });
    watcher->setFuture(QtConcurrent::run(scanPathsWorker, commonScanPaths()));
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

void EmailFileScannerDialog::onOpenContainingFolderClicked() {
    int row = m_results_table->currentRow();
    if (row < 0) {
        return;
    }
    auto* item = m_results_table->item(row, 0);
    if (item == nullptr) {
        return;
    }
    QFileInfo file_info(item->text());
    QDesktopServices::openUrl(QUrl::fromLocalFile(file_info.absolutePath()));
}

// ============================================================================
// Helpers
// ============================================================================

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
