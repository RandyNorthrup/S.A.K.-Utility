// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/install_summary_dialog.h"

#include "sak/layout_constants.h"
#include "sak/style_constants.h"

#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

namespace sak {

namespace {
constexpr int kDialogMinWidth = 520;
constexpr int kDialogMinHeight = 380;
constexpr int kPackageColumnWidth = 180;
constexpr int kStatusColumnWidth = 90;
constexpr int kDurationColumnWidth = 80;

enum JobColumn {
    ColPackage = 0,
    ColStatus,
    ColDuration,
    ColError,
    ColCount
};
}  // namespace

InstallSummaryDialog::InstallSummaryDialog(const AppInstallationWorker::Stats& stats,
                                           const QVector<MigrationJob>& jobs,
                                           QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(tr("Installation Summary"));
    setMinimumSize(kDialogMinWidth, kDialogMinHeight);
    setupUi(stats, jobs);
    populateJobTable(jobs);
}

void InstallSummaryDialog::setupUi(const AppInstallationWorker::Stats& stats,
                                   const QVector<MigrationJob>& /*jobs*/) {
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(ui::kSpacingDefault);
    layout->setContentsMargins(
        ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium, ui::kMarginMedium);

    // Summary headline
    QString headline;
    if (stats.failed == 0 && stats.cancelled == 0) {
        headline = tr("All %1 packages installed successfully.").arg(stats.success);
    } else {
        headline = tr("%1 succeeded, %2 failed, %3 skipped, %4 cancelled")
                       .arg(stats.success)
                       .arg(stats.failed)
                       .arg(stats.skipped)
                       .arg(stats.cancelled);
    }

    m_summary_label = new QLabel(headline, this);
    m_summary_label->setWordWrap(true);
    m_summary_label->setStyleSheet(
        ui::fontSizeWeightColorStyle(ui::kFontSizeSection,
                                     ui::kFontWeightBold,
                                     stats.failed > 0 ? QString::fromLatin1(ui::kColorError)
                                                      : QString::fromLatin1(ui::kColorSuccess)));
    layout->addWidget(m_summary_label);

    // Per-package table
    m_job_table = new QTableWidget(this);
    m_job_table->setColumnCount(ColCount);
    m_job_table->setHorizontalHeaderLabels(
        {tr("Package"), tr("Status"), tr("Duration"), tr("Error")});
    m_job_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_job_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_job_table->horizontalHeader()->setStretchLastSection(true);
    m_job_table->verticalHeader()->setVisible(false);
    m_job_table->setColumnWidth(ColPackage, kPackageColumnWidth);
    m_job_table->setColumnWidth(ColStatus, kStatusColumnWidth);
    m_job_table->setColumnWidth(ColDuration, kDurationColumnWidth);
    layout->addWidget(m_job_table, 1);

    // Close button
    m_close_button = new QPushButton(tr("Close"), this);
    m_close_button->setStyleSheet(ui::kPrimaryButtonStyle);
    m_close_button->setDefault(true);
    connect(m_close_button, &QPushButton::clicked, this, &QDialog::accept);

    auto* button_row = new QHBoxLayout();
    button_row->addStretch();
    button_row->addWidget(m_close_button);
    layout->addLayout(button_row);
}

void InstallSummaryDialog::populateJobTable(const QVector<MigrationJob>& jobs) {
    m_job_table->setSortingEnabled(false);
    m_job_table->setUpdatesEnabled(false);
    const QSignalBlocker blocker(m_job_table);
    const int count = jobs.size();
    m_job_table->setRowCount(count);
    for (int row = 0; row < count; ++row) {
        const auto& job = jobs.at(row);

        // Package name (prefer display name, fallback to ID)
        QString display_name = job.appName.isEmpty() ? job.packageId : job.appName;
        m_job_table->setItem(row, ColPackage, new QTableWidgetItem(display_name));

        // Status with color
        QString status_text;
        QString status_color;
        switch (job.status) {
        case MigrationStatus::Success:
            status_text = tr("Success");
            status_color = ui::kColorSuccess;
            break;
        case MigrationStatus::Failed:
            status_text = tr("Failed");
            status_color = ui::kColorError;
            break;
        case MigrationStatus::Skipped:
            status_text = tr("Skipped");
            status_color = ui::kColorWarning;
            break;
        case MigrationStatus::Cancelled:
            status_text = tr("Cancelled");
            status_color = ui::kColorTextMuted;
            break;
        default:
            status_text = tr("Pending");
            status_color = ui::kColorTextMuted;
            break;
        }
        auto* status_item = new QTableWidgetItem(status_text);
        status_item->setForeground(QColor(status_color));
        m_job_table->setItem(row, ColStatus, status_item);

        // Duration
        QString duration_text;
        if (job.startTime.isValid() && job.endTime.isValid()) {
            qint64 elapsed_sec = job.startTime.secsTo(job.endTime);
            if (elapsed_sec < kSecondsPerMinute) {
                duration_text = tr("%1s").arg(elapsed_sec);
            } else {
                duration_text = tr("%1m %2s")
                                    .arg(elapsed_sec / kSecondsPerMinute)
                                    .arg(elapsed_sec % kSecondsPerMinute);
            }
        }
        m_job_table->setItem(row, ColDuration, new QTableWidgetItem(duration_text));

        // Error message
        m_job_table->setItem(row, ColError, new QTableWidgetItem(job.errorMessage));
    }
    m_job_table->setUpdatesEnabled(true);
    m_job_table->setSortingEnabled(true);
}

}  // namespace sak
