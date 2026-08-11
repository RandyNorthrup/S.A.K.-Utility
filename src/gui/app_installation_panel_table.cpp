// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/app_installation_panel.h"
#include "sak/chocolatey_manager.h"
#include "sak/layout_constants.h"
#include "sak/logger.h"
#include "sak/message_box_helpers.h"

#include <QApplication>
#include <QFileDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QStyle>

#include <algorithm>

using sak::AppInstallationPanel;
using sak::ChocolateyManager;

namespace {
constexpr int kQueueSaveStatusMessageMs = sak::kTimerStatusMessageMs;

bool isAllowedQueuePackageChar(QChar ch) {
    return ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('-') ||
           ch == QLatin1Char('_') || ch == QLatin1Char('+');
}

// A queue package id is valid only if it is non-empty, not option-like (a leading '-'
// would be parsed by Chocolatey as a flag such as --force), and composed solely of
// allowed characters. A malformed id is REJECTED (skipped), never rewritten, so it can
// never reach the privileged install queue.
bool queuePackageIdIsValid(const QString& id) {
    return !id.isEmpty() && !id.startsWith(QLatin1Char('-')) &&
           std::ranges::all_of(id, isAllowedQueuePackageChar);
}

// A version is optional; when present it must contain only allowed characters so a
// malformed token cannot flow into an install command.
bool queueVersionIsValid(const QString& version) {
    return version.isEmpty() || std::ranges::all_of(version, isAllowedQueuePackageChar);
}
}  // namespace

// Results table columns (shared by online and offline)
enum ResultColumn {
    RColPackage = 0,
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

namespace {
QIcon iconForPublisher(const QString& publisher) {
    if (publisher == "Microsoft") {
        return QApplication::style()->standardIcon(QStyle::SP_ComputerIcon);
    }
    if (publisher == "Google" || publisher == "Mozilla") {
        return QApplication::style()->standardIcon(QStyle::SP_DriveNetIcon);
    }
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}
}  // namespace

QIcon AppInstallationPanel::publisherIcon(const QString& package_id) const {
    if (package_id.isEmpty()) {
        return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
    }
    const QString lower_pkg = package_id.toLower();

    // Try exact match first
    if (s_publisherMap.contains(lower_pkg)) {
        return iconForPublisher(s_publisherMap[lower_pkg]);
    }

    // Try prefix match
    for (auto it = s_publisherMap.constBegin(); it != s_publisherMap.constEnd(); ++it) {
        if (lower_pkg.startsWith(it.key())) {
            return iconForPublisher(it.value());
        }
    }

    // Fallback: generic application icon
    return QApplication::style()->standardIcon(QStyle::SP_FileIcon);
}

QString AppInstallationPanel::lookupPublisher(const QString& package_id) {
    const QString lower = package_id.toLower();
    if (s_publisherMap.contains(lower)) {
        return s_publisherMap[lower];
    }
    for (auto it = s_publisherMap.constBegin(); it != s_publisherMap.constEnd(); ++it) {
        if (lower.startsWith(it.key())) {
            return it.value();
        }
    }
    return {};
}

// ============================================================================
// Results Table
// ============================================================================

void AppInstallationPanel::updateResultsFromSearch(const QString& output) {
    auto packages = m_choco_manager->parseSearchResults(output);

    m_onlineResultsModel->setRowCount(static_cast<int>(packages.size()));

    int row = 0;
    for (const auto& pkg : packages) {
        auto* pkg_item = new QStandardItem(pkg.package_id);
        pkg_item->setIcon(publisherIcon(pkg.package_id));
        m_onlineResultsModel->setItem(row, RColPackage, pkg_item);

        m_onlineResultsModel->setItem(row, RColVersion, new QStandardItem(pkg.version));

        const QString pub = lookupPublisher(pkg.package_id);
        m_onlineResultsModel->setItem(row, RColPublisher, new QStandardItem(pub));

        row++;
    }

    m_onlineResultsTable->scrollToTop();

    const int count = static_cast<int>(packages.size());
    Q_EMIT logOutput(QString("Search returned %1 result(s)").arg(count));
    Q_EMIT statusMessage(tr("Found %1 package(s)").arg(count), sak::kTimerStatusMessageMs);
}

// ============================================================================
// Queue Display
// ============================================================================

void AppInstallationPanel::updateQueueDisplay() {
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

    const bool has_items = !m_installQueue.isEmpty();
    // Gate on the single in-flight authority, not the online flag alone: an offline
    // deployment operation drives the same Chocolatey install root, so Install must stay
    // disabled while it runs.
    const bool idle = !packageOperationInFlight();
    m_clearQueueButton->setEnabled(has_items && idle);
    m_installButton->setEnabled(has_items && idle);
    m_saveQueueButton->setEnabled(has_items);
    m_removeFromQueueButton->setEnabled(false);  // Reset until selection
}

// ============================================================================
// Controls
// ============================================================================

void AppInstallationPanel::enableControls(bool enabled) {
    m_searchButton->setEnabled(enabled);
    m_searchEdit->setEnabled(enabled);
    m_onlinePresetCombo->setEnabled(enabled);
    m_addToQueueButton->setEnabled(false);  // Re-enabled on selection change
    m_removeFromQueueButton->setEnabled(false);
    m_clearQueueButton->setEnabled(enabled && !m_installQueue.isEmpty());
    m_installButton->setEnabled(enabled && !m_installQueue.isEmpty());
}

// ============================================================================
// Save / Load Queue
// ============================================================================

void AppInstallationPanel::saveQueueToFile() {
    if (m_installQueue.isEmpty()) {
        sak::showInformationLogged(this,
                                   tr("Save App List"),
                                   tr("The install queue is empty. Add packages before saving."));
        return;
    }

    const QString file_path = QFileDialog::getSaveFileName(
        this, tr("Save App List"), QString(), tr("JSON Files (*.json)"));
    if (file_path.isEmpty()) {
        return;
    }

    QJsonArray arr;
    for (const auto& entry : m_installQueue) {
        QJsonObject obj;
        obj["package_id"] = entry.package_id;
        obj["version"] = entry.version;
        obj["publisher"] = entry.publisher;
        arr.append(obj);
    }

    const QJsonDocument doc(arr);
    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        sak::logWarning(("Save Failed: Could not write to file: " + file_path).toStdString());
        sak::showWarningLogged(this,
                               tr("Save Failed"),
                               tr("Could not write to file:\n%1").arg(file_path));
        return;
    }
    const QByteArray json_bytes = doc.toJson(QJsonDocument::Indented);
    // Fail closed: a short write or a failed flush leaves a truncated/corrupt file on
    // disk. Surface it as a save failure instead of reporting a clean "Saved".
    if (file.write(json_bytes) != json_bytes.size() || !file.flush()) {
        sak::logWarning("Incomplete write to file: {}", file_path.toStdString());
        file.close();
        sak::showWarningLogged(this,
                               tr("Save Failed"),
                               tr("Could not write the full app list to file:\n%1").arg(file_path));
        return;
    }
    file.close();

