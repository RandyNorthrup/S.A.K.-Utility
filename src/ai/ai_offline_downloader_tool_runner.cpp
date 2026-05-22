// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_offline_downloader_tool_runner.h"

namespace sak::ai {

namespace {

QJsonObject toolError(const QString& message) {
    QJsonObject result;
    result[QStringLiteral("success")] = false;
    result[QStringLiteral("error_message")] = message;
    return result;
}

bool runOperationSupported(const QString& operation) {
    return operation == QLatin1String("direct_download") ||
           operation == QLatin1String("build_bundle") ||
           operation == QLatin1String("install_bundle");
}

}  // namespace

QJsonObject AiOfflineDownloaderToolRunner::run(const QJsonObject& args,
                                               const AiOfflineDownloaderToolCallbacks& callbacks) {
    const QString operation =
        args.value(QStringLiteral("operation")).toString().trimmed().toLower();
    const QString query = args.value(QStringLiteral("query")).toString().trimmed();

    if (operation.isEmpty()) {
        return toolError(QStringLiteral("Offline downloader requires operation"));
    }
    if (operation == QLatin1String("presets")) {
        return callbacks.presets_result
                   ? callbacks.presets_result()
                   : toolError(
                         QStringLiteral("Offline downloader presets callback is not configured"));
    }
    if (operation == QLatin1String("search")) {
        return callbacks.search_result
                   ? callbacks.search_result(args, query)
                   : toolError(
                         QStringLiteral("Offline downloader search callback is not configured"));
    }
    if (!runOperationSupported(operation)) {
        return toolError(
            QStringLiteral("Unsupported offline downloader operation: %1").arg(operation));
    }
    if (!callbacks.is_running || !callbacks.run_operation) {
        return toolError(QStringLiteral("Offline downloader runner is not configured"));
    }
    if (callbacks.is_running()) {
        return toolError(QStringLiteral("An offline deployment operation is already running"));
    }
    return callbacks.run_operation(args, operation);
}

}  // namespace sak::ai
