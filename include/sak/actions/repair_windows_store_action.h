// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Repair Windows Store Action
 * 
 * Resets Windows Store app and re-registers all UWP apps.
 */
class RepairWindowsStoreAction : public QuickAction {
    Q_OBJECT

public:
    explicit RepairWindowsStoreAction(QObject* parent = nullptr);

    QString name() const override { return "Repair Windows Store"; }
    QString description() const override { return "Reset Store and re-register UWP apps"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Troubleshooting; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    /// @brief Status and version information for the Windows Store package
    struct StorePackageInfo {
        QString name;
        QString version;
        QString publisher;
        QString status;
        bool is_installed;
        bool is_registered;
    };
    
    StorePackageInfo checkStorePackage();
    bool resetWindowsStoreCache();
    bool resetStorePackage();
    bool reregisterWindowsStore();
    bool resetStoreServices();
    int checkStoreEventLogs();

    /// @brief Build the diagnostic report string from repair phase results
    QString buildRepairReport(const StorePackageInfo& before_info, int error_count,
                              bool cache_reset, bool package_reset,
                              bool reregistered, bool services_restarted,
                              const StorePackageInfo& after_info, int post_error_count);
    void finalizeRepairResult(bool cache_reset, bool package_reset, bool reregistered,
                              bool services_restarted, const StorePackageInfo& after_info,
                              const QString& report, const QDateTime& start_time);
};

} // namespace sak

