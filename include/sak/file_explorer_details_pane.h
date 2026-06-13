// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_details_pane.h
/// @brief File Explorer preview, properties, safety, and evidence tabs.

#pragma once

#include <QPlainTextEdit>
#include <QTabWidget>

namespace sak {

class FileExplorerDetailsPane : public QTabWidget {
    Q_OBJECT

public:
    explicit FileExplorerDetailsPane(QWidget* parent = nullptr);

    [[nodiscard]] QPlainTextEdit* previewText() const;
    [[nodiscard]] QPlainTextEdit* propertiesText() const;
    [[nodiscard]] QPlainTextEdit* safetyText() const;
    [[nodiscard]] QPlainTextEdit* evidenceText() const;

private:
    QPlainTextEdit* makeDetailsText(const QString& accessible_name);

    QPlainTextEdit* m_preview_text{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
};

}  // namespace sak
