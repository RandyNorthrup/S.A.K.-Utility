#pragma once

#include <QApplication>
#include <QString>

namespace sak::ui {

QString windows11ThemeStyleSheet();
void applyWindows11Theme(QApplication& app);
void installTooltipHelper(QApplication& app);

} // namespace sak::ui
