// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_package_tool_planner.h"

#include <QJsonValue>

#include <algorithm>

namespace sak::ai {

namespace {
constexpr int kPackageToolDefaultTimeoutSeconds = 1800;
constexpr int kPackageToolMinTimeoutSeconds = 5;
constexpr int kPackageToolMaxTimeoutSeconds = 7200;

bool isAllowedPackageChar(QChar ch) {
    return ch.isLetterOrNumber() || ch == QLatin1Char('.') || ch == QLatin1Char('-') ||
           ch == QLatin1Char('_') || ch == QLatin1Char('+');
}

// A package id is valid only if it is non-empty, not option-like (a leading '-' would
// be parsed by Chocolatey as an option, e.g. -s / --force), and every character is
// allowed. Used to REJECT (never rewrite) ids with disallowed characters.
bool packageTokenIsValid(const QString& trimmed) {
    return !trimmed.isEmpty() && !trimmed.startsWith(QLatin1Char('-')) &&
           std::all_of(trimmed.cbegin(), trimmed.cend(), isAllowedPackageChar);
}

// Fail closed: a version that is PRESENT but not a string must be rejected rather
// than silently collapsing to "" (== install "latest"), which could install an
// unintended build. An absent/null version legitimately means "latest". Returns a
// non-empty error string on rejection, and an empty string when the value is allowed.
QString packageVersionError(const QJsonValue& version_val) {
    if (!version_val.isUndefined() && !version_val.isNull() && !version_val.isString()) {
        return QStringLiteral("Invalid version: expected a string");
    }
    return {};
}
}  // namespace

bool AiPackageToolPlanner::isQueryOperation(const QString& operation) {
    return operation == QLatin1String("version") || operation == QLatin1String("search") ||
           operation == QLatin1String("outdated");
}

bool AiPackageToolPlanner::isReadOperation(const QString& operation) {
    return operation == QLatin1String("is_installed") ||
           operation == QLatin1String("installed_version");
}

bool AiPackageToolPlanner::isChangeOperation(const QString& operation) {
    return operation == QLatin1String("install") || operation == QLatin1String("uninstall") ||
           operation == QLatin1String("upgrade");
}

AiPackageToolPlan AiPackageToolPlanner::buildPlan(const QJsonObject& args) {
    AiPackageToolPlan plan;
    plan.operation = args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    plan.query = args.value(QStringLiteral("query")).toString().trimmed();
    const QString raw_package_id = args.value(QStringLiteral("package_id")).toString().trimmed();
    plan.timeout_seconds = std::clamp(
        args.value(QStringLiteral("timeout_seconds")).toInt(kPackageToolDefaultTimeoutSeconds),
        kPackageToolMinTimeoutSeconds,
        kPackageToolMaxTimeoutSeconds);

    if (plan.operation.isEmpty()) {
        plan.error_message = QStringLiteral("Package manager requires operation");
        return plan;
    }
    const QJsonValue version_val = args.value(QStringLiteral("version"));
    const QString version_error = packageVersionError(version_val);
    if (!version_error.isEmpty()) {
        plan.error_message = version_error;
        return plan;
    }
    plan.version = version_val.toString().trimmed();
    // Fail closed: a query beginning with '-' would be parsed by Chocolatey as an
    // option (e.g. --source=https://attacker), not a search term -- argument injection.
    if (plan.query.startsWith(QLatin1Char('-'))) {
        plan.error_message = QStringLiteral("Invalid search query: %1").arg(plan.query);
        return plan;
    }
    // Fail closed: an id with disallowed characters is REJECTED, never silently
    // rewritten by deleting characters -- a rewrite could resolve to a DIFFERENT
    // real package and install/uninstall the wrong thing.
    if (!raw_package_id.isEmpty() && !packageTokenIsValid(raw_package_id)) {
        plan.error_message = QStringLiteral("Invalid package identifier: %1").arg(raw_package_id);
        return plan;
    }
    plan.package_id = raw_package_id;

    plan.query_operation = isQueryOperation(plan.operation);
    plan.read_operation = isReadOperation(plan.operation);
    plan.change_operation = isChangeOperation(plan.operation);
    if (!plan.query_operation && !plan.read_operation && !plan.change_operation) {
        plan.error_message =
            QStringLiteral("Unsupported package manager operation: %1").arg(plan.operation);
    }
    return plan;
}

}  // namespace sak::ai
