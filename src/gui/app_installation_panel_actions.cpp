// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_panel.h"
#include "sak/app_installation_worker.h"
#include "sak/chocolatey_manager.h"
#include "sak/logger.h"
#include "sak/migration_report.h"
#include "sak/offline_deployment_worker.h"
#include "sak/package_list_manager.h"

#include <QApplication>
#include <QFileDialog>
#include <QMessageBox>
#include <QtConcurrent>

#include <algorithm>

using sak::AppInstallationPanel;
using sak::AppInstallationWorker;
using sak::ChocolateyManager;
using sak::MigrationJob;
using sak::MigrationReport;
using sak::MigrationStatus;

// Results table columns (must match app_installation_panel.cpp)
enum ResultColumn {
    RColCheck = 0,
    RColPackage,
    RColVersion,
    RColPublisher,
    RColCount
};

// ============================================================================
// Search
// ============================================================================

void AppInstallationPanel::onSearch() {
    if (m_search_in_progress) {
        Q_EMIT logOutput("Search already in progress...");
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        sak::logWarning("Search attempted but Chocolatey is not initialized");
        QMessageBox::warning(this,
                             tr("Chocolatey Not Available"),
                             tr("Chocolatey is not initialized. Search is unavailable."));
        return;
    }

    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        QMessageBox::information(this, tr("Search"), tr("Please enter a search term."));
        return;
    }

    m_search_in_progress = true;
    m_searchButton->setEnabled(false);
    Q_EMIT statusMessage(tr("Searching for \"%1\"...").arg(query), 0);
    Q_EMIT logOutput(QString("Searching Chocolatey for: %1").arg(query));

    // Run search in background
    m_searchFuture = QtConcurrent::run([this, query]() {
        auto result = m_choco_manager->searchPackage(query, 50);

        QMetaObject::invokeMethod(
            this,
            [this, result]() {
                onSearchCompleted(result.success, result.output, result.error_message);
            },
            Qt::QueuedConnection);
    });
}

void AppInstallationPanel::onSearchCompleted(bool success,
                                             const QString& output,
                                             const QString& errorMessage) {
    m_search_in_progress = false;
    m_searchButton->setEnabled(true);

    if (success) {
        sak::logInfo("[AppInstallationPanel] Search completed successfully ({} bytes output)",
                     output.size());
        updateResultsFromSearch(output);
        return;
    }
    sak::logWarning("[AppInstallationPanel] Search failed: {}", errorMessage.toStdString());
    Q_EMIT logOutput(QString("Search failed: %1").arg(errorMessage));
    Q_EMIT statusMessage(tr("Search failed"), 3000);
}

void AppInstallationPanel::onCategoryChanged(int index) {
    if (index < 0) {
        return;
    }
    // Map category index to search term
    static const char* const categoryQueries[] = {
        "",           // All
        "browser",    // Browsers
        "developer",  // Development
        "media",      // Media
        "utility",    // Utilities
        "security",   // Security
        "office",     // Productivity
        "chat"        // Communication
    };

    if (index > 0 && index < static_cast<int>(std::size(categoryQueries))) {
        m_searchEdit->setText(QString::fromLatin1(categoryQueries[index]));
        onSearch();
    }
}

// ============================================================================
// Queue Management
// ============================================================================

void AppInstallationPanel::onAddToQueue() {
    int added = 0;
    for (int i = 0; i < m_resultsModel->rowCount(); ++i) {
        auto* checkItem = m_resultsModel->item(i, RColCheck);
        if (!checkItem || checkItem->checkState() != Qt::Checked) {
            continue;
        }

        auto* packageItem = m_resultsModel->item(i, RColPackage);
        auto* versionItem = m_resultsModel->item(i, RColVersion);
        if (!packageItem) {
            continue;
        }

        QString packageId = packageItem->text();
        QString version = versionItem ? versionItem->text() : QString();

        // Check for duplicates
        bool duplicate = std::any_of(m_installQueue.cbegin(),
                                     m_installQueue.cend(),
                                     [&packageId](const QueueEntry& entry) {
                                         return entry.package_id == packageId;
                                     });

        if (!duplicate) {
            auto* publisherItem = m_resultsModel->item(i, RColPublisher);
            QueueEntry entry;
            entry.package_id = packageId;
            entry.version = version;
            entry.publisher = publisherItem ? publisherItem->text() : QString();
            m_installQueue.append(entry);
            added++;
        }

        // Uncheck after adding
        checkItem->setCheckState(Qt::Unchecked);
    }

    if (added > 0) {
        updateQueueDisplay();
        Q_EMIT logOutput(QString("Added %1 package(s) to install queue").arg(added));
        Q_EMIT statusMessage(QString("%1 package(s) added to queue").arg(added), 3000);
    } else {
        Q_EMIT logOutput("No new packages added (already in queue or none selected)");
    }

    m_addToQueueButton->setEnabled(false);
}

