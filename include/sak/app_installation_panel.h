// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QWidget>
#include <QTableView>
#include <QPushButton>
#include <QLabel>
#include <QComboBox>
#include <QLineEdit>
#include <QListWidget>
#include <QStandardItemModel>
#include <QSplitter>
#include <QGroupBox>
#include <QFuture>
#include <QHash>
#include <QIcon>
#include <memory>
#include <atomic>

class QVBoxLayout;

namespace sak {

class ChocolateyManager;
class AppInstallationWorker;
class MigrationReport;
class LogToggleSwitch;

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
 * - Real-time log output
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
    /** @brief Filter search results by the selected category */
    void onCategoryChanged(int index);

private:
    /** @brief Build the panel layout and child widgets */
    void setupUi();
    /** @brief Build the search bar section */
    void setupUi_searchBar(QVBoxLayout* layout);
    /** @brief Build the package results table panel */
    void setupUi_packageTable(QSplitter* splitter);
    /** @brief Build the install queue section panel */
    void setupUi_queueSection(QSplitter* splitter);
    /** @brief Build the bottom bar with log toggle */
    void setupUi_bottomBar(QVBoxLayout* layout);
    /** @brief Wire signals/slots between widgets and backend */
    void setupConnections();
    /** @brief Connect search, category, queue, and install button signals */
    void setupSearchAndQueueConnections();
    /** @brief Connect worker progress and completion signals */
    void setupWorkerConnections();
    /** @brief Parse Chocolatey search output into the results model */
    void updateResultsFromSearch(const QString& output);
    /** @brief Refresh the queue list widget from m_installQueue */
    void updateQueueDisplay();
    /** @brief Enable or disable interactive controls during install */
    void enableControls(bool enabled);
    /** @brief Persist the current install queue to a JSON file */
    void saveQueueToFile();
    /** @brief Load a previously saved install queue from disk */
    void loadQueueFromFile();
    /** @brief Return a publisher-specific icon for a Chocolatey package */
    QIcon publisherIcon(const QString& packageId) const;

    // Publisher icon cache
    static QHash<QString, QString> s_publisherMap;

    // Search section
    QLineEdit* m_searchEdit{nullptr};
    QComboBox* m_categoryCombo{nullptr};
    QPushButton* m_searchButton{nullptr};

    // Results table
    QTableView* m_resultsTable{nullptr};
    QStandardItemModel* m_resultsModel{nullptr};

    // Queue section
    QListWidget* m_queueList{nullptr};
    QPushButton* m_addToQueueButton{nullptr};
    QPushButton* m_removeFromQueueButton{nullptr};
    QPushButton* m_clearQueueButton{nullptr};

    // Install section
    QPushButton* m_installButton{nullptr};
    QPushButton* m_cancelButton{nullptr};

    // Bottom bar
    LogToggleSwitch* m_logToggle{nullptr};
    QPushButton* m_saveQueueButton{nullptr};

    // Data
    QVector<QueueEntry> m_installQueue;

    // Backend
    std::shared_ptr<ChocolateyManager> m_choco_manager;
    std::shared_ptr<AppInstallationWorker> m_worker;

    // Async
    QFuture<void> m_searchFuture;
    std::atomic<bool> m_search_in_progress{false};
    bool m_install_in_progress{false};

}; // class AppInstallationPanel

} // namespace sak
