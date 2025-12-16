// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
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
    struct BloatwareItem {
        QString name;
        QString type; // UWP App, Win32 Program, Startup Item
        qint64 size;
        QString removal_method;
        bool is_safe_to_remove;
    };

    QVector<BloatwareItem> m_bloatware;
    qint64 m_total_size{0};

    QVector<BloatwareItem> scanForBloatware();
    void scanUWPApps();
    void scanVendorSoftware();
    void scanStartupBloat();
    bool isBloatware(const QString& app_name);
};

} // namespace sak

