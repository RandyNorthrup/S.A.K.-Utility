// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file app_data_program_prompt.h
/// @brief The question both profile wizards ask before touching application data.

#include "sak/app_data_sources.h"
#include "sak/message_box_helpers.h"

#include <QCoreApplication>
#include <QMessageBox>
#include <QStringList>
#include <QVector>
#include <QWidget>

namespace sak {

/// @brief Ask the person to close programs holding the selected @p sources open. A running program
///        can lock its files or rewrite them during the copy.
/// @return true to go ahead, because nothing is running or the person chose to anyway.
[[nodiscard]] inline bool askToCloseProgramsHolding(QWidget* parent,
                                                    const QVector<AppDataSourceInfo>& sources) {
    for (;;) {
        const QStringList held = appDataSourcesHeldOpen(sources, runningProcessNames());
        if (held.isEmpty()) {
            return true;
        }
        const auto answer = showQuestionLogged(
            parent,
            QCoreApplication::translate("sak::AppDataPrograms", "Close Programs First"),
            QCoreApplication::translate(
                "sak::AppDataPrograms",
                "These programs are running, and a running program can lock its files or change "
                "them during the copy:\n\n%1\n\nClose them and choose Retry, or choose Ignore to "
                "go ahead anyway. The log names each file that could not be copied.")
                .arg(held.join(QLatin1Char('\n'))),
            QMessageBox::Retry | QMessageBox::Ignore | QMessageBox::Cancel,
            QMessageBox::Retry);
        if (answer == QMessageBox::Ignore) {
            return true;
        }
        if (answer != QMessageBox::Retry) {
            return false;
        }
    }
}

}  // namespace sak
