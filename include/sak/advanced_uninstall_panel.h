// Copyright (c) 2025-2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file advanced_uninstall_panel.h
/// @brief Main UI panel for Advanced Uninstall — Revo Uninstaller-style
///        deep application removal with leftover scanning and cleanup

#pragma once

#include "sak/advanced_uninstall_types.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QSplitter>
#include <QTableWidget>
#include <QWidget>

#include <memory>
#include <type_traits>

class QVBoxLayout;
class QHBoxLayout;
class QDialog;
class QCheckBox;
class QRadioButton;
class QListWidget;
class QFormLayout;
class QDialogButtonBox;

namespace sak {

class AdvancedUninstallController;
class LogToggleSwitch;

/// @brief Advanced Uninstall Panel — deep program removal with leftover scanning
///
/// Layout: Toolbar → Program List Table → Leftover Results Table → Status Bar
///
/// Supports standard uninstall, forced uninstall, UWP removal, batch
/// uninstall, restore point creation, and multi-level leftover scanning.
class AdvancedUninstallPanel : public QWidget {
    Q_OBJECT

public:
    explicit AdvancedUninstallPanel(QWidget* parent = nullptr);
    ~AdvancedUninstallPanel() override;

    AdvancedUninstallPanel(const AdvancedUninstallPanel&) = delete;
    AdvancedUninstallPanel& operator=(const AdvancedUninstallPanel&) = delete;
    AdvancedUninstallPanel(AdvancedUninstallPanel&&) = delete;
    AdvancedUninstallPanel& operator=(AdvancedUninstallPanel&&) = delete;