    Q_EMIT logOutput(
        QString("Saved %1 package(s) to %2").arg(m_installQueue.size()).arg(file_path));
    Q_EMIT statusMessage(QString("App list saved (%1 packages)").arg(m_installQueue.size()),
                         kQueueSaveStatusMessageMs);
}

void AppInstallationPanel::loadQueueFromFile() {
    const QString file_path = QFileDialog::getOpenFileName(
        this, tr("Load App List"), QString(), tr("JSON Files (*.json)"));
    if (file_path.isEmpty()) {
        return;
    }

    QJsonArray arr;
    if (!parseQueueFile(file_path, arr)) {
        return;
    }

    int added = 0;
    int skipped = 0;
    importQueueEntries(arr, added, skipped);

    updateQueueDisplay();

    QString msg = QString("Loaded %1 package(s)").arg(added);
    if (skipped > 0) {
        msg += QString(", %1 skipped (duplicate or invalid)").arg(skipped);
    }
    Q_EMIT logOutput(msg + QString(" from %1").arg(file_path));
    Q_EMIT statusMessage(msg, sak::kTimerStatusMessageMs);
}

bool AppInstallationPanel::parseQueueFile(const QString& file_path, QJsonArray& out_array) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        sak::logWarning(("Load Failed: Could not read file: " + file_path).toStdString());
        sak::showWarningLogged(this,
                               tr("Load Failed"),
                               tr("Could not read file:\n%1").arg(file_path));
        return false;
    }

    // Bound the read: the queue file is user-selected and otherwise fed whole into
    // readAll()/JSON parse. Reject an oversized or unreadable-size file so a hostile or
    // corrupt path cannot exhaust memory. A real app list is a few KB.
    constexpr qint64 kMaxQueueFileBytes = 8LL * 1024 * 1024;
    const qint64 file_size = file.size();
    if (file_size < 0 || file_size > kMaxQueueFileBytes) {
        sak::logWarning(
            ("Load Failed: App list file too large or unreadable: " + file_path).toStdString());
        sak::showWarningLogged(this,
                               tr("Load Failed"),
                               tr("App list file is too large or unreadable:\n%1").arg(file_path));
        file.close();
        return false;
    }

    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse_error);
    file.close();

    if (parse_error.error != QJsonParseError::NoError) {
        sak::logWarning(("Load Failed: Invalid JSON: " + parse_error.errorString()).toStdString());
        sak::showWarningLogged(this,
                               tr("Load Failed"),
                               tr("Invalid JSON:\n%1").arg(parse_error.errorString()));
        return false;
    }

    if (!doc.isArray()) {
        sak::logWarning("Load Failed: Expected a JSON array of packages.");
        sak::showWarningLogged(this, tr("Load Failed"), tr("Expected a JSON array of packages."));
        return false;
    }

    out_array = doc.array();
    return true;
}

void AppInstallationPanel::importQueueEntries(const QJsonArray& arr, int& added, int& skipped) {
    for (const auto& val : arr) {
        // A malformed entry must be surfaced, not silently vanish: count it as
        // skipped so a partial import can never be reported as a clean load.
        if (!val.isObject()) {
            skipped++;
            continue;
        }
        QJsonObject obj = val.toObject();
        QString pkg_id = obj["package_id"].toString().trimmed();
        // Fail closed on a malformed/option-like id or version rather than letting it
        // reach the privileged install queue; count it as skipped so a partial import
        // is never reported as a clean load.
        if (!queuePackageIdIsValid(pkg_id)) {
            skipped++;
            continue;
        }
        const QString version = obj["version"].toString();
        if (!queueVersionIsValid(version)) {
            skipped++;
            continue;
        }

        const bool duplicate = std::ranges::any_of(m_installQueue,

                                                   [&pkg_id](const auto& existing) {
                                                       return existing.package_id == pkg_id;
                                                   });
        if (duplicate) {
            skipped++;
            continue;
        }

        QueueEntry entry;
        entry.package_id = pkg_id;
        entry.version = version;
        entry.publisher = obj["publisher"].toString();
        m_installQueue.append(entry);
        added++;
    }
}
