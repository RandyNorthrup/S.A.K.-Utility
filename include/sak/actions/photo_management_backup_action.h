// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include "sak/user_profile_types.h"
#include <QDir>
#include <QPair>
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Photo Management Backup Action
 * 
 * Backs up Lightroom catalogs, Photoshop settings, and photo editing software data.
 */
class PhotoManagementBackupAction : public QuickAction {
    Q_OBJECT

public:
    explicit PhotoManagementBackupAction(const QString& backup_location, QObject* parent = nullptr);

    QString name() const override { return "Photo Management Backup"; }
    QString description() const override { return "Backup Lightroom catalogs and Photoshop settings"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::QuickBackup; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Represents a photo editing software data location (catalog, presets, or settings)
    struct PhotoSoftwareData {
        QString software_name;
        QString data_type; // Catalog, Presets, Settings
        QString path;
        qint64 size;
    };

    QString m_backup_location;
    QVector<UserProfile> m_user_profiles;
    QVector<PhotoSoftwareData> m_photo_data;
    qint64 m_total_size{0};

    void scanLightroomCatalogs();
    void scanPhotoshopSettings();
    void scanCaptureOne();

    /// @brief Backup a single photo software data item (file or directory)
    /// @return {success, bytes_copied}
    QPair<bool, qint64> backupPhotoItem(const PhotoSoftwareData& data, const QDir& backup_dir);
};

} // namespace sak

