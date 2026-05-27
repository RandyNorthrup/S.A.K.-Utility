// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>

namespace sak::app_paths {

[[nodiscard]] QString applicationDirectory();
[[nodiscard]] bool isPackaged();
[[nodiscard]] QString dataRoot();
[[nodiscard]] QString configDirectory();
[[nodiscard]] QString configFilePath();
[[nodiscard]] QString logsDirectory();
[[nodiscard]] QString tempDirectory();
[[nodiscard]] bool ensureDirectory(const QString& path);

}  // namespace sak::app_paths
