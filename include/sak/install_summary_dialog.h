// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_installation_worker.h"

#include <QDialog>

class QLabel;
class QTableWidget;
class QPushButton;

namespace sak {

/// @brief Modal summary dialog shown after a software installation run completes.
///
/// Displays per-package results (success/failed/skipped) with overall statistics.
class InstallSummaryDialog : public QDialog {
    Q_OBJECT

public:
    explicit InstallSummaryDialog(const AppInstallationWorker::Stats& stats,
                                  const QVector<MigrationJob>& jobs,
                                  QWidget* parent = nullptr);
    ~InstallSummaryDialog() override = default;

    InstallSummaryDialog(const InstallSummaryDialog&) = delete;
    InstallSummaryDialog& operator=(const InstallSummaryDialog&) = delete;

private:
    void setupUi(const AppInstallationWorker::Stats& stats, const QVector<MigrationJob>& jobs);
    void populateJobTable(const QVector<MigrationJob>& jobs);

    QLabel* m_summary_label{nullptr};
    QTableWidget* m_job_table{nullptr};
    QPushButton* m_close_button{nullptr};
};

}  // namespace sak
