// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QApplication>
#include <QString>

namespace sak::ui {

QString windows11ThemeStyleSheet();
void applyWindows11Theme(QApplication& app);
void installTooltipHelper(QApplication& app);

} // namespace sak::ui
