// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

/// @file accessibility_audit.h
/// @brief The accessibility audit: what it holds every widget to, and which windows it looks at.
///
/// The application's --accessibility-audit mode and a unit test run the same code, so what the
/// audit finds can be found without running the application.

#include <QList>
#include <QString>
#include <QStringList>

class QWidget;

namespace sak {

/// @brief What the audit found. Each entry describes one widget and where it sits.
struct AccessibilityFindings {
    /// A control a screen reader or voice control has no explicit name for.
    QStringList missing;
    /// A button whose accessible name leaves out the words on it.
    QStringList mislabeled;
    /// A label whose accessible name stops a screen reader reading its text.
    QStringList hides_text;
    /// A named label with no text, so an image, that has no description of what it shows.
    QStringList undescribed_image;
    /// How many widgets were looked at.
    qsizetype checked{0};

    [[nodiscard]] bool passed() const {
        return missing.isEmpty() && mislabeled.isEmpty() && hides_text.isEmpty() &&
               undescribed_image.isEmpty();
    }

    /// Add what another walk found.
    AccessibilityFindings& operator+=(const AccessibilityFindings& other);
};

/// @brief Audit @p widget and everything inside it.
[[nodiscard]] AccessibilityFindings auditWidgetTree(const QWidget& widget);

/// @brief Audit @p main_window, then each dialog the application can build without doing anything:
///        no file opened, no drive or network touched, nothing shown. Dialogs built inside the
///        function that runs them are out of reach; docs/RELEASE_READINESS.md lists them.
[[nodiscard]] AccessibilityFindings auditApplication(const QWidget& main_window);

}  // namespace sak
