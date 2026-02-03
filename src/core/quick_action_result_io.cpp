// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#include "sak/quick_action_result_io.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace sak {

QString actionStatusToString(QuickAction::ActionStatus status) {
    switch (status) {
        case QuickAction::ActionStatus::Idle:
            return "Idle";
        case QuickAction::ActionStatus::Scanning:
            return "Scanning";
        case QuickAction::ActionStatus::Ready:
            return "Ready";
        case QuickAction::ActionStatus::Running:
            return "Running";
        case QuickAction::ActionStatus::Success:
            return "Success";
        case QuickAction::ActionStatus::Failed:
            return "Failed";
        case QuickAction::ActionStatus::Cancelled:
            return "Cancelled";
    }

    return "Idle";
}

QuickAction::ActionStatus actionStatusFromString(const QString& status) {
    const QString normalized = status.trimmed().toLower();
    if (normalized == "scanning") return QuickAction::ActionStatus::Scanning;
    if (normalized == "ready") return QuickAction::ActionStatus::Ready;
    if (normalized == "running") return QuickAction::ActionStatus::Running;
    if (normalized == "success") return QuickAction::ActionStatus::Success;
    if (normalized == "failed") return QuickAction::ActionStatus::Failed;
    if (normalized == "cancelled") return QuickAction::ActionStatus::Cancelled;
    return QuickAction::ActionStatus::Idle;
}

bool writeExecutionResultFile(
    const QString& file_path,
    const QuickAction::ExecutionResult& result,
    QuickAction::ActionStatus status,
    QString* error_message) {
    QJsonObject json;
    json.insert("success", result.success);
    json.insert("message", result.message);
    json.insert("bytes_processed", static_cast<double>(result.bytes_processed));
    json.insert("files_processed", static_cast<double>(result.files_processed));
    json.insert("duration_ms", static_cast<double>(result.duration_ms));
    json.insert("output_path", result.output_path);
    json.insert("log", result.log);
    json.insert("status", actionStatusToString(status));

    QJsonDocument doc(json);

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_message) {
            *error_message = QString("Failed to write result file: %1").arg(file.errorString());
        }
        return false;
    }

    file.write(doc.toJson(QJsonDocument::Compact));
    return true;
}

bool readExecutionResultFile(
    const QString& file_path,
    QuickAction::ExecutionResult* result,
    QuickAction::ActionStatus* status,
    QString* error_message) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error_message) {
            *error_message = QString("Failed to read result file: %1").arg(file.errorString());
        }
        return false;
    }

    auto data = file.readAll();
    QJsonParseError parse_error;
    QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (doc.isNull() || !doc.isObject()) {
        if (error_message) {
            *error_message = QString("Invalid result file JSON: %1").arg(parse_error.errorString());
        }
        return false;
    }

    QJsonObject json = doc.object();
    if (result) {
        result->success = json.value("success").toBool(false);
        result->message = json.value("message").toString();
        result->bytes_processed = static_cast<qint64>(json.value("bytes_processed").toDouble(0));
        result->files_processed = static_cast<qint64>(json.value("files_processed").toDouble(0));
        result->duration_ms = static_cast<qint64>(json.value("duration_ms").toDouble(0));
        result->output_path = json.value("output_path").toString();
        result->log = json.value("log").toString();
    }

    if (status) {
        *status = actionStatusFromString(json.value("status").toString());
    }

    return true;
}

} // namespace sak
