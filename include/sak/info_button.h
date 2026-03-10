// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QPointer>
#include <QToolButton>

class QFrame;
class QLabel;

namespace sak {

/**
 * @brief Clickable info icon that shows a themed popup with explanatory text.
 *
 * Renders a blue circle-i icon (Windows 11 accent color #0078D4).
 * Clicking the button toggles a popup frame styled to match the app's
 * Windows 11 light theme.
 *
 * Usage examples:
 * @code
 *   // Standalone button
 *   auto* btn = new InfoButton(tr("Explains what this setting does"), parent);
 *
 *   // Label + info button helper for QFormLayout
 *   formLayout->addRow(InfoButton::createInfoLabel(tr("Setting:"),
 *                      tr("This setting controls …"), parent), widget);
 * @endcode
 */
class InfoButton : public QToolButton {
    Q_OBJECT

public:
    explicit InfoButton(const QString& infoText, QWidget* parent = nullptr);

    /**
     * @brief Create a composite widget: [QLabel] [InfoButton]
     *
     * Suitable for use as the label argument in QFormLayout::addRow().
     * @param labelText  The label text (e.g. "Thread Count:")
     * @param infoText   The explanatory text shown in the popup
     * @param parent     Parent widget
     * @return A QWidget* containing the label and info button side-by-side
     */
    static QWidget* createInfoLabel(const QString& labelText,
                                    const QString& infoText,
                                    QWidget* parent = nullptr);

private Q_SLOTS:
    void togglePopup();

private:
    static QIcon createInfoIcon(int size);

    QString m_infoText;
    QPointer<QFrame> m_popup;
};

}  // namespace sak