void AppInstallationPanel::onRemoveFromQueue() {
    auto selectedItems = m_queueList->selectedItems();
    if (selectedItems.isEmpty()) {
        return;
    }

    // Collect indices to remove (in reverse order)
    QVector<int> indices;
    for (auto* item : selectedItems) {
        indices.append(m_queueList->row(item));
    }
    std::sort(indices.begin(), indices.end(), std::greater<int>());

    for (int idx : indices) {
        if (idx >= 0 && idx < m_installQueue.size()) {
            Q_EMIT logOutput(QString("Removed from queue: %1").arg(m_installQueue[idx].package_id));
            m_installQueue.removeAt(idx);
        }
    }

    updateQueueDisplay();
}

void AppInstallationPanel::onClearQueue() {
    if (m_installQueue.isEmpty()) {
        return;
    }

    m_installQueue.clear();
    updateQueueDisplay();
    Q_EMIT logOutput("Install queue cleared");
}

// ============================================================================
// Installation
// ============================================================================

void AppInstallationPanel::onInstallAll() {
    if (m_installQueue.isEmpty()) {
        QMessageBox::information(
            this,
            tr("Empty Queue"),
            tr("No packages in the install queue. Search for packages and add them first."));
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        sak::logWarning("Installation attempted but Chocolatey is not initialized");
        QMessageBox::warning(this,
                             tr("Chocolatey Not Available"),
                             tr("Chocolatey is not initialized. Installation is unavailable."));
        return;
    }

    if (m_worker->isRunning()) {
        QMessageBox::information(this,
                                 tr("Installation In Progress"),
                                 tr("An installation is already running."));
        return;
    }

    auto reply =
        QMessageBox::question(this,
                              tr("Confirm Installation"),
                              tr("Install %1 package(s) via Chocolatey?\n\n"
                                 "This may take several minutes depending on package sizes.")
                                  .arg(m_installQueue.size()),
                              QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(QString("=== Installing %1 Package(s) ===").arg(m_installQueue.size()));

    m_install_in_progress = true;
    enableControls(false);
    m_installButton->setVisible(false);
    m_cancelButton->setVisible(true);
    m_cancelButton->setEnabled(true);

    // Create a MigrationReport from the queue for the worker
    auto report = std::make_shared<MigrationReport>();
    for (const auto& entry : m_installQueue) {
        MigrationReport::MigrationEntry reportEntry;
        reportEntry.app_name = entry.package_id;
        reportEntry.choco_package = entry.package_id;
        reportEntry.available = true;
        reportEntry.selected = true;
        reportEntry.available_version = entry.version;
        reportEntry.confidence = 1.0;
        reportEntry.match_type = "exact";
        reportEntry.status = "pending";
        report->addEntry(reportEntry);
    }

    int queued = m_worker->startMigration(report, 1);  // Sequential installation
    sak::logInfo("[AppInstallationPanel] startMigration returned {} queued jobs", queued);
    if (queued == 0) {
        Q_EMIT logOutput("No packages queued for installation.");
        m_install_in_progress = false;
        m_cancelButton->setVisible(false);
        m_installButton->setVisible(true);
        enableControls(true);
    }
}

void AppInstallationPanel::onCancelInstall() {
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
        Q_EMIT logOutput("Installation cancelled by user");
    }
}

// ============================================================================
// Offline Deployment Actions
// ============================================================================

