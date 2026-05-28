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
    int timeout_ms{kDefaultNetworkTransferTimeoutMs};
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

[[nodiscard]] NetworkTransferResult runNetworkTransfer(
    const NetworkTransferRequest& request,
    const NetworkCancelCheck& should_cancel = {},
    const NetworkProgressCallback& progress = {});

}  // namespace sak
