// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file file_explorer_details_pane.h
/// @brief File Explorer preview, properties, safety, and evidence tabs.

#pragma once

#include <QLabel>
#include <QPlainTextEdit>
#include <QStackedWidget>
#include <QTabWidget>

namespace sak {

class FileExplorerDetailsPane : public QTabWidget {
    Q_OBJECT

public:
    explicit FileExplorerDetailsPane(QWidget* parent = nullptr);

    [[nodiscard]] QPlainTextEdit* previewText() const;
    [[nodiscard]] QLabel* previewImage() const;
    [[nodiscard]] QLabel* previewCaption() const;
    [[nodiscard]] QPlainTextEdit* propertiesText() const;
    [[nodiscard]] QPlainTextEdit* safetyText() const;
    [[nodiscard]] QPlainTextEdit* evidenceText() const;

    /// Switch the Preview tab between the text/hex view (false) and the image view (true).
    void showImagePreview(bool image);

private:
    QPlainTextEdit* makeDetailsText(const QString& accessible_name);
    QWidget* buildPreviewTab();

    QPlainTextEdit* m_preview_text{nullptr};
    QLabel* m_preview_caption{nullptr};
    QLabel* m_preview_image{nullptr};
    QStackedWidget* m_preview_stack{nullptr};
    QPlainTextEdit* m_properties_text{nullptr};
    QPlainTextEdit* m_safety_text{nullptr};
    QPlainTextEdit* m_evidence_text{nullptr};
};

}  // namespace sak