void AppInstallationPanel::onPresetSelected(int index) {
    if (index <= 0) {
        return;
    }

    QStringList names = m_list_manager->presetNames();
    int preset_index = index - 1;
    if (preset_index < 0 || preset_index >= names.size()) {
        return;
    }

    auto preset = m_list_manager->preset(names[preset_index]);

    // Add preset entries to the offline list widget
    int added = 0;
    for (const auto& entry : preset.entries) {
        // Check for duplicates
        bool exists = false;
        for (int row = 0; row < m_offlineListWidget->count(); ++row) {
            QString item_data = m_offlineListWidget->item(row)->data(Qt::UserRole).toString();
            if (item_data.compare(entry.package_id, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }

        if (!exists) {
            auto* item =
                new QListWidgetItem(QString("%1  (%2)").arg(entry.package_id, entry.notes));
            item->setData(Qt::UserRole, entry.package_id);
            item->setData(Qt::UserRole + 1, entry.version);
            m_offlineListWidget->addItem(item);
            added++;
        }
    }

    bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);

    Q_EMIT logOutput(QString("Loaded preset '%1': %2 packages added").arg(preset.name).arg(added));

    m_presetCombo->setCurrentIndex(0);
}

void AppInstallationPanel::onAddToOfflineList() {
    QString package_id = m_offlinePackageEdit->text().trimmed();
    if (package_id.isEmpty()) {
        return;
    }

    // Check for duplicates
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        QString item_data = m_offlineListWidget->item(row)->data(Qt::UserRole).toString();
        if (item_data.compare(package_id, Qt::CaseInsensitive) == 0) {
            Q_EMIT logOutput(QString("Package '%1' already in list").arg(package_id));
            return;
        }
    }

    auto* item = new QListWidgetItem(package_id);
    item->setData(Qt::UserRole, package_id);
    item->setData(Qt::UserRole + 1, QString());  // latest version
    m_offlineListWidget->addItem(item);

    m_offlinePackageEdit->clear();

    bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);

    Q_EMIT logOutput(QString("Added to offline list: %1").arg(package_id));
}

void AppInstallationPanel::onRemoveFromOfflineList() {
    auto selected = m_offlineListWidget->selectedItems();
    if (selected.isEmpty()) {
        return;
    }

    for (auto* item : selected) {
        Q_EMIT logOutput(
            QString("Removed from offline list: %1").arg(item->data(Qt::UserRole).toString()));
        delete m_offlineListWidget->takeItem(m_offlineListWidget->row(item));
    }

    bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);
}

void AppInstallationPanel::onClearOfflineList() {
    if (m_offlineListWidget->count() == 0) {
        return;
    }

    m_offlineListWidget->clear();
    m_offlineClearButton->setEnabled(false);
    m_buildBundleButton->setEnabled(false);
    m_directDownloadButton->setEnabled(false);
    m_saveOfflineListButton->setEnabled(false);

    Q_EMIT logOutput("Offline package list cleared");
}

void AppInstallationPanel::onBuildBundle() {
    if (m_offlineListWidget->count() == 0) {
        QMessageBox::information(this,
                                 tr("Empty List"),
                                 tr("Add packages to the list before building a bundle."));
        return;
    }

    if (m_offline_worker->isRunning()) {
        QMessageBox::information(this,
                                 tr("Operation In Progress"),
                                 tr("An offline deployment operation is already running."));
        return;
    }

    QString output_dir = QFileDialog::getExistingDirectory(
        this, tr("Select Output Directory for Deployment Bundle"));

    if (output_dir.isEmpty()) {
        return;
    }

    // Collect packages from list
    QVector<QPair<QString, QString>> packages;
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        auto* item = m_offlineListWidget->item(row);
        QString pkg_id = item->data(Qt::UserRole).toString();
        QString version = item->data(Qt::UserRole + 1).toString();
        packages.append({pkg_id, version});
    }

    Q_EMIT logOutput(
        QString("=== Building Offline Bundle: %1 package(s) ===").arg(packages.size()));

    m_offline_in_progress = true;
    enableOfflineControls(false);

    m_offline_worker->buildDeploymentBundle(packages,
                                            output_dir,
                                            tr("S.A.K. Utility offline deployment bundle"));
}

void AppInstallationPanel::onInstallFromBundle() {
    if (m_offline_worker->isRunning()) {
        QMessageBox::information(this,
                                 tr("Operation In Progress"),
                                 tr("An offline deployment operation is already running."));
        return;
    }

    QString manifest_path = QFileDialog::getOpenFileName(
        this, tr("Select Deployment Manifest"), QString(), tr("JSON Files (*.json)"));

    if (manifest_path.isEmpty()) {
        return;
    }

    // The packages directory is alongside the manifest
    QFileInfo manifest_info(manifest_path);
    QString packages_dir = manifest_info.dir().absolutePath() + "/packages";

    if (!QDir(packages_dir).exists()) {
        QMessageBox::warning(this,
                             tr("Missing Packages"),
                             tr("The 'packages' directory was not found alongside the manifest."));
        return;
    }

    Q_EMIT logOutput(QString("=== Installing from Bundle: %1 ===").arg(manifest_path));

    m_offline_in_progress = true;
    enableOfflineControls(false);

    m_offline_worker->installFromBundle(manifest_path, packages_dir);
}

