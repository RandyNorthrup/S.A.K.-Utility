// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_details_pane.h"

#include "sak/layout_constants.h"

#include <QScrollArea>
#include <QVBoxLayout>

namespace sak {

FileExplorerDetailsPane::FileExplorerDetailsPane(QWidget* parent) : QTabWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerDetailsTabs"));
    setAccessibleName(tr("Explorer preview, properties, safety, and evidence"));
    setMinimumWidth(kFileExplorerDetailsPaneMinW);
    setMaximumWidth(kFileExplorerDetailsPaneMaxW);

    m_properties_text = makeDetailsText(tr("Explorer item properties"));
    m_properties_text->setObjectName(QStringLiteral("fileExplorerPropertiesText"));
    m_safety_text = makeDetailsText(tr("Explorer target safety"));
    m_safety_text->setObjectName(QStringLiteral("fileExplorerSafetyText"));
    m_evidence_text = makeDetailsText(tr("Explorer evidence details"));
    m_evidence_text->setObjectName(QStringLiteral("fileExplorerEvidenceText"));

    addTab(buildPreviewTab(), tr("Preview"));
    addTab(m_properties_text, tr("Properties"));
    addTab(m_safety_text, tr("Safety"));
    addTab(m_evidence_text, tr("Evidence"));
}

QWidget* FileExplorerDetailsPane::buildPreviewTab() {
    auto* container = new QWidget(this);
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    m_preview_caption = new QLabel(container);
    m_preview_caption->setObjectName(QStringLiteral("fileExplorerPreviewCaption"));
    m_preview_caption->setWordWrap(true);
    m_preview_caption->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_preview_caption->setAccessibleName(tr("Explorer preview caption"));
    layout->addWidget(m_preview_caption);

    m_preview_stack = new QStackedWidget(container);
    m_preview_stack->setObjectName(QStringLiteral("fileExplorerPreviewStack"));
    m_preview_text = makeDetailsText(tr("Explorer preview details"));
    m_preview_text->setObjectName(QStringLiteral("fileExplorerPreviewText"));

    auto* scroll = new QScrollArea(container);
    scroll->setWidgetResizable(true);
    m_preview_image = new QLabel(scroll);
    m_preview_image->setObjectName(QStringLiteral("fileExplorerPreviewImage"));
    m_preview_image->setAlignment(Qt::AlignCenter);
    m_preview_image->setAccessibleName(tr("Explorer image preview"));
    scroll->setWidget(m_preview_image);

    m_preview_stack->addWidget(m_preview_text);
    m_preview_stack->addWidget(scroll);
    layout->addWidget(m_preview_stack, 1);
    return container;
}

void FileExplorerDetailsPane::showImagePreview(bool image) {
    if (m_preview_stack) {
        m_preview_stack->setCurrentIndex(image ? 1 : 0);
    }
}

QPlainTextEdit* FileExplorerDetailsPane::makeDetailsText(const QString& accessible_name) {
    auto* editor = new QPlainTextEdit(this);
    editor->setReadOnly(true);
    editor->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    editor->setAccessibleName(accessible_name);
    return editor;
}

QPlainTextEdit* FileExplorerDetailsPane::previewText() const {
    return m_preview_text;
}

QLabel* FileExplorerDetailsPane::previewImage() const {
    return m_preview_image;
}

QLabel* FileExplorerDetailsPane::previewCaption() const {
    return m_preview_caption;
}

QPlainTextEdit* FileExplorerDetailsPane::propertiesText() const {
    return m_properties_text;
}

QPlainTextEdit* FileExplorerDetailsPane::safetyText() const {
    return m_safety_text;
}

QPlainTextEdit* FileExplorerDetailsPane::evidenceText() const {
    return m_evidence_text;
}

}  // namespace sak
