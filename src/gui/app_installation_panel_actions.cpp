// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_panel.h"
#include "sak/chocolatey_manager.h"
#include "sak/app_installation_worker.h"
#include "sak/migration_report.h"

#include <QMessageBox>
#include <QApplication>
#include <QtConcurrent>
#include <algorithm>

using sak::AppInstallationPanel;
using sak::ChocolateyManager;
using sak::AppInstallationWorker;
using sak::MigrationReport;
using sak::MigrationJob;
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

void AppInstallationPanel::onSearch()
{
    if (m_search_in_progress) {
        Q_EMIT logOutput("Search already in progress...");
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        QMessageBox::warning(this, tr("Chocolatey Not Available"),
            tr("Chocolatey is not initialized. Search is unavailable."));
        return;
    }

    QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        QMessageBox::information(this, tr("Search"),
            tr("Please enter a search term."));
        return;
    }

    m_search_in_progress = true;
    m_searchButton->setEnabled(false);
    Q_EMIT statusMessage(tr("Searching for \"%1\"...").arg(query), 0);
    Q_EMIT logOutput(QString("Searching Chocolatey for: %1").arg(query));

    // Run search in background
    m_searchFuture = QtConcurrent::run([this, query]() {
        auto result = m_choco_manager->searchPackage(query, 50);

        QMetaObject::invokeMethod(this, [this, result]() {
            onSearchCompleted(result.success, result.output, result.error_message);
        }, Qt::QueuedConnection);
    });
}

void AppInstallationPanel::onSearchCompleted(bool success, const QString& output, const QString& errorMessage)
{
    m_search_in_progress = false;
    m_searchButton->setEnabled(true);

    if (success) {
        updateResultsFromSearch(output);
        return;
    }
    Q_EMIT logOutput(QString("Search failed: %1").arg(errorMessage));
    Q_EMIT statusMessage(tr("Search failed"), 3000);
}

void AppInstallationPanel::onCategoryChanged(int index)
{
    // Map category index to search term
    static const char* const categoryQueries[] = {
        "",               // All
        "browser",        // Browsers
        "developer",      // Development
        "media",          // Media
        "utility",        // Utilities
        "security",       // Security
        "office",         // Productivity
        "chat"            // Communication
    };

    if (index > 0 && index < static_cast<int>(std::size(categoryQueries))) {
        m_searchEdit->setText(QString::fromLatin1(categoryQueries[index]));
        onSearch();
    }
}

// ============================================================================
// Queue Management
// ============================================================================

void AppInstallationPanel::onAddToQueue()
{
    int added = 0;
    for (int i = 0; i < m_resultsModel->rowCount(); ++i) {
        auto* checkItem = m_resultsModel->item(i, RColCheck);
        if (!checkItem || checkItem->checkState() != Qt::Checked) {
            continue;
        }

        auto* packageItem = m_resultsModel->item(i, RColPackage);
        auto* versionItem = m_resultsModel->item(i, RColVersion);
        if (!packageItem) continue;

        QString packageId = packageItem->text();
        QString version = versionItem ? versionItem->text() : QString();

        // Check for duplicates
        bool duplicate = std::any_of(m_installQueue.cbegin(), m_installQueue.cend(),
            [&packageId](const QueueEntry& entry) { return entry.package_id == packageId; });

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

void AppInstallationPanel::onRemoveFromQueue()
{
    auto selectedItems = m_queueList->selectedItems();
    if (selectedItems.isEmpty()) return;

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

void AppInstallationPanel::onClearQueue()
{
    if (m_installQueue.isEmpty()) return;

    m_installQueue.clear();
    updateQueueDisplay();
    Q_EMIT logOutput("Install queue cleared");
}

// ============================================================================
// Installation
// ============================================================================

void AppInstallationPanel::onInstallAll()
{
    if (m_installQueue.isEmpty()) {
        QMessageBox::information(this, tr("Empty Queue"),
            tr("No packages in the install queue. Search for packages and add them first."));
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        QMessageBox::warning(this, tr("Chocolatey Not Available"),
            tr("Chocolatey is not initialized. Installation is unavailable."));
        return;
    }

    if (m_worker->isRunning()) {
        QMessageBox::information(this, tr("Installation In Progress"),
            tr("An installation is already running."));
        return;
    }

    auto reply = QMessageBox::question(this, tr("Confirm Installation"),
        tr("Install %1 package(s) via Chocolatey?\n\n"
           "This may take several minutes depending on package sizes.")
            .arg(m_installQueue.size()),
        QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) return;

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
    if (queued == 0) {
        Q_EMIT logOutput("No packages queued for installation.");
        m_install_in_progress = false;
        m_cancelButton->setVisible(false);
        m_installButton->setVisible(true);
        enableControls(true);
    }
}

void AppInstallationPanel::onCancelInstall()
{
    if (m_worker && m_worker->isRunning()) {
        m_worker->cancel();
        Q_EMIT logOutput("Installation cancelled by user");
    }
}
