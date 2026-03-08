// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_panel.h"
#include "sak/chocolatey_manager.h"
#include "sak/logger.h"
#include "sak/layout_constants.h"

#include <QApplication>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QStyle>

using sak::AppInstallationPanel;
using sak::ChocolateyManager;

// Results table columns (must match app_installation_panel.cpp)
enum ResultColumn {
    RColCheck = 0,
    RColPackage,
    RColVersion,
    RColPublisher,
    RColCount
};

// Well-known publisher map: package-id prefix -> publisher name
QHash<QString, QString> AppInstallationPanel::s_publisherMap = {
    {"googlechrome", "Google"},
    {"google-chrome", "Google"},
    {"google-", "Google"},
    {"firefox", "Mozilla"},
    {"thunderbird", "Mozilla"},
    {"vlc", "VideoLAN"},
    {"vscode", "Microsoft"},
    {"visualstudio", "Microsoft"},
    {"microsoft-", "Microsoft"},
    {"dotnet", "Microsoft"},
    {"powershell", "Microsoft"},
    {"notepadplusplus", "Don Ho"},
    {"7zip", "Igor Pavlov"},
    {"gimp", "GIMP Team"},
    {"git", "Git SCM"},
    {"python", "Python Software Foundation"},
    {"nodejs", "OpenJS Foundation"},
    {"java", "Oracle"},
    {"openjdk", "Eclipse Adoptium"},
    {"steam", "Valve"},
    {"discord", "Discord Inc."},
    {"slack", "Salesforce"},
    {"zoom", "Zoom Video Communications"},
    {"obs", "OBS Project"},
    {"putty", "Simon Tatham"},
    {"winscp", "Martin Prikryl"},
    {"filezilla", "FileZilla Project"},
    {"libreoffice", "The Document Foundation"},
    {"audacity", "Audacity Team"},
    {"inkscape", "Inkscape Project"},
    {"blender", "Blender Foundation"},
    {"cmake", "Kitware"},
    {"curl", "Daniel Stenberg"},
    {"wget", "GNU Project"},
    {"winrar", "RARLAB"},
    {"adobe", "Adobe"},
    {"spotify", "Spotify AB"},
    {"dropbox", "Dropbox Inc."},
    {"postman", "Postman Inc."},
    {"docker", "Docker Inc."},
    {"virtualbox", "Oracle"},
    {"vim", "Bram Moolenaar"},
    {"neovim", "Neovim Team"},
    {"atom", "GitHub"},
    {"sublimetext", "Sublime HQ"},
    {"brave", "Brave Software"},
    {"opera", "Opera Software"},
    {"vivaldi", "Vivaldi Technologies"},
    {"edge", "Microsoft"},
    {"teams", "Microsoft"},
    {"skype", "Microsoft"},
    {"onedrive", "Microsoft"},
    {"sysinternals", "Microsoft"},
    {"wireshark", "Wireshark Foundation"},
    {"nmap", "Nmap Project"},
    {"keepass", "Dominik Reichl"},
    {"bitwarden", "Bitwarden Inc."},
    {"1password", "AgileBits"},
    {"handbrake", "HandBrake Team"},
    {"ffmpeg", "FFmpeg Team"},
    {"winmerge", "WinMerge Team"},
    {"everything", "voidtools"},
    {"autohotkey", "AutoHotkey Foundation"},
};

// ============================================================================
// Publisher lookup
// ============================================================================

QIcon AppInstallationPanel::publisherIcon(const QString& packageId) const
{
    // Look up the publisher from the known map
    QString lowerPkg = packageId.toLower();

    // Try exact match first
    if (s_publisherMap.contains(lowerPkg)) {
        // Use themed icons for major publishers
        const QString& pub = s_publisherMap[lowerPkg];
        if (pub == "Microsoft") {
            return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
        }
        if (pub == "Google" || pub == "Mozilla") {
            return QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon);
        }
    }

    // Try prefix match
    for (auto it = s_publisherMap.constBegin(); it != s_publisherMap.constEnd(); ++it) {
        if (lowerPkg.startsWith(it.key())) {
            const QString& pub = it.value();
            if (pub == "Microsoft") {
                return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
            }
            if (pub == "Google" || pub == "Mozilla") {
                return QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon);
            }
            // Known publisher but no special icon
            return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
        }
    }

    // Fallback: generic application icon
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

static QString lookupPublisher(const QString& packageId,
                               const QHash<QString, QString>& map)
{
    QString lower = packageId.toLower();
    if (map.contains(lower)) {
        return map[lower];
    }
    for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
        if (lower.startsWith(it.key())) {
            return it.value();
        }
    }
    return {};
}

// ============================================================================
// Results Table
// ============================================================================

void AppInstallationPanel::updateResultsFromSearch(const QString& output)
{
    auto packages = m_choco_manager->parseSearchResults(output);

    // Disable sorting during population
    const bool wasSortingEnabled = m_resultsTable->isSortingEnabled();
    if (wasSortingEnabled) {
        m_resultsTable->setSortingEnabled(false);
    }

    {
        QSignalBlocker blocker(m_resultsModel);
        m_resultsModel->setRowCount(0);
        m_resultsModel->setRowCount(static_cast<int>(packages.size()));

        int row = 0;
        for (const auto& pkg : packages) {
            // Checkbox
            auto* checkItem = new QStandardItem();
            checkItem->setCheckable(true);
            checkItem->setCheckState(Qt::Unchecked);
            m_resultsModel->setItem(row, RColCheck, checkItem);

            // Package ID with publisher-aware icon
            auto* pkgItem = new QStandardItem(pkg.package_id);
            pkgItem->setIcon(publisherIcon(pkg.package_id));
            m_resultsModel->setItem(row, RColPackage, pkgItem);

            // Version
            m_resultsModel->setItem(row, RColVersion, new QStandardItem(pkg.version));

            // Publisher
            QString pub = lookupPublisher(pkg.package_id, s_publisherMap);
            m_resultsModel->setItem(row, RColPublisher, new QStandardItem(pub));

            row++;
        }
    }

    if (wasSortingEnabled) {
        m_resultsTable->setSortingEnabled(true);
    }

    // Force update
    m_resultsTable->viewport()->update();
    m_resultsTable->scrollToTop();

    int count = static_cast<int>(packages.size());
    Q_EMIT logOutput(QString("Search returned %1 result(s)").arg(count));
    Q_EMIT statusMessage(tr("Found %1 package(s)").arg(count), sak::kTimerStatusMessageMs);
}

