// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QTextEdit>
#include <QWidget>

class QTimer;

namespace sak {

/**
 * @brief A shared log window that snaps to the main window's right edge.
 *
 * Owned by MainWindow. All panels emit logOutput signals which are routed
 * here. The user can drag the window freely; it snaps back to the main
 * window edge when released nearby.
 */
class DetachableLogWindow : public QWidget {
    Q_OBJECT

public:
    explicit DetachableLogWindow(const QString& title, QWidget* parent = nullptr);
    ~DetachableLogWindow() override;

    /** Append a timestamped log entry. */
    void appendLog(const QString& message);

    /** Clear the log. */
    void clearLog();

    /** Get the underlying text edit (read-only access). */
    QTextEdit* logTextEdit() const { return m_logEdit; }

    /** Toggle visibility. */
    void setLogVisible(bool visible);
    [[nodiscard]] bool isLogVisible() const;

    /** Re-snap to main window (called when main window moves/resizes). */
    void repositionIfAnchored();

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

Q_SIGNALS:
    void visibilityChanged(bool visible);

private:
    void snapToMainWindow();
    QWidget* findMainWindow() const;

    QTextEdit* m_logEdit{nullptr};
    bool m_anchored{true};
    bool m_programmaticMove{false};
    QTimer* m_snapTimer{nullptr};
};

/**
 * @brief A styled toggle switch widget for showing/hiding the log window.
 *
 * Renders as a rounded slider-style toggle that matches the application
 * button style.
 */
class LogToggleSwitch : public QWidget {
    Q_OBJECT

public:
    explicit LogToggleSwitch(const QString& label, QWidget* parent = nullptr);

    bool isChecked() const { return m_checked; }
    void setChecked(bool checked);

Q_SIGNALS:
    void toggled(bool checked);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;

private:
    bool m_checked{false};
    QString m_label;
};

}  // namespace sak
