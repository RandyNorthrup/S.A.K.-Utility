// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file partition_manager_scenario_step.h
/// @brief One step of a certification row: what a person's Apply does, on one pinned disk.

#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace certification {

/// Runs the step a job file describes, and writes its report to the path the job names.
/// @return the process's exit code.
int runScenarioStep(const QString& job_path);

/// Pure checks of a step's refusals. Touches no disk and starts no process.
void selfTestScenarioStep(QJsonArray* checks);

}  // namespace certification
