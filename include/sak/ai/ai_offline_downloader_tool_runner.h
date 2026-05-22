// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

struct AiOfflineDownloaderToolCallbacks {
    std::function<bool()> is_running;
    std::function<QJsonObject()> presets_result;
    std::function<QJsonObject(const QJsonObject& args, const QString& query)> search_result;
    std::function<QJsonObject(const QJsonObject& args, const QString& operation)> run_operation;
};

class AiOfflineDownloaderToolRunner {
public:
    [[nodiscard]] static QJsonObject run(const QJsonObject& args,
                                         const AiOfflineDownloaderToolCallbacks& callbacks);
};

}  // namespace sak::ai
