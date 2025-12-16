// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include "sak/quick_action.h"
#include <QString>

namespace sak {

/**
 * @brief Disable Visual Effects Action
 * 
 * Disables Windows visual effects (animations, transparency, shadows)
 * for improved performance on lower-end systems.
 */
class DisableVisualEffectsAction : public QuickAction {
    Q_OBJECT

public:
    explicit DisableVisualEffectsAction(QObject* parent = nullptr);

    QString name() const override { return "Disable Visual Effects"; }
    QString description() const override { return "Reduce animations for better performance"; }
    QIcon icon() const override { return QIcon(); }
    ActionCategory category() const override { return ActionCategory::SystemOptimization; }
    bool requiresAdmin() const override { return false; }

    void scan() override;
    void execute() override;

private:
    bool m_effects_enabled{false};
    int m_effects_count{0};

    bool areVisualEffectsEnabled();
    bool disableVisualEffects();
};

} // namespace sak

