// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QJsonObject>
#include <QString>

#include <functional>

namespace sak::ai {

/// @brief Host callbacks for the local offline-downloader tool surface.
///
/// `presets_result` is required only for `presets`; `search_result` only for `search`.
/// `is_running` and `run_operation` are both required for `direct_download`, `build_bundle`, and
/// `install_bundle`. Search receives the original arguments plus a trimmed query. Run receives the
/// original arguments plus the trimmed, lower-case operation. Callback results are returned
/// unchanged and therefore must carry the JSON tool contract described on run().
struct AiOfflineDownloaderToolCallbacks {
    std::function<bool()> is_running;
    std::function<QJsonObject()> presets_result;
    std::function<QJsonObject(const QJsonObject& args, const QString& query)> search_result;
    std::function<QJsonObject(const QJsonObject& args, const QString& operation)> run_operation;
};

class AiOfflineDownloaderToolRunner {
public:
    AiOfflineDownloaderToolRunner() = delete;

    /// @brief Validate and route one offline-downloader tool request.
    /// @param args JSON object containing required string `operation`; `search` also reads `query`.
    /// @param callbacks Operation-specific callbacks described above.
    /// @return Callback result unchanged, or a runner-generated failure object with
    ///         `success: false` and non-empty string `error_message`. Successful callbacks must
    ///         return `success: true`; callback failures must return `success: false` plus
    ///         `error_message`. Run operations are process-wide serialized and return the same
    ///         failure schema when another operation is already claimed or running.
    [[nodiscard]] static QJsonObject run(const QJsonObject& args,
                                         const AiOfflineDownloaderToolCallbacks& callbacks);
};

}  // namespace sak::ai
