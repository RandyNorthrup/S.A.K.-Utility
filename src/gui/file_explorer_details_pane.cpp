// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "sak/file_explorer_details_pane.h"

#include "sak/layout_constants.h"

namespace sak {

FileExplorerDetailsPane::FileExplorerDetailsPane(QWidget* parent) : QTabWidget(parent) {
    setObjectName(QStringLiteral("fileExplorerDetailsTabs"));
    setAccessibleName(tr("Explorer preview, properties, safety, and evidence"));
    setMinimumWidth(kFileExplorerDetailsPaneMinW);
    setMaximumWidth(kFileExplorerDetailsPaneMaxW);

    m_preview_text = makeDetailsText(tr("Explorer preview details"));
    m_preview_text->setObjectName(QStringLiteral("fileExplorerPreviewText"));
    m_properties_text = makeDetailsText(tr("Explorer item properties"));
    m_properties_text->setObjectName(QStringLiteral("fileExplorerPropertiesText"));
    m_safety_text = makeDetailsText(tr("Explorer target safety"));
    m_safety_text->setObjectName(QStringLiteral("fileExplorerSafetyText"));
    m_evidence_text = makeDetailsText(tr("Explorer evidence details"));
    m_evidence_text->setObjectName(QStringLiteral("fileExplorerEvidenceText"));

    addTab(m_preview_text, tr("Preview"));
    addTab(m_properties_text, tr("Properties"));
    addTab(m_safety_text, tr("Safety"));
    addTab(m_evidence_text, tr("Evidence"));
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
