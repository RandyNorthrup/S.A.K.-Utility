#include "gui/windows11_theme.h"

#include <QAction>
#include <QAbstractButton>
#include <QComboBox>
#include <QEvent>
#include <QGraphicsDropShadowEffect>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QTabWidget>
#include <QTextEdit>
#include <QToolTip>
#include <QWidget>

namespace sak::ui {

namespace {

QString normalizeText(const QString& text) {
    QString cleaned = text;
    cleaned.remove('&');
    return cleaned.trimmed();
}

QString inferTooltip(QWidget* widget) {
    if (!widget) {
        return {};
    }

    if (!widget->accessibleName().isEmpty()) {
        return widget->accessibleName();
    }

    if (auto* button = qobject_cast<QAbstractButton*>(widget)) {
        return normalizeText(button->text());
    }

    if (auto* label = qobject_cast<QLabel*>(widget)) {
        return normalizeText(label->text());
    }

    if (auto* group = qobject_cast<QGroupBox*>(widget)) {
        return normalizeText(group->title());
    }

    if (auto* edit = qobject_cast<QLineEdit*>(widget)) {
        if (!edit->placeholderText().isEmpty()) {
            return edit->placeholderText();
        }
    }

    if (auto* combo = qobject_cast<QComboBox*>(widget)) {
        if (!combo->placeholderText().isEmpty()) {
            return combo->placeholderText();
        }
        return normalizeText(combo->currentText());
    }

    if (auto* text = qobject_cast<QTextEdit*>(widget)) {
        if (!text->placeholderText().isEmpty()) {
            return text->placeholderText();
        }
    }

    if (!widget->windowTitle().isEmpty()) {
        return normalizeText(widget->windowTitle());
    }

    if (!widget->objectName().isEmpty()) {
        return widget->objectName();
    }

    return widget->metaObject()->className();
}

void applyTabTooltips(QTabWidget* tabs) {
    if (!tabs) {
        return;
    }

    for (int i = 0; i < tabs->count(); ++i) {
        if (tabs->tabToolTip(i).isEmpty()) {
            tabs->setTabToolTip(i, normalizeText(tabs->tabText(i)));
        }
    }
}

void applyTooltips(QWidget* root) {
    if (!root) {
        return;
    }

    auto widgets = root->findChildren<QWidget*>();
    widgets.prepend(root);

    for (QWidget* widget : widgets) {
        if (auto* tabs = qobject_cast<QTabWidget*>(widget)) {
            applyTabTooltips(tabs);
        }

        if (widget->toolTip().isEmpty()) {
            const QString tooltip = inferTooltip(widget);
            if (!tooltip.isEmpty()) {
                widget->setToolTip(tooltip);
            }
        }
    }

    const auto actions = root->findChildren<QAction*>();
    for (QAction* action : actions) {
        if (!action->toolTip().isEmpty()) {
            continue;
        }

        if (!action->statusTip().isEmpty()) {
            action->setToolTip(action->statusTip());
        } else {
            action->setToolTip(normalizeText(action->text()));
        }
    }
}

void applyShadow(QWidget* widget) {
    if (!widget || widget->graphicsEffect()) {
        return;
    }

    const bool should_shadow = qobject_cast<QGroupBox*>(widget)
        || widget->property("sakElevated").toBool()
        || widget->property("sakCard").toBool()
        || (widget->styleSheet().contains("border-radius") && widget->styleSheet().contains("background-color"));

    if (should_shadow) {
        auto* shadow = new QGraphicsDropShadowEffect(widget);
        shadow->setBlurRadius(22.0);
        shadow->setColor(QColor(15, 23, 42, 38));
        shadow->setOffset(0.0, 6.0);
        widget->setGraphicsEffect(shadow);
    }
}

class TooltipEventFilter final : public QObject {
public:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (!watched || !event) {
            return QObject::eventFilter(watched, event);
        }