void AppInstallationPanel::onDirectDownload() {
    if (m_offlineListWidget->count() == 0) {
        QMessageBox::information(this, tr("Empty List"), tr("Add packages before downloading."));
        return;
    }

    if (m_offline_worker->isRunning()) {
        QMessageBox::information(this,
                                 tr("Operation In Progress"),
                                 tr("An offline deployment operation is already running."));
        return;
    }

    QString output_dir = QFileDialog::getExistingDirectory(this, tr("Select Download Directory"));

    if (output_dir.isEmpty()) {
        return;
    }

    QVector<QPair<QString, QString>> packages;
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        auto* item = m_offlineListWidget->item(row);
        QString pkg_id = item->data(Qt::UserRole).toString();
        QString version = item->data(Qt::UserRole + 1).toString();
        packages.append({pkg_id, version});
    }

    Q_EMIT logOutput(QString("=== Direct Download: %1 package(s) ===").arg(packages.size()));

    m_offline_in_progress = true;
    enableOfflineControls(false);

    m_offline_worker->directDownload(packages, output_dir);
}

void AppInstallationPanel::onCancelOfflineOperation() {
    if (m_offline_worker && m_offline_worker->isRunning()) {
        m_offline_worker->cancel();
        Q_EMIT logOutput("Offline operation cancelled by user");
    }
}

void AppInstallationPanel::onSaveOfflineList() {
    if (m_offlineListWidget->count() == 0) {
        return;
    }

    QString file_path = QFileDialog::getSaveFileName(
        this, tr("Save Package List"), "package_list.json", tr("JSON Files (*.json)"));

    if (file_path.isEmpty()) {
        return;
    }

    auto list = PackageListManager::createList("Custom List", "User-created package list");
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        auto* item = m_offlineListWidget->item(row);
        PackageListManager::addPackage(list,
                                       item->data(Qt::UserRole).toString(),
                                       item->data(Qt::UserRole + 1).toString());
    }

    if (PackageListManager::saveToFile(list, file_path)) {
        Q_EMIT logOutput(QString("Package list saved: %1").arg(file_path));
    } else {
        sak::logError("[AppInstallationPanel] Failed to save package list");
        QMessageBox::warning(this, tr("Save Failed"), tr("Could not save the package list."));
    }
}

void AppInstallationPanel::onLoadOfflineList() {
    QString file_path = QFileDialog::getOpenFileName(
        this, tr("Load Package List"), QString(), tr("JSON Files (*.json)"));

    if (file_path.isEmpty()) {
        return;
    }

    auto list = PackageListManager::loadFromFile(file_path);
    if (list.entries.isEmpty()) {
        QMessageBox::warning(this,
                             tr("Load Failed"),
                             tr("The selected file contains no packages or is invalid."));
        return;
    }

    int added = 0;
    for (const auto& entry : list.entries) {
        bool exists = false;
        for (int row = 0; row < m_offlineListWidget->count(); ++row) {
            QString item_data = m_offlineListWidget->item(row)->data(Qt::UserRole).toString();
            if (item_data.compare(entry.package_id, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            QString label = entry.notes.isEmpty()
                                ? entry.package_id
                                : QString("%1  (%2)").arg(entry.package_id, entry.notes);
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, entry.package_id);
            item->setData(Qt::UserRole + 1, entry.version);
            m_offlineListWidget->addItem(item);
            added++;
        }
    }

    bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);

    Q_EMIT logOutput(QString("Loaded list '%1': %2 packages added").arg(list.name).arg(added));
}

// ============================================================================
// Offline Deployment Helpers
// ============================================================================

void AppInstallationPanel::updateOfflineListDisplay() {
    // Already managed inline — list widget items track their own data
}

void AppInstallationPanel::enableOfflineControls(bool enabled) {
    m_presetCombo->setEnabled(enabled);
    m_offlinePackageEdit->setEnabled(enabled);
    m_offlineAddButton->setEnabled(enabled);
    m_offlineRemoveButton->setEnabled(enabled && !m_offlineListWidget->selectedItems().isEmpty());
    m_offlineClearButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_buildBundleButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_directDownloadButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_installFromBundleButton->setEnabled(enabled);
    m_saveOfflineListButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_loadOfflineListButton->setEnabled(enabled);
}
