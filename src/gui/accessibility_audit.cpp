// Copyright (c) 2026 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

/// @file accessibility_audit.cpp
/// @brief The accessibility audit. See accessibility_audit.h.

#include "sak/accessibility_audit.h"

#include "sak/accessible_name_rule.h"
#include "sak/app_installation_worker.h"
#include "sak/email_attachments_browser_dialog.h"
#include "sak/email_calendar_dialog.h"
#include "sak/email_contacts_dialog.h"
#include "sak/email_file_scanner_dialog.h"
#include "sak/email_inspector_controller.h"
#include "sak/file_explorer_properties_dialog.h"
#include "sak/image_flasher_settings_dialog.h"
#include "sak/install_summary_dialog.h"
#include "sak/linux_iso_download_dialog.h"
#include "sak/linux_iso_downloader.h"
#include "sak/per_user_customization_dialog.h"
#include "sak/user_profile_backup_wizard.h"
#include "sak/user_profile_restore_wizard.h"
#include "sak/windows_iso_download_dialog.h"
#include "sak/windows_iso_downloader.h"

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTabWidget>
#include <QTextDocumentFragment>
#include <QTextEdit>
#include <QWidget>
#include <QWizard>

#include <memory>
#include <vector>

namespace sak {

namespace {

constexpr int kParentDepthLimit = 5;

bool isKnownQtInternalWidget(const QWidget* widget) {
    if (widget == nullptr) {
        return true;
    }
    if ((qobject_cast<const QScrollBar*>(widget) != nullptr) ||
        (qobject_cast<const QHeaderView*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QLineEdit*>(widget) != nullptr) &&
        (qobject_cast<const QComboBox*>(widget->parentWidget()) != nullptr)) {
        return true;
    }
    // The back arrow QWizard draws for itself in the Windows style. Qt names it.
    if ((qobject_cast<const QWizard*>(widget->parentWidget()) != nullptr) &&
        (qobject_cast<const QAbstractButton*>(widget) != nullptr)) {
        return true;
    }
    const QString class_name = QString::fromLatin1(widget->metaObject()->className());
    const QString object_name = widget->objectName();
    static const QStringList kInternalClasses = {QStringLiteral("QTableCornerButton"),
                                                 QStringLiteral("QComboBoxListView"),
                                                 QStringLiteral("QLineEditIconButton"),
                                                 QStringLiteral("CheckHeaderView")};
    static const QStringList kInternalObjects = {QStringLiteral("qt_spinbox_lineedit"),
                                                 QStringLiteral("qt_menubar_ext_button")};
    return kInternalClasses.contains(class_name) || kInternalObjects.contains(object_name);
}

bool isAuditedInteractiveWidget(const QWidget* widget) {
    if (qobject_cast<const QAbstractButton*>(widget) != nullptr) {
        return true;
    }
    if ((qobject_cast<const QLineEdit*>(widget) != nullptr) ||
        (qobject_cast<const QComboBox*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QAbstractSpinBox*>(widget) != nullptr) ||
        (qobject_cast<const QAbstractSlider*>(widget) != nullptr)) {
        return true;
    }
    if ((qobject_cast<const QTextEdit*>(widget) != nullptr) ||
        (qobject_cast<const QPlainTextEdit*>(widget) != nullptr)) {
        return true;
    }
    return (qobject_cast<const QAbstractItemView*>(widget) != nullptr) ||
           (qobject_cast<const QTabWidget*>(widget) != nullptr);
}

QString objectDebugLabel(const QObject* object) {
    QString label = QString::fromLatin1(object->metaObject()->className());
    if (!object->objectName().isEmpty()) {
        label += QStringLiteral("#%1").arg(object->objectName());
    }
    return label;
}

QString textDebugLabel(const QWidget* widget) {
    if (const auto* button = qobject_cast<const QAbstractButton*>(widget);
        (button != nullptr) && !button->text().trimmed().isEmpty()) {
        return QStringLiteral(" text=\"%1\"").arg(button->text());
    }
    if (const auto* label = qobject_cast<const QLabel*>(widget);
        (label != nullptr) && !label->text().trimmed().isEmpty()) {
        return QStringLiteral(" text=\"%1\"").arg(label->text());
    }
    if (const auto* line_edit = qobject_cast<const QLineEdit*>(widget);
        (line_edit != nullptr) && !line_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(line_edit->placeholderText());
    }
    if (const auto* text_edit = qobject_cast<const QTextEdit*>(widget);
        (text_edit != nullptr) && !text_edit->placeholderText().trimmed().isEmpty()) {
        return QStringLiteral(" placeholder=\"%1\"").arg(text_edit->placeholderText());
    }
    return {};
}

QString parentDebugLabel(const QWidget* widget) {
    QStringList parents;
    const QObject* parent = widget->parent();
    while ((parent != nullptr) && parents.size() < kParentDepthLimit) {
        parents << objectDebugLabel(parent);
        parent = parent->parent();
    }
    return parents.isEmpty()
               ? QString{}
               : QStringLiteral(" parent=\"%1\"").arg(parents.join(QStringLiteral(" > ")));
}

QString debugLabel(const QWidget* widget) {
    return objectDebugLabel(widget) + textDebugLabel(widget) + parentDebugLabel(widget);
}

QString namedDebugLabel(const QWidget* widget) {
    return debugLabel(widget) + QStringLiteral(" name=\"%1\"").arg(widget->accessibleName());
}

/// The words a QLabel shows. A rich-text label holds markup, which a screen reader never says.
QString visibleLabelText(const QLabel& label) {
    const bool rich = label.textFormat() == Qt::RichText ||
                      (label.textFormat() == Qt::AutoText && Qt::mightBeRichText(label.text()));
    return rich ? QTextDocumentFragment::fromHtml(label.text()).toPlainText() : label.text();
}

void auditLabel(const QLabel& label, AccessibilityFindings& findings) {
    if (label.accessibleName().trimmed().isEmpty()) {
        return;  // Qt reads the text itself, which is what a text label wants.
    }
    const QString text = visibleLabelText(label);
    if (text.trimmed().isEmpty()) {
        if (label.accessibleDescription().trimmed().isEmpty()) {
            findings.undescribed_image << namedDebugLabel(&label);
        }
        return;
    }
    if (!accessibleNameHoldsLabel(label.accessibleName(), text)) {
        findings.hides_text << namedDebugLabel(&label);
    }
}

void auditControl(const QWidget& widget, AccessibilityFindings& findings) {
    if (isKnownQtInternalWidget(&widget) || !isAuditedInteractiveWidget(&widget)) {
        return;
    }
    const auto* button = qobject_cast<const QAbstractButton*>(&widget);
    if (widget.accessibleName().trimmed().isEmpty()) {
        // A button with words on it and no name is read by those words, which is the rule below
        // met exactly. Anything else with no name has nothing for a screen reader to say.
        if ((button == nullptr) || button->text().trimmed().isEmpty()) {
            findings.missing << debugLabel(&widget);
        }
        return;
    }
    if ((button != nullptr) &&
        !accessibleNameHoldsLabel(button->accessibleName(), button->text())) {
        findings.mislabeled << namedDebugLabel(&widget);
    }
}

FileManagementEntry samplePropertiesEntry() {
    FileManagementEntry entry;
    entry.name = QStringLiteral("sample.txt");
    entry.regular_file = true;
    return entry;
}

/// Everything a dialog below needs to exist, and the dialogs. None of these constructors opens a
/// file, reaches a drive or the network, or starts work: that waits for the user or for showEvent,
/// and nothing here is shown.
struct AuditableDialogs {
    EmailInspectorController email_controller;
    LinuxISODownloader linux_downloader;
    WindowsISODownloader windows_downloader;
    UserProfile profile;
    std::vector<std::unique_ptr<QWidget>> dialogs;

