// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Clear Print Spooler Action
 * 
 * Stops spooler service, clears stuck print jobs, restarts service.
 */
class ClearPrintSpoolerAction : public QuickAction {
    Q_OBJECT

public:
    explicit ClearPrintSpoolerAction(QObject* parent = nullptr);

    QString name() const override { return "Clear Print Spooler"; }
    QString description() const override { return "Clear stuck print jobs"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::Maintenance; }
    bool requiresAdmin() const override { return true; }

    void scan() override;
    void execute() override;

private:
    int m_stuck_jobs{0};
    qint64 m_spooler_size{0};

    void stopSpooler();
    void clearSpoolFolder();
    void startSpooler();
    int countSpoolFiles();
};

} // namespace sak

