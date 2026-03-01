// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QString>
#include <QList>

namespace sak {

/**
 * @brief Backup Known Networks Action
 *
 * Scans all Windows known WiFi profiles via netsh and saves them to a JSON file
 * compatible with the WiFi Manager panel's Load function.
 *
 * Output format: JSON array of { location, ssid, password, security, hidden }
 */
class BackupKnownNetworksAction : public QuickAction {
    Q_OBJECT

public:
    explicit BackupKnownNetworksAction(const QString& backup_location, QObject* parent = nullptr);

    QString name()        const override { return "Backup Known Networks"; }
    QString description() const override { return "Export all known WiFi profiles to a JSON file loadable in WiFi Manager"; }
    QIcon   icon()        const override { return QIcon(); }
    ActionCategory category()     const override { return ActionCategory::QuickBackup; }
    bool   requiresAdmin()        const override { return false; }

    void scan()    override;
    void execute() override;

private:
    struct NetworkEntry {
        QString ssid;
        QString password;
        QString security;
        bool    hidden{false};
    };

    QString              m_backup_location;
    QList<NetworkEntry>  m_entries;  // populated during scan, reused in execute

    /** Collect all known WiFi profile details via netsh */
    QList<NetworkEntry> collectProfiles() const;

    /** Fetch detail for a single WiFi profile and populate a NetworkEntry */
    NetworkEntry fetchProfileDetail(
        const QString& name,
        const QRegularExpression& keyRe,
        const QRegularExpression& authRe,
        const QRegularExpression& nbRe) const;

    /// @brief Build JSON from collected profiles and write to backup file
    void buildAndWriteBackup(const QList<NetworkEntry>& entries, const QDateTime& startTime);
};

} // namespace sak
