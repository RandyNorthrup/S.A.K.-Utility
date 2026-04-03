// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QComboBox>
#include <QFuture>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QIcon>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardItemModel>
#include <QTableView>
#include <QTabWidget>
#include <QWidget>

#include <atomic>
#include <memory>

class QVBoxLayout;

namespace sak {

class ChocolateyManager;
class AppInstallationWorker;
class MigrationReport;
class LogToggleSwitch;
class OfflineDeploymentWorker;
class PackageListManager;

/**
 * @brief App Installation Panel (formerly App Migration)
 *
 * Provides a Chocolatey package manager UI for searching, browsing,
 * and installing software packages from the bundled Chocolatey repository.
 *
 * Features:
 * - Search Chocolatey package repository
 * - Category-based browsing
 * - Add packages to install queue
 * - Sequential installation with progress tracking
 * - Offline deployment: build internalized package bundles
 * - Install from local offline bundles
 * - Curated preset package lists
 * - Real-time log output
 *
 * The panel uses a tab layout:
 *   Tab 0 — Online Install (search, queue, install via Chocolatey)
 *   Tab 1 — Offline Deploy (build bundles, install from bundles, presets)
 *
 * Thread-Safety: UI updates occur on main thread.
 * Search and install operations run on background threads.
 */
class AppInstallationPanel : public QWidget {
    Q_OBJECT

public:
    /**
     * @brief Package entry for queue display
     */
    struct QueueEntry {
        QString package_id;
        QString version;
        QString publisher;
    };

    explicit AppInstallationPanel(QWidget* parent = nullptr);
    ~AppInstallationPanel() override;

