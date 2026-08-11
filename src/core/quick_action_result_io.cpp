// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/quick_action_result_io.h"

#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <cmath>
#include <limits>

namespace sak {

namespace {

// A result counter (bytes/files/duration) is a non-negative integer. An absent
// counter defaults to zero (matching the historical toDouble(0)); a value that is
// present but non-numeric, negative, non-finite, or outside the qint64 range means
// the result file is corrupt -- fail closed rather than truncate, because casting
// an out-of-range double straight to qint64 is undefined behavior.
bool readResultCounter(const QJsonValue& value, qint64* out) {
    if (value.isUndefined() || value.isNull()) {
        *out = 0;
        return true;
    }
    if (!value.isDouble()) {
        return false;
    }
    const double d = value.toDouble();
    // qint64::max() rounds UP to 2^63 as a double, so reject with >= to keep the
    // cast in-range (a value at or above 2^63 is not representable as qint64).
    if (!std::isfinite(d) || d < 0.0 ||
        d >= static_cast<double>(std::numeric_limits<qint64>::max())) {
        return false;
    }
    *out = static_cast<qint64>(d);
    return true;
}

}  // namespace

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
    // Reads a status out of a persisted result file. An empty or unrecognised
    // value maps to Idle, which is the conservative answer: it never claims an
    // action succeeded. No assert -- a damaged result file is data.
    const QString normalized = status.trimmed().toLower();
    if (normalized == "scanning") {
        return QuickAction::ActionStatus::Scanning;
    }
    if (normalized == "ready") {
        return QuickAction::ActionStatus::Ready;
    }
    if (normalized == "running") {
        return QuickAction::ActionStatus::Running;
    }
    if (normalized == "success") {
        return QuickAction::ActionStatus::Success;
    }
    if (normalized == "failed") {
        return QuickAction::ActionStatus::Failed;
    }
    if (normalized == "cancelled") {
        return QuickAction::ActionStatus::Cancelled;
    }
    return QuickAction::ActionStatus::Idle;
}

bool writeExecutionResultFile(const QString& file_path,
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

    const QJsonDocument doc(json);

    QFile file(file_path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error_message != nullptr) {
            *error_message = QString("Failed to write result file: %1").arg(file.errorString());
        }
        return false;
    }

    const QByteArray json_bytes = doc.toJson(QJsonDocument::Compact);
    if (file.write(json_bytes) != json_bytes.size()) {
        if (error_message != nullptr) {
            *error_message = QStringLiteral("Incomplete write of result file");
        }
        return false;
    }
    return true;
}

namespace {

// Opens a persisted result file and hands back its top-level JSON object. Returns an
// empty string on success (and fills *json); on any failure returns the caller-facing
// error text and leaves *json untouched -- so the caller propagates one string instead
// of repeating the open/size/parse fail-closed branches inline.
QString loadResultJsonObject(const QString& file_path, QJsonObject* json) {
    QFile file(file_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return QString("Failed to read result file: %1").arg(file.errorString());
    }

    // A result file is a handful of small scalar fields; a multi-megabyte one is corrupt or
    // hostile (this path is fed by an on-disk file). Bound it so an oversized/attacker-swapped
    // file cannot force an unbounded allocation via readAll(); fail closed rather than slurp it.
    constexpr qint64 kMaxResultFileBytes = 4 * 1024 * 1024;
    if (file.size() > kMaxResultFileBytes) {
        return QStringLiteral("Result file is too large to be a valid result.");
    }

    auto data = file.readAll();
    QJsonParseError parse_error;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &parse_error);
    if (doc.isNull() || !doc.isObject()) {
        return QString("Invalid result file JSON: %1").arg(parse_error.errorString());
    }

    *json = doc.object();
    return QString();
}

// Fills *result from the parsed object. Returns an empty string on success, or the
// caller-facing error text when a counter field is corrupt. The counters go through
// readResultCounter (which fails closed on non-numeric/negative/out-of-range values);
// the string/bool fields use Qt's coercing accessors, matching the historical defaults.
QString populateExecutionResult(const QJsonObject& json, QuickAction::ExecutionResult* result) {
    qint64 bytes_processed = 0;
    qint64 files_processed = 0;
    qint64 duration_ms = 0;
    if (!readResultCounter(json.value("bytes_processed"), &bytes_processed) ||
        !readResultCounter(json.value("files_processed"), &files_processed) ||
        !readResultCounter(json.value("duration_ms"), &duration_ms)) {
        return QStringLiteral(
            "Result file has an invalid numeric field (non-numeric, negative, or out of "
            "range).");
    }
    result->success = json.value("success").toBool(false);
    result->message = json.value("message").toString();
    result->bytes_processed = bytes_processed;
    result->files_processed = files_processed;
    result->duration_ms = duration_ms;
    result->output_path = json.value("output_path").toString();
    result->log = json.value("log").toString();
    return QString();
}

}  // namespace

bool readExecutionResultFile(const QString& file_path,
                             QuickAction::ExecutionResult* result,
                             QuickAction::ActionStatus* status,
                             QString* error_message) {
    QJsonObject json;
    const QString load_error = loadResultJsonObject(file_path, &json);
    if (!load_error.isEmpty()) {
        if (error_message != nullptr) {
            *error_message = load_error;
        }
        return false;
    }

    if (result != nullptr) {
        const QString populate_error = populateExecutionResult(json, result);
        if (!populate_error.isEmpty()) {
            if (error_message != nullptr) {
                *error_message = populate_error;
            }
            return false;
        }
    }

    if (status != nullptr) {
        *status = actionStatusFromString(json.value("status").toString());
    }

    return true;
}

}  // namespace sak
