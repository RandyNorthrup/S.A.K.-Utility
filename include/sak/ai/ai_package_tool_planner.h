// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

struct AiPackageToolPlan {
    QString operation;
    QString query;
    QString package_id;
    QString version;
    int timeout_seconds = 1800;
    bool query_operation = false;
    bool read_operation = false;
    bool change_operation = false;
    QString error_message;

    [[nodiscard]] bool ok() const { return error_message.isEmpty(); }
};

class AiPackageToolPlanner {
public:
    [[nodiscard]] static AiPackageToolPlan buildPlan(const QJsonObject& args);
    [[nodiscard]] static QString safePackageToken(const QString& value);
    [[nodiscard]] static bool isQueryOperation(const QString& operation);
    [[nodiscard]] static bool isReadOperation(const QString& operation);
    [[nodiscard]] static bool isChangeOperation(const QString& operation);
};

}  // namespace sak::ai