    /// @brief Access the log toggle switch for MainWindow connection
    [[nodiscard]] LogToggleSwitch* logToggle() const { return m_log_toggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdate(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    // Toolbar actions
    void onRefreshClicked();
    void onUninstallClicked();
    void onForcedUninstallClicked();
    void onBatchUninstallClicked();
    void onSearchTextChanged(const QString& text);
    void onViewFilterChanged(int index);

    // Controller signals
    void onEnumerationStarted();
    void onEnumerationProgress(int current, int total);
    void onEnumerationFinished(QVector<sak::ProgramInfo> programs);
    void onEnumerationFailed(const QString& error);

    void onUninstallStarted(const QString& programName);
    void onUninstallProgress(int percent, const QString& phase);
    void onLeftoverScanFinished(QVector<sak::LeftoverItem> leftovers);
    void onUninstallFinished(sak::UninstallReport report);
    void onUninstallFailed(const QString& error);
    void onUninstallCancelled();

    void onCleanupStarted(int totalItems);
    void onItemCleaned(const QString& path, bool success);
    void onCleanupFinished(int succeeded, int failed, qint64 bytesRecovered);
    void onRebootPendingItems(QStringList paths);

    // Program table
    void onProgramSelectionChanged();
    void onProgramDoubleClicked(int row, int column);
    void onProgramContextMenu(const QPoint& pos);

    // Leftover table
    void onLeftoverSelectionChanged();
    void onSelectAll();
    void onSelectAllSafe();
    void onDeselectAll();
    void onDeleteSelectedLeftovers();

private:
    // ── UI Setup ──
    void setupUi();
    void createToolbar(QVBoxLayout* layout);
    void createProgramTable(QVBoxLayout* layout);
    void createLeftoverSection(QVBoxLayout* layout);
    void createStatusBar(QVBoxLayout* layout);

    // ── Context Menu Actions ──
    void contextUninstall();
    void contextForcedUninstall();
    void contextAddToQueue();
    void contextOpenInstallLocation();
    void contextCopyProgramName();
    void contextCopyUninstallCommand();
    void contextShowProperties();
    void contextRemoveRegistryEntry();

    // ── Dialog Helpers ──
    void showUninstallConfirmation(const ProgramInfo& program);
    void showForcedUninstallDialog(const ProgramInfo& program);
    void showBatchUninstallDialog();
    void showProgramProperties(const ProgramInfo& program);
    void showSettingsDialog();

    // Dialog helper builders (split for TigerStyle function-length)
    void populateBatchUninstallQueueList(const QVector<UninstallQueueItem>& queue,
                                         QListWidget* queueList,
                                         qint64* totalBytesOut) const;
    void wireBatchUninstallQueueActions(QListWidget* queueList,
                                        QLabel* headerLabel,
                                        QLabel* totalLabel,
                                        QPushButton* removeBtn,
                                        QPushButton* clearBtn,
                                        QDialog* dialog);

    QCheckBox* addBatchUninstallOptions(QDialog* dialog, QVBoxLayout* layout) const;
    QDialogButtonBox* addBatchUninstallButtons(QDialog* dialog, QVBoxLayout* layout) const;

    void populateProgramPropertiesForm(const ProgramInfo& program,
                                       QWidget* scrollWidget,
                                       QFormLayout* formLayout) const;

    QCheckBox* addSettingsSelectionGroup(QDialog* dialog, QVBoxLayout* layout) const;
    QCheckBox* addSettingsDeletionGroup(QDialog* dialog, QVBoxLayout* layout) const;
    QCheckBox* addSettingsRestorePointGroup(QDialog* dialog, QVBoxLayout* layout) const;
    void addSettingsScanLevelGroup(QDialog* dialog,
                                   QVBoxLayout* layout,
                                   QRadioButton*& safeRadio,
                                   QRadioButton*& moderateRadio,
                                   QRadioButton*& advancedRadio) const;
    QCheckBox* addSettingsDisplayGroup(QDialog* dialog, QVBoxLayout* layout) const;

    // ── Table Population ──
    void populateProgramTable(const QVector<ProgramInfo>& programs);
    void populateLeftoverTable(const QVector<LeftoverItem>& leftovers);
    void populateLeftoverRow(int row, const LeftoverItem& item);
    QString leftoverTypeText(LeftoverItem::Type type) const;
    void clearLeftoverTable();

    // ── Filtering & Sorting ──
    void applyFilter();
    [[nodiscard]] bool matchesFilter(const ProgramInfo& program) const;
    [[nodiscard]] QString formatSize(qint64 bytes) const;

    // ── Utility ──
    void logMessage(const QString& message);
    void setOperationRunning(bool running);
    [[nodiscard]] ProgramInfo selectedProgram() const;
    [[nodiscard]] QVector<ProgramInfo> selectedPrograms() const;
    [[nodiscard]] QVector<LeftoverItem> selectedLeftovers() const;
    void updateStatusCounts();

    // ── Controller ──
    std::unique_ptr<AdvancedUninstallController> m_controller;

    // ── Toolbar Widgets ──
    QLineEdit* m_search_edit{nullptr};
    QComboBox* m_view_filter_combo{nullptr};
    QPushButton* m_refresh_button{nullptr};
    QPushButton* m_uninstall_button{nullptr};
    QPushButton* m_forced_uninstall_button{nullptr};
    QPushButton* m_batch_button{nullptr};
    QPushButton* m_settings_button{nullptr};

    // ── Program Table ──
    QTableWidget* m_program_table{nullptr};
    QLabel* m_program_count_label{nullptr};
    QLabel* m_total_size_label{nullptr};

    // ── Leftover Section ──
    QWidget* m_leftover_section{nullptr};
    QTableWidget* m_leftover_table{nullptr};
    QLabel* m_leftover_header_label{nullptr};
    QLabel* m_leftover_count_label{nullptr};
    QPushButton* m_select_all_button{nullptr};
    QPushButton* m_select_safe_button{nullptr};
    QPushButton* m_deselect_all_button{nullptr};
    QPushButton* m_delete_selected_button{nullptr};

    // ── Status Bar ──
    int m_cleanup_progress{0};
    int m_cleanup_total{0};

    // ── Log toggle ──
    LogToggleSwitch* m_log_toggle{nullptr};

    // ── Data ──
    QVector<ProgramInfo> m_allPrograms;        ///< Full unfiltered list
    QVector<ProgramInfo> m_filteredPrograms;   ///< Currently displayed programs
    QVector<LeftoverItem> m_currentLeftovers;  ///< Current leftover scan results
    QString m_searchFilter;
    ViewFilter m_viewFilter = ViewFilter::All;

    // ── Table Column Indices ──
    static constexpr int kColCheck = 0;
    static constexpr int kColIcon = 1;
    static constexpr int kColName = 2;
    static constexpr int kColPublisher = 3;
    static constexpr int kColVersion = 4;
    static constexpr int kColSize = 5;
    static constexpr int kColDate = 6;
    static constexpr int kProgramColumnCount = 7;

    static constexpr int kLeftoverColCheck = 0;
    static constexpr int kLeftoverColRisk = 1;
    static constexpr int kLeftoverColType = 2;
    static constexpr int kLeftoverColPath = 3;
    static constexpr int kLeftoverColSize = 4;
    static constexpr int kLeftoverColumnCount = 5;
};

// ── Compile-Time Invariants ─────────────────────────────────────────────────

static_assert(std::is_base_of_v<QWidget, AdvancedUninstallPanel>,
              "AdvancedUninstallPanel must inherit QWidget.");
static_assert(!std::is_copy_constructible_v<AdvancedUninstallPanel>,
              "AdvancedUninstallPanel must not be copy-constructible.");

}  // namespace sak
