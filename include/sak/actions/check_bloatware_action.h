// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"

#include <QDateTime>
#include <QString>
#include <QVector>

namespace sak {

/**
 * @brief Check Bloatware Action
 *
 * Scans for and removes common Windows bloatware and vendor junk.
 */
class CheckBloatwareAction : public QuickAction {
    Q_OBJECT

public:
    explicit CheckBloatwareAction(QObject* parent = nullptr);

    QString name() const override { return "Check for Bloatware"; }
    QString description() const override { return "Detect and remove Windows bloatware"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    /// @brief Represents a single item of pre-installed bloatware detected on the system
    struct BloatwareItem {
        QString name;
        QString type;  // UWP App, Win32 Program, Startup Item
        qint64 size;
        QString removal_method;
        bool is_safe_to_remove;
    };

    QVector<BloatwareItem> m_bloatware;
    qint64 m_total_size{0};

    /// @brief Accumulated bloatware scan output data
    struct BloatwareScanResult {
        QString report;
        QString structured_output;
        int bloatware_count = 0;
        qint64 total_size = 0;
        int apps_scanned = 0;
    };

    /// @brief Statistics from bloatware pattern matching
    struct BloatwareMatchStats {
        int apps_count = 0;
        int installed_scanned = 0;
        int provisioned_scanned = 0;
        int safe_to_remove = 0;
    };

    QVector<BloatwareItem> scanForBloatware();
    BloatwareItem buildBloatwareItem(const QString& name,
                                     const QString& package_full,
                                     const QString& source,
                                     double size_mb,
                                     bool is_safe);
    void scanUWPApps();
    void scanVendorSoftware();
    void scanStartupBloat();
    bool isBloatware(const QString& app_name);

    struct ClassificationState {
        QSet<QString> seen;
        int installed_scanned = 0;
        int provisioned_scanned = 0;
        int safe_to_remove = 0;
        QVector<QPair<QString, QPair<QString, double>>> detected_bloatware;
    };

    // TigerStyle helpers for execute() decomposition
    void executeScanApps(const QDateTime& start_time, QString& scan_output, QString& report);
    void executeMatchBloatware(const QString& scan_output, BloatwareScanResult& result);
    void classifyAppEntry(const QJsonObject& app,
                          const QMap<QString, QPair<QString, bool>>& bloatware_patterns,
                          ClassificationState& state,
                          BloatwareScanResult& result);
    void formatBloatwareMatchReport(const QVector<QPair<QString, QPair<QString, double>>>& detected,
                                    const BloatwareMatchStats& stats,
                                    BloatwareScanResult& result);
    void executeBuildReport(const QDateTime& start_time, BloatwareScanResult& result);
};

}  // namespace sak