        if (event->type() == QEvent::Show || event->type() == QEvent::Polish) {
            auto* widget = qobject_cast<QWidget*>(watched);
            if (!widget) {
                return QObject::eventFilter(watched, event);
            }

            if (!widget->property("sakTooltipsApplied").toBool()) {
                applyTooltips(widget);
                widget->setProperty("sakTooltipsApplied", true);
            }

            if (!widget->property("sakShadowApplied").toBool()) {
                applyShadow(widget);
                widget->setProperty("sakShadowApplied", true);
            }
        }

        return QObject::eventFilter(watched, event);
    }
};

} // namespace

QString windows11ThemeStyleSheet() {
    return QStringLiteral(R"(
        * {
            font-family: "Segoe UI";
            font-size: 10pt;
        }

        QWidget {
            color: #0f172a;
            background-color: #f3f5f9;
        }

        QMainWindow {
            background-color: #eef2f7;
        }

        QDialog, QFrame, QGroupBox {
            background-color: rgba(255, 255, 255, 0.92);
        }

        QToolTip {
            color: #0f172a;
            background-color: rgba(255, 255, 255, 0.95);
            border: 1px solid #cbd5e1;
            border-radius: 8px;
            padding: 6px 10px;
        }

        QMenuBar {
            background-color: rgba(255, 255, 255, 0.85);
            border-bottom: 1px solid rgba(148, 163, 184, 0.4);
        }

        QToolBar {
            background-color: rgba(255, 255, 255, 0.88);
            border-bottom: 1px solid rgba(148, 163, 184, 0.35);
            spacing: 8px;
            padding: 6px;
        }

        QMenuBar::item {
            padding: 6px 12px;
            border-radius: 6px;
            background: transparent;
        }

        QMenuBar::item:selected {
            background-color: rgba(59, 130, 246, 0.15);
        }

        QMenu {
            background-color: rgba(255, 255, 255, 0.97);
            border: 1px solid #cbd5e1;
            border-radius: 8px;
            padding: 6px;
        }

        QMenu::item {
            padding: 6px 16px;
            border-radius: 6px;
        }

        QMenu::item:selected {
            background-color: rgba(59, 130, 246, 0.18);
        }

        QTabWidget::pane {
            border: 1px solid rgba(148, 163, 184, 0.45);
            border-radius: 12px;
            padding: 0px;
            background-color: rgba(255, 255, 255, 0.8);
        }

        QTabBar::tab {
            background: rgba(226, 232, 240, 0.9);
            border-radius: 10px;
            padding: 6px 12px;
            margin: 2px;
            color: #1e293b;
        }

        QTabBar::tab:selected {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:1,
                stop:0 #3b82f6, stop:1 #2563eb);
            color: #ffffff;
        }

        QPushButton, QToolButton {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #3b82f6, stop:1 #2563eb);
            color: #ffffff;
            border: 1px solid #1d4ed8;
            border-radius: 10px;
            padding: 8px 14px;
        }

        QPushButton:hover, QToolButton:hover {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #4f8efc, stop:1 #3b82f6);
        }

        QPushButton:pressed, QToolButton:pressed {
            background: qlineargradient(x1:0, y1:0, x2:0, y2:1,
                stop:0 #2563eb, stop:1 #1d4ed8);
        }

        QPushButton:disabled, QToolButton:disabled {
            background: #cbd5e1;
            color: #64748b;
            border: 1px solid #cbd5e1;
        }

        QLineEdit, QPlainTextEdit, QTextEdit, QComboBox, QSpinBox, QDoubleSpinBox, QDateEdit, QTimeEdit {
            background-color: rgba(255, 255, 255, 0.98);
            border: 1px solid #cbd5e1;
            border-radius: 10px;
            padding: 6px 10px;
            selection-background-color: rgba(59, 130, 246, 0.3);
        }

        QLineEdit:focus, QPlainTextEdit:focus, QTextEdit:focus, QComboBox:focus, QSpinBox:focus, QDoubleSpinBox:focus, QDateEdit:focus, QTimeEdit:focus {
            border: 1px solid #3b82f6;
            background-color: #ffffff;
        }

        QComboBox::drop-down {
            border-left: 1px solid #cbd5e1;
            width: 24px;
        }

        QCheckBox, QRadioButton {
            spacing: 8px;
        }

        QCheckBox::indicator, QRadioButton::indicator {
            width: 16px;
            height: 16px;
        }

        QCheckBox::indicator {
            border: 1px solid #94a3b8;
            border-radius: 4px;
            background: #f8fafc;
        }

        QCheckBox::indicator:checked {
            background: #3b82f6;
            border: 1px solid #2563eb;
        }

        QRadioButton::indicator {
            border: 1px solid #94a3b8;
            border-radius: 8px;
            background: #f8fafc;
        }

        QRadioButton::indicator:checked {
            background: #3b82f6;
            border: 1px solid #2563eb;
        }

        QProgressBar {
            border: 1px solid #cbd5e1;
            border-radius: 9px;
            background: rgba(226, 232, 240, 0.85);
            text-align: center;
        }

        QProgressBar::chunk {
            background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
                stop:0 #3b82f6, stop:1 #1d4ed8);
            border-radius: 8px;
        }

        QGroupBox {
            border: 1px solid rgba(148, 163, 184, 0.55);
            border-radius: 12px;
            margin-top: 18px;
            padding: 18px 10px 10px 10px;
        }

        QGroupBox::title {
            subcontrol-origin: margin;
            subcontrol-position: top left;
            padding: 0px 8px;
            color: #334155;
        }

        QHeaderView::section {
            background-color: rgba(226, 232, 240, 0.95);
            border: none;
            padding: 6px 8px;
        }

        QTableView, QListView, QTreeView {
            background: rgba(255, 255, 255, 0.96);
            border: 1px solid #cbd5e1;
            border-radius: 10px;
            gridline-color: rgba(148, 163, 184, 0.3);
            selection-background-color: rgba(59, 130, 246, 0.2);
            selection-color: #0f172a;
            padding: 4px;
        }

        QAbstractItemView::item {
            padding: 6px;
            border-radius: 6px;
        }

        QAbstractItemView::item:selected {
            background: rgba(59, 130, 246, 0.22);
        }

        QScrollArea {
            border: none;
            background: transparent;
        }

        QSplitter::handle {
            background: rgba(148, 163, 184, 0.4);
        }

        QSlider::groove:horizontal {
            height: 6px;
            background: rgba(148, 163, 184, 0.4);
            border-radius: 3px;
        }

        QSlider::handle:horizontal {
            background: #3b82f6;
            border: 1px solid #2563eb;
            width: 16px;
            margin: -6px 0;
            border-radius: 8px;
        }

        QSlider::groove:vertical {
            width: 6px;
            background: rgba(148, 163, 184, 0.4);
            border-radius: 3px;
        }

        QSlider::handle:vertical {
            background: #3b82f6;
            border: 1px solid #2563eb;
            height: 16px;
            margin: 0 -6px;
            border-radius: 8px;
        }

        QScrollBar:vertical, QScrollBar:horizontal {
            background: transparent;
            border: none;
            margin: 2px;
        }

        QScrollBar::groove:vertical, QScrollBar::groove:horizontal {
            background: transparent;
            border: none;
        }

        QScrollBar::handle:vertical, QScrollBar::handle:horizontal {
            background: rgba(148, 163, 184, 0.6);
            border-radius: 6px;
            min-height: 32px;
            min-width: 32px;
        }

        QScrollBar::add-line, QScrollBar::sub-line {
            background: transparent;
            border: none;
            width: 0px;
            height: 0px;
        }

        QScrollBar::add-page, QScrollBar::sub-page {
            background: transparent;
        }

        QScrollBar::handle:hover {
            background: rgba(59, 130, 246, 0.7);
        }

        QAbstractScrollArea::corner {
            background: transparent;
        }

        QStatusBar {
            background: rgba(255, 255, 255, 0.9);
            border-top: 1px solid rgba(148, 163, 184, 0.4);
        }
    )");
}

void applyWindows11Theme(QApplication& app) {
    app.setStyleSheet(windows11ThemeStyleSheet());
}

void installTooltipHelper(QApplication& app) {
    app.installEventFilter(new TooltipEventFilter());
}

} // namespace sak::ui
