// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_busy.h"
#include "sak/app_installation_panel.h"
#include "sak/app_installation_worker.h"
#include "sak/chocolatey_manager.h"
#include "sak/elevation_gate.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"
#include "sak/migration_report.h"
#include "sak/offline_deployment_constants.h"
#include "sak/offline_deployment_worker.h"
#include "sak/package_list_manager.h"

#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QtConcurrent>

#include <algorithm>

using sak::AppInstallationPanel;
using sak::AppInstallationWorker;
using sak::ChocolateyManager;
using sak::MigrationJob;
using sak::MigrationReport;
using sak::MigrationStatus;

// Results table columns (shared by online and offline)
enum ResultColumn {
    RColPackage = 0,
    RColVersion,
    RColPublisher,
    RColCount
};

constexpr int kPackageSearchResultLimit = 50;

namespace {
// Neutralize a locally-produced (choco stderr) error string before it is logged or
// shown: QString::simplified() collapses embedded newlines/control whitespace that could
// otherwise forge log lines, and the length is bounded so a runaway message cannot bloat
// a log line or a table cell. Fails closed toward a short, single-line message.
[[nodiscard]] QString sanitizeErrorForDisplay(const QString& raw) {
    constexpr int kMaxErrorChars = 500;
    QString cleaned = raw.simplified();
    if (cleaned.size() > kMaxErrorChars) {
        cleaned.truncate(kMaxErrorChars);
        cleaned.append(QStringLiteral("..."));
    }
    return cleaned;
}

// Max package names listed in the pre-install review dialog before a "... and N more"
// summary; bounds the dialog height for a large bundle while still previewing the set.
constexpr int kMaxReviewNamesShown = 40;

// Best-effort read of a deployment manifest's package list for the pre-install review
// dialog. Mirrors OfflineDeploymentWorker::readManifest's extraction (size cap + the
// "packages" array) but is display-only: the worker still performs the authoritative
// fail-closed validation before any package is written. Returns an empty list on any
// read/parse failure, which the caller treats as fail-closed.
[[nodiscard]] QStringList readBundlePackageLabels(const QString& manifest_path) {
    QFile file(manifest_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    if (file.size() > sak::offline::kMaxManifestBytes) {
        return {};
    }
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    file.close();
    if (parse_error.error != QJsonParseError::NoError) {
        return {};
    }
    QStringList labels;
    const QJsonArray packages = doc.object().value(QStringLiteral("packages")).toArray();
    for (const auto& value : packages) {
        const QJsonObject pkg = value.toObject();
        const QString id = pkg.value(QStringLiteral("package_id")).toString();
        if (id.isEmpty()) {
            continue;
        }
        const QString version = pkg.value(QStringLiteral("version")).toString();
        labels.append(version.isEmpty() ? id : QStringLiteral("%1 (%2)").arg(id, version));
    }
    return labels;
}

// Show the bundle package-review confirmation (parity with onInstallAll's count confirm).
// Returns true only when the user explicitly accepts; fails closed on any other reply.
[[nodiscard]] bool confirmBundleInstall(QWidget* parent, const QStringList& labels) {
    QStringList shown = labels;
    QString overflow;
    if (shown.size() > kMaxReviewNamesShown) {
        overflow = QObject::tr("\n... and %1 more").arg(shown.size() - kMaxReviewNamesShown);
        shown = shown.mid(0, kMaxReviewNamesShown);
    }
    const QString body = QObject::tr(
                             "Install the following %1 package(s) from the deployment bundle?\n\n"
                             "%2%3\n\n"
                             "This may take several minutes depending on package sizes.")
                             .arg(labels.size())
                             .arg(shown.join(QStringLiteral("\n")), overflow);
    const auto reply = sak::showQuestionLogged(parent,
                                               QObject::tr("Confirm Bundle Installation"),
                                               body,
                                               QMessageBox::Yes | QMessageBox::No);
    return reply == QMessageBox::Yes;
}
}  // namespace

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
        sak::showWarningLogged(this,
                               tr("Chocolatey Not Available"),
                               tr("Chocolatey is not initialized. Search is unavailable."));
        return;
    }

    const QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        sak::showInformationLogged(this, tr("Search"), tr("Please enter a search term."));
        return;
    }

    m_search_in_progress = true;
    m_searchButton->setEnabled(false);
    m_addToQueueButton->setEnabled(false);

    // Show "searching..." indicator in the results table
    m_onlineResultsModel->setRowCount(0);
    m_onlineResultsModel->setRowCount(1);
    auto* searching_item = new QStandardItem(tr("Searching for \"%1\"...").arg(query));
    searching_item->setEnabled(false);
    m_onlineResultsModel->setItem(0, RColPackage, searching_item);

    Q_EMIT statusMessage(tr("Searching for \"%1\"...").arg(query), 0);
    Q_EMIT logOutput(QString("Searching Chocolatey for: %1").arg(query));

    // Run search in background
    m_searchFuture = QtConcurrent::run([this, query]() {
        auto result = m_choco_manager->searchPackage(query, kPackageSearchResultLimit);

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
                                             const QString& error_message) {
    m_search_in_progress = false;
    m_searchButton->setEnabled(true);

    if (success) {
        sak::logInfo("[AppInstallationPanel] Search completed successfully ({} bytes output)",
                     output.size());
        updateResultsFromSearch(output);
        return;
    }

    // Show failure in the results table
    const QString safe_error = sanitizeErrorForDisplay(error_message);
    m_onlineResultsModel->setRowCount(0);
    m_onlineResultsModel->setRowCount(1);
    auto* item = new QStandardItem(tr("Search failed: %1").arg(safe_error));
    item->setEnabled(false);
    m_onlineResultsModel->setItem(0, RColPackage, item);

    sak::logWarning("[AppInstallationPanel] Search failed: {}", safe_error.toStdString());
    Q_EMIT logOutput(QString("Search failed: %1").arg(safe_error));
    Q_EMIT statusMessage(tr("Search failed"), kTimerStatusMessageMs);
}

void AppInstallationPanel::onOnlinePresetSelected(int index) {
    if (index <= 0) {
        return;
    }

    QStringList names = m_list_manager->presetNames();
    const int preset_index = index - 1;
    if (preset_index < 0 || preset_index >= names.size()) {
        return;
    }

    auto preset = m_list_manager->preset(names[preset_index]);

    // Replace queue contents with the selected preset
    m_installQueue.clear();

    for (const auto& entry : preset.entries) {
        QueueEntry queue_entry;
        queue_entry.package_id = entry.package_id;
        queue_entry.version = entry.version;
        m_installQueue.append(queue_entry);
    }

    updateQueueDisplay();

    Q_EMIT logOutput(
        QString("Loaded preset '%1': %2 packages").arg(preset.name).arg(preset.entries.size()));
}

// ============================================================================
// Queue Management
// ============================================================================

void AppInstallationPanel::onAddToQueue() {
    auto indexes = m_onlineResultsTable->selectionModel()->selectedRows();
    if (indexes.isEmpty()) {
        return;
    }

    const int row = indexes.first().row();
    auto* pkg_item = m_onlineResultsModel->item(row, RColPackage);
    if ((pkg_item == nullptr) || !pkg_item->isEnabled()) {
        return;
    }

    QString package_id = pkg_item->text();
    if (package_id.isEmpty()) {
        return;
    }

    // Check for duplicates
    const bool duplicate = std::ranges::any_of(m_installQueue,

                                               [&package_id](const QueueEntry& entry) {
                                                   return entry.package_id == package_id;
                                               });

    if (duplicate) {
        Q_EMIT logOutput(QString("Package '%1' already in queue").arg(package_id));
        return;
    }

    auto* version_item = m_onlineResultsModel->item(row, RColVersion);
    auto* publisher_item = m_onlineResultsModel->item(row, RColPublisher);

    QueueEntry entry;
    entry.package_id = package_id;
    entry.version = (version_item != nullptr) ? version_item->text() : QString();
    entry.publisher = (publisher_item != nullptr) ? publisher_item->text() : QString();
    m_installQueue.append(entry);

    updateQueueDisplay();
    Q_EMIT logOutput(QString("Added '%1' to install queue").arg(package_id));
    Q_EMIT statusMessage(tr("Added %1 to queue").arg(package_id), kTimerStatusMessageMs);
}

void AppInstallationPanel::onRemoveFromQueue() {
    auto selected_items = m_queueList->selectedItems();
    if (selected_items.isEmpty()) {
        return;
    }

    // Collect indices to remove (in reverse order)
    QVector<int> indices;
    for (auto* item : selected_items) {
        indices.append(m_queueList->row(item));
    }
    std::ranges::sort(indices, std::greater<int>());

    for (const int idx : indices) {
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

std::shared_ptr<MigrationReport> AppInstallationPanel::buildInstallMigrationReport() const {
    auto report = std::make_shared<MigrationReport>();
    for (const auto& entry : m_installQueue) {
        MigrationReport::MigrationEntry report_entry;
        report_entry.app_name = entry.package_id;
        report_entry.choco_package = entry.package_id;
        report_entry.available = true;
        report_entry.selected = true;
        report_entry.available_version = entry.version;
        report_entry.confidence = 1.0;
        report_entry.match_type = "exact";
        report_entry.status = "pending";
        report->addEntry(report_entry);
    }
    return report;
}

bool AppInstallationPanel::packageOperationInFlight() const {
    sak::PackageOperationActivity activity;
    activity.online_claimed = m_install_in_progress;
    activity.online_worker_running = m_worker && m_worker->isRunning();
    activity.offline_claimed = m_offline_in_progress;
    activity.offline_worker_running = m_offline_worker && m_offline_worker->isRunning();
    return sak::packageOperationInFlight(activity);
}

void AppInstallationPanel::refreshPackageOperationControls() {
    // Both groups are driven from the single authority, so an operation on either path
    // disables the other path's controls and neither group can be re-enabled while any
    // package work is still in flight.
    const bool idle = !packageOperationInFlight();
    enableControls(idle);
    enableOfflineControls(idle);
}

void AppInstallationPanel::setInstallInProgressUi(bool running) {
    m_install_in_progress = running;
    refreshPackageOperationControls();
    m_installButton->setVisible(!running);
    m_cancelButton->setVisible(running);
    m_cancelButton->setEnabled(running);
}

void AppInstallationPanel::setOfflineInProgressUi(bool running) {
    m_offline_in_progress = running;
    refreshPackageOperationControls();
}

void AppInstallationPanel::onInstallAll() {
    if (m_installQueue.isEmpty()) {
        sak::showInformationLogged(
            this,
            tr("Empty Queue"),
            tr("No packages in the install queue. Search for packages and add them first."));
        return;
    }

    // Cross-path guard, checked BEFORE the elevation prompt: the offline deployment path
    // drives Chocolatey against the same machine locations, so an offline bundle install or
    // download in flight blocks an online install just as another online install does.
    if (packageOperationInFlight()) {
        sak::showInformationLogged(this,
                                   tr("Operation In Progress"),
                                   tr("A package operation is already running. Wait for it to "
                                      "finish or cancel it before installing."));
        return;
    }

    // Tier 2: Chocolatey installation modifies system directories
    auto gate = sak::showElevationGate(
        this,
        tr("Package Installation"),
        tr("Installing packages via Chocolatey requires administrator privileges."));
    if (gate != sak::ElevationGateResult::AlreadyElevated) {
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        sak::logWarning("Installation attempted but Chocolatey is not initialized");
        sak::showWarningLogged(this,
                               tr("Chocolatey Not Available"),
                               tr("Chocolatey is not initialized. Installation is unavailable."));
        return;
    }

    auto reply =
        sak::showQuestionLogged(this,
                                tr("Confirm Installation"),
                                tr("Install %1 package(s) via Chocolatey?\n\n"
                                   "This may take several minutes depending on package sizes.")
                                    .arg(m_installQueue.size()),
                                QMessageBox::Yes | QMessageBox::No);

    if (reply != QMessageBox::Yes) {
        return;
    }

    Q_EMIT logOutput(QString("=== Installing %1 Package(s) ===").arg(m_installQueue.size()));

    setInstallInProgressUi(true);

    auto report = buildInstallMigrationReport();
    const int queued = m_worker->startMigration(report, 1);  // Sequential installation
    sak::logInfo("[AppInstallationPanel] startMigration returned {} queued jobs", queued);
    if (queued == 0) {
        Q_EMIT logOutput("No packages queued for installation.");
        setInstallInProgressUi(false);
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
    const int preset_index = index - 1;
    if (preset_index < 0 || preset_index >= names.size()) {
        return;
    }

    auto preset = m_list_manager->preset(names[preset_index]);

    // Replace list contents with the selected preset
    m_offlineListWidget->clear();

    for (const auto& entry : preset.entries) {
        auto* item = new QListWidgetItem(QString("%1  (%2)").arg(entry.package_id, entry.notes));
        item->setData(Qt::UserRole, entry.package_id);
        item->setData(Qt::UserRole + 1, entry.version);
        m_offlineListWidget->addItem(item);
    }

    const bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);

    Q_EMIT logOutput(
        QString("Loaded preset '%1': %2 packages").arg(preset.name).arg(preset.entries.size()));
}

void AppInstallationPanel::onOfflineSearch() {
    const QString query = m_offlinePackageEdit->text().trimmed();
    if (query.isEmpty()) {
        return;
    }

    if (!m_choco_manager->isInitialized()) {
        sak::logWarning("Offline search attempted but Chocolatey is not initialized");
        sak::showWarningLogged(this,
                               tr("Chocolatey Not Available"),
                               tr("Chocolatey is not initialized. Search is unavailable."));
        return;
    }

    if (m_offline_search_in_progress) {
        Q_EMIT logOutput("Package search already in progress...");
        return;
    }

    m_offline_search_in_progress = true;
    m_offlineSearchButton->setEnabled(false);
    m_offlineResultsModel->setRowCount(0);
    m_offlineAddButton->setEnabled(false);

    m_offlineResultsModel->setRowCount(1);
    auto* searching_item = new QStandardItem(tr("Searching for \"%1\"...").arg(query));
    searching_item->setEnabled(false);
    m_offlineResultsModel->setItem(0, RColPackage, searching_item);
    Q_EMIT logOutput(QString("Searching Chocolatey repository for: %1").arg(query));

    m_offlineSearchFuture = QtConcurrent::run([this, query]() {
        auto result = m_choco_manager->searchPackage(query, offline::kSearchResultsDefault);

        QMetaObject::invokeMethod(
            this,
            [this, result]() {
                onOfflineSearchCompleted(result.success, result.output, result.error_message);
            },
            Qt::QueuedConnection);
    });
}

void AppInstallationPanel::onOfflineSearchCompleted(bool success,
                                                    const QString& output,
                                                    const QString& error_message) {
    m_offline_search_in_progress = false;
    m_offlineSearchButton->setEnabled(true);
    m_offlineResultsModel->setRowCount(0);

    if (!success) {
        const QString safe_error = sanitizeErrorForDisplay(error_message);
        sak::logWarning("[AppInstallationPanel] Offline search failed: {}",
                        safe_error.toStdString());
        m_offlineResultsModel->setRowCount(1);
        auto* item = new QStandardItem(tr("Search failed: %1").arg(safe_error));
        item->setEnabled(false);
        m_offlineResultsModel->setItem(0, RColPackage, item);
        Q_EMIT logOutput(QString("Search failed: %1").arg(safe_error));
        return;
    }

    auto packages = m_choco_manager->parseSearchResults(output);

    if (packages.empty()) {
        m_offlineResultsModel->setRowCount(1);
        auto* item = new QStandardItem(tr("No packages found"));
        item->setEnabled(false);
        m_offlineResultsModel->setItem(0, RColPackage, item);
        Q_EMIT logOutput("Package search returned no results");
        return;
    }

    // Never display more than the limit that was requested from choco
    // (offline::kSearchResultsDefault). Capping here fails closed against an over-long
    // parse result and removes the unchecked size_t->int narrowing on setRowCount.
    const int limit = offline::kSearchResultsDefault;
    const int display_count = packages.size() > static_cast<decltype(packages.size())>(limit)
                                  ? limit
                                  : static_cast<int>(packages.size());
    m_offlineResultsModel->setRowCount(display_count);
    for (int row = 0; row < display_count; ++row) {
        const auto& pkg = packages[static_cast<decltype(packages.size())>(row)];
        auto* pkg_item = new QStandardItem(pkg.package_id);
        pkg_item->setIcon(publisherIcon(pkg.package_id));
        m_offlineResultsModel->setItem(row, RColPackage, pkg_item);

        m_offlineResultsModel->setItem(row, RColVersion, new QStandardItem(pkg.version));

        const QString pub = lookupPublisher(pkg.package_id);
        m_offlineResultsModel->setItem(row, RColPublisher, new QStandardItem(pub));
    }

    m_offlineResultsTable->scrollToTop();
    Q_EMIT logOutput(QString("Found %1 package(s)").arg(display_count));
}

void AppInstallationPanel::onAddToOfflineList() {
    auto indexes = m_offlineResultsTable->selectionModel()->selectedRows();
    if (indexes.isEmpty()) {
        return;
    }

    const int row = indexes.first().row();
    auto* pkg_item = m_offlineResultsModel->item(row, RColPackage);
    if ((pkg_item == nullptr) || !pkg_item->isEnabled()) {
        return;
    }

    const QString package_id = pkg_item->text();
    if (package_id.isEmpty()) {
        return;
    }

    // Check for duplicates
    for (int i = 0; i < m_offlineListWidget->count(); ++i) {
        const QString item_data = m_offlineListWidget->item(i)->data(Qt::UserRole).toString();
        if (item_data.compare(package_id, Qt::CaseInsensitive) == 0) {
            Q_EMIT logOutput(QString("Package '%1' already in list").arg(package_id));
            return;
        }
    }

    auto* version_item = m_offlineResultsModel->item(row, RColVersion);
    const QString version = (version_item != nullptr) ? version_item->text() : QString();

    auto* item = new QListWidgetItem(QString("%1  (v%2)").arg(package_id, version));
    item->setData(Qt::UserRole, package_id);
    item->setData(Qt::UserRole + 1, version);
    m_offlineListWidget->addItem(item);

    const bool has_items = m_offlineListWidget->count() > 0;
    m_offlineClearButton->setEnabled(has_items);
    m_buildBundleButton->setEnabled(has_items);
    m_directDownloadButton->setEnabled(has_items);
    m_saveOfflineListButton->setEnabled(has_items);

    Q_EMIT logOutput(QString("Added to offline list: %1 (v%2)").arg(package_id, version));
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

    const bool has_items = m_offlineListWidget->count() > 0;
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

    // Reset preset selection to match the now-empty list
    m_presetCombo->blockSignals(true);
    m_presetCombo->setCurrentIndex(0);
    m_presetCombo->blockSignals(false);

    Q_EMIT logOutput("Offline package list cleared");
}

void AppInstallationPanel::onBuildBundle() {
    if (m_offlineListWidget->count() == 0) {
        sak::showInformationLogged(this,
                                   tr("Empty List"),
                                   tr("Add packages to the list before building a bundle."));
        return;
    }

    // Single authority: an ONLINE Chocolatey install in flight blocks this too (both paths
    // drive choco against the same machine locations).
    if (packageOperationInFlight()) {
        sak::showInformationLogged(this,
                                   tr("Operation In Progress"),
                                   tr("A package operation is already running. Wait for it to "
                                      "finish or cancel it before building a bundle."));
        return;
    }

    const QString output_dir = QFileDialog::getExistingDirectory(
        this, tr("Select Output Directory for Deployment Bundle"));

    if (output_dir.isEmpty()) {
        return;
    }

    // Collect packages from list
    QVector<QPair<QString, QString>> packages;
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        auto* item = m_offlineListWidget->item(row);
        const QString pkg_id = item->data(Qt::UserRole).toString();
        const QString version = item->data(Qt::UserRole + 1).toString();
        packages.append({pkg_id, version});
    }

    Q_EMIT logOutput(
        QString("=== Building Offline Bundle: %1 package(s) ===").arg(packages.size()));

    setOfflineInProgressUi(true);

    const auto mode = static_cast<sak::PayloadMode>(m_payloadModeCombo->currentData().toInt());
    m_offline_worker->buildDeploymentBundle(
        packages, output_dir, tr("S.A.K. Utility deployment payload"), mode);
}

void AppInstallationPanel::onInstallFromBundle() {
    // Single authority: an ONLINE Chocolatey install in flight blocks this too -- a bundle
    // install and an online install both run choco against the same install root.
    if (packageOperationInFlight()) {
        sak::showInformationLogged(this,
                                   tr("Operation In Progress"),
                                   tr("A package operation is already running. Wait for it to "
                                      "finish or cancel it before installing from a bundle."));
        return;
    }

    const QString manifest_path = QFileDialog::getOpenFileName(
        this, tr("Select Deployment Manifest"), QString(), tr("JSON Files (*.json)"));

    if (manifest_path.isEmpty()) {
        return;
    }

    // The packages directory is alongside the manifest
    const QFileInfo manifest_info(manifest_path);
    const QString packages_dir = manifest_info.dir().absolutePath() + "/packages";

    if (!QDir(packages_dir).exists()) {
        sak::showWarningLogged(
            this,
            tr("Missing Packages"),
            tr("The 'packages' directory was not found alongside the manifest."));
        return;
    }

    // Installing packages via choco writes machine locations -> require elevation
    // (parity with onInstallAll), so a non-elevated run does not silently stall or
    // half-install.
    auto gate = sak::showElevationGate(
        this,
        tr("Install from Bundle"),
        tr("Installing packages from a deployment payload requires administrator privileges."));
    if (gate != sak::ElevationGateResult::AlreadyElevated) {
        return;
    }

    // Package-review confirmation (parity with onInstallAll's count confirm): a bundle
    // install is a destructive multi-package system change, so preview the packages the
    // manifest will install and require an explicit confirm before the worker writes
    // anything. This read is display-only; the worker still runs authoritative validation.
    const QStringList package_labels = readBundlePackageLabels(manifest_path);
    if (package_labels.isEmpty()) {
        sak::showWarningLogged(
            this,
            tr("Manifest Unreadable"),
            tr("The deployment manifest lists no installable packages or could not be read."));
        return;
    }
    if (!confirmBundleInstall(this, package_labels)) {
        return;
    }

    Q_EMIT logOutput(QString("=== Installing from Bundle: %1 ===").arg(manifest_path));

    setOfflineInProgressUi(true);

    m_offline_worker->installFromBundle(manifest_path, packages_dir, m_airGapCheck->isChecked());
}

void AppInstallationPanel::onDirectDownload() {
    if (m_offlineListWidget->count() == 0) {
        sak::showInformationLogged(this, tr("Empty List"), tr("Add packages before downloading."));
        return;
    }

    // Single authority: an ONLINE Chocolatey install in flight blocks this too.
    if (packageOperationInFlight()) {
        sak::showInformationLogged(this,
                                   tr("Operation In Progress"),
                                   tr("A package operation is already running. Wait for it to "
                                      "finish or cancel it before downloading."));
        return;
    }

    const QString output_dir = QFileDialog::getExistingDirectory(this,
                                                                 tr("Select Download Directory"));

    if (output_dir.isEmpty()) {
        return;
    }

    QVector<QPair<QString, QString>> packages;
    for (int row = 0; row < m_offlineListWidget->count(); ++row) {
        auto* item = m_offlineListWidget->item(row);
        const QString pkg_id = item->data(Qt::UserRole).toString();
        const QString version = item->data(Qt::UserRole + 1).toString();
        packages.append({pkg_id, version});
    }

    Q_EMIT logOutput(QString("=== Direct Download: %1 package(s) ===").arg(packages.size()));

    setOfflineInProgressUi(true);

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

    const QString file_path = QFileDialog::getSaveFileName(
        this, tr("Save Package List"), "package_list.json", tr("JSON Files (*.json)"));

    if (file_path.isEmpty()) {
        return;
    }

    auto list = PackageListManager::createList("Custom List", "User-created package list");
    const int widget_count = m_offlineListWidget->count();
    int accepted = 0;
    for (int row = 0; row < widget_count; ++row) {
        auto* item = m_offlineListWidget->item(row);
        if (PackageListManager::addPackage(list,
                                           item->data(Qt::UserRole).toString(),
                                           item->data(Qt::UserRole + 1).toString())) {
            ++accepted;
        }
    }

    // Fail closed: if addPackage dropped any entry (capacity/duplicate/empty id), do not
    // write a file that reports success while silently omitting packages the user
    // believes are captured. Nothing is written when the list is incomplete.
    if (accepted < widget_count) {
        sak::logError("[AppInstallationPanel] Package list incomplete: {} of {} entries accepted",
                      accepted,
                      widget_count);
        sak::showWarningLogged(
            this,
            tr("Save Failed"),
            tr("The package list could not be saved completely (%1 of %2 entries were rejected). "
               "Nothing was written.")
                .arg(widget_count - accepted)
                .arg(widget_count));
        return;
    }

    if (PackageListManager::saveToFile(list, file_path)) {
        Q_EMIT logOutput(QString("Package list saved: %1").arg(file_path));
    } else {
        sak::logError("[AppInstallationPanel] Failed to save package list");
        sak::showWarningLogged(this, tr("Save Failed"), tr("Could not save the package list."));
    }
}

void AppInstallationPanel::onLoadOfflineList() {
    const QString file_path = QFileDialog::getOpenFileName(
        this, tr("Load Package List"), QString(), tr("JSON Files (*.json)"));

    if (file_path.isEmpty()) {
        return;
    }

    auto list = PackageListManager::loadFromFile(file_path);
    if (list.entries.isEmpty()) {
        sak::showWarningLogged(this,
                               tr("Load Failed"),
                               tr("The selected file contains no packages or is invalid."));
        return;
    }

    int added = 0;
    for (const auto& entry : list.entries) {
        bool exists = false;
        for (int row = 0; row < m_offlineListWidget->count(); ++row) {
            const QString item_data = m_offlineListWidget->item(row)->data(Qt::UserRole).toString();
            if (item_data.compare(entry.package_id, Qt::CaseInsensitive) == 0) {
                exists = true;
                break;
            }
        }
        if (!exists) {
            const QString label = entry.notes.isEmpty()
                                      ? entry.package_id
                                      : QString("%1  (%2)").arg(entry.package_id, entry.notes);
            auto* item = new QListWidgetItem(label);
            item->setData(Qt::UserRole, entry.package_id);
            item->setData(Qt::UserRole + 1, entry.version);
            m_offlineListWidget->addItem(item);
            added++;
        }
    }

    const bool has_items = m_offlineListWidget->count() > 0;
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
    // Already managed inline - list widget items track their own data
}

void AppInstallationPanel::enableOfflineControls(bool enabled) {
    m_presetCombo->setEnabled(enabled);
    m_offlinePackageEdit->setEnabled(enabled);
    m_offlineSearchButton->setEnabled(enabled);
    m_offlineAddButton->setEnabled(enabled &&
                                   m_offlineResultsTable->selectionModel()->hasSelection());
    m_offlineRemoveButton->setEnabled(enabled && !m_offlineListWidget->selectedItems().isEmpty());
    m_offlineClearButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_buildBundleButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_directDownloadButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_installFromBundleButton->setEnabled(enabled);
    m_saveOfflineListButton->setEnabled(enabled && m_offlineListWidget->count() > 0);
    m_loadOfflineListButton->setEnabled(enabled);
    m_payloadModeCombo->setEnabled(enabled);
    m_airGapCheck->setEnabled(enabled);
}
