// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file ost_converter_widget.h
/// @brief UI widget for the OST/PST Converter tab

#pragma once

#include "sak/ost_converter_types.h"

#include <QWidget>

#include <memory>
#include <type_traits>

class QCheckBox;
class QComboBox;
class QDateEdit;
class QGroupBox;
class QLabel;
class QLineEdit;

class QPushButton;
class QSpinBox;
class QTableWidget;
class QVBoxLayout;

namespace sak {

class OstConverterController;

/// @brief Main UI for the OST/PST Converter tab
class OstConverterWidget : public QWidget {
    Q_OBJECT

public:
    explicit OstConverterWidget(QWidget* parent = nullptr);
    ~OstConverterWidget() override;

    // Non-copyable, non-movable
    OstConverterWidget(const OstConverterWidget&) = delete;
    OstConverterWidget& operator=(const OstConverterWidget&) = delete;
    OstConverterWidget(OstConverterWidget&&) = delete;
    OstConverterWidget& operator=(OstConverterWidget&&) = delete;

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);

private Q_SLOTS:
    // File queue actions
    void onAddFilesClicked();
    void onRemoveFileClicked();
    void onClearQueueClicked();

    // Conversion control
    void onConvertClicked();
    void onCancelClicked();

    // Controller signals
    void onFileAdded(int index, sak::OstConversionJob job);
    void onFileRemoved(int index);
    void onQueueCleared();
    void onConversionStarted(int total_files);
    void onFileConversionStarted(int file_index);
    void onFileProgressUpdated(int file_index,
                               int items_done,
                               int items_total,
                               QString current_folder);
    void onFileConversionComplete(int file_index, sak::OstConversionResult result);
    void onAllConversionsComplete(sak::OstConversionBatchResult result);
    void onErrorOccurred(int file_index, QString message);

    // Format change
    void onFormatChanged(int index);

    // Report
    void onViewReportClicked();

private:
    void setupUi();
    void connectController();
    void setConvertingState(bool converting);

    // Sub-builders
    QWidget* createFileQueueSection();
    QWidget* createOutputSettingsSection();
    QWidget* createFilterSection();
    QWidget* createRecoverySection();
    QWidget* createImapSection();
    QWidget* createButtonBar();

    // Helpers
    [[nodiscard]] OstConversionConfig buildConfig() const;
    void updateQueueRow(int row, const OstConversionJob& job);
    [[nodiscard]] static QString formatBytes(qint64 bytes);
    [[nodiscard]] static QString statusLabel(OstConversionJob::Status status);
    void loadSettings();
    void saveSettings();

    // Controller
    std::unique_ptr<OstConverterController> m_controller;

    // -- Source Files Section ---------------------------------------------
    QTableWidget* m_queue_table{nullptr};
    QPushButton* m_add_files_button{nullptr};
    QPushButton* m_remove_button{nullptr};
    QPushButton* m_clear_button{nullptr};

    // -- Output Settings Section ------------------------------------------
    QComboBox* m_format_combo{nullptr};
    QLineEdit* m_output_dir_edit{nullptr};
    QPushButton* m_browse_button{nullptr};
    QCheckBox* m_split_check{nullptr};
    QComboBox* m_split_size_combo{nullptr};
    QCheckBox* m_preserve_folders_check{nullptr};
    QCheckBox* m_prefix_date_check{nullptr};
    QSpinBox* m_threads_spin{nullptr};

    // -- Filter Section ---------------------------------------------------
    QGroupBox* m_filter_group{nullptr};
    QDateEdit* m_date_from_edit{nullptr};
    QDateEdit* m_date_to_edit{nullptr};
    QCheckBox* m_date_filter_check{nullptr};
    QLineEdit* m_sender_filter_edit{nullptr};
    QLineEdit* m_recipient_filter_edit{nullptr};

    // -- Recovery Options -------------------------------------------------
    QGroupBox* m_recovery_group{nullptr};
    QCheckBox* m_recover_deleted_check{nullptr};
    QCheckBox* m_deep_recovery_check{nullptr};
    QCheckBox* m_skip_corrupt_check{nullptr};

    // -- IMAP Upload Section ----------------------------------------------
    QGroupBox* m_imap_group{nullptr};
    QLineEdit* m_imap_host_edit{nullptr};
    QSpinBox* m_imap_port_spin{nullptr};
    QCheckBox* m_imap_ssl_check{nullptr};
    QComboBox* m_imap_auth_combo{nullptr};
    QLineEdit* m_imap_user_edit{nullptr};
    QLineEdit* m_imap_password_edit{nullptr};

    // -- Progress Tracking ------------------------------------------------
    int m_total_files{0};
    int m_files_done{0};

    // -- Action Buttons ---------------------------------------------------
    QPushButton* m_convert_button{nullptr};
    QPushButton* m_cancel_button{nullptr};
    QPushButton* m_view_report_button{nullptr};
};

// Compile-Time Invariants
static_assert(std::is_base_of_v<QWidget, OstConverterWidget>,
              "OstConverterWidget must inherit QWidget.");
static_assert(!std::is_copy_constructible_v<OstConverterWidget>,
              "OstConverterWidget must not be copy-constructible.");

}  // namespace sak
