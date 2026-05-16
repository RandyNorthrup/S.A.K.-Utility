// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QString>

namespace sak::ai {

[[nodiscard]] QString credentialDirectory();
[[nodiscard]] QString sessionRootDirectory();
[[nodiscard]] QString workflowLibraryDirectory();

}  // namespace sak::ai
