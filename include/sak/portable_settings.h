// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "sak/app_paths.h"

#include <QSettings>

namespace sak {

/// UI state that is not configuration -- column widths, splitter positions, last folders, tags,
/// explorer sessions -- kept in the portable data folder (data/config/ui_state.ini).
///
/// A default-constructed QSettings writes HKEY_CURRENT_USER on Windows, which a portable tool must
/// not do. Configuration proper goes through ConfigManager (data/config/Utility.ini); this file is
/// separate so ConfigManager stays the only writer of Utility.ini.
class PortableSettings final : public QSettings {
public:
    PortableSettings() : QSettings(app_paths::uiStateFilePath(), QSettings::IniFormat) {}
};

}  // namespace sak
