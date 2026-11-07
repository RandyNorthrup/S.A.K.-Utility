// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file email_file_scanner_dialog.h
/// @brief Dialog to scan for PST/OST/MBOX files in common locations

#pragma once

#include <QDialog>
#include <QShowEvent>

class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QVBoxLayout;
class EmailFileScannerDialogTests;  // unit-test friend (global namespace)

namespace sak {

/// @brief Scans common locations for email data files and lets
///        the user pick one to open.
class EmailFileScannerDialog : public QDialog {
    Q_OBJECT

    friend class ::EmailFileScannerDialogTests;

public:
    explicit EmailFileScannerDialog(QWidget* parent = nullptr);
    ~EmailFileScannerDialog() override;

    /// The path the user chose (empty if cancelled)
    [[nodiscard]] QString selectedFilePath() const;

    /// Mailboxes under a Thunderbird Profiles folder. Thunderbird keeps each mailbox as a file with
    /// no extension, in Mail or ImapMail account folders and in .sbd subfolders of those.
    [[nodiscard]] static QStringList thunderbirdMailboxes(const QString& profiles_dir);

    /// The file patterns a scan lists, besides Thunderbird's mailboxes with no extension.
    [[nodiscard]] static QStringList mailFilePatterns();

    /// @brief A found file's size as the results table shows it: the app's one byte format
    ///        ("60.0 KB", as Email Inspector and the OST converter show it; review 3, F38).
    [[nodiscard]] static QString sizeText(qint64 bytes);

private Q_SLOTS:
    void onScanClicked();
    void onOpenClicked();
    void onOpenContainingFolderClicked();
    void onFileDoubleClicked(int row, int column);

private:
    void setupUi();
    void setupResultsTable(QVBoxLayout* layout);
    void setupStatusRow(QVBoxLayout* layout);
    void setupButtonRow(QVBoxLayout* layout);
    void showEvent(QShowEvent* event) override;
    void showScanFailure(const QString& message);
    [[nodiscard]] static QStringList commonScanPaths();

    QTableWidget* m_results_table{nullptr};
    QPushButton* m_scan_button{nullptr};
    QPushButton* m_open_button{nullptr};
    QPushButton* m_open_folder_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    QLabel* m_status_label{nullptr};
    QProgressBar* m_progress_bar{nullptr};
    QString m_selected_path;
    int m_files_found{0};
    bool m_has_scanned{false};
};

}  // namespace sak