    AuditableDialogs() {
        add<EmailAttachmentsBrowserDialog>(&email_controller, PstFolderTree{});
        add<EmailCalendarDialog>(&email_controller, QVector<uint64_t>{});
        add<EmailContactsDialog>(&email_controller, QVector<uint64_t>{});
        add<EmailFileScannerDialog>();
        add<FileExplorerPropertiesDialog>(FileManagementTarget{},
                                          QVector<FileManagementEntry>{samplePropertiesEntry()});
        add<ImageFlasherSettingsDialog>();
        add<InstallSummaryDialog>(AppInstallationWorker::Stats{}, QVector<MigrationJob>{});
        add<LinuxISODownloadDialog>(&linux_downloader);
        add<PerUserCustomizationDialog>(profile);
        add<UserProfileBackupWizard>();
        add<UserProfileRestoreWizard>();
        add<WindowsISODownloadDialog>(&windows_downloader);
    }

    template <typename Dialog, typename... Args>
    void add(Args&&... args) {
        dialogs.push_back(std::make_unique<Dialog>(std::forward<Args>(args)...));
    }
};

}  // namespace

AccessibilityFindings& AccessibilityFindings::operator+=(const AccessibilityFindings& other) {
    missing << other.missing;
    mislabeled << other.mislabeled;
    hides_text << other.hides_text;
    undescribed_image << other.undescribed_image;
    checked += other.checked;
    return *this;
}

AccessibilityFindings auditWidgetTree(const QWidget& widget) {
    AccessibilityFindings findings;
    QList<const QWidget*> widgets{&widget};
    const auto children = widget.findChildren<QWidget*>();
    widgets.reserve(widgets.size() + children.size());
    for (const QWidget* child : children) {
        widgets.append(child);
    }
    for (const QWidget* each : widgets) {
        ++findings.checked;
        auditControl(*each, findings);
        if (const auto* label = qobject_cast<const QLabel*>(each)) {
            auditLabel(*label, findings);
        }
    }
    return findings;
}

AccessibilityFindings auditApplication(const QWidget& main_window) {
    AccessibilityFindings findings = auditWidgetTree(main_window);
    const AuditableDialogs built;
    for (const auto& dialog : built.dialogs) {
        findings += auditWidgetTree(*dialog);
    }
    return findings;
}

}  // namespace sak