// ============================================================================
// Queue Display
// ============================================================================

void AppInstallationPanel::updateQueueDisplay()
{
    m_queueList->clear();

    for (const auto& entry : m_installQueue) {
        QString text = entry.package_id;
        if (!entry.version.isEmpty()) {
            text += QString(" (%1)").arg(entry.version);
        }
        if (!entry.publisher.isEmpty()) {
            text += QString("  -  %1").arg(entry.publisher);
        }
        auto* item = new QListWidgetItem(publisherIcon(entry.package_id), text);
        m_queueList->addItem(item);
    }

    bool hasItems = !m_installQueue.isEmpty();
    m_clearQueueButton->setEnabled(hasItems && !m_install_in_progress);
    m_installButton->setEnabled(hasItems && !m_install_in_progress);
    m_saveQueueButton->setEnabled(hasItems);
    m_removeFromQueueButton->setEnabled(false);  // Reset until selection
}

// ============================================================================
// Controls
// ============================================================================

void AppInstallationPanel::enableControls(bool enabled)
{
    m_searchButton->setEnabled(enabled);
    m_searchEdit->setEnabled(enabled);
    m_categoryCombo->setEnabled(enabled);
    m_addToQueueButton->setEnabled(false);  // Re-enabled on checkbox state change
    m_removeFromQueueButton->setEnabled(false);
    m_clearQueueButton->setEnabled(enabled && !m_installQueue.isEmpty());
    m_installButton->setEnabled(enabled && !m_installQueue.isEmpty());
}

// ============================================================================
// Save / Load Queue
// ============================================================================

void AppInstallationPanel::saveQueueToFile()
{
    if (m_installQueue.isEmpty()) {
        QMessageBox::information(this, tr("Save App List"),
            tr("The install queue is empty. Add packages before saving."));
        return;
    }

    QString filePath = QFileDialog::getSaveFileName(
        this, tr("Save App List"), QString(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) return;

    QJsonArray arr;
    for (const auto& entry : m_installQueue) {
        QJsonObject obj;
        obj["package_id"] = entry.package_id;
        obj["version"]    = entry.version;
        obj["publisher"]  = entry.publisher;
        arr.append(obj);
    }

    QJsonDocument doc(arr);
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning(("Save Failed: Could not write to file: " + filePath).toStdString());
        QMessageBox::warning(this, tr("Save Failed"),
            tr("Could not write to file:\n%1").arg(filePath));
        return;
    }
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    if (file.write(json_bytes) != json_bytes.size()) {
        sak::logWarning("Incomplete write to file: {}", filePath.toStdString());
    }
    file.close();

    Q_EMIT logOutput(QString("Saved %1 package(s) to %2")
                         .arg(m_installQueue.size())
                         .arg(filePath));
    Q_EMIT statusMessage(
        QString("App list saved (%1 packages)").arg(m_installQueue.size()), 3000);
}

void AppInstallationPanel::loadQueueFromFile()
{
    QString filePath = QFileDialog::getOpenFileName(
        this, tr("Load App List"), QString(), tr("JSON Files (*.json)"));
    if (filePath.isEmpty()) return;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logWarning(("Load Failed: Could not read file: " + filePath).toStdString());
        QMessageBox::warning(this, tr("Load Failed"),
            tr("Could not read file:\n%1").arg(filePath));
        return;
    }

    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parseError);
    file.close();

    if (parseError.error != QJsonParseError::NoError) {
        sak::logWarning(("Load Failed: Invalid JSON: " + parseError.errorString()).toStdString());
        QMessageBox::warning(this, tr("Load Failed"),
            tr("Invalid JSON:\n%1").arg(parseError.errorString()));
        return;
    }

    if (!doc.isArray()) {
        sak::logWarning("Load Failed: Expected a JSON array of packages.");
        QMessageBox::warning(this, tr("Load Failed"),
            tr("Expected a JSON array of packages."));
        return;
    }

    int added = 0;
    int skipped = 0;
    const QJsonArray arr = doc.array();
    for (const auto& val : arr) {
        if (!val.isObject()) continue;
        QJsonObject obj = val.toObject();
        QString pkgId = obj["package_id"].toString().trimmed();
        if (pkgId.isEmpty()) continue;

        // Skip duplicates
        bool duplicate = false;
        for (const auto& existing : m_installQueue) {
            if (existing.package_id == pkgId) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            skipped++;
            continue;
        }

        QueueEntry entry;
        entry.package_id = pkgId;
        entry.version    = obj["version"].toString();
        entry.publisher  = obj["publisher"].toString();
        m_installQueue.append(entry);
        added++;
    }

    updateQueueDisplay();

    QString msg = QString("Loaded %1 package(s)").arg(added);
    if (skipped > 0) {
        msg += QString(", %1 duplicate(s) skipped").arg(skipped);
    }
    Q_EMIT logOutput(msg + QString(" from %1").arg(filePath));
    Q_EMIT statusMessage(msg, sak::kTimerStatusMessageMs);
}