    // Disable copy and move
    AppInstallationPanel(const AppInstallationPanel&) = delete;
    AppInstallationPanel& operator=(const AppInstallationPanel&) = delete;
    AppInstallationPanel(AppInstallationPanel&&) = delete;
    AppInstallationPanel& operator=(AppInstallationPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdated(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    /** @brief Execute a Chocolatey package search with the current query */
    void onSearch();
    /** @brief Add the selected search result to the install queue */
    void onAddToQueue();
    /** @brief Remove the selected entry from the install queue */
    void onRemoveFromQueue();
    /** @brief Clear all entries from the install queue */
    void onClearQueue();
    /** @brief Begin sequential installation of all queued packages */
    void onInstallAll();
    /** @brief Abort the running installation sequence */
    void onCancelInstall();
    /** @brief Load a preset package list into the online install queue */
    void onOnlinePresetSelected(int index);

    // ------ Offline Deployment Slots ------

    /** @brief Load a preset package list into the offline deploy list */
    void onPresetSelected(int index);
    /** @brief Search Chocolatey packages for the offline deploy list */
    void onOfflineSearch();
    /** @brief Add selected search result to the offline deploy list */
    void onAddToOfflineList();
    /** @brief Remove selected packages from the offline deploy list */
    void onRemoveFromOfflineList();
    /** @brief Clear all entries from the offline deploy list */
    void onClearOfflineList();
    /** @brief Build an internalized deployment bundle from current list */
    void onBuildBundle();
    /** @brief Install packages from a local deployment bundle */
    void onInstallFromBundle();
    /** @brief Direct-download .nupkg files without internalization */
    void onDirectDownload();
    /** @brief Cancel the running offline deployment operation */
    void onCancelOfflineOperation();
    /** @brief Save current offline list to a JSON file */
    void onSaveOfflineList();
    /** @brief Load an offline list from a JSON file */
    void onLoadOfflineList();

private:
    /** @brief Build the panel layout and child widgets */
    void setupUi();
    /** @brief Build the search bar section */
    void setupUi_searchBar(QVBoxLayout* layout);
    /** @brief Build the package results table panel */
    void setupUi_packageTable(QHBoxLayout* sideBySide);
    /** @brief Build the install queue section panel */
    void setupUi_queueSection(QHBoxLayout* sideBySide);
    /** @brief Build the installation actions section */
    void setupUi_installActions(QVBoxLayout* layout);
    /** @brief Build the bottom bar with log toggle */
    void setupUi_bottomBar(QVBoxLayout* layout);
    /** @brief Build the offline deployment tab content */
    void setupUi_offlineTab(QTabWidget* tabs);
    /** @brief Wire signals/slots between widgets and backend */
    void setupConnections();
    /** @brief Connect search, category, queue, and install button signals */
    void setupSearchAndQueueConnections();
    /** @brief Connect worker progress and completion signals */
    void setupWorkerConnections();
    /** @brief Connect offline deployment signals */
    void setupOfflineConnections();
    /** @brief Handle NuGet search completion on the main thread */
    void onOfflineSearchCompleted(bool success,
                                  const QString& output,
                                  const QString& error_message);
    /** @brief Parse Chocolatey search output into the results model */
    void updateResultsFromSearch(const QString& output);
    /** @brief Handle search completion on the main thread */
    void onSearchCompleted(bool success, const QString& output, const QString& errorMessage);
    /** @brief Refresh the queue list widget from m_installQueue */
    void updateQueueDisplay();
    /** @brief Enable or disable interactive controls during install */
    void enableControls(bool enabled);
    /** @brief Persist the current install queue to a JSON file */
    void saveQueueToFile();
    /** @brief Load a previously saved install queue from disk */
    void loadQueueFromFile();
    bool parseQueueFile(const QString& filePath, QJsonArray& out_array);
    void importQueueEntries(const QJsonArray& arr, int& added, int& skipped);
    QIcon publisherIcon(const QString& packageId) const;
    static QString lookupPublisher(const QString& packageId);

    /** @brief Refresh the offline deploy list widget from m_offlineList */
    void updateOfflineListDisplay();
    /** @brief Enable/disable offline deployment controls during operations */
    void enableOfflineControls(bool enabled);

    // Publisher icon cache
    static QHash<QString, QString> s_publisherMap;

    // Search section
    QLineEdit* m_searchEdit{nullptr};
    QComboBox* m_onlinePresetCombo{nullptr};
    QPushButton* m_searchButton{nullptr};

    // Results table
    QTableView* m_onlineResultsTable{nullptr};
    QStandardItemModel* m_onlineResultsModel{nullptr};

    // Queue section
    QListWidget* m_queueList{nullptr};
    QPushButton* m_addToQueueButton{nullptr};
    QPushButton* m_removeFromQueueButton{nullptr};
    QPushButton* m_clearQueueButton{nullptr};

    // Install section
    QPushButton* m_installButton{nullptr};
    QPushButton* m_cancelButton{nullptr};
    QProgressBar* m_progressBar{nullptr};
    QLabel* m_progressLabel{nullptr};

    // Bottom bar
    LogToggleSwitch* m_logToggle{nullptr};
    QPushButton* m_saveQueueButton{nullptr};

    // Tab widget
    QTabWidget* m_tabWidget{nullptr};

    // Offline deployment UI
    QComboBox* m_presetCombo{nullptr};
    QListWidget* m_offlineListWidget{nullptr};
    QLineEdit* m_offlinePackageEdit{nullptr};
    QPushButton* m_offlineSearchButton{nullptr};
    QTableView* m_offlineResultsTable{nullptr};
    QStandardItemModel* m_offlineResultsModel{nullptr};
    QPushButton* m_offlineAddButton{nullptr};
    QPushButton* m_offlineRemoveButton{nullptr};
    QPushButton* m_offlineClearButton{nullptr};
    QPushButton* m_buildBundleButton{nullptr};
    QPushButton* m_installFromBundleButton{nullptr};
    QPushButton* m_directDownloadButton{nullptr};
    QPushButton* m_cancelOfflineButton{nullptr};
    QPushButton* m_saveOfflineListButton{nullptr};
    QPushButton* m_loadOfflineListButton{nullptr};
    QProgressBar* m_offlineProgressBar{nullptr};
    QLabel* m_offlineProgressLabel{nullptr};
    QLabel* m_offlineStatusLabel{nullptr};

    // Data
    QVector<QueueEntry> m_installQueue;

    // Offline deployment data
    std::unique_ptr<PackageListManager> m_list_manager;

    // Backend
    std::shared_ptr<ChocolateyManager> m_choco_manager;
    std::shared_ptr<AppInstallationWorker> m_worker;
    std::unique_ptr<OfflineDeploymentWorker> m_offline_worker;

    // Async
    QFuture<void> m_searchFuture;
    QFuture<void> m_offlineSearchFuture;
    std::atomic<bool> m_search_in_progress{false};
    std::atomic<bool> m_offline_search_in_progress{false};
    bool m_install_in_progress{false};
    bool m_offline_in_progress{false};

};  // class AppInstallationPanel

}  // namespace sak
