// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/ai/ai_offline_downloader_tool_runner.h"

#include "sak/ai/ai_tool_error.h"

#include <atomic>

namespace sak::ai {

namespace {

std::atomic_bool g_run_operation_claimed{false};

class ScopedRunOperationClaim final {
public:
    ScopedRunOperationClaim() {
        bool expected = false;
        m_acquired = g_run_operation_claimed.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel, std::memory_order_acquire);
    }

    ~ScopedRunOperationClaim() {
        if (m_acquired) {
            g_run_operation_claimed.store(false, std::memory_order_release);
        }
    }

    ScopedRunOperationClaim(const ScopedRunOperationClaim&) = delete;
    ScopedRunOperationClaim& operator=(const ScopedRunOperationClaim&) = delete;

    [[nodiscard]] bool acquired() const { return m_acquired; }

private:
    bool m_acquired{false};
};

bool runOperationSupported(const QString& operation) {
    return operation == QLatin1String("direct_download") ||
           operation == QLatin1String("build_bundle") ||
           operation == QLatin1String("install_bundle");
}

QJsonObject runSupportedOperation(const QJsonObject& args,
                                  const QString& operation,
                                  const AiOfflineDownloaderToolCallbacks& callbacks) {
    if (!callbacks.is_running || !callbacks.run_operation) {
        return toolError(QStringLiteral("Offline downloader runner is not configured"));
    }
    // Claim check plus worker dispatch as one process-wide critical section. Worker isRunning()
    // remains runtime authority, but checking it alone lets two callers both observe idle before
    // either callback starts worker.
    const ScopedRunOperationClaim operation_claim;
    if (!operation_claim.acquired() || callbacks.is_running()) {
        return toolError(QStringLiteral("An offline deployment operation is already running"));
    }
    return callbacks.run_operation(args, operation);
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
    return runSupportedOperation(args, operation, callbacks);
}

}  // namespace sak::ai
