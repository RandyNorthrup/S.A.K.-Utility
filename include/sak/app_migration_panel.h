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

namespace sak {

class ChocolateyManager;
class AppMigrationWorker;
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
class AppMigrationPanel : public QWidget {
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

    explicit AppMigrationPanel(QWidget* parent = nullptr);
    ~AppMigrationPanel() override;

    // Disable copy and move
    AppMigrationPanel(const AppMigrationPanel&) = delete;
    AppMigrationPanel& operator=(const AppMigrationPanel&) = delete;
    AppMigrationPanel(AppMigrationPanel&&) = delete;
    AppMigrationPanel& operator=(AppMigrationPanel&&) = delete;

    /** @brief Access the log toggle switch for MainWindow connection */
    LogToggleSwitch* logToggle() const { return m_logToggle; }

Q_SIGNALS:
    void statusMessage(const QString& message, int timeout_ms);
    void progressUpdated(int current, int maximum);
    void logOutput(const QString& message);

private Q_SLOTS:
    void onSearch();
    void onAddToQueue();
    void onRemoveFromQueue();
    void onClearQueue();
    void onInstallAll();
    void onCancelInstall();
    void onCategoryChanged(int index);

private:
    void setupUI();
    void setupConnections();
    void updateResultsFromSearch(const QString& output);
    void updateQueueDisplay();
    void enableControls(bool enabled);
    void saveQueueToFile();
    void loadQueueFromFile();
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
    std::shared_ptr<ChocolateyManager> m_chocoManager;
    std::shared_ptr<AppMigrationWorker> m_worker;

    // Async
    QFuture<void> m_searchFuture;
    std::atomic<bool> m_searchInProgress{false};
    bool m_installInProgress{false};

}; // class AppMigrationPanel

} // namespace sak
