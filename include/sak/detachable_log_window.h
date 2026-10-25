// Copyright (c) 2025 Randy Northrup. All rights reserved.
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <QMargins>
#include <QRect>
#include <QStringList>
#include <QTextEdit>
#include <QWidget>

class QEvent;
class QPushButton;
class QTimer;

namespace sak {

class FollowScrollController;

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

    /// Replace the shown log with @p lines (one panel's backing store). Does nothing while the
    /// window is hidden and returns false: whoever shows it refills it then (MainWindow does on
    /// visibilityChanged). Rebuilding a 5000-line document on every tab switch while nobody
    /// could see it was pure GUI-thread work. One edit block per refill.
    bool replaceLog(const QStringList& lines);

    /** Clear the log. */
    void clearLog();

    /** Get the underlying text edit (read-only access). */
    QTextEdit* logTextEdit() const { return m_logEdit; }

    /** Toggle visibility. */
    void setLogVisible(bool visible);
    [[nodiscard]] bool isLogVisible() const;

    /** Re-snap to main window (called when main window moves/resizes). */
    void repositionIfAnchored();

    /// Where the anchored window goes: its frame against the main window's right edge and top,
    /// its content as tall as the main window's, but the whole frame (content plus
    /// @p log_frame) always inside @p available (the screen's work area). Beside a maximized
    /// main window there is no room on the right, so it overlaps the right edge instead of
    /// opening off-screen. topLeft() is the frame position for move(); size() is the content
    /// size for resize(). Pure; tested offscreen.
    [[nodiscard]] static QRect anchoredGeometry(const QRect& main_frame,
                                                const QRect& main_geometry,
                                                int log_width,
                                                const QMargins& log_frame,
                                                const QRect& available);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void moveEvent(QMoveEvent* event) override;

Q_SIGNALS:
    void visibilityChanged(bool visible);

private:
    void snapToMainWindow();
    QWidget* findMainWindow() const;

    QTextEdit* m_logEdit{nullptr};
    QPushButton* m_jumpToNewestButton{nullptr};
    FollowScrollController* m_logScrollController{nullptr};
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
    [[nodiscard]] QSize sizeHint() const override;
    [[nodiscard]] QSize minimumSizeHint() const override;

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
