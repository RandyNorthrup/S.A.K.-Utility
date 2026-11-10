// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/layout_constants.h"

#include <QByteArray>
#include <QList>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;

namespace sak {

inline constexpr int kDefaultNetworkTransferTimeoutMs = kTimeoutProcessLongMs;

enum class NetworkTransferMethod {
    Get,
    Post,
    Head
};

struct NetworkTransferRequest {
    QUrl url;
    NetworkTransferMethod method{NetworkTransferMethod::Get};
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> raw_headers;
    /// Per-transfer deadline. MUST be positive: a value <= 0 would disable Qt's transfer
    /// timeout and leave the deadline timer unarmed, letting a stalled server block the
    /// synchronous runNetworkTransfer() call forever. Such a request is rejected outright.
    int timeout_ms{kDefaultNetworkTransferTimeoutMs};
    /// Abort once the response body exceeds this many bytes, bounding the in-memory
    /// buffer so a hostile/oversized remote response cannot exhaust memory. A value <= 0
    /// is NOT unlimited: runNetworkTransfer clamps it to a 512 MiB safety cap so an
    /// omitted limit can never permit unbounded buffering.
    qint64 max_response_bytes{0};
};

struct NetworkTransferResult {
    bool success{false};
    bool timed_out{false};
    bool cancelled{false};
    int http_status{0};
    QByteArray body;
    QString error_message;
    qint64 elapsed_ms{0};
    qint64 bytes_received{0};
    qint64 bytes_total{0};
};

using NetworkCancelCheck = std::function<bool()>;
using NetworkProgressCallback = std::function<void(qint64 received, qint64 total)>;

/// @brief Bounded asynchronous GET used by small metadata/checksum consumers.
struct NetworkMetadataRequest {
    /// HTTPS only; other schemes fail before QNetworkAccessManager sees them.
    QUrl url;
    QList<QPair<QByteArray, QByteArray>> raw_headers;
    /// Total deadline and Qt inactivity timeout. Non-positive values fail closed.
    int timeout_ms{kDefaultNetworkTransferTimeoutMs};
    /// Hard streamed/body cap. Non-positive values fail closed rather than becoming unlimited.
    qint64 max_response_bytes{0};
};

using NetworkMetadataFaultInjector =
    std::function<std::optional<NetworkTransferResult>(const NetworkMetadataRequest& request)>;

/// @brief RAII test seam for deterministic offline/timeout/HTTP/body responses.
class ScopedNetworkMetadataFaultInjector {
public:
    explicit ScopedNetworkMetadataFaultInjector(NetworkMetadataFaultInjector injector);
    ~ScopedNetworkMetadataFaultInjector();
    ScopedNetworkMetadataFaultInjector(const ScopedNetworkMetadataFaultInjector&) = delete;
    ScopedNetworkMetadataFaultInjector& operator=(const ScopedNetworkMetadataFaultInjector&) =
        delete;
    ScopedNetworkMetadataFaultInjector(ScopedNetworkMetadataFaultInjector&&) = delete;
    ScopedNetworkMetadataFaultInjector& operator=(ScopedNetworkMetadataFaultInjector&&) = delete;

private:
    NetworkMetadataFaultInjector m_previous;
};

/// @brief Start one HTTPS metadata GET. Abort the returned reply to cancel it; completion remains
///        asynchronous and QNetworkReply::finished-compatible.
/// @return Reply owned by @p manager; never null. Invalid requests finish with an explicit error.
[[nodiscard]] QNetworkReply* startNetworkMetadataGet(QNetworkAccessManager& manager,
                                                     const NetworkMetadataRequest& request);

/// @brief Consume a finished metadata reply. Success requires 2xx, bounded body, and no error.
[[nodiscard]] NetworkTransferResult takeNetworkMetadataResult(QNetworkReply* reply);

[[nodiscard]] NetworkTransferResult runNetworkTransfer(
    const NetworkTransferRequest& request,
    const NetworkCancelCheck& should_cancel = {},
    const NetworkProgressCallback& progress = {});

}  // namespace sak
