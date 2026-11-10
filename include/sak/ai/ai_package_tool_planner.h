// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

namespace sak::ai {

inline constexpr int kDefaultPackageToolTimeoutSeconds = 1800;

struct AiPackageToolPlan {
    QString operation;
    QString query;
    QString package_id;
    QString version;
    int timeout_seconds = kDefaultPackageToolTimeoutSeconds;
    bool query_operation = false;
    bool read_operation = false;
    bool change_operation = false;
    QString error_message;

    // Fail closed: a plan is usable only when it carries no error AND resolves to
    // exactly one recognized operation class. A default-constructed or otherwise
    // unclassified plan (blank operation, no class) is therefore NOT ok(), so an
    // accidental default return can never expose a blank/stale execution target.
    [[nodiscard]] bool ok() const {
        return error_message.isEmpty() && (query_operation || read_operation || change_operation);
    }
};

class AiPackageToolPlanner {
public:
    [[nodiscard]] static AiPackageToolPlan buildPlan(const QJsonObject& args);
    [[nodiscard]] static bool isQueryOperation(const QString& operation);
    [[nodiscard]] static bool isReadOperation(const QString& operation);
    [[nodiscard]] static bool isChangeOperation(const QString& operation);
};

}  // namespace sak::ai
